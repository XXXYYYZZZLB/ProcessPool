#include <iostream>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>

using namespace std;

class PP
{
public:
    int data = 100;
    int *arr;

    PP(int num)
    {

        arr = new int[10];
        for (int i = 0; i < num; i++)
        {
            arr[i] = fork();
            int ret = arr[i];
            if (ret > 0)
            {
                cout << "I am Father >>";
                print();
                continue;
            }
            else
            {
                data += i;
                cout << "I am Son! "
                     << "data:" << data << " pid:";
                print();
                break;
            }
        }
    }
    ~PP(){};
    void print()
    {
        cout << getpid() << " say: hello!" << endl;
    }
};

int main()
{
    PP p(5);
    cout <<"#### pid "<<getpid();
    for (int i = 0; i < 10; i++)
    {
        cout <<" "<< p.arr[i] << " ";
    }
    cout<<endl;

    return 0;
}