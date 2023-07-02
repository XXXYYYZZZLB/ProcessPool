#pragma once

/**
 * 半同步/半异步并发模式进程池
 * 为了避免在父子进程之间传递文件描述符，将接收连接操作放在子进程中
 * 一个客户连接上的所有任务始终是由一个子进程处理的
 */

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

struct process
{
    pid_t _pid;
    int _pipefd[2];
};

/**
 * 进程池类
 * 单例模式，以保证程序最多创建一个process_pool实例，这是程序正确处理信号的必要条件
 */
template <typename T>
class ProcessPool
{
private:
    ProcessPool(int listen_fd, int process_number = 8);
    static ProcessPool<T> *_instance;

public:
    // 单例模式，以保证程序最多创建一个process_pool实例，这是程序正确处理信号的必要条件
    static ProcessPool<T> *create(int listen_fd, int process_number = 8)
    {
        if (_instance == NULL)
        {
            _instance = new ProcessPool<T>(listen_fd, process_number);
        }
        return _instance;
    }
    void run();

private:
    void setupSigPipe();
    void runParent();
    void runChild();

private:
    // 进程池语序的最大子进程数量
    static const int MAX_PROCESS_NUMBER = 16;
    // 每个子进程最多能处理的客户数量
    static const int USER_PER_PROCESS = 65536;
    // epoll 最多能处理的事件数
    static const int MAX_EVENT_NUMBER = 10000;
    // 进程池的进程总数
    int _process_number;
    // 子进程在池中的序号，从0开始
    int _index;
    // 每个进程都有一个epollfd
    int _epoll_fd;
    // 监听套接字
    int _listen_fd;
    // 子进程通过_stop来决定是否停止运行
    int _stop;
    // 保存所有子进程的描述信息
    process *_sub_process;
};

template <typename T>
ProcessPool<T> *ProcessPool<T>::_instance = nullptr;

// 用于处理信号的管道，以统一事件源，后面称之为信号管道
static int sig_pipe_fd[2];

static int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

static void addfd(int epollfd, int fd)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

// 从epollfd标识的epoll内核事件表中删除fd上的所有注册事件
static void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

static void sig_handler(int sig)
{
    int save_errno = errno;
    int msg = sig;
    send(sig_pipe_fd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}
static void addsig(int sig, void(handler)(int), bool restart = true)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart)
    {
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

// 进程池构造函数，参数listenfd是监听socket，它必须在创建进程池之前被创建，
// 否则子进程无法直接引用它，参数process_number指定进程池中子进程的数量
template <typename T>
ProcessPool<T>::ProcessPool(int listen_fd, int process_number)
    : _listen_fd(listen_fd), _process_number(process_number), _index(-1), _stop(false)
{
    assert((process_number > 0) && (process_number <= MAX_PROCESS_NUMBER));
    _sub_process = new process[process_number]; /*struct process { pid_t _pid; int _pipefd[2]; }; */

    // 创建process_number个子进程，并建立它们和父进程之间的管道
    for (int i = 0; i < process_number;i++)
    {
        // 在linux下，使用socketpair函数能够创建一对套节字进行进程间通信（IPC）
        int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, _sub_process[i]._pipefd);
        assert(ret != -1);
        _sub_process[i]._pid = fork();//父进程存的是子进程的pid
        assert(_sub_process[i]._pid >= 0);

        if (_sub_process[i]._pid > 0)
        { // 父进程
            close(_sub_process[i]._pipefd[1]);
            continue;
        }
        else
        { // 子进程
            close(_sub_process[i]._pipefd[0]);
            _index = i;
            break; // 这才是进程池的精华 子进程需要退出循环，不然子进程也会fork
        }
    }
}

// 统一事件源，这个pipe不是用来和父进程通信的pipe
template <typename T>
void ProcessPool<T>::setupSigPipe()
{
    _epoll_fd = epoll_create(5);
    assert(_epoll_fd != -1);

    int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, sig_pipe_fd);
    assert(ret != -1);

    setnonblocking(sig_pipe_fd[1]); // sig_handler写的一般设置为非阻塞
    addfd(_epoll_fd, sig_pipe_fd[0]);

    // 设置信号处理函数
    addsig(SIGCHLD, sig_handler);
    addsig(SIGTERM, sig_handler);
    addsig(SIGINT, sig_handler);
    addsig(SIGPIPE, SIG_IGN);
}

template <typename T>
void ProcessPool<T>::run()
{
    _index == -1 ? runParent() : runChild();
}

