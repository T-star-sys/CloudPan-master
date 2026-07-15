/*************************************************************************
	> File Name: conSer.cpp
	> Author: qinyu
	> Mail: qinyu.LT@gmail.com 
	> Created Time: 2016年04月20日 星期三 22时38分48秒
 ************************************************************************/
#include "MCache.h"
#include "conSer.h"

#include <algorithm>
#include <cctype>
#include <iomanip>

namespace
{
const int LOGIN_FAIL_LIMIT = 5;
const int LOGIN_BAN_SECONDS = 60;
const int MAX_GUARD_CLIENTS = 128;
const int DEFAULT_THREAD_COUNT = 4;

int configuredThreadCount()
{
    const char *value = getenv("CLOUDPAN_THREADS");
    if(value == NULL || value[0] == '\0')
        return DEFAULT_THREAD_COUNT;

    char *end = NULL;
    long count = strtol(value,&end,10);
    if(*end != '\0' || count < 1 || count > 64)
        return DEFAULT_THREAD_COUNT;
    return static_cast<int>(count);
}

bool sendAll(int fd,const string &data)
{
    size_t sent = 0;
    while(sent < data.size())
    {
        ssize_t n = send(fd,data.c_str() + sent,data.size() - sent,0);
        if(n < 0)
        {
            if(errno == EINTR)
                continue;
            return false;
        }
        if(n == 0)
            return false;
        sent += n;
    }
    return true;
}

string upperCopy(string value)
{
    for(size_t i = 0;i < value.size();++i)
        value[i] = static_cast<char>(toupper(static_cast<unsigned char>(value[i])));
    return value;
}

const char *adminToken()
{
    const char *token = getenv("CLOUDPAN_ADMIN_TOKEN");
    return (token != NULL && token[0] != '\0') ? token : "cloudpan-admin";
}
}

struct conSer::SharedState
{
    struct GuardEntry
    {
        char ip[INET_ADDRSTRLEN];
        unsigned int failures;
        time_t bannedUntil;
    };

    pthread_mutex_t mutex;
    time_t startedAt;
    unsigned long totalRequests;
    unsigned long failedLogins;
    unsigned int activeWorkers;
    unsigned int configuredThreads;
    GuardEntry guards[MAX_GUARD_CLIENTS];
};

