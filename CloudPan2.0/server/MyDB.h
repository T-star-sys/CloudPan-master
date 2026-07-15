/*************************************************************************
	> File Name: MyDB.h
	> Author: qinyu
	> Mail: qinyu.LT@gmail.com 
	> Created Time: 2016年04月19日 星期二 22时57分10秒
 ************************************************************************/
#ifndef _MYDB_H
#define _MYDB_H

#include "../public.h"
#include <mysql/mysql.h>
#include "sqlConnPool.h"

class MyDB
{
public:
    MyDB();
    ~MyDB();
    // 服务启动时预创建连接；CLOUDPAN_DB_CONNECTIONS 可覆盖默认数量。
    static bool initializePool(string host,string user,string password,string db_name,size_t defaultSize);
    static SqlPoolStats getPoolStats();
    // 建立到指定 MySQL 数据库的连接。
    bool initDB(string host,string user,string password,string db_name);
    // 执行 SQL；SELECT 的结果会被保存到 res，INSERT/DELETE/UPDATE 只执行不产生结果集。
    bool execSQL(string sql);
    // 调试用：打印最近一次 SELECT 保存的结果。
    void showResult();
    // 返回最近一次 SELECT 的结果。结果被扁平化存储，不保留二维行列结构。
    list<string> getResult();
private:
    MyDB(const MyDB &);
    MyDB &operator=(const MyDB &);

    MYSQL *connection;//从连接池借出的连接，析构时自动归还
    list<string> res;//把查询结果保存成字符串列表，方便其他代码拿结果
};

#endif
