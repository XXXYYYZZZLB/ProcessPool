#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <pthread.h>

/**
 * 信号是一种异步事件
 * 信号的处理和程序的主循环是两条不同的执行路线
 * 很显然，信号处理函数需要尽快的执行完毕，确保信号不会被屏蔽
 * 一种典型的解决方案是：
 * 把信号的主要处理逻辑放到程序的主循环中，当信号处理函数被触发的时候
 * 它只是简单的通知主循环接收到信号，并把信号值传递给主循环
 * 主循环再根据接收到的信号执行目标信号的逻辑代码
 * 
 * 信号处理函数通常使用管道来将信号传递给主循环
 * 主循环怎么知道管道可读呢，就用IO多路复用
 */

#define MAX_EVENT_NUMBER 1024
static int pipefd[2];

int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

void addfd(int epollfd, int fd)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

// 信号处理函数
void sig_handler(int sig)
{
    // 保证可重入性
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char *)&msg, 1, 0); // 将信号值写入管道
    errno = save_errno;
}

// 设置信号处理函数
void addsig(int sig)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = sig_handler;
    sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

int main(int argc, char *argv[])
{
    if (argc <= 2)
    {
        printf("Usage: %s ip_adress port_number\n", basename(argv[0]));
        return 1;
    }
    const char *ip = argv[1];
    int port = atoi(argv[2]);

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &address.sin_addr);
    address.sin_port = htons(port);

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    if (ret == -1)
    {
        printf("errno is %d\n", errno);
        return 1;
    }
    ret = listen(listenfd, 5);
    if(ret <0){
        printf("errno is %d\n", errno);
    }
    assert(ret != -1);

    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    assert(epollfd != -1);

    addfd(epollfd, listenfd);

    // 创建管道，注册pipefd[0]的可读事件
    ret = socketpair(AF_UNIX, SOCK_STREAM, 0, pipefd);
    if(ret <0){
        printf("errno is %d\n", errno);
    }
    assert(ret != -1);
    setnonblocking(pipefd[0]); //?
    addfd(epollfd, pipefd[0]);

    // 设置一些信号处理函数
    addsig(SIGHUP);
    addsig(SIGCHLD);
    addsig(SIGTERM);
    addsig(SIGINT);

    bool stop_server = false;

    while (!stop_server)
    {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if ((number < 0) && (errno != EINTR))
        {
            printf("epoll failure\n");
            break;
        }

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;
            if (sockfd == listenfd)
            {
                struct sockaddr_in client_address;
                socklen_t client_address_len = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr *)&client_address,
                                    &client_address_len);
                addfd(epollfd, connfd);
            }
            // 如果就绪的是pipefd[0];就处理信号
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[10024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1)
                {
                    continue;
                }
                else if (ret == 0)
                {
                    continue;
                }
                else
                {
                    // 读取到信号
                    // 每个信号占1个字节
                    // 顺便说明如何安全的终止服务器主循环
                    for (int i = 0; i < ret; i++)
                    {
                        switch (signals[i])
                        {
                        case SIGCHLD:
                        case SIGHUP:
                        {
                            continue;
                        }
                        case SIGTERM:
                        case SIGINT:
                        {
                            stop_server = true;
                        }
                        }
                    }
                }
            }
            else
            {
            }
        }
    }
    printf("close fds\n");
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    return 0;
}