struct conSer::ServiceTask
{
    conSer *server;
    int conn;
    struct sockaddr_in peer;
};

    conSer::conSer(int port,const char *addr)
    {
        this->port = port;
        this->addr = addr;
        // 创建 TCP 监听 socket。listenfd 后续只用于 bind/listen/accept。
        if((listenfd=socket(PF_INET,SOCK_STREAM,0)) < 0)
            ERR_EXIT("socket");
        memset(&seraddr,0,sizeof(seraddr));
        seraddr.sin_family = AF_INET;
        seraddr.sin_port = htons(port);
        // 监听本机所有网卡地址；addr 参数当前没有实际参与绑定。
        seraddr.sin_addr.s_addr = htonl(INADDR_ANY);

        shared = new SharedState;
        memset(shared,0,sizeof(SharedState));
        if(pthread_mutex_init(&shared->mutex,NULL) != 0)
            ERR_EXIT("pthread_mutex_init");
        shared->startedAt = time(NULL);
    }
    conSer::~conSer()
    {
        if(shared != NULL)
        {
            pthread_mutex_destroy(&shared->mutex);
            delete shared;
        }
        close(listenfd);
    }
    
    void conSer::Socket()
    {
	    int on = 1;
        // 允许服务端退出后快速重新绑定同一个端口，调试重启时避免 Address already in use。
        if(setsockopt(listenfd,SOL_SOCKET,SO_REUSEADDR,&on,sizeof(on)) < 0 )
            ERR_EXIT("setsockopt");
    }
    
    void conSer::Bind()
    {
        // ::bind 明确调用全局 socket API，避免和 C++ std::bind 发生名字冲突。
        if(::bind(listenfd,(struct sockaddr*)&seraddr,sizeof(seraddr)) < 0 )
            ERR_EXIT("bind");
    }
    
    void conSer::Listen()
    {
        // listen 后 listenfd 进入监听状态，客户端才能 connect 到该端口。
        if(listen(listenfd,SOMAXCONN) < 0)
            ERR_EXIT("listen");
    }

    // 主线程只负责接收连接，再把通信 fd 投递给固定线程池。
    void conSer::epoll_work()
    {
        struct epoll_event ev,events[EPOLL_SIZE];
        int epfd = epoll_create1(0);
        if(epfd == -1)
            ERR_EXIT("epoll_create1");
        ev.events = EPOLLIN;
        ev.data.fd = listenfd;
        if(epoll_ctl(epfd,EPOLL_CTL_ADD,listenfd,&ev) == -1)
            ERR_EXIT("epoll_ctl");

        while(1)
        {
            int events_count = epoll_wait(epfd,events,EPOLL_SIZE,-1);
            if(events_count < 0)
            {
                if(errno == EINTR)
                    continue;
                ERR_EXIT("epoll_wait");
            }
            for(int i = 0;i < events_count;++i)
            {
                if(events[i].data.fd != listenfd)
                    continue;

                struct sockaddr_in peer;
                socklen_t peerlen = sizeof(peer);
                int conn = accept(listenfd,(struct sockaddr*)&peer,&peerlen);
                if(conn < 0)
                {
                    if(errno == EINTR)
                        continue;
                    perror("accept");
                    continue;
                }

                ServiceTask *task = static_cast<ServiceTask *>(malloc(sizeof(ServiceTask)));
                if(task == NULL)
                {
                    close(conn);
                    continue;
                }
                task->server = this;
                task->conn = conn;
                task->peer = peer;
                if(addWorkToPool(serviceTask,task) != 0)
                {
                    close(conn);
                    free(task);
                }
            }
        }
    }

    void *conSer::serviceTask(void *arg)
    {
        ServiceTask *task = static_cast<ServiceTask *>(arg);
        mysql_thread_init();
        task->server->workerStarted();
        task->server->do_service(task->conn,task->peer);
        task->server->workerFinished();
        mysql_thread_end();
        return NULL;
    }

    void conSer::Run()
    {
        // send 写入已断开的连接时返回错误，不让整个服务进程退出。
        signal(SIGPIPE,SIG_IGN);
        Socket();
        Bind();
        Listen();
        int threadCount = configuredThreadCount();
        if(!MyDB::initializePool(SERADDR,DBUSER,DBPSSW,DBNAME,threadCount))
        {
            cerr<<"Failed to initialize MySQL connection pool"<<endl;
            exit(EXIT_FAILURE);
        }
        SqlPoolStats databasePool = MyDB::getPoolStats();
        cout<<"MySQL connection pool started, connections="<<databasePool.total<<endl;
        shared->configuredThreads = static_cast<unsigned int>(threadCount);
        if(createPool(threadCount) != 0)
            ERR_EXIT("createPool");
        cout<<"Management server thread pool started, workers="<<threadCount<<endl;
        epoll_work();
        destroyPool();
    }

    void conSer::do_service(int conn,const struct sockaddr_in &peer)
    {
        // 管理协议的每个 TCP 连接只处理一条短命令。
        string message;
        char buf[512];
        while(message.size() < 2048)
        {
            ssize_t ret;
            do
            {
                ret = read(conn,buf,sizeof(buf));
            }
            while(ret < 0 && errno == EINTR);

            if(ret < 0)
            {
                perror("read");
                close(conn);
                return;
            }
            if(ret == 0)
                break;

            message.append(buf,ret);
            size_t nulPos = message.find('\0');
            size_t linePos = message.find('\n');
            size_t endPos = string::npos;
            if(nulPos != string::npos)
                endPos = nulPos;
            if(linePos != string::npos && (endPos == string::npos || linePos < endPos))
                endPos = linePos;
            if(endPos != string::npos)
            {
                message.resize(endPos);
                break;
            }
        }

        if(message.empty())
        {
            close(conn);
            return;
        }

        requestReceived();
        get_cmd(message,peer);
        sendAll(conn,message);
        close(conn);
    }

    void conSer::selectFTServer(string &response)
    {
        // 当前项目只有一个文件传输服务器，直接返回固定 IP 和 TPT 端口。
        string FTS_IP = SERADDR;
        int FTS_PORT = TPT;
        stringstream ss;
        string port;
        ss<<FTS_PORT;
        ss>>port;
        response += FTS_IP+" "+port;
    }

    void conSer::selectCServer(string &response)
    {
        // 当前项目只有一个聊天服务器，直接返回固定 IP 和 MPT 端口。
        string CS_IP = SERADDR;
        int CS_PORT = MPT;
        stringstream ss;
        string port;
        ss<<CS_PORT;
        ss>>port;
        response += CS_IP+" "+port;
    }
    
    void conSer::get_cmd(string &message,const struct sockaddr_in &peer)
    {
        list<string> args;
        stringstream cmdStream(message);
        string token;
        int select = ERR;
        if(!(cmdStream >> select))
        {
            message = "ERR invalid command\n";
            return;
        }
        while(cmdStream >> token)
            args.push_back(token);

        if(select == HEARTBEAT)
        {
            heartbeatReport(message);
            return;
        }
        if(select == ADMIN)
        {
            executeAdminCommand(args,message);
            return;
        }

        if(args.size() < 2)
        {
            message = "ERR invalid user info\n";
            return;
        }

        string userid = args.front();
        args.pop_front();
        string passwd = args.front();
        args.pop_front();

        // 文件和聊天操作只需要获得对应服务地址，不访问用户数据库。
        if(select == UPLODE || select == DOWNLOAD || select == RM || select == LS)
        {
            stringstream response;
            response<<select<<" ";
            message = response.str();
            selectFTServer(message);
            return;
        }
        if(select == CHART)
        {
            message = "5 ";
            selectCServer(message);
            return;
        }
        if(select != ENTER && select != REGISTER)
        {
            message = "ERR unsupported command\n";
            return;
        }

        char clientIpBuffer[INET_ADDRSTRLEN] = {0};
        if(inet_ntop(AF_INET,&peer.sin_addr,clientIpBuffer,sizeof(clientIpBuffer)) == NULL)
            strncpy(clientIpBuffer,"unknown",sizeof(clientIpBuffer) - 1);
        string clientIp = clientIpBuffer;
        int remainingSeconds = 0;
        if(isTemporarilyBlocked(clientIp,remainingSeconds))
        {
            stringstream response;
            response<<"ERR temporarily blocked "<<remainingSeconds<<"s\n";
            message = response.str();
            return;
        }

        MyDB db;
        if(!db.initDB(SERADDR,DBUSER,DBPSSW,DBNAME))
        {
            message = "ERR mysql connecting\n";
            return;
        }

        MCache mem(SERADDR,MCA);
        string sqlExec;
        if(select == ENTER)
        {
            // 登录优先查询缓存，未命中时查询 MySQL。
            {
            char *cachedPasswd = mem.getValue(userid.c_str());
            if(cachedPasswd != NULL && strncmp(cachedPasswd,passwd.c_str(),32) == 0)
            {
                clearLoginFailures(clientIp);
                message = "1\n";
            }
            else
            {
                sqlExec="select exists(select *from user where user_id='" +userid + "' and password='"+passwd+"')";
                if(!db.execSQL(sqlExec))
                {
                    free(cachedPasswd);
                    message = "ERR mysql query failed\n";
                    return;
                }
                list<string> result = db.getResult();
                if(!result.empty() && strncmp("1", (*(result.begin())).c_str(), 1) == 0)
                {
                    mem.insertValue(userid.c_str(),passwd.c_str(),180);
                    clearLoginFailures(clientIp);
                    message = "1\n";
                }
                else
                {
                    bool banned = recordLoginFailure(clientIp,remainingSeconds);
                    stringstream response;
                    response<<"ERR userid or password";
                    if(banned)
                        response<<"; temporarily blocked "<<remainingSeconds<<"s";
                    response<<"\n";
                    message = response.str();
                }
            }
            free(cachedPasswd);
            }
            return;
        }

        if(select == REGISTER)
        {
            sqlExec="select exists (select *from user where user_id='"+userid+"')";
            if(!db.execSQL(sqlExec))
            {
                message = "ERR mysql query failed\n";
                return;
            }
            list<string> result = db.getResult();
            if(!result.empty() && result.front() == "1")
            {
                message = "ERR user existed\n";
            }
            else
            {
                sqlExec="insert into user(user_id,password) select '"+userid+"','"+passwd+"' from dual where not exists(select *from user where user.user_id='"+userid+"')";
                if(!db.execSQL(sqlExec))
                {
                    message = "ERR mysql query failed\n";
                    return;
                }
                mem.insertValue(userid.c_str(),passwd.c_str(),180);
                clearLoginFailures(clientIp);
                message = "register success!\n";
            }
            return;
        }
    }

    void conSer::heartbeatReport(string &message)
    {
        time_t now = time(NULL);
        unsigned long totalRequests;
        unsigned long failedLogins;
        unsigned int activeWorkers;
        unsigned int configuredThreads;
        int bannedClients = 0;
        time_t startedAt;

        pthread_mutex_lock(&shared->mutex);
        for(int i = 0;i < MAX_GUARD_CLIENTS;++i)
        {
            if(shared->guards[i].bannedUntil > 0 && shared->guards[i].bannedUntil <= now)
                memset(&shared->guards[i],0,sizeof(shared->guards[i]));
            if(shared->guards[i].bannedUntil > now)
                ++bannedClients;
        }
        totalRequests = shared->totalRequests;
        failedLogins = shared->failedLogins;
        activeWorkers = shared->activeWorkers;
        configuredThreads = shared->configuredThreads;
        startedAt = shared->startedAt;
        pthread_mutex_unlock(&shared->mutex);

        SqlPoolStats databasePool = MyDB::getPoolStats();

        stringstream response;
        response<<"OK heartbeat"
                <<" timestamp="<<now
                <<" uptime="<<(now - startedAt)
                <<" requests="<<totalRequests
                <<" threads="<<configuredThreads
                <<" active="<<activeWorkers
                <<" db_capacity="<<databasePool.capacity
                <<" db_available="<<databasePool.available
                <<" db_in_use="<<databasePool.inUse
                <<" failed_logins="<<failedLogins
                <<" banned="<<bannedClients<<"\n";
        message = response.str();
    }

    void conSer::executeAdminCommand(list<string> &args,string &message)
    {
        if(args.size() < 2)
        {
            message = "ERR admin usage: 9 <token> <command> [args]\n";
            return;
        }

        string suppliedToken = args.front();
        args.pop_front();
        string command = upperCopy(args.front());
        args.pop_front();

        if(suppliedToken != adminToken())
        {
            message = "ERR admin authentication failed\n";
            return;
        }

        if(command == "STATUS")
        {
            heartbeatReport(message);
            message.replace(3,9,"status");
            return;
        }
        if(command == "HELP")
        {
            message = "OK commands: STATUS, BAN <ip> [seconds], UNBAN <ip>, RESET, HELP\n";
            return;
        }
        if(command == "BAN")
        {
            if(args.empty())
            {
                message = "ERR BAN requires an IPv4 address\n";
                return;
            }
            string ip = args.front();
            args.pop_front();
            struct in_addr checkedAddress;
            if(inet_pton(AF_INET,ip.c_str(),&checkedAddress) != 1)
            {
                message = "ERR invalid IPv4 address\n";
                return;
            }
            int seconds = LOGIN_BAN_SECONDS;
            if(!args.empty())
                seconds = atoi(args.front().c_str());
            if(seconds <= 0)
                seconds = LOGIN_BAN_SECONDS;
            if(seconds > 86400)
                seconds = 86400;
            setManualBan(ip,seconds);
            stringstream response;
            response<<"OK banned "<<ip<<" for "<<seconds<<"s\n";
            message = response.str();
            return;
        }
        if(command == "UNBAN")
        {
            if(args.empty())
            {
                message = "ERR UNBAN requires an IPv4 address\n";
                return;
            }
            string ip = args.front();
            message = removeBan(ip) ? "OK unbanned " + ip + "\n" : "ERR address not found\n";
            return;
        }
        if(command == "RESET")
        {
            resetGuardState();
            message = "OK login guard reset\n";
            return;
        }

        message = "ERR unknown admin command\n";
    }

    bool conSer::isTemporarilyBlocked(const string &ip,int &remainingSeconds)
    {
        bool blocked = false;
        time_t now = time(NULL);
        remainingSeconds = 0;

        pthread_mutex_lock(&shared->mutex);
        for(int i = 0;i < MAX_GUARD_CLIENTS;++i)
        {
            SharedState::GuardEntry &entry = shared->guards[i];
            if(entry.ip[0] == '\0' || ip != entry.ip)
                continue;
            if(entry.bannedUntil > now)
            {
                blocked = true;
                remainingSeconds = static_cast<int>(entry.bannedUntil - now);
            }
            else if(entry.bannedUntil > 0)
            {
                memset(&entry,0,sizeof(entry));
            }
            break;
        }
        pthread_mutex_unlock(&shared->mutex);
        return blocked;
    }

    bool conSer::recordLoginFailure(const string &ip,int &remainingSeconds)
    {
        time_t now = time(NULL);
        int freeSlot = -1;
        int selected = -1;
        bool banned = false;
        remainingSeconds = 0;

        pthread_mutex_lock(&shared->mutex);
        ++shared->failedLogins;
        for(int i = 0;i < MAX_GUARD_CLIENTS;++i)
        {
            SharedState::GuardEntry &entry = shared->guards[i];
            if(entry.bannedUntil > 0 && entry.bannedUntil <= now)
                memset(&entry,0,sizeof(entry));
            if(entry.ip[0] == '\0' && freeSlot < 0)
                freeSlot = i;
            if(entry.ip[0] != '\0' && ip == entry.ip)
            {
                selected = i;
                break;
            }
        }
        if(selected < 0)
            selected = freeSlot;
        if(selected >= 0)
        {
            SharedState::GuardEntry &entry = shared->guards[selected];
            if(entry.ip[0] == '\0')
                strncpy(entry.ip,ip.c_str(),sizeof(entry.ip) - 1);
            ++entry.failures;
            if(entry.failures >= LOGIN_FAIL_LIMIT)
            {
                entry.bannedUntil = now + LOGIN_BAN_SECONDS;
                remainingSeconds = LOGIN_BAN_SECONDS;
                banned = true;
            }
        }
        pthread_mutex_unlock(&shared->mutex);
        return banned;
    }

    void conSer::clearLoginFailures(const string &ip)
    {
        pthread_mutex_lock(&shared->mutex);
        for(int i = 0;i < MAX_GUARD_CLIENTS;++i)
        {
            if(shared->guards[i].ip[0] != '\0' && ip == shared->guards[i].ip)
            {
                memset(&shared->guards[i],0,sizeof(shared->guards[i]));
                break;
            }
        }
        pthread_mutex_unlock(&shared->mutex);
    }

    void conSer::setManualBan(const string &ip,int seconds)
    {
        int selected = -1;
        int freeSlot = -1;
        time_t now = time(NULL);

        pthread_mutex_lock(&shared->mutex);
        for(int i = 0;i < MAX_GUARD_CLIENTS;++i)
        {
            SharedState::GuardEntry &entry = shared->guards[i];
            if(entry.bannedUntil > 0 && entry.bannedUntil <= now)
                memset(&entry,0,sizeof(entry));
            if(entry.ip[0] == '\0' && freeSlot < 0)
                freeSlot = i;
            if(entry.ip[0] != '\0' && ip == entry.ip)
            {
                selected = i;
                break;
            }
        }
        if(selected < 0)
            selected = freeSlot >= 0 ? freeSlot : 0;
        memset(&shared->guards[selected],0,sizeof(shared->guards[selected]));
        strncpy(shared->guards[selected].ip,ip.c_str(),sizeof(shared->guards[selected].ip) - 1);
        shared->guards[selected].failures = LOGIN_FAIL_LIMIT;
        shared->guards[selected].bannedUntil = now + seconds;
        pthread_mutex_unlock(&shared->mutex);
    }

    bool conSer::removeBan(const string &ip)
    {
        bool removed = false;
        pthread_mutex_lock(&shared->mutex);
        for(int i = 0;i < MAX_GUARD_CLIENTS;++i)
        {
            if(shared->guards[i].ip[0] != '\0' && ip == shared->guards[i].ip)
            {
                memset(&shared->guards[i],0,sizeof(shared->guards[i]));
                removed = true;
                break;
            }
        }
        pthread_mutex_unlock(&shared->mutex);
        return removed;
    }

    void conSer::resetGuardState()
    {
        pthread_mutex_lock(&shared->mutex);
        memset(shared->guards,0,sizeof(shared->guards));
        shared->failedLogins = 0;
        pthread_mutex_unlock(&shared->mutex);
    }

    void conSer::workerStarted()
    {
        pthread_mutex_lock(&shared->mutex);
        ++shared->activeWorkers;
        pthread_mutex_unlock(&shared->mutex);
    }

    void conSer::workerFinished()
    {
        pthread_mutex_lock(&shared->mutex);
        if(shared->activeWorkers > 0)
            --shared->activeWorkers;
        pthread_mutex_unlock(&shared->mutex);
    }

    void conSer::requestReceived()
    {
        pthread_mutex_lock(&shared->mutex);
        ++shared->totalRequests;
        pthread_mutex_unlock(&shared->mutex);
    }
    

int main()
{
    conSer ser(CPT,SERADDR);
    ser.Run();

    return 0;
}
