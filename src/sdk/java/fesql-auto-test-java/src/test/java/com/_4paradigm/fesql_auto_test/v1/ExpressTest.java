package com._4paradigm.fesql_auto_test.v1;

import com._4paradigm.fesql.sqlcase.model.SQLCase;
import com._4paradigm.fesql.sqlcase.model.SQLCaseType;
import com._4paradigm.fesql_auto_test.common.FesqlTest;
import com._4paradigm.fesql_auto_test.entity.FesqlDataProviderList;
import com._4paradigm.fesql_auto_test.executor.ExecutorFactory;
import io.qameta.allure.Feature;
import io.qameta.allure.Story;
import lombok.extern.slf4j.Slf4j;
import org.testng.annotations.DataProvider;
import org.testng.annotations.Test;

import java.io.FileNotFoundException;

/**
 * @author zhaowei
 * @date 2020/6/11 2:53 PM
 */
@Slf4j
@Feature("Express")
public class ExpressTest extends FesqlTest {

    @DataProvider
    public Object[] testExpressCase() throws FileNotFoundException {
        FesqlDataProviderList dp = FesqlDataProviderList.dataProviderGenerator(
                new String[]{
                        "/integration/v1/expression/"
                });
        return dp.getCases().toArray();
    }
    @Story("batch")
    @Test(dataProvider = "testExpressCase")
    public void testExpress(SQLCase testCase) throws Exception {
        ExecutorFactory.build(executor, testCase, SQLCaseType.kBatch).run();
    }
    @Story("request")
    @Test(dataProvider = "testExpressCase")
    public void testExpressRequestMode(SQLCase testCase) throws Exception {
        ExecutorFactory.build(executor, testCase, SQLCaseType.kRequest).run();
    }
    @Story("requestWithSp")
    @Test(dataProvider = "testExpressCase")
    public void testExpressRequestModeWithSp(SQLCase testCase) throws Exception {
        ExecutorFactory.build(executor, testCase, SQLCaseType.kRequestWithSp).run();
    }
    @Story("requestWithSpAysn")
    @Test(dataProvider = "testExpressCase")
    public void testExpressRequestModeWithSpAysn(SQLCase testCase) throws Exception {
        ExecutorFactory.build(executor, testCase, SQLCaseType.kRequestWithSpAsync).run();
    }
}
