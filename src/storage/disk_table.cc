//
// Created by yangjun on 12/14/18.
//

#include "storage/disk_table.h"
#include "logging.h"
#include "base/hash.h"
#include "base/file_util.h"

using ::baidu::common::INFO;
using ::baidu::common::WARNING;
using ::baidu::common::DEBUG;

DECLARE_string(ssd_root_path);
DECLARE_string(hdd_root_path);

namespace rtidb {
namespace storage {

static rocksdb::Options ssd_option_template;
static rocksdb::Options hdd_option_template;
static bool options_template_initialized = false;

DiskTable::DiskTable(const std::string &name, uint32_t id, uint32_t pid,
                     const std::map<std::string, uint32_t> &mapping, 
                     uint64_t ttl, ::rtidb::api::TTLType ttl_type,
                     ::rtidb::common::StorageMode storage_mode) :
        Table(storage_mode, name, id, pid, ttl * 60 * 1000, true, 0, mapping, ttl_type, 
                ::rtidb::api::CompressType::kNoCompress),
        offset_(0) {
    if (!options_template_initialized) {
        initOptionTemplate();
    }
    db_ = nullptr;
}

DiskTable::DiskTable(const ::rtidb::api::TableMeta& table_meta) :
        Table(table_meta.storage_mode(), table_meta.name(), table_meta.tid(), table_meta.pid(),
                0, true, 0, std::map<std::string, uint32_t>(), 
                ::rtidb::api::TTLType::kAbsoluteTime, ::rtidb::api::CompressType::kNoCompress) {
    table_meta_.CopyFrom(table_meta);
    if (!options_template_initialized) {
        initOptionTemplate();
    }
    db_ = nullptr;
}

DiskTable::~DiskTable() {
    for (auto handle : cf_hs_) {
        delete handle;
    }
    if (db_ != nullptr) {
        delete db_;
    }
}

void DiskTable::initOptionTemplate() {
    std::shared_ptr<rocksdb::Cache> cache = rocksdb::NewLRUCache(512 << 20, 8); //Can be set by flags
    //SSD options template
    ssd_option_template.max_open_files = -1;
    ssd_option_template.env->SetBackgroundThreads(1, rocksdb::Env::Priority::HIGH); //flush threads
    ssd_option_template.env->SetBackgroundThreads(4, rocksdb::Env::Priority::LOW);  //compaction threads
    ssd_option_template.memtable_prefix_bloom_size_ratio = 0.02;
    ssd_option_template.compaction_style = rocksdb::kCompactionStyleLevel;
    ssd_option_template.level0_file_num_compaction_trigger = 10;
    ssd_option_template.level0_slowdown_writes_trigger = 20;
    ssd_option_template.level0_stop_writes_trigger = 40;
    ssd_option_template.write_buffer_size = 64 << 20;
    ssd_option_template.target_file_size_base = 64 << 20;
    ssd_option_template.max_bytes_for_level_base = 512 << 20;

    rocksdb::BlockBasedTableOptions table_options;
    table_options.cache_index_and_filter_blocks = true;
    table_options.pin_l0_filter_and_index_blocks_in_cache = true;
    table_options.block_cache = cache;
    // table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
    table_options.whole_key_filtering = false;
    table_options.block_size = 256 << 10;
    table_options.use_delta_encoding = false;
    ssd_option_template.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));

    //HDD options template
    hdd_option_template.max_open_files = -1;
    hdd_option_template.env->SetBackgroundThreads(1, rocksdb::Env::Priority::HIGH); //flush threads
    hdd_option_template.env->SetBackgroundThreads(1, rocksdb::Env::Priority::LOW);  //compaction threads
    hdd_option_template.memtable_prefix_bloom_size_ratio = 0.02;
    hdd_option_template.optimize_filters_for_hits = true;
    hdd_option_template.level_compaction_dynamic_level_bytes = true;
    hdd_option_template.max_file_opening_threads = 1; //set to the number of disks on which the db root folder is mounted
    hdd_option_template.compaction_readahead_size = 16 << 20;
    hdd_option_template.new_table_reader_for_compaction_inputs = true;
    hdd_option_template.compaction_style = rocksdb::kCompactionStyleLevel;
    hdd_option_template.level0_file_num_compaction_trigger = 10;
    hdd_option_template.level0_slowdown_writes_trigger = 20;
    hdd_option_template.level0_stop_writes_trigger = 40;
    hdd_option_template.write_buffer_size = 256 << 20;
    hdd_option_template.target_file_size_base = 256 << 20;
    hdd_option_template.max_bytes_for_level_base = 1024 << 20;
    hdd_option_template.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));

    options_template_initialized = true;
}

