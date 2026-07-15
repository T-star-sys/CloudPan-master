/*************************************************************************
	> File Name: conSer.cpp
	> Author: qinyu
	> Mail: qinyu.LT@gmail.com 
	> Created Time: 2016年04月20日 星期三 22时38分48秒
 ************************************************************************/
#include "MCache.h"
#include "conSer.h"
    conSer::conSer(int port,const char *addr)
    {
        // 创建 TCP 监听 socket。listenfd 后续只用于 bind/listen/accept。
        if((listenfd=socket(PF_INET,SOCK_STREAM,0)) < 0)
            ERR_EXIT("socket");
        memset(&seraddr,0,sizeof(seraddr));
        seraddr.sin_family = AF_INET;
        seraddr.sin_port = htons(port);
        // 监听本机所有网卡地址；addr 参数当前没有实际参与绑定。
        seraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    conSer::~conSer(){}//delete db;}
    
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

    //采用select方法:
    /*int conSer::accept_timeout(unsigned int wait_seconds)
    {
        int ret;
	    socklen_t peerlen = sizeof(peeraddr);
        
        if(wait_seconds > 0)
        {
            fd_set accept_fdset;
            struct timeval timeout;
            FD_ZERO(&accept_fdset);
            FD_SET(listenfd,&accept_fdset);
            timeout.tv_sec = wait_seconds;
            timeout.tv_usec = 0;

            do{
                ret = select(listenfd+1,&accept_fdset,NULL,NULL,&timeout);
            }while(ret<0 && errno == EINTR);
            
            if(ret == -1)
            {
                return -1;
            }
            else if(ret == 0)
            {
                errno = ETIMEDOUT;
                return -1;
            }
        }

        if(&peeraddr != NULL)
            ret = accept(listenfd,(struct sockaddr*)&peeraddr,&peerlen);
        else
            ret = accept(listenfd,NULL,NULL);

        return ret;
    }
    */

    //int conSer::accept_timeout(unsigned int wait_seconds)
    //采用epoll方法:
    void conSer::epoll_work()
    {
        struct epoll_event ev,events[EPOLL_SIZE];
        // epoll 负责等待监听 fd 上的新连接事件。
        int epfd = epoll_create(EPOLL_SIZE);
        if(epfd == -1) {
            perror("epoll_create");
            exit(0);
        }
        ev.events = EPOLLIN;
        ev.data.fd = listenfd;
        // 把监听 fd 加入 epoll，listenfd 可读代表有新客户端连接可 accept。
        int epctl = epoll_ctl(epfd,EPOLL_CTL_ADD,listenfd,&ev);
        if(epctl == -1) {
            perror("epoll_ctl");
            exit(0);
        }

        while(1)
        {
            int events_count = epoll_wait(epfd,events,EPOLL_SIZE,-1);
            int i=0;

            for(;i<events_count;++i)
            {
                if(events[i].data.fd == listenfd)
                {
                    int conn;
	                socklen_t peerlen = sizeof(peeraddr);
                    pid_t pid;
                    
                    system("clear");
                    // accept 返回通信 fd conn；后续读写业务数据都用 conn，不用 listenfd。
                    conn = accept(listenfd,(struct sockaddr*)&peeraddr,&peerlen);
                    if(conn < 0)
                    {
                        perror("accept");
                        continue;
                    }
                    {
                        cout<<endl<<"=============== Service ================"<<endl;
                        cout<<" EPOLL:Received New Connection Request "<<endl;
                        cout<<"  confd="<<conn;
                        cout<<"  ip="<<inet_ntoa(peeraddr.sin_addr);
                        cout<<"  port="<<ntohs(peeraddr.sin_port)<<endl;
                        cout<<"========================================"<<endl;
                        
                        pid = fork();
                        if(pid == -1)
                            ERR_EXIT("fork");
           
                        if(pid == 0)
                        {
                            // 子进程只服务当前客户端，不再接受新连接，所以关闭监听 fd。
                            close(listenfd);
                            cout<<"Start do_service"<<endl;
                
                            do_service(conn);

                            cout<<"End of do_service"<<endl;
                            cout<<"========================================"<<endl;
                            exit(EXIT_SUCCESS);
                        }else
                            // 父进程继续监听新连接，不处理该客户端，所以关闭通信 fd。
                            close(conn);    
                    }
                }
            }
        }
    }

    void conSer::Run()
    {
        Socket();
        Bind();
	    Listen();
        epoll_work();
	    /*
        //socklen_t peerlen = sizeof(peeraddr);
        int conn;
        pid_t pid;

        while(1)
        {
            //if((conn = accept(listenfd,(struct sockaddr*)&peeraddr,&peerlen)) < 0)
            //    ERR_EXIT("accept");
	        //cout<<"ip="<<inet_ntoa(peeraddr.sin_addr)<<" port="<<ntohs(peeraddr.sin_port)<<endl;
            
            conn = accept_timeout(1);
            if(conn == -1)
                ERR_EXIT("accept_timeout");
	        cout<<"ip="<<inet_ntoa(peeraddr.sin_addr)<<" port="<<ntohs(peeraddr.sin_port)<<endl;

            pid = fork();
            if(pid == -1)
                ERR_EXIT("fork");
           
            if(pid == 0)
            {
                close(listenfd);
                cout<<"enter do_service"<<endl;
                
                do_service(conn);

                cout<<"end of do_service"<<endl;
                exit(EXIT_SUCCESS);
            }else
                close(conn);
        }*/
    }

    void conSer::do_service(int conn)
    {
        while(1)
        {
            // conn 是 accept 返回的通信 fd，从客户端读取一条管理命令。
            char buf[1024] = {0};
            int ret = read(conn, buf, sizeof(buf) - 1);
            if(ret > 0)
            {
                buf[ret] = '\0';
                msg = buf;
            }
            else if(ret == 0)
            {
                cout<<"client closed connection"<<endl;
                close(conn);
                break;
            }
            else
            {
                perror("read");
                close(conn);
            }
            //cout<<"ret:"<<ret<<" msg:"<<msg.c_str()<<endl;
            // 根据 msg 中的命令生成响应内容，仍然放回 msg。
            get_cmd();
            //msg = "get it!";
            //cout<<"the msg= "<<msg<<endl;
            // 把 get_cmd 生成的响应写回客户端。
            write(conn,(void*)msg.c_str(),msg.size());
            break;
            //while(1){}
        }
    }

    /*
    void sendToDB()
    {
        MyDB db;
        list<string>::iterator it=sql.pop_back();
        char *user = *it;
        db.initDB("localhost","root","qinyu","FTP");
        db.execSQL("select password from user where user_id = 'qinyu'");
    }*/

    void conSer::selectFTServer()
    {
        // 当前项目只有一个文件传输服务器，直接返回固定 IP 和 TPT 端口。
        string FTS_IP = SERADDR;
        int FTS_PORT = TPT;
        stringstream ss;
        string port;
        ss<<FTS_PORT;
        ss>>port;
        msg += FTS_IP+" "+port;
    }

    void conSer::selectCServer()
    {
        // 当前项目只有一个聊天服务器，直接返回固定 IP 和 MPT 端口。
        string CS_IP = SERADDR;
        int CS_PORT = MPT;
        stringstream ss;
        string port;
        ss<<CS_PORT;
        ss>>port;
        msg += CS_IP+" "+port;
    }
    
    void conSer::get_cmd()
    {
        // 客户端消息格式类似："1 user password_md5"。
        // 第一个字段是操作码，后续字段临时保存到 sql(list<string>)。
        sql.clear();
        stringstream cmdStream(msg);
        string token;
        string sqlExec;
        int select;
        //cout<<cmd<<endl;
        //sql.push_back(cmd);
        if(!(cmdStream >> select))
        {
            msg = "invalid cmd";
            return;
        }
        while(cmdStream >> token)
        {
            //cout<<ptr<<endl;
            sql.push_back(token);
        }

        MyDB *db = new MyDB();
        // 管理服务器的登录/注册需要访问 MySQL 中的 user 表。
        if(!db->initDB(SERADDR,DBUSER,DBPSSW,DBNAME))
        {
            msg = "ser error(mysql connecting)";
            select = -1;
        }

        if(sql.size() < 2)
        {
            msg = "invalid user info";
            delete db;
            return;
        }

	    string userid=*(sql.begin());
	    sql.pop_front();
	    string passwd=*(sql.begin());
	    sql.pop_front();

        MCache mem(SERADDR,MCA);
        //mem.insertValue(userid.c_str(),passwd.c_str(),180);

        //cout<<"user:"<<userid<<" "<<"password:"<<passwd<<endl;
        switch(select)
        {
        //登录：加入memcached后，不需要从数据库中查询
        case ENTER:
            /*//cout<<select<<" enter"<<endl;
	        //select exists(select *from user where user_id='用户名' and password='密码');
	        sqlExec="select exists(select *from user where user_id='" +userid + "' and password='"+passwd+"')";
            db->execSQL(sqlExec);
            msg=*(db->getResult()).begin(); 
            */
            //MCache mem(SERADDR,MCA);
            {
            // 登录优先查 memcached。缓存命中时无需访问 MySQL。
            char *cachedPasswd = mem.getValue(userid.c_str());
            if(cachedPasswd != NULL && strncmp(cachedPasswd,passwd.c_str(),32) == 0)
            {
                msg = "1";
            }
            else
            {
                // 缓存未命中时回退查 MySQL；查到后再写回 memcached。
                sqlExec="select exists(select *from user where user_id='" +userid + "' and password='"+passwd+"')";
                db->execSQL(sqlExec);
                list<string> result = db->getResult();
                if(!result.empty() && strncmp("1", (*(result.begin())).c_str(), 1) == 0)
                {
                    mem.insertValue(userid.c_str(),passwd.c_str(),180);
                    msg = "1";
                }
                else
                {
                    msg = "userid or passwd error!";
                }
            }
            free(cachedPasswd);
            }
            break;

        //注册：插入mysql数据库并且插入到memcached缓存中
        case REGISTER:
	        //insert into user(user_id,password) select '用户名','密码' from dual where not exists(select *from user where user.user_id='用户名');
            // 注册前先判断用户名是否已存在；exists 返回 1 表示存在，0 表示不存在。
            sqlExec="select exists (select *from user where user_id='"+userid+"')";
            db->execSQL(sqlExec);//检查 user 表里是否存在相关 user_id 的记录
            {
            list<string> result = db->getResult();
            msg = result.empty() ? "0" : *(result.begin());
            }
            if(strncmp("1",msg.c_str(),1) == 0)//表格里为二进制数
            {
                msg = "The user existed!";
            }else{
                // 不存在时插入新用户，并把密码 MD5 缓存到 memcached。
                sqlExec="insert into user(user_id,password) select '"+userid+"','"+passwd+"' from dual where not exists(select *from user where user.user_id='"+userid+"')";
                db->execSQL(sqlExec);
                msg = "register success!";
                mem.insertValue(userid.c_str(),passwd.c_str(),180);
            }
            break;

        case UPLODE:
            // 管理服务器不处理文件内容，只返回文件传输服务器地址。
            msg = "3 ";
            selectFTServer();
            break;

        case DOWNLOAD:
            // 下载同样转交给 FTS。
            msg = "4 ";
            selectFTServer();
            break;

        case RM:
            // 删除文件由 FTS 处理，管理服务器只返回 FTS 地址。
            msg = "6 ";
            selectFTServer();
            break;

        case LS:
            // 查看文件列表也由 FTS 查询 files 表后返回。
            msg = "7 ";
            selectFTServer();
            break;

        case CHART:
            // 聊天请求转交给聊天服务器。
            msg = "5 ";
            selectCServer();
            break;

            break;
        }
        delete db;
    }
    

int main()
{
    conSer ser(CPT,SERADDR);
    ser.Run();

    return 0;
}
