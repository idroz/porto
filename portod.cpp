#include "portod.h"
#include "fdstream.h"

extern "C" {
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
}

/* Example:
 * nc -U /run/porto.socket
 * create: { name: "test" }
 * list: { }
 *
 */

static int CreateRpcServer(const char *path)
{
    int fd;
    struct sockaddr_un my_addr;

    memset(&my_addr, 0, sizeof(struct sockaddr_un));

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr<<"socket() error: "<<strerror(errno)<<std::endl;
        return -1;
    }

    my_addr.sun_family = AF_UNIX;
    strncpy(my_addr.sun_path, RPC_SOCK_PATH, sizeof(my_addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *) &my_addr,
             sizeof(struct sockaddr_un)) < 0) {
        std::cerr<<"bind() error: "<<strerror(errno)<<std::endl;
        return -2;
    }

    if (listen(fd, 0) < 0) {
        std::cerr<<"listen() error: "<<strerror(errno)<<std::endl;
        return -3;
    }

    return 0;
}

static int sfd;
void cleanup(int signum)
{
    close(sfd);
}

int main(int argc, const char *argv[])
{
    TContainerHolder cholder;
    struct sockaddr_un peer_addr;
    socklen_t peer_addr_size;
    int cfd;
    int ret;

    sfd = CreateRpcServer(RPC_SOCK_PATH);
    if (sfd < 0) {
        std::cerr<<"Can't create RPC server"<<std::endl;
        return sfd;
    }

    signal(SIGINT, cleanup);

    while (true) {
        peer_addr_size = sizeof(struct sockaddr_un);
        cfd = accept(sfd, (struct sockaddr *) &peer_addr,
                     &peer_addr_size);
        if (cfd < 0) {
            std::cerr<<"accept() error: "<<strerror(errno)<<std::endl;
            break;
        }

        cout<<"New client"<<endl;

        FdStream str(cfd);
        ret = HandleRpcFromStream(cholder, str.ist, str.ost);
    }

    cleanup(SIGINT);

    return ret;
}