bool DiskTable::InitColumnFamilyDescriptor() {
    cf_ds_.clear();
    cf_ds_.push_back(rocksdb::ColumnFamilyDescriptor(rocksdb::kDefaultColumnFamilyName, 
            rocksdb::ColumnFamilyOptions()));
    for (auto iter = mapping_.begin(); iter != mapping_.end(); ++iter) {
        rocksdb::ColumnFamilyOptions cfo;
        if (storage_mode_ == ::rtidb::common::StorageMode::kSSD) {
            cfo = rocksdb::ColumnFamilyOptions(ssd_option_template);
            options_ = ssd_option_template;
        } else {
            cfo = rocksdb::ColumnFamilyOptions(hdd_option_template);
            options_ = hdd_option_template;
        }
        cfo.comparator = &cmp_;
        cfo.prefix_extractor.reset(new KeyTsPrefixTransform());
        if (ttl_type_ == ::rtidb::api::TTLType::kAbsoluteTime && ttl_ > 0) {
            cfo.compaction_filter_factory = std::make_shared<AbsoluteTTLFilterFactory>(ttl_);
        }
        cf_ds_.push_back(rocksdb::ColumnFamilyDescriptor(iter->first, cfo));
        PDLOG(DEBUG, "add cf_name %s. tid %u pid %u", iter->first.c_str(), id_, pid_);
    }
    return true;
}

bool DiskTable::Init() {
    if (mapping_.empty()) {
        for (int32_t i = 0; i < table_meta_.dimensions_size(); i++) {
            mapping_.insert(std::make_pair(table_meta_.dimensions(i),
                           (uint32_t)i));
            PDLOG(INFO, "add index name %s, idx %d to table %s, tid %u, pid %u", table_meta_.dimensions(i).c_str(),
                    i, table_meta_.name().c_str(), table_meta_.tid(), table_meta_.pid());
        }
        // add default dimension
        if (mapping_.empty()) {
            mapping_.insert(std::make_pair("idx0", 0));
            PDLOG(INFO, "no index specified with default");
        }
    }
    if (table_meta_.has_mode() && table_meta_.mode() != ::rtidb::api::TableMode::kTableLeader) {
        is_leader_ = false;
    }
    if (table_meta_.has_ttl()) {
        ttl_ = table_meta_.ttl() * 60 * 1000;
        new_ttl_.store(ttl_.load());
    }
    if (table_meta_.has_schema()) schema_ = table_meta_.schema();
    if (table_meta_.has_ttl_type()) ttl_type_ = table_meta_.ttl_type();
    if (table_meta_.has_compress_type()) compress_type_ = table_meta_.compress_type();
    idx_cnt_ = mapping_.size();

    InitColumnFamilyDescriptor();
    std::string root_path = storage_mode_ == ::rtidb::common::StorageMode::kSSD ? FLAGS_ssd_root_path : FLAGS_hdd_root_path;
    std::string path = root_path + "/" + std::to_string(id_) + "_" + std::to_string(pid_) + "/data";
    if (!::rtidb::base::MkdirRecur(path)) {
        PDLOG(WARNING, "fail to create path %s", path.c_str());
        return false;
    }
    options_.create_if_missing = true;
    options_.error_if_exists = true;
    options_.create_missing_column_families = true;
    rocksdb::Status s = rocksdb::DB::Open(options_, path, cf_ds_, &cf_hs_, &db_);
    if (!s.ok()) {
        PDLOG(WARNING, "rocksdb open failed. tid %u pid %u error %s", id_, pid_, s.ToString().c_str());
        return false;
    } 
    return true;
}

