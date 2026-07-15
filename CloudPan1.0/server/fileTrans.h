/*************************************************************************
	> File Name: fileTrans.h
	> Author: qinyu
	> Mail: qinyu.LT@gmail.com 
	> Created Time: 2016年04月21日 星期三 12时54分19秒
 ************************************************************************/

#ifndef _FILETRANS_H
#define _FILETRANS_H

#include "../public.h"

class FileTrans
{
public:
    FileTrans(int port,const char*addr);
    ~FileTrans();
    // 启动文件传输服务器，监听 TPT 端口并处理上传、下载、删除、列表请求。
    void Run();
private:
    // 发送文件内容给客户端。
    bool sendFile();
    // 接收客户端上传的文件并写入数据库记录。
    bool recvFile();
    // 按 m_fileinfo 中的路径创建/写入服务端文件。
    bool createFile();
    // 删除用户文件记录及相关缓存。
    bool removeFile(string path);
    // 下载前从数据库查询文件元信息。
    void sendFileInfo();
    // 从命令参数列表中解析文件元信息。
    void getFileInfo();
    // 查询并返回当前用户文件列表。
    void showFileList();
    // 解析客户端发来的文件操作命令。
    void getCmd();

private:
    // trans_fd 是监听 fd；connfd 是 accept 返回的通信 fd。
    int trans_fd;
    int connfd;
    string user_id;
    string msg;
    list<string> sql;
    struct fileinfo m_fileinfo;
    struct filedata m_filedata;
    struct sockaddr_in seraddr,peeraddr;
};

#endif
