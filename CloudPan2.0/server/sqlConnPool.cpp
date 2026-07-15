#include "sqlConnPool.h"

#include <errno.h>
#include <iostream>
#include <time.h>

namespace
{
pthread_once_t mysqlLibraryOnce = PTHREAD_ONCE_INIT;

void initializeMysqlLibrary()
{
    mysql_library_init(0,NULL,NULL);
}

void addMilliseconds(struct timespec &time,unsigned int milliseconds)
{
    time.tv_sec += milliseconds / 1000;
    time.tv_nsec += static_cast<long>(milliseconds % 1000) * 1000000L;
    if(time.tv_nsec >= 1000000000L)
    {
        ++time.tv_sec;
        time.tv_nsec -= 1000000000L;
    }
}
}

SqlConnectionPool &SqlConnectionPool::instance()
{
    static SqlConnectionPool pool;
    return pool;
}

SqlConnectionPool::SqlConnectionPool()
    : maxConnections_(0),totalConnections_(0),inUseConnections_(0),
      initialized_(false),shuttingDown_(false)
{
    pthread_mutex_init(&mutex_,NULL);
    pthread_cond_init(&availableCondition_,NULL);
}

SqlConnectionPool::~SqlConnectionPool()
{
    shutdown();
    pthread_cond_destroy(&availableCondition_);
    pthread_mutex_destroy(&mutex_);
}

bool SqlConnectionPool::initialize(const std::string &host,
                                   const std::string &user,
                                   const std::string &password,
                                   const std::string &database,
                                   size_t maxConnections)
{
    if(maxConnections == 0)
        return false;

    pthread_once(&mysqlLibraryOnce,initializeMysqlLibrary);
    pthread_mutex_lock(&mutex_);
    if(initialized_)
    {
        pthread_mutex_unlock(&mutex_);
        return true;
    }

    host_ = host;
    user_ = user;
    password_ = password;
    database_ = database;
    maxConnections_ = maxConnections;
    shuttingDown_ = false;

    for(size_t i = 0;i < maxConnections_;++i)
    {
        MYSQL *connection = createConnection();
        if(connection != NULL)
        {
            availableConnections_.push_back(connection);
            ++totalConnections_;
        }
    }
    bool success = totalConnections_ > 0;
    initialized_ = success;
    pthread_mutex_unlock(&mutex_);

    if(!success)
        std::cerr<<"MySQL connection pool initialization failed"<<std::endl;
    return success;
}

MYSQL *SqlConnectionPool::createConnection()
{
    MYSQL *connection = mysql_init(NULL);
    if(connection == NULL)
        return NULL;

    if(mysql_real_connect(connection,host_.c_str(),user_.c_str(),password_.c_str(),
                          database_.c_str(),0,NULL,0) == NULL)
    {
        std::cerr<<"MySQL connection error: "<<mysql_error(connection)<<std::endl;
        mysql_close(connection);
        return NULL;
    }
    mysql_set_character_set(connection,"utf8mb4");
    return connection;
}

MYSQL *SqlConnectionPool::acquire(unsigned int timeoutMilliseconds)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME,&deadline);
    addMilliseconds(deadline,timeoutMilliseconds);

    pthread_mutex_lock(&mutex_);
    while(initialized_ && !shuttingDown_ && availableConnections_.empty() &&
          totalConnections_ >= maxConnections_)
    {
        int waitResult = pthread_cond_timedwait(&availableCondition_,&mutex_,&deadline);
        if(waitResult == ETIMEDOUT)
        {
            pthread_mutex_unlock(&mutex_);
            return NULL;
        }
    }

    if(!initialized_ || shuttingDown_)
    {
        pthread_mutex_unlock(&mutex_);
        return NULL;
    }

    MYSQL *connection = NULL;
    if(!availableConnections_.empty())
    {
        connection = availableConnections_.front();
        availableConnections_.pop_front();
        ++inUseConnections_;
        pthread_mutex_unlock(&mutex_);
    }
    else
    {
        // 初始化时若只建立了部分连接，可按需补足到上限。
        ++totalConnections_;
        ++inUseConnections_;
        pthread_mutex_unlock(&mutex_);
        connection = createConnection();
        if(connection == NULL)
        {
            pthread_mutex_lock(&mutex_);
            --totalConnections_;
            --inUseConnections_;
            pthread_cond_signal(&availableCondition_);
            pthread_mutex_unlock(&mutex_);
            return NULL;
        }
    }

    if(mysql_ping(connection) == 0)
        return connection;

    mysql_close(connection);
    connection = createConnection();
    if(connection != NULL)
        return connection;

    pthread_mutex_lock(&mutex_);
    --totalConnections_;
    --inUseConnections_;
    pthread_cond_signal(&availableCondition_);
    pthread_mutex_unlock(&mutex_);
    return NULL;
}

void SqlConnectionPool::release(MYSQL *connection)
{
    if(connection == NULL)
        return;

    bool valid = mysql_ping(connection) == 0;
    pthread_mutex_lock(&mutex_);
    if(inUseConnections_ > 0)
        --inUseConnections_;

    if(shuttingDown_ || !initialized_ || !valid)
    {
        if(totalConnections_ > 0)
            --totalConnections_;
        pthread_cond_signal(&availableCondition_);
        pthread_mutex_unlock(&mutex_);
        mysql_close(connection);
        return;
    }

    availableConnections_.push_back(connection);
    pthread_cond_signal(&availableCondition_);
    pthread_mutex_unlock(&mutex_);
}

SqlPoolStats SqlConnectionPool::stats()
{
    SqlPoolStats result;
    pthread_mutex_lock(&mutex_);
    result.capacity = maxConnections_;
    result.total = totalConnections_;
    result.available = availableConnections_.size();
    result.inUse = inUseConnections_;
    pthread_mutex_unlock(&mutex_);
    return result;
}

bool SqlConnectionPool::isInitialized()
{
    pthread_mutex_lock(&mutex_);
    bool result = initialized_ && !shuttingDown_;
    pthread_mutex_unlock(&mutex_);
    return result;
}

void SqlConnectionPool::shutdown()
{
    std::deque<MYSQL *> connections;
    pthread_mutex_lock(&mutex_);
    if(!initialized_ && availableConnections_.empty())
    {
        pthread_mutex_unlock(&mutex_);
        return;
    }

    shuttingDown_ = true;
    initialized_ = false;
    connections.swap(availableConnections_);
    if(totalConnections_ >= connections.size())
        totalConnections_ -= connections.size();
    pthread_cond_broadcast(&availableCondition_);
    pthread_mutex_unlock(&mutex_);

    while(!connections.empty())
    {
        mysql_close(connections.front());
        connections.pop_front();
    }
}