bool DiskTable::Put(const std::string &pk, uint64_t time, const char *data, uint32_t size) {
    rocksdb::Status s;
    rocksdb::Slice spk = rocksdb::Slice(CombineKeyTs(pk, time));
    s = db_->Put(rocksdb::WriteOptions(), cf_hs_[1], spk, rocksdb::Slice(data, size));
    if (s.ok()) {
        offset_.fetch_add(1, std::memory_order_relaxed);
        return true;
    } else {
        PDLOG(DEBUG, "Put failed. tid %u pid %u msg %s", id_, pid_, s.ToString().c_str());
        return false;
    }
}

bool DiskTable::Put(uint64_t time, const std::string &value, const Dimensions &dimensions) {
    rocksdb::WriteBatch batch;
    rocksdb::Status s;
    Dimensions::const_iterator it = dimensions.begin();
    for (; it != dimensions.end(); ++it) {
        if (it->idx() >= (cf_hs_.size() - 1)) {
            PDLOG(WARNING, "failed putting key %s to dimension %u in table tid %u pid %u",
                            it->key().c_str(), it->idx(), id_, pid_);
            return false;
        }
        rocksdb::Slice spk = rocksdb::Slice(CombineKeyTs(it->key(), time));
        batch.Put(cf_hs_[it->idx() + 1], spk, value);
    }
    s = db_->Write(rocksdb::WriteOptions(), &batch);
    if (s.ok()) {
        offset_.fetch_add(1, std::memory_order_relaxed);
        return true;
    } else {
        PDLOG(DEBUG, "Put failed. tid %u pid %u msg %s", id_, pid_, s.ToString().c_str());
        return false;
    }
}

bool DiskTable::Put(const Dimensions& dimensions, const TSDimensions& ts_dimemsions, const std::string& value) {
    // TODO
    return true;
}

bool DiskTable::Put(const ::rtidb::api::LogEntry& entry) {
    // TODO
    return true;
}

bool DiskTable::Delete(const std::string& pk, uint32_t idx) {
    rocksdb::Status s = db_->DeleteRange(rocksdb::WriteOptions(), cf_hs_[idx + 1], 
                rocksdb::Slice(CombineKeyTs(pk, UINT64_MAX)), rocksdb::Slice(CombineKeyTs(pk, 0)));
    if (s.ok()) {
        offset_.fetch_add(1, std::memory_order_relaxed);
        return true;
    } else {
        PDLOG(DEBUG, "Delete failed. tid %u pid %u msg %s", id_, pid_, s.ToString().c_str());
        return false;
    }
}

bool DiskTable::Get(uint32_t idx, const std::string& pk, uint64_t ts, std::string& value) {
    rocksdb::Status s;
    rocksdb::Slice spk = rocksdb::Slice(CombineKeyTs(pk, ts));
    s = db_->Get(rocksdb::ReadOptions(), cf_hs_[idx + 1], spk, &value);
    if (s.ok()) {
        return true;
    } else {
        return false;
    }
}

bool DiskTable::Get(const std::string& pk, uint64_t ts, std::string& value) {
    return Get(0, pk, ts, value);
}

bool DiskTable::LoadTable() {
    InitColumnFamilyDescriptor();
    options_.create_if_missing = false;
    options_.error_if_exists = false;
    options_.create_missing_column_families = false;
    std::string root_path = storage_mode_ == ::rtidb::common::StorageMode::kSSD ? FLAGS_ssd_root_path : FLAGS_hdd_root_path;
    std::string path = root_path + "/" + std::to_string(id_) + "_" + std::to_string(pid_) + "/data";
    if (!rtidb::base::IsExists(path)) {
        return false;
    }
    rocksdb::Status s = rocksdb::DB::Open(options_, path, cf_ds_, &cf_hs_, &db_);
    PDLOG(DEBUG, "Load DB. tid %u pid %u ColumnFamilyHandle size %d,", id_, pid_, cf_hs_.size());
    if (!s.ok()) {
        PDLOG(WARNING, "Load DB failed. tid %u pid %u msg %s", id_, pid_, s.ToString().c_str());
    }    
    return true;
}