template <typename T>
void ProcessPool<T>::runChild()
{
    setupSigPipe(); // 统一事件源 信号
    // 每个子进程通过其在进程池中的序号值index_找到与父进程通信的管道
    int pipefd = _sub_process[_index]._pipefd[1];
    // 子进程需要监听管道文件描述符pipefd，因为父进程通过它来通知子进程accept新链接
    addfd(_epoll_fd, pipefd);

    epoll_event events[MAX_EVENT_NUMBER];

    // 每个进程负责的客户连接数组
    T *users = new T[USER_PER_PROCESS];

    int number = 0;
    int ret = -1;

    while (!_stop)
    {
        number = epoll_wait(_epoll_fd, events, MAX_EVENT_NUMBER, -1);
        if ((number < 0) && (errno != EINTR))
        {
            printf("epoll failure\n");
            break;
        }
        for (int i = 0; i < number; i++)
        {
            int sockfd = events->data.fd;
            if ((sockfd == pipefd) && (events[i].events & EPOLLIN))
            {
                // 从父子进程之间的管道读取数据，并将结果保存在变量client中。如果读取成功，则表示有新客户连接到来
                int client = 0;
                ret = recv(sockfd, (char *)&client, sizeof(client), 0);
                if (((ret < 0) && (errno != EAGAIN)) || ret == 0)
                {
                    continue;
                }
                else
                {
                    struct sockaddr_in client_address;
                    socklen_t len = sizeof(sockaddr_in);
                    int connfd = accept(_listen_fd, (struct sockaddr *)&client_address, &len);
                    if (connfd < 0)
                    {
                        printf("errno is: %d\n", errno);
                        continue;
                    }
                    addfd(_epoll_fd, connfd);

                    // 模板类T必须要实现init方法，以初始化一个客户连接，我们直接使用connfd来索引逻辑处理对象（T类型的对象），以提高程序效率
                    users[connfd].init(_epoll_fd, connfd, client_address);
                }
            }
            // 下面处理子进程接收到的 信号
            else if ((sockfd == sig_pipe_fd[0]) && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];
                ret = recv(sig_pipe_fd[0], signals, sizeof(signals), 0);
                if (ret <= 0)
                {
                    continue;
                }
                else
                {
                    for (int i = 0; i < ret; ++i)
                    {
                        switch (signals[i])
                        {
                        case SIGCHLD://回收cgi的子进程
                        {
                            pid_t pid;
                            int stat;
                            while ((pid = waitpid(-1, &stat, WNOHANG)) > 0)
                            {
                                continue;
                            }
                            break;
                        }
                        case SIGTERM:
                        case SIGINT:
                        {
                            _stop = true;
                            break;
                        }
                        default:
                            break;
                        }
                    }
                }
            }
            // 如果是其他可读数据，那么必然是客户请求到来，调用逻辑处理对象的process方法处理之
            else if (events[i].events & EPOLLIN)
            {
                users[sockfd].process();
            }
            else
            {
                continue;
            }
        }
    }
    delete[] users;
    users = NULL;
    close(pipefd);
    close(_epoll_fd);
}

template <typename T>
void ProcessPool<T>::runParent()
{
    setupSigPipe();

    // 父进程监听listenfd
    addfd(_epoll_fd, _listen_fd);

    epoll_event events[MAX_EVENT_NUMBER];
    int sub_process_counter = 0;
    int number = 0;
    int ret = -1;

    while (!_stop)
    {
        number = epoll_wait(_epoll_fd, events, MAX_EVENT_NUMBER, -1);
        if ((number < 0) && (errno != EINTR))
        {
            printf("epoll failure\n");
            break;
        }

        for (int i = 0; i < number; ++i)
        {
            int sockfd = events[i].data.fd;
            if (sockfd == _listen_fd)
            {
                // 如果有新连接到来，就采用Round Robin的方式为其分配一个子进程处理
                int i = sub_process_counter;
                do
                {
                    if (_sub_process[i]._pid != -1) // 选一个仍存活的进程
                        break;

                    i = (i + 1) % _process_number;
                } while (i != sub_process_counter);

                if (_sub_process[i]._pid == -1)
                { // pid=-1说明找了一圈也没有找到一个活的进程，也就是所有子进程都死了，那么退出服务器
                    _stop = true;
                    break;
                }
                sub_process_counter = (i + 1) % _process_number;
                int new_conn = 1;                                                         // 这个值没意义，统一事件源
                send(_sub_process[i]._pipefd[0], (char *)&new_conn, sizeof(new_conn), 0); // 随便发送一个信号，让子进程识别
                printf("send request to child: %d\n", i);
            }
            else if ((sockfd == sig_pipe_fd[0]) && (events[i].events & EPOLLIN))
            {
                // 处理父进程收到的 信号
                int sig;
                char signals[1024];
                ret = recv(sig_pipe_fd[0], signals, sizeof(signals), 0);
                if (ret <= 0)
                    continue;
                else
                {
                    for (int i = 0; i < ret; ++i)
                    {
                        switch (signals[i])
                        {
                        case SIGCHLD:
                        {
                            pid_t pid;
                            int stat;
                            while ((pid = waitpid(-1, &stat, WNOHANG)) > 0)
                            {
                                for (int i = 0; i < _process_number; ++i)
                                {
                                    // 如果进程池中第i个子进程退出了，则主进程关闭相应的通信管道，并设置响应的pid_位-1，以标记该子进程已经退出
                                    if (_sub_process[i]._pid == pid)
                                    {
                                        printf("child %d join\n", i);
                                        close(_sub_process[i]._pipefd[0]);
                                        _sub_process[i]._pid = -1; // 标记为-1
                                    }
                                }
                            }
                            // 如果所有的子进程都退出了，则父进程也退出
                            _stop = true;
                            for (int i = 0; i < _process_number; ++i)
                            {
                                if (_sub_process[i]._pid != -1)
                                    _stop = false;
                            }
                            break;
                        }
                        case SIGTERM:
                        case SIGINT:
                        {
                            // 如果父进程收到终止信号，那么就杀死所有子进程，并等待它们全部结束。当然，通知子进程结束的更好
                            // 方法是向父子进程之间的通信管道发送特殊数据
                            printf("kill all the child now\n");
                            for (int i = 0; i < _process_number; ++i)
                            {
                                int pid = _sub_process[i]._pid;
                                if (pid != -1)
                                {
                                    kill(pid, SIGTERM);
                                }
                            }
                            break;
                        }
                        default:
                            break;
                        }
                    }
                }
            }
            else
            {
                continue;
            }
        }
    }
    close(_epoll_fd);
}