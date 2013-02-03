#include "wheatserver.h"

void dispatchRequest(int fd, char *ip, int port)
{
    struct protocol *ptcol = spotProtocol(ip, port, fd);
    struct app *application = spotAppInterface();
    struct client *c = initClient(fd, ip, port, ptcol, application);
    if (c == NULL)
        return ;
    int ret;
    do {
        int n = syncRecvData(fd, &c->buf);
        if (n == -1) {
            freeClient(c);
            return ;
        }
        ret = ptcol->parser(c);
        if (ret == -1) {
            freeClient(c);
            return ;
        }
    }while(ret != 1);
    ret = application->constructor(c);
    if (ret != 0) {
        wheatLog(WHEAT_WARNING, "app faileds");
    }
    syncSendData(fd, &c->res_buf);
    close(fd);
    freeClient(c);
}

void syncWorkerCron()
{
    if (wheatNonBlock(Server.neterr, Server.ipfd) == NET_WRONG) {
        wheatLog(WHEAT_WARNING, "Set nonblock %d failed: %s", Server.ipfd, Server.neterr);
        halt(1);
    }

    while (WorkerProcess->alive) {
        int fd, ret;

        char ip[46];
        int port;
        fd = wheatTcpAccept(Server.neterr, Server.ipfd, ip, &port);
        if (fd == NET_WRONG) {
            goto accepterror;
        }
        if ((ret = wheatNonBlock(Server.neterr, fd)) == NET_WRONG)
            goto accepterror;
        if ((ret = wheatCloseOnExec(Server.neterr, fd)) == NET_WRONG)
            goto accepterror;
        dispatchRequest(fd, ip, port);
        continue;
accepterror:
        if (errno != EAGAIN)
            wheatLog(WHEAT_NOTICE, "workerCron: %s", Server.neterr);
        if (WorkerProcess->ppid != getppid()) {
            wheatLog(WHEAT_NOTICE, "parent change, worker shutdown");
            return ;
        }
        struct timeval tvp;
        fd_set rset;
        tvp.tv_sec = Server.idle_timeout;
        tvp.tv_usec = 0;
        FD_ZERO(&rset);
        FD_SET(Server.ipfd, &rset);

        ret = select(Server.ipfd+1, &rset, NULL, NULL, &tvp);
        if (ret >= 0)
            continue;
        else {
            if (errno == EAGAIN || errno == EINTR)
                continue;
            wheatLog(WHEAT_WARNING, "workerCron() select failed: %s", strerror(errno));
            return ;
        }
    }
}

void setupSync()
{
}

int syncSendData(int fd, wstr *buf)
{
    return writeBulkTo(fd, buf);
}

int syncRecvData(int fd, wstr *buf)
{
    return readBulkFrom(fd, buf);
}