void DiskTable::SchedGc() {
    if (ttl_ == 0) {
        return;
    }
    if (ttl_type_ == ::rtidb::api::TTLType::kLatestTime) {
        GcHead();
    } /*else {
        rocksdb will delete expired key when compact
        GcTTL();
    }*/
}

void DiskTable::GcTTL() {
    uint64_t start_time = ::baidu::common::timer::get_micros() / 1000;
    uint64_t expire_time = GetExpireTime();
    if (expire_time < 1) {
        return;
    }
    for (auto cf_hs : cf_hs_) {
        rocksdb::ReadOptions ro = rocksdb::ReadOptions();
        rocksdb::WriteOptions wo = rocksdb::WriteOptions();
        const rocksdb::Snapshot* snapshot = db_->GetSnapshot();
        ro.snapshot = snapshot;
        //ro.prefix_same_as_start = true;
        ro.pin_data = true;
        rocksdb::Iterator* it = db_->NewIterator(ro, cf_hs);
        it->SeekToFirst();
        std::string last_pk;
        while (it->Valid()) {
            std::string cur_pk;
            uint64_t ts = 0;
            ParseKeyAndTs(it->key(), cur_pk, ts);
            if (cur_pk == last_pk) {
                if (ts == 0 || ts >= expire_time) {
                    it->Next();
                    continue;
                } else {
                    rocksdb::Status s = db_->DeleteRange(wo, cf_hs, 
                            rocksdb::Slice(CombineKeyTs(cur_pk, ts)), rocksdb::Slice(CombineKeyTs(cur_pk, 0)));
                    if (!s.ok()) {
                        PDLOG(WARNING, "Delete failed. tid %u pid %u msg %s", id_, pid_, s.ToString().c_str());
                    }
                    it->Seek(rocksdb::Slice(CombineKeyTs(cur_pk, 0)));
                }
            } else {
                last_pk = cur_pk;
                it->Next();
            }
        }
        delete it;
        db_->ReleaseSnapshot(snapshot);
    }
    uint64_t time_used = ::baidu::common::timer::get_micros() / 1000 - start_time;
    PDLOG(INFO, "Gc used %lu second. tid %u pid %u", time_used / 1000, id_, pid_);
}

void DiskTable::GcHead() {
    uint64_t start_time = ::baidu::common::timer::get_micros() / 1000;
    uint64_t ttl_num = ttl_ / 60 / 1000;
    if (ttl_num < 1) {
        return;
    }
    for (auto cf_hs : cf_hs_) {
        rocksdb::ReadOptions ro = rocksdb::ReadOptions();
        rocksdb::WriteOptions wo = rocksdb::WriteOptions();
        const rocksdb::Snapshot* snapshot = db_->GetSnapshot();
        ro.snapshot = snapshot;
        //ro.prefix_same_as_start = true;
        ro.pin_data = true;
        rocksdb::Iterator* it = db_->NewIterator(ro, cf_hs);
        it->SeekToFirst();
        std::string last_pk;
        uint64_t count = 0;
        while (it->Valid()) {
            std::string cur_pk;
            uint64_t ts = 0;
            ParseKeyAndTs(it->key(), cur_pk, ts);
            if (cur_pk == last_pk) {
                if (ts == 0 || count < ttl_num) {
                    it->Next();
                    count++;
                    continue;
                } else {
                    rocksdb::Status s = db_->DeleteRange(wo, cf_hs, 
                            rocksdb::Slice(CombineKeyTs(cur_pk, ts)), rocksdb::Slice(CombineKeyTs(cur_pk, 0)));
                    if (!s.ok()) {
                        PDLOG(WARNING, "Delete failed. tid %u pid %u msg %s", id_, pid_, s.ToString().c_str());
                    }    
                    it->Seek(rocksdb::Slice(CombineKeyTs(cur_pk, 0)));
                }
            } else {
                count = 1;
                last_pk = cur_pk;
                it->Next();
            }
        }
        delete it;
        db_->ReleaseSnapshot(snapshot);
    }
    uint64_t time_used = ::baidu::common::timer::get_micros() / 1000 - start_time;
    PDLOG(INFO, "Gc used %lu second. tid %u pid %u", time_used / 1000, id_, pid_);
}

