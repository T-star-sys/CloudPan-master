#ifndef SQL_CONN_POOL_H
#define SQL_CONN_POOL_H

#include <mysql/mysql.h>
#include <pthread.h>

#include <cstddef>
#include <deque>
#include <string>

struct SqlPoolStats
{
    size_t capacity;
    size_t total;
    size_t available;
    size_t inUse;
};

// 线程安全的 MySQL C API 连接池。每个连接同一时刻只会借给一个工作线程。
class SqlConnectionPool
{
public:
    static SqlConnectionPool &instance();

    bool initialize(const std::string &host,
                    const std::string &user,
                    const std::string &password,
                    const std::string &database,
                    size_t maxConnections);
    MYSQL *acquire(unsigned int timeoutMilliseconds = 5000);
    void release(MYSQL *connection);
    SqlPoolStats stats();
    bool isInitialized();
    void shutdown();

private:
    SqlConnectionPool();
    ~SqlConnectionPool();
    SqlConnectionPool(const SqlConnectionPool &);
    SqlConnectionPool &operator=(const SqlConnectionPool &);

    MYSQL *createConnection();

    pthread_mutex_t mutex_;
    pthread_cond_t availableCondition_;
    std::deque<MYSQL *> availableConnections_;
    std::string host_;
    std::string user_;
    std::string password_;
    std::string database_;
    size_t maxConnections_;
    size_t totalConnections_;
    size_t inUseConnections_;
    bool initialized_;
    bool shuttingDown_;
};

#endif
