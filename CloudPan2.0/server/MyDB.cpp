/*************************************************************************
	> File Name: MyDB.cpp
	> Author: qinyu
	> Mail: qinyu.LT@gmail.com 
	> Created Time: 2016年04月19日 星期二 22时57分44秒
 ************************************************************************/

#include "MyDB.h"
#include <iostream>
using namespace std;

MyDB::MyDB()
    : connection(NULL)
{
}
MyDB::~MyDB()
{
    if(connection != NULL)
    {
        SqlConnectionPool::instance().release(connection);
        connection = NULL;
    }
}

bool MyDB::initializePool(string host,string user,string password,string db_name,size_t defaultSize)
{
    size_t poolSize = defaultSize > 0 ? defaultSize : 4;
    const char *configuredSize = getenv("CLOUDPAN_DB_CONNECTIONS");
    if(configuredSize != NULL && configuredSize[0] != '\0')
    {
        char *end = NULL;
        long value = strtol(configuredSize,&end,10);
        if(*end == '\0' && value >= 1 && value <= 128)
            poolSize = static_cast<size_t>(value);
    }

    return SqlConnectionPool::instance().initialize(host,user,password,db_name,poolSize);
}

SqlPoolStats MyDB::getPoolStats()
{
    return SqlConnectionPool::instance().stats();
}

bool MyDB::initDB(string host,string user,string password,string db_name)
{
    if(connection != NULL)
        return true;

    SqlConnectionPool &pool = SqlConnectionPool::instance();
    if(!pool.isInitialized() && !initializePool(host,user,password,db_name,4))
        return false;
    connection = pool.acquire();
    if(connection == NULL)
    {
        cout<<"Mysql connection pool exhausted or unavailable"<<endl;
        return false;
    }
    return true;
}

bool MyDB::execSQL(string sql)
{
    if(connection == NULL)
        return false;

    unsigned int j;
    // 每次执行新 SQL 前清空旧结果，避免 getResult 取到上一次查询的数据。
    res.clear();
    // mysql_query 负责把 SQL 字符串发送给 MySQL 服务器执行。
    if(mysql_query(connection,sql.c_str()))
    {
        cout<<"Query Error"<<mysql_error(connection)<<endl;
        // 在线程池中不能因单个请求的 SQL 错误终止整个服务器进程。
        return false;
    }else
    {
        if(mysql_field_count(connection) == 0)//若为0（0列）则表示没有结果集，直接返回
        {
            return true;
        }

        MYSQL_RES *result = mysql_store_result(connection);//获取结果集
        if(result == NULL)
        {
            cout<<"Store Result Error"<<mysql_error(connection)<<endl;
            return false;
        }

        MYSQL_ROW row;
        while((row = mysql_fetch_row(result)))//依次取出每一行记录，直到取完
        {
            for(j=0;j<mysql_num_fields(result);++j)
            {
                // MySQL 中 NULL 字段会以空指针返回，这里转成空字符串保存。
                res.push_back(row[j] ? row[j] : "");
            }
        }
        //释放结果集的内存
        mysql_free_result(result);
    }
    return true;
}

list<string> MyDB::getResult()
{
    return res;
}

void MyDB::showResult()
{
    cout<<"showResult():";
    cout<<res.size()<<endl;
    list<string>::iterator it;
    for(it=res.begin();it != res.end();++it)
    {
        cout<<*it<<endl;
    }
}