uint64_t DiskTable::GetExpireTime(uint64_t ttl) {
    if (ttl == 0 || ttl_type_ == ::rtidb::api::TTLType::kLatestTime) {
        return 0;
    }
    uint64_t cur_time = ::baidu::common::timer::get_micros() / 1000;
    return cur_time - ttl * 60 * 1000;
}

uint64_t DiskTable::GetExpireTime() {
    if (ttl_.load(std::memory_order_relaxed) == 0
            || ttl_type_ == ::rtidb::api::TTLType::kLatestTime) {
        return 0;
    }
    uint64_t cur_time = ::baidu::common::timer::get_micros() / 1000;
    return cur_time - ttl_.load(std::memory_order_relaxed);
}


bool DiskTable::IsExpire(const ::rtidb::api::LogEntry& entry) {
    // TODO
    return false;
}

TableIterator* DiskTable::NewIterator(const std::string &pk, Ticket& ticket) {
    return DiskTable::NewIterator(0, pk, ticket);
}

TableIterator* DiskTable::NewIterator(uint32_t idx, const std::string& pk, Ticket& ticket) {
    rocksdb::ReadOptions ro = rocksdb::ReadOptions();
    const rocksdb::Snapshot* snapshot = db_->GetSnapshot();
    ro.snapshot = snapshot;
    ro.prefix_same_as_start = true;
    ro.pin_data = true;
    rocksdb::Iterator* it = db_->NewIterator(ro, cf_hs_[idx + 1]);
    return new DiskTableIterator(db_, it, snapshot, pk);
}

TableIterator* DiskTable::NewIterator(uint32_t index, uint32_t ts_idx, const std::string& pk, Ticket& ticket) {
    // TODO
    return NULL;
}

TableIterator* DiskTable::NewTraverseIterator(uint32_t idx) {
    rocksdb::ReadOptions ro = rocksdb::ReadOptions();
    const rocksdb::Snapshot* snapshot = db_->GetSnapshot();
    ro.snapshot = snapshot;
    //ro.prefix_same_as_start = true;
    ro.pin_data = true;
    rocksdb::Iterator* it = db_->NewIterator(ro, cf_hs_[idx + 1]);
    uint64_t expire_value = 0;
    if (ttl_ == 0) {
        expire_value = 0;
    } else if (ttl_type_ == ::rtidb::api::TTLType::kLatestTime) {
        expire_value = ttl_ / 60 / 1000;
    } else {
        uint64_t cur_time = ::baidu::common::timer::get_micros() / 1000;
        expire_value = cur_time - ttl_.load(std::memory_order_relaxed);
    }
    return new DiskTableTraverseIterator(db_, it, snapshot, ttl_type_, expire_value);
}

TableIterator* DiskTable::NewTraverseIterator(uint32_t index, uint32_t ts_idx) {
    // TODO
    return NULL;
}

DiskTableIterator::DiskTableIterator(rocksdb::DB* db, rocksdb::Iterator* it, 
            const rocksdb::Snapshot* snapshot, const std::string& pk) : 
            db_(db), it_(it), snapshot_(snapshot), pk_(pk), ts_(0) {
}

DiskTableIterator::~DiskTableIterator() {
    delete it_;
    db_->ReleaseSnapshot(snapshot_);
}

bool DiskTableIterator::Valid() {
    if (it_ == NULL || !it_->Valid()) {
        return false;
    }
    std::string cur_pk;
    ParseKeyAndTs(it_->key(), cur_pk, ts_);
    return cur_pk == pk_;
}

void DiskTableIterator::Next() {
    return it_->Next();
}

rtidb::base::Slice DiskTableIterator::GetValue() const {
    rocksdb::Slice value = it_->value();
    return rtidb::base::Slice(value.data(), value.size());
}

std::string DiskTableIterator::GetPK() const {
    return pk_;
}

uint64_t DiskTableIterator::GetKey() const {
    return ts_;
}

void DiskTableIterator::SeekToFirst() {
    it_->Seek(rocksdb::Slice(CombineKeyTs(pk_, UINT64_MAX)));
}

void DiskTableIterator::Seek(uint64_t ts) {
    it_->Seek(rocksdb::Slice(CombineKeyTs(pk_, ts)));
}

DiskTableTraverseIterator::DiskTableTraverseIterator(rocksdb::DB* db, rocksdb::Iterator* it, 
        const rocksdb::Snapshot* snapshot, ::rtidb::api::TTLType ttl_type, uint64_t expire_value) : 
        db_(db), it_(it), snapshot_(snapshot), ttl_type_(ttl_type), record_idx_(0), expire_value_(expire_value) {
}

DiskTableTraverseIterator::~DiskTableTraverseIterator() {
    delete it_;
    db_->ReleaseSnapshot(snapshot_);
}

bool DiskTableTraverseIterator::Valid() {
    return it_->Valid();
}

void DiskTableTraverseIterator::Next() {
    it_->Next();
    if (it_->Valid()) {
        std::string tmp_pk;
        ParseKeyAndTs(it_->key(), tmp_pk, ts_);
        if (tmp_pk == pk_) {
            record_idx_++;
        } else {
            pk_ = tmp_pk;
            record_idx_ = 1;
        }
        if (IsExpired()) {
            NextPK();
        }
    }
}

rtidb::base::Slice DiskTableTraverseIterator::GetValue() const {
    rocksdb::Slice value = it_->value();
    return rtidb::base::Slice(value.data(), value.size());
}

std::string DiskTableTraverseIterator::GetPK() const {
    return pk_;
}

uint64_t DiskTableTraverseIterator::GetKey() const {
    return ts_;
}

void DiskTableTraverseIterator::SeekToFirst() {
    it_->SeekToFirst();
    record_idx_ = 1;
    if (it_->Valid()) {
        ParseKeyAndTs(it_->key(), pk_, ts_);
        if (IsExpired()) {
            NextPK();
        }
    }
}

void DiskTableTraverseIterator::Seek(const std::string& pk, uint64_t time) {
    if (ttl_type_ == ::rtidb::api::TTLType::kLatestTime) {
        it_->Seek(rocksdb::Slice(CombineKeyTs(pk, UINT64_MAX)));
        record_idx_ = 0;
        while (it_->Valid()) {
            record_idx_++;
            ParseKeyAndTs(it_->key(), pk_, ts_);
            if (pk_ == pk) {
                if (IsExpired()) {
                    NextPK();
                    break;
                } 
                if (ts_ >= time) {
                    it_->Next();
                    continue;
                }
            } else {
                record_idx_ = 1;
                if (IsExpired()) {
                    NextPK();
                }
            }
            break;
        }
    } else {
        it_->Seek(rocksdb::Slice(CombineKeyTs(pk, time)));
        while (it_->Valid()) {
            ParseKeyAndTs(it_->key(), pk_, ts_);
            if (pk_ == pk) {
                if (ts_ >= time) {
                    it_->Next();
                    continue;
                }
                if (IsExpired()) {
                    NextPK();
                }    
                break;
            } else {
                if (IsExpired()) {
                    NextPK();
                }    
            }
            break;
        }
    }
}

bool DiskTableTraverseIterator::IsExpired() {
    if (expire_value_ == 0) {
        return false;
    }
    if (ttl_type_ == ::rtidb::api::TTLType::kLatestTime) {
        return record_idx_ > expire_value_;
    }
    return ts_ < expire_value_;
}

void DiskTableTraverseIterator::NextPK() {
    std::string last_pk = pk_;
    it_->Seek(rocksdb::Slice(CombineKeyTs(last_pk, 0)));
    record_idx_ = 1;
    while (it_->Valid()) {
        std::string tmp_pk;
        ParseKeyAndTs(it_->key(), tmp_pk, ts_);
        if (tmp_pk != last_pk) {
            if (!IsExpired()) {
                pk_ = tmp_pk;
                return;
            } else {
                last_pk = tmp_pk;
                it_->Seek(rocksdb::Slice(CombineKeyTs(last_pk, 0)));
                record_idx_ = 1;
            }
        } else {
            it_->Next();
        }
    }
}

}
}