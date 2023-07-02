#include "ProcessPool.hpp"

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

template <typename T>
ProcessPool<T>::ProcessPool(int listen_fd, int process_number)
    : _listen_fd(listen_fd), _process_number(process_number), _index(-1), _stop(false)
{
    assert((process_number > 0) && (process_number <= MAX_PROCESS_NUMBER));
    _sub_process = new process[process_number]; /*struct process { pid_t _pid; int _pipefd[2]; }; */

    // 创建process_number个子进程，并建立它们和父进程之间的管道
    for (int i = 0; i < process_number)
    {
        // 在linux下，使用socketpair函数能够创建一对套节字进行进程间通信（IPC）
        int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, _sub_process[i]._pipefd);
        assert(ret != -1);
        _sub_process[i]._pid = fork();
        assert(_sub_process[i]._pid >= 0);

        if (_sub_process[i]._pid > 0)
        { // 父进程
            close(_sub_process[i]._pipefd[1]);
            continue;
        }
        else
        {
            close(_sub_process[i]._pipefd[0]);
            _index = i;
            break;   //这才是进程池的精华 子进程需要退出循环，不然子进程也会fork 
        }
    }
}
