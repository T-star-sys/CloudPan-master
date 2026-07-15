/*************************************************************************
	> File Name: conCli.cpp
	> Author: qinyu
	> Mail: qinyu.LT@gmail.com
	> Created Time: 2016年04月19日 星期二 22时48分39秒
 ************************************************************************/
#include "md5.h"
#include "conCli.h"

#include <fstream>
#include <limits>
#include <sys/wait.h>

// 保证把 len 字节完整发送出去；send 可能一次只发送部分数据
//1.socket 发送缓冲区空间不够
//2.对方接收太慢
//3.数据太大
//4.系统调用被信号打断
static bool sendAll(int fd,const char *buf,size_t len)
{
    size_t sent = 0;
    while(sent < len)
    {
        ssize_t n = send(fd,buf + sent,len - sent,0);
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

// string 版本的完整发送封装，内部复用 char* 版本。
static bool sendAll(int fd,const string &data)
{
    return sendAll(fd,data.c_str(),data.size());
}

// 按行接收数据，直到读到 '\n'；聊天和文件协议的响应头都用它读取。
static bool recvLine(int fd,string &line)
{
    line.clear();
    char ch;
    while(1)
    {
        ssize_t n = recv(fd,&ch,1,0);
        if(n < 0)
        {
            if(errno == EINTR)
                continue;
            return false;
        }
        if(n == 0)
            return false;
        if(ch == '\n')
            return true;
        line += ch;
    }
}

// 创建 TCP socket 并连接到指定 IP 和端口，成功返回通信 fd，失败返回 -1。
static int connectToServer(const string &ip,int port)
{
    int fd = socket(PF_INET,SOCK_STREAM,0);
    if(fd < 0)
        return -1;

    struct sockaddr_in addr;
    memset(&addr,0,sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip.c_str());

    if(connect(fd,(struct sockaddr*)&addr,sizeof(addr)) < 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

// 从路径中截取文件名，例如 /tmp/a.txt -> a.txt。
static string baseName(const string &path)
{
    size_t pos = path.find_last_of("/\\");
    if(pos == string::npos)
        return path;
    return path.substr(pos + 1);
}

// 构造客户端对象，保存管理服务器地址，并初始化登录状态。
conCli::conCli(int port,const char *addr)
{
    this->port = port;
    m_addr = addr;
    sock = -1;
    logged_in = false;

    memset(&seraddr,0,sizeof(seraddr));
    seraddr.sin_family = AF_INET;
    seraddr.sin_port = htons(port);
    seraddr.sin_addr.s_addr = inet_addr(m_addr);
}

// 析构时关闭可能仍然打开的 socket。
conCli::~conCli()
{
    if(sock >= 0)
        close(sock);
}

// 客户端主循环：不断进入 do_work，根据登录状态显示不同菜单。
void conCli::Run()
{
    while(1)
        do_work();
}

// 打印未登录时的欢迎菜单：登录或注册。
void conCli::wellcome()
{
    cout<<"===================================="<<endl;
    cout<<"=   Welcome To My Network Disk!    ="<<endl;
    cout<<"===================================="<<endl;
    cout<<"=                                  ="<<endl;
    cout<<"=       1.登录       2.注册       ="<<endl;
    cout<<"=                                  ="<<endl;
    cout<<"===================================="<<endl;
    cout<<"Your Choice >: ";
}

// 打印登录后的功能菜单。
void conCli::function()
{
    cout<<"===================================="<<endl;
    cout<<"=   Welcome To My Network Disk!    ="<<endl;
    cout<<"===================================="<<endl;
    cout<<"=                                  ="<<endl;
    cout<<"=       5.聊天                     ="<<endl;
    cout<<"=       3.上传文件                 ="<<endl;
    cout<<"=       4.下载文件                 ="<<endl;
    cout<<"=       6.删除文件                 ="<<endl;
    cout<<"=       7.查看文件列表             ="<<endl;
    cout<<"=       0.退出                     ="<<endl;
    cout<<"=                                  ="<<endl;
    cout<<"===================================="<<endl;
    cout<<"Your Choice >: ";
}

// 向管理服务器发送登录/注册请求，并根据返回结果更新登录状态。
void conCli::sendmsg()
{
    int fd = connectToServer(m_addr,CPT);
    if(fd < 0)
    {
        cout<<"连接管理服务器失败"<<endl;
        return;
    }

    if(!sendAll(fd,msg.c_str(),msg.size() + 1))
    {
        close(fd);
        cout<<"发送请求失败"<<endl;
        return;
    }

    char buf[1024] = {0};
    int ret = recv(fd,buf,sizeof(buf) - 1,0);
    close(fd);
    if(ret <= 0)
    {
        cout<<"服务器没有返回结果"<<endl;
        return;
    }

    buf[ret] = '\0';
    msg = buf;
    if(strncmp("1",msg.c_str(),1) == 0)
    {
        current_user = pending_user;
        logged_in = true;
        cout<<"登录成功"<<endl;
    }
    else if(strncmp("register success!",msg.c_str(),17) == 0)
    {
        current_user = pending_user;
        logged_in = true;
        cout<<"注册成功，已自动登录"<<endl;
    }
    else
    {
        cout<<msg<<endl;
    }
}

// 读取用户名和密码，把密码转成 MD5 后拼入 msg，再调用 sendmsg 发送。
void conCli::getinfor()
{
    string user,passwd;

    cout<<"用户名：";
    cin>>user;
    pending_user = user;
    msg += user;
    cout<<"密  码：";
    cin>>passwd;
    msg = msg + " " + MD5(passwd).toString();
    sendmsg();
}

// 向管理服务器请求文件传输服务器地址。
// op 可以是上传、下载、删除或查看列表，返回的 ip/port 用于连接 FTS。
bool conCli::requestFTServer(int op,string &ip,int &port)
{
    int fd = connectToServer(m_addr,CPT);
    if(fd < 0)
    {
        cout<<"连接管理服务器失败"<<endl;
        return false;
    }

    stringstream req;
    req<<op<<" "<<current_user<<" 0";//拼接字符串
    string request = req.str();
    if(!sendAll(fd,request.c_str(),request.size() + 1))
    {
        close(fd);
        return false;
    }

    char buf[1024] = {0};
    int ret = recv(fd,buf,sizeof(buf) - 1,0);
    close(fd);
    if(ret <= 0)
        return false;

    int cmd = 0;
    stringstream resp(string(buf,ret));
    resp>>cmd>>ip>>port;
    return cmd == op && !ip.empty() && port > 0;
}

// 上传文件：读取本地路径，计算 MD5，连接 FTS，先发文件头再发送文件内容。
bool conCli::uploadFile()
{
    string path;
    cout<<"请输入要上传的本地文件路径：";
    cin>>path;

    struct stat st;
    if(stat(path.c_str(),&st) < 0 || !S_ISREG(st.st_mode))//stat获取文件信息，S_ISREG判断是否为普通文件
    {
        cout<<"文件不存在或不是普通文件"<<endl;
        return false;
    }

    ifstream md5In(path.c_str(),ios::binary);//二进制方式”打开要上传的文件，准备用来计算 MD5
    if(!md5In)
    {
        cout<<"打开文件失败"<<endl;
        return false;
    }

    string fileName = baseName(path);
    string fileMd5 = MD5(md5In).toString();
    string ip;
    int ftsPort = 0;
    if(!requestFTServer(UPLODE,ip,ftsPort))
    {
        cout<<"获取文件传输服务器失败"<<endl;
        return false;
    }

    int fd = connectToServer(ip,ftsPort);
    if(fd < 0)
    {
        cout<<"连接文件传输服务器失败"<<endl;
        return false;
    }

    stringstream header;
    header<<UPLODE<<" "<<current_user<<" "<<fileName<<" "<<fileMd5<<" "<<st.st_size<<"\n";
    if(!sendAll(fd,header.str()))
    {
        close(fd);
        cout<<"发送上传请求失败"<<endl;
        return false;
    }

    string line;
    if(!recvLine(fd,line) || line != "READY")
    {
        close(fd);
        cout<<"服务端拒绝上传："<<line<<endl;
        return false;
    }

    ifstream in(path.c_str(),ios::binary);
    char buf[BUFFSIZE];
    while(in)
    {
        // 每次读取一块数据，读到多少就发送多少。
        in.read(buf,sizeof(buf));
        streamsize n = in.gcount();
        if(n > 0 && !sendAll(fd,buf,(size_t)n))
        {
            close(fd);
            cout<<"上传过程中连接中断"<<endl;
            return false;
        }
    }

    if(recvLine(fd,line))
        cout<<line<<endl;
    else
        cout<<"上传完成，但没有收到服务端确认"<<endl;

    close(fd);
    return true;
}

// 下载文件：向 FTS 请求指定文件，收到文件头后把文件内容保存到 clidir 目录。
bool conCli::downloadFile()
{
    string fileName;
    cout<<"请输入要下载的文件名：";
    cin>>fileName;

    string ip;
    int ftsPort = 0;
    if(!requestFTServer(DOWNLOAD,ip,ftsPort))
    {
        cout<<"获取文件传输服务器失败"<<endl;
        return false;
    }

    int fd = connectToServer(ip,ftsPort);
    if(fd < 0)
    {
        cout<<"连接文件传输服务器失败"<<endl;
        return false;
    }

    stringstream request;
    request<<DOWNLOAD<<" "<<current_user<<" "<<fileName<<"\n";
    if(!sendAll(fd,request.str()))
    {
        close(fd);
        cout<<"发送下载请求失败"<<endl;
        return false;
    }

    string line;
    if(!recvLine(fd,line))
    {
        close(fd);
        cout<<"下载失败：服务器无响应"<<endl;
        return false;
    }
    if(line.compare(0,3,"OK ") != 0)
    {
        close(fd);
        cout<<line<<endl;
        return false;
    }

    string ok,recvName;
    long long fileSize = 0;
    stringstream header(line);
    header>>ok>>fileSize>>recvName;
    if(recvName.empty())
        recvName = fileName;

    mkdir(CLI_HOME_PATH,0755);
    string savePath = string(CLI_HOME_PATH) + "/" + recvName;
    int out = open(savePath.c_str(),O_WRONLY | O_CREAT | O_TRUNC,0666);
    if(out < 0)
    {
        close(fd);
        cout<<"创建本地文件失败"<<endl;
        return false;
    }

    char buf[BUFFSIZE];
    long long received = 0;
    while(received < fileSize)
    {
        // 按文件大小精确接收，避免多读下一条协议数据。
        size_t need = (size_t)((fileSize - received) > BUFFSIZE ? BUFFSIZE : (fileSize - received));
        ssize_t n = recv(fd,buf,need,0);
        if(n <= 0)
        {
            close(out);
            close(fd);
            cout<<"下载过程中连接中断"<<endl;
            return false;
        }
        if(write(out,buf,n) != n)
        {
            close(out);
            close(fd);
            cout<<"写入本地文件失败"<<endl;
            return false;
        }
        received += n;
    }

    close(out);
    close(fd);
    cout<<"下载完成："<<savePath<<endl;
    return true;
}

// 删除文件：向 FTS 发送删除请求，服务端会删除数据库记录和磁盘文件。
bool conCli::removeFile()
{
    string fileName;
    cout<<"请输入要删除的文件名：";
    cin>>fileName;

    string ip;
    int ftsPort = 0;
    if(!requestFTServer(RM,ip,ftsPort))
    {
        cout<<"获取文件传输服务器失败"<<endl;
        return false;
    }

    int fd = connectToServer(ip,ftsPort);
    if(fd < 0)
    {
        cout<<"连接文件传输服务器失败"<<endl;
        return false;
    }

    stringstream request;
    request<<RM<<" "<<current_user<<" "<<fileName<<"\n";
    if(!sendAll(fd,request.str()))
    {
        close(fd);
        cout<<"发送删除请求失败"<<endl;
        return false;
    }

    string line;
    if(!recvLine(fd,line))
    {
        close(fd);
        cout<<"删除失败：服务器无响应"<<endl;
        return false;
    }

    close(fd);
    cout<<line<<endl;
    return line.compare(0,2,"OK") == 0;
}

// 查看文件列表：向 FTS 发送 LS 请求并打印当前用户所有文件名。
bool conCli::listFiles()
{
    string ip;
    int ftsPort = 0;
    if(!requestFTServer(LS,ip,ftsPort))
    {
        cout<<"获取文件传输服务器失败"<<endl;
        return false;
    }

    int fd = connectToServer(ip,ftsPort);
    if(fd < 0)
    {
        cout<<"连接文件传输服务器失败"<<endl;
        return false;
    }

    stringstream request;
    request<<LS<<" "<<current_user<<"\n";
    if(!sendAll(fd,request.str()))
    {
        close(fd);
        cout<<"发送查看文件列表请求失败"<<endl;
        return false;
    }

    string line;
    if(!recvLine(fd,line))
    {
        close(fd);
        cout<<"查看文件列表失败：服务器无响应"<<endl;
        return false;
    }

    close(fd);
    cout<<"我的文件："<<(line.empty() ? "暂无文件" : line)<<endl;
    return true;
}

// 向管理服务器请求聊天服务器地址。
bool conCli::requestCServer(string &ip,int &port)
{
    int fd = connectToServer(m_addr,CPT);
    if(fd < 0)
    {
        cout<<"连接管理服务器失败"<<endl;
        return false;
    }

    stringstream req;
    req<<CHART<<" "<<current_user<<" 0";
    string request = req.str();
    if(!sendAll(fd,request.c_str(),request.size() + 1))
    {
        close(fd);
        return false;
    }

    char buf[1024] = {0};
    int ret = recv(fd,buf,sizeof(buf) - 1,0);
    close(fd);
    if(ret <= 0)
        return false;

    int cmd = 0;
    stringstream resp(string(buf,ret));
    resp>>cmd>>ip>>port;
    return cmd == CHART && !ip.empty() && port > 0;
}

// 聊天功能：连接聊天服务器，父进程负责发送输入，子进程负责接收显示消息。
void conCli::chat()
{
    string ip;
    int chatPort = 0;
    if(!requestCServer(ip,chatPort))
    {
        cout<<"获取聊天服务器失败"<<endl;
        return;
    }

    int fd = connectToServer(ip,chatPort);
    if(fd < 0)
    {
        cout<<"连接聊天服务器失败"<<endl;
        return;
    }

    if(!sendAll(fd,current_user + "\n"))
    {
        close(fd);
        cout<<"发送用户名失败"<<endl;
        return;
    }

    cout<<"进入聊天室，输入 /quit 退出"<<endl;

    pid_t pid = fork();
    if(pid < 0)
    {
        perror("fork");
        close(fd);
        return;
    }

    if(pid == 0)
    {
        string line;
        while(recvLine(fd,line))
            cout<<line<<endl;
        close(fd);
        exit(0);
    }

    cin.ignore(numeric_limits<streamsize>::max(),'\n');
    string line;
    while(getline(cin,line))
    {
        if(line == "/quit")
            break;
        if(!sendAll(fd,line + "\n"))
        {
            cout<<"发送聊天消息失败"<<endl;
            break;
        }
    }

    close(fd);
    kill(pid,SIGTERM);
    waitpid(pid,NULL,0);
}

// 根据登录状态分发用户操作：未登录处理登录/注册，登录后处理各业务功能。
void conCli::do_work()
{
    int select;

    if(!logged_in)
    {
        wellcome();
        if(!(cin>>select))//防止用户没有正常输入数字，导致程序继续用错误数据运行
            exit(0);

        switch(select)
        {
        case ENTER:
            msg = "1 ";
            getinfor();
            break;
        case REGISTER:
            msg = "2 ";
            getinfor();
            break;
        default:
            cout<<"请选择正确的操作"<<endl;
            break;
        }
        return;
    }

    function();
    if(!(cin>>select))
        exit(0);

    switch(select)
    {
    case CHART:
        chat();
        break;
    case UPLODE:
        uploadFile();
        break;
    case DOWNLOAD:
        downloadFile();
        break;
    case RM:
        removeFile();
        break;
    case LS:
        listFiles();
        break;
    case 0:
        exit(0);
    default:
        cout<<"请选择正确的操作"<<endl;
        break;
    }

    cout<<"按回车键继续...";
    cin.ignore(numeric_limits<streamsize>::max(),'\n');
    cin.get();
}

// 程序入口：创建客户端并连接本机管理服务器。
int main()
{
    conCli cli(CPT,"127.0.0.1");
    cli.Run();
    return 0;
}
