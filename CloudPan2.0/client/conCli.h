/*************************************************************************
	> File Name: conCli.h
	> Author: qinyu
	> Mail: qinyu.LT@gmail.com 
	> Created Time: 2016年04月19日 星期二 22时50分11秒
 ************************************************************************/

#ifndef CONCLI_H
#define CONCLI_H
#include "../public.h"

class conCli
{
public:
    conCli(int port,const char *addr);
    ~conCli();
    void Run();
    void wellcome();
    void function();
    void do_work();
    void sendmsg();
    void getinfor();
    // 下列接口用于文件/聊天扩展；当前 conCli.cpp 版本主要实现登录注册流程。
    bool requestFTServer(int op,string &ip,int &port);
    bool uploadFile();
    bool downloadFile();
    bool removeFile();
    bool listFiles();
    bool requestCServer(string &ip,int &port);
    void chat();
    bool heartbeat();
    void adminCommand();
private:
    // 与管理服务器 6666 建立的 TCP 连接。
    int sock;
    //int trans_fd;
    //int check_fd;
    int port;
    const char *m_addr;
    //msgInfo msg;
    string msg;
    // 登录成功后记录当前用户；pending_user 用于登录/注册请求尚未确认前暂存用户名。
    string current_user;
    string pending_user;
    bool logged_in;//
    //list<string> msg;
    //list<string>::iterator it;
    struct sockaddr_in seraddr;
};

#endif
