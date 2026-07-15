/*************************************************************************
	> File Name: conSer.h
	> Author: qinyu
	> Mail: qinyu.LT@gmail.com 
	> Created Time: 2016年04月19日 星期二 22时51分03秒
 ************************************************************************/

#ifndef CONSER_H
#define CONSER_H
#include "../public.h"
#include "MyDB.h"

class conSer
{
public:

    conSer(int port,const char *addr);
    ~conSer();
    void Socket();
    void Bind();
    void Listen();
    int accept_timeout(unsigned int wait_seconds);//select
    void epoll_work();                            //epoll
    void Run();

private:
    // 文件服务目前只有一个实例；该函数负责把 FTS 的 IP/端口写回给客户端。
    void selectFTServer();
    // 聊天服务目前只有一个实例；该函数负责把聊天服务器的 IP/端口写回给客户端。
    void selectCServer();
    // 使用 conn 这个通信 fd 读取客户端请求、分发处理、写回响应。
    void do_service(int conn);
    // 解析 msg 中的命令字和参数，并执行登录、注册、服务分发等管理逻辑。
    void get_cmd();

private:
    // 监听 fd：只负责 bind/listen/accept，不直接承载业务数据。
    int listenfd;
    int port;
    char *addr;
    list<string> sql;//临时保存客户端发来的命令参数，名字叫 sql 但实际是 token/args 列表。
    // 当前请求和响应复用这个字符串：先保存客户端请求，get_cmd 后改成服务端响应。
    string msg;
    //msgInfo msg;
    struct sockaddr_in seraddr,peeraddr;
    //MyDB *db;
};

#endif
