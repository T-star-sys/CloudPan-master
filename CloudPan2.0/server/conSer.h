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
#include "tPool.h"

#include <pthread.h>
#include <time.h>

class conSer
{
public:

    conSer(int port,const char *addr);
    ~conSer();
    void Socket();
    void Bind();
    void Listen();
    void epoll_work();
    void Run();

private:
    // 文件服务目前只有一个实例；该函数负责把 FTS 的 IP/端口写回给客户端。
    void selectFTServer(string &response);
    // 聊天服务目前只有一个实例；该函数负责把聊天服务器的 IP/端口写回给客户端。
    void selectCServer(string &response);
    // 使用 conn 这个通信 fd 读取客户端请求、分发处理、写回响应。
    void do_service(int conn,const struct sockaddr_in &peer);
    // 解析当前任务的命令和参数，并执行登录、注册、服务分发等管理逻辑。
    void get_cmd(string &request,const struct sockaddr_in &peer);
    // 生成包含运行时间、请求数和暂禁数量的心跳报告。
    void heartbeatReport(string &response);
    // 处理 STATUS、BAN、UNBAN、RESET 和 HELP 管理命令。
    void executeAdminCommand(list<string> &args,string &response);

    struct ServiceTask;
    static void *serviceTask(void *arg);

    bool isTemporarilyBlocked(const string &ip,int &remainingSeconds);
    bool recordLoginFailure(const string &ip,int &remainingSeconds);
    void clearLoginFailures(const string &ip);
    void setManualBan(const string &ip,int seconds);
    bool removeBan(const string &ip);
    void resetGuardState();
    void workerStarted();
    void workerFinished();
    void requestReceived();

    struct SharedState;

private:
    // 监听 fd：只负责 bind/listen/accept，不直接承载业务数据。
    int listenfd;
    int port;
    const char *addr;
    struct sockaddr_in seraddr;
    // 多个工作线程通过互斥锁安全更新统计信息和 IP 暂禁记录。
    SharedState *shared;
};

#endif
