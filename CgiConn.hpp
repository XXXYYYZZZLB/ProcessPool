#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "ProcessPool.hpp"

class CgiConn
{
public:
    CgiConn() : _sockfd(-1), _read_idx(-1)
    {
        memset(_buf, 0, sizeof(_buf));
    }

public:
    void init(int epollfd, int sockfd, const sockaddr_in &client_addr)
    {
        _epollfd = epollfd;
        _sockfd = sockfd;
        _address = client_addr;
        _read_idx = 0;
    }
    void process()
    {
        int idx = 0;
        int ret = 1;

        // 循环读取和分析客户数据
        while (true)
        {
            idx = _read_idx;
            ret = recv(_sockfd, _buf + idx, BUFFER_SIZE - 1 - idx, 0);
            // 如果读操作发生错误，则关闭客户链接，如果只是暂时无数据可读，则退出循环
            if (ret < 0)
            {
                if (errno != EAGAIN)
                {
                    removefd(_epollfd, _sockfd);
                }
                break;
            }
            // 如果对方关闭，本服务器也关闭
            else if (ret == 0)
            {
                removefd(_epollfd, _sockfd);
                break;
            }
            else
            {
                _read_idx += ret;
                printf("user content is: %s", _buf);
                // 如果遇到字符CRLF,则开始处理客户请求
                printf("-------- _buf ------------\n");
                for (; idx < _read_idx; ++idx)
                {
                    if ((idx >= 1) && (_buf[idx - 1] == '\r') && (_buf[idx] == '\n')) // 这里查找CRLF采用简单遍历已读数据的方法
                        break;
                }
            }
            // 如果没有遇到字符CRLF,则需要读取更多客户数据
            if (idx == _read_idx)
            {
                continue;
            }
            _buf[idx - 1] = '\0';

            char *file_name = _buf;
            // 判断客户要运行的CGI程序是否存在
            if (access(file_name, F_OK) == -1)
            {
                removefd(_epollfd, _sockfd); // 不存在就不连接了
                break;
            }

            // 创建子进程来执行CGI程序
            ret = fork();
            if (ret == -1)
            {
                removefd(_epollfd, _sockfd);
                break;
            }
            else if (ret > 0)
            {
                // 父进程只需关闭连接
                removefd(_epollfd, _sockfd);
                break; // 父进程break
            }
            else
            {
                // 子进程将标准输出定向到sockfd_,并执行CGI程序
                close(STDOUT_FILENO);
                dup(_sockfd);
                execl(_buf, _buf, 0);
                exit(0);
            }
        }
    }

private:
    // 读缓冲区的大小
    static const int BUFFER_SIZE = 1024;
    static int _epollfd;
    int _sockfd;
    sockaddr_in _address;
    char _buf[BUFFER_SIZE];
    // 标记缓冲区中已读入客户数据的最后一个字节的下一个位置
    int _read_idx;
};

int CgiConn::_epollfd = -1;


