#include "CgiConn.hpp"

int main(int argc, char **argv)
{
    if (argc <= 2)
    {
        printf("usage: %s ip_address port_number\n", basename(argv[0]));
        return -1;
    }

    const char *ip = argv[1];
    int port = atoi(argv[2]);

    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    int ret = 0;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &address.sin_addr);
    address.sin_port = htons(port);

    int on = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    if (ret == -1)
    {
        printf("what: %m\n");
        return -1;
    }

    ret = listen(listenfd, 5);
    assert(ret != -1);

    ProcessPool<CgiConn> *pool = ProcessPool<CgiConn>::create(listenfd);
    if (pool)
    {
        pool->run();
        delete pool;
    }

    close(listenfd);

    return 0;
}