/*************************************************************************
	> File Name: fileTrans.cpp
	> Author: qinyu
	> Mail: qinyu.LT@gmail.com
	> Created Time: 2016年04月22日 星期五 12时55分04秒
 ************************************************************************/

#include "md5.h"
#include "MCache.h"
#include "MyDB.h"
#include "fileTrans.h"

#include <fstream>

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

static bool sendAll(int fd,const string &data)
{
    return sendAll(fd,data.c_str(),data.size());
}

static bool recvLine(int fd,string &line)
{
    line.clear();
    char ch;
    while(1)
    {
        ssize_t n = recv(fd,&ch,1,0);
        if(n < 0)
        {
            if(errno == EINTR)//EINTR代表系统调用被信号打断了，不是真正的错误，可以重新接收
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

static string sqlQuote(const string &value)
{
    string out = "'";
    for(size_t i = 0;i < value.size();++i)
    {
        if(value[i] == '\'')
            out += "\\'";
        else
            out += value[i];
    }
    out += "'";
    return out;
}

static string safeFileName(const string &name)
{
    size_t pos = name.find_last_of("/\\");
    string file = (pos == string::npos) ? name : name.substr(pos + 1);
    return file.empty() ? "unnamed" : file;
}

static string md5File(const string &path)
{
    ifstream in(path.c_str(),ios::binary);
    if(!in)
        return "";
    return MD5(in).toString();
}

FileTrans::FileTrans(int port,const char *addr)
{
    if((trans_fd=socket(PF_INET,SOCK_STREAM,0)) < 0)
        ERR_EXIT("socket");
    memset(&seraddr,0,sizeof(seraddr));
    seraddr.sin_family = AF_INET;
    seraddr.sin_port = htons(port);
    seraddr.sin_addr.s_addr = htonl(INADDR_ANY);

    int on = 1;
    if(setsockopt(trans_fd,SOL_SOCKET,SO_REUSEADDR,&on,sizeof(on)) < 0 )
        ERR_EXIT("setsockopt");
    if(::bind(trans_fd,(struct sockaddr*)&seraddr,sizeof(seraddr)) < 0 )
        ERR_EXIT("bind");
    if(listen(trans_fd,SOMAXCONN) < 0)
        ERR_EXIT("listen");
    connfd = -1;
}

FileTrans::~FileTrans()
{
    close(trans_fd);
    if(connfd >= 0)
        close(connfd);
}

void FileTrans::Run()
{
    struct epoll_event ev,events[EPOLL_SIZE];
    int epfd = epoll_create(EPOLL_SIZE);
    if(epfd < 0)
        ERR_EXIT("epoll_create");
    ev.events = EPOLLIN;
    ev.data.fd = trans_fd;
    epoll_ctl(epfd,EPOLL_CTL_ADD,trans_fd,&ev);

    while(1)
    {
        int events_count = epoll_wait(epfd,events,EPOLL_SIZE,-1);
        for(int i=0;i<events_count;++i)
        {
            if(events[i].data.fd != trans_fd)
                continue;

            socklen_t peerlen = sizeof(peeraddr);
            connfd = accept(trans_fd,(struct sockaddr *)&peeraddr,&peerlen);
            if(connfd < 0)
            {
                perror("accept");
                continue;
            }

            pid_t pid = fork();
            if(pid == -1)
                ERR_EXIT("FileTrans fork");
            if(pid == 0)
            {
                close(trans_fd);
                if(!recvLine(connfd,msg))
                {
                    close(connfd);
                    exit(EXIT_FAILURE);
                }

                getCmd();
                if(!msg.empty())
                    sendAll(connfd,msg);
                close(connfd);
                exit(EXIT_SUCCESS);
            }

            close(connfd);
            connfd = -1;
        }
    }
}

void FileTrans::getCmd()
{
    sql.clear();
    stringstream cmdStream(msg);
    int select = ERR;
    string token;

    if(!(cmdStream>>select))
    {
        msg = "ERR invalid command\n";
        return;
    }
    while(cmdStream>>token)//读取字符串直到读到末尾或是流进入错误状态
        sql.push_back(token);
    if(sql.empty())
    {
        msg = "ERR invalid user\n";
        return;
    }

    user_id = sql.front();
    sql.pop_front();

    switch(select)
    {
    case UPLODE:
        getFileInfo();
        recvFile();
        break;
    case DOWNLOAD:
        if(sql.empty())
        {
            msg = "ERR missing file name\n";
            return;
        }
        m_fileinfo.file_name = safeFileName(sql.front());
        sendFileInfo();
        if(msg.empty())
            sendFile();
        break;
    case RM:
        if(sql.empty())
        {
            msg = "ERR missing file name\n";
            return;
        }
        removeFile(sql.front());
        break;
    case LS:
        showFileList();
        break;
    default:
        msg = "ERR unsupported command\n";
        break;
    }
}

void FileTrans::getFileInfo()
{
    if(sql.size() < 3)
    {
        msg = "ERR missing file info\n";
        return;
    }

    m_fileinfo.file_name = safeFileName(sql.front());
    sql.pop_front();
    m_fileinfo.file_MD5 = sql.front();
    sql.pop_front();
    m_fileinfo.file_size = atoi(sql.front().c_str());
    sql.pop_front();
    m_fileinfo.file_path = string(SER_HOME_PATH) + "/" + user_id + "_" + m_fileinfo.file_name;
    m_fileinfo.file_chunk_size = 0;//每个块有编号、偏移量、大小、状态，可以单独重传、断点续传、并发上传。
    m_fileinfo.chunk_size = BUFFSIZE;
    m_fileinfo.trans_status = false;
    msg.clear();
}

void FileTrans::sendFileInfo()
{
    MyDB db;
    if(!db.initDB(SERADDR,DBUSER,DBPSSW,DBNAME))
    {
        msg = "ERR mysql connecting\n";
        return;
    }

    string selectSql = "select file_name,file_MD5,file_path,file_size,trans_status "
                       "from files where user_id=" + sqlQuote(user_id) +
                       " and file_name=" + sqlQuote(m_fileinfo.file_name) +
                       " order by file_path desc limit 1";
    db.execSQL(selectSql);
    list<string> res = db.getResult();
    if(res.size() < 5)
    {
        msg = "ERR file not found\n";
        return;
    }

    list<string>::iterator it = res.begin();
    m_fileinfo.file_name = *it++;
    m_fileinfo.file_MD5 = *it++;
    m_fileinfo.file_path = *it++;
    m_fileinfo.file_size = atoi((*it++).c_str());
    m_fileinfo.trans_status = (*it == "ok");
    msg.clear();
}

bool FileTrans::recvFile()
{
    if(!msg.empty())
        return false;

    mkdir(SER_HOME_PATH,0755);
    if(!sendAll(connfd,string("READY\n")))
    {
        msg = "ERR send ready failed\n";
        return false;
    }

    if(!createFile())
    {
        msg = "ERR save file failed\n";
        return false;
    }

    if(md5File(m_fileinfo.file_path) != m_fileinfo.file_MD5)
    {
        unlink(m_fileinfo.file_path.c_str());
        msg = "ERR file md5 mismatch\n";
        return false;
    }

    m_fileinfo.trans_status = true;

    MCache mem(SERADDR,MCA);
    mem.insertValue(m_fileinfo.file_MD5.c_str(),m_fileinfo.file_name.c_str(),180);

    MyDB db;
    if(!db.initDB(SERADDR,DBUSER,DBPSSW,DBNAME))
    {
        msg = "ERR mysql connecting\n";
        return false;
    }

    stringstream ss;
    ss<<"insert into files(file_name,user_id,file_MD5,file_path,file_size,trans_status) values("
      <<sqlQuote(m_fileinfo.file_name)<<","
      <<sqlQuote(user_id)<<","
      <<sqlQuote(m_fileinfo.file_MD5)<<","
      <<sqlQuote(m_fileinfo.file_path)<<","
      <<m_fileinfo.file_size<<","
      <<sqlQuote("ok")<<")";
    db.execSQL(ss.str());
    msg = "OK file upload success\n";
    return true;
}

bool FileTrans::sendFile()
{
    int fd = open(m_fileinfo.file_path.c_str(), O_RDONLY);
    if(fd < 0)
    {
        msg = "ERR open file failed\n";
        return false;
    }

    struct stat statBuf;
    if(fstat(fd,&statBuf) < 0)
    {
        close(fd);
        msg = "ERR stat file failed\n";
        return false;
    }

    stringstream header;
    header<<"OK "<<statBuf.st_size<<" "<<m_fileinfo.file_name<<"\n";
    if(!sendAll(connfd,header.str()))
    {
        close(fd);
        msg.clear();
        return false;
    }

    off_t offset = 0;
    while(offset < statBuf.st_size)
    {
        ssize_t n = sendfile(connfd,fd,&offset,statBuf.st_size - offset);
        if(n < 0)
        {
            if(errno == EINTR)
                continue;
            close(fd);
            msg.clear();
            return false;
        }
        if(n == 0)
            break;
    }

    close(fd);
    msg.clear();
    return true;
}

bool FileTrans::removeFile(string path)
{
    string fileName = safeFileName(path);
    MyDB db;
    if(!db.initDB(SERADDR,DBUSER,DBPSSW,DBNAME))
    {
        msg = "ERR mysql connecting\n";
        return false;
    }

    string selectSql = "select file_MD5,file_path from files where user_id=" + sqlQuote(user_id) +
                       " and file_name=" + sqlQuote(fileName) + " limit 1";
    db.execSQL(selectSql);
    list<string> res = db.getResult();
    if(res.size() < 2)
    {
        msg = "ERR file not found\n";
        return false;
    }

    list<string>::iterator it = res.begin();
    string md5 = *it++;
    string filePath = *it;
    MCache mem(SERADDR,MCA);
    mem.deleteKey(md5.c_str());
    unlink(filePath.c_str());

    string deleteSql = "delete from files where user_id=" + sqlQuote(user_id) +
                       " and file_name=" + sqlQuote(fileName);
    db.execSQL(deleteSql);
    msg = "OK file removed\n";
    return true;
}

bool FileTrans::createFile()
{
    int fd = open(m_fileinfo.file_path.c_str(), O_WRONLY|O_CREAT|O_TRUNC, 0666);
    if(fd == -1)
        return false;

    char recvBuf[BUFFSIZE];
    int received = 0;
    while(received < m_fileinfo.file_size)
    {
        int need = m_fileinfo.file_size - received;
        if(need > BUFFSIZE)
            need = BUFFSIZE;

        ssize_t nbytes = recv(connfd,recvBuf,need,0);
        if(nbytes < 0)
        {
            if(errno == EINTR)
                continue;
            close(fd);
            return false;
        }
        if(nbytes == 0)
        {
            close(fd);
            return false;
        }

        ssize_t written = 0;
        while(written < nbytes)
        {
            ssize_t n = write(fd,recvBuf + written,nbytes - written);
            if(n <= 0)
            {
                close(fd);
                return false;
            }
            written += n;
        }
        received += nbytes;
    }

    close(fd);
    return true;
}

void FileTrans::showFileList()
{
    MyDB db;
    if(!db.initDB(SERADDR,DBUSER,DBPSSW,DBNAME))
    {
        msg = "ERR mysql connecting\n";
        return;
    }

    string selectSql="select file_name from files where user_id=" + sqlQuote(user_id);
    db.execSQL(selectSql);
    list<string> res=db.getResult();

    msg.clear();
    for(list<string>::iterator it=res.begin();it!=res.end();++it)
    {
        if(!msg.empty())
            msg += " ";
        msg += *it;
    }
    msg += "\n";
}

int main()
{
    FileTrans ft(TPT,SERADDR);
    ft.Run();
    return 0;
}
