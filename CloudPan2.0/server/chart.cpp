/*************************************************************************
    > File Name: chart.cpp
    > Author: qinyu
 ************************************************************************/

#include "chart.h"
#include <map>

static bool sendAll(int fd,const string &data)
{
    // send 不保证一次写完全部数据，循环发送直到完整写出。
    size_t sent = 0;
    while(sent < data.size())
    {
        ssize_t n = send(fd,data.c_str() + sent,data.size() - sent,0);
        if(n <= 0)
            return false;
        sent += n;
    }
    return true;
}

static bool recvLine(int fd,string &line)
{
    // 聊天协议按行传输：一条消息以 '\n' 结束。
    line.clear();
    char ch;
    while(1)
    {
        ssize_t n = recv(fd,&ch,1,0);
        if(n <= 0)
            return false;
        if(ch == '\n')
            return true;
        line += ch;
    }
}

static void broadcast(const map<int,string> &users,const string &msg,int exceptFd = -1)
{
    // 遍历当前在线用户，把消息发给每个连接；exceptFd 可用于排除发送者。
    map<int,string>::const_iterator it;
    for(it = users.begin(); it != users.end(); ++it)
    {
        if(it->first == exceptFd)
            continue;
        sendAll(it->first,msg);
    }
}

int main()
{
    // 聊天服务器监听 MPT 端口，客户端登录后会从管理服务器拿到该端口。
    int listenfd = socket(PF_INET,SOCK_STREAM,0);
    if(listenfd < 0)
        ERR_EXIT("socket");

    int on = 1;
    if(setsockopt(listenfd,SOL_SOCKET,SO_REUSEADDR,&on,sizeof(on)) < 0)
        ERR_EXIT("setsockopt");

    struct sockaddr_in addr;
    memset(&addr,0,sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MPT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if(::bind(listenfd,(struct sockaddr*)&addr,sizeof(addr)) < 0)
        ERR_EXIT("bind");
    if(listen(listenfd,SOMAXCONN) < 0)
        ERR_EXIT("listen");

    cout<<"Chart server listen on "<<MPT<<endl;

    map<int,string> users;
    // users 保存 在线连接fd -> 用户名，用于广播和下线提示。
    fd_set readfds;
    int maxfd = listenfd;

    while(1)
    {
        FD_ZERO(&readfds);
        // 每轮 select 前都要重新设置 fd 集合，因为 select 会修改集合内容。
        FD_SET(listenfd,&readfds);
        maxfd = listenfd;

        map<int,string>::iterator it;
        for(it = users.begin(); it != users.end(); ++it)
        {
            FD_SET(it->first,&readfds);
            if(it->first > maxfd)
                maxfd = it->first;
        }

        int ret = select(maxfd + 1,&readfds,NULL,NULL,NULL);
        if(ret < 0)
        {
            if(errno == EINTR)
                continue;
            ERR_EXIT("select");
        }

        if(FD_ISSET(listenfd,&readfds))
        {
            // 监听 fd 可读代表有新用户连接聊天室。
            struct sockaddr_in peeraddr;
            socklen_t peerlen = sizeof(peeraddr);
            int conn = accept(listenfd,(struct sockaddr*)&peeraddr,&peerlen);
            if(conn < 0)
            {
                perror("accept");
                continue;
            }

            string username;
            // 客户端连上后第一行发送用户名，用于后续消息前缀。
            if(!recvLine(conn,username) || username.empty())
            {
                close(conn);
                continue;
            }

            users[conn] = username;
            string welcome = "[system] welcome " + username + "\n";
            sendAll(conn,welcome);
            broadcast(users,"[system] " + username + " joined chat\n",conn);
            cout<<username<<" joined from "<<inet_ntoa(peeraddr.sin_addr)<<":"<<ntohs(peeraddr.sin_port)<<endl;
        }

        map<int,string>::iterator cur = users.begin();
        while(cur != users.end())
        {
            int fd = cur->first;
            string username = cur->second;
            ++cur;

            if(!FD_ISSET(fd,&readfds))
                continue;

            string line;
            // 某个用户 fd 可读：读取一行聊天消息；读取失败表示用户断开。
            if(!recvLine(fd,line))
            {
                users.erase(fd);
                close(fd);
                broadcast(users,"[system] " + username + " left chat\n");
                cout<<username<<" left chat"<<endl;
                continue;
            }

            if(line.empty())
                continue;
            // 正常聊天消息广播给所有在线用户。
            broadcast(users,"[" + username + "] " + line + "\n");
            cout<<"["<<username<<"] "<<line<<endl;
        }
    }

    close(listenfd);
    return 0;
}
