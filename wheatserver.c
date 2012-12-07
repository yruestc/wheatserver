#include "wheatserver.h"

struct globalServer Server;

void initGlobalServerConfig()
{
    Server.bind_addr = NULL;
    Server.port = WHEAT_SERVERPORT;
    Server.ipfd = 0;
    Server.logfile = NULL;
    Server.verbose = WHEAT_VERBOSE;
    Server.worker_number = 2;
    Server.graceful_timeout = WHEATSERVER_GRACEFUL_TIME;
    Server.idle_timeout = WHEATSERVER_IDLE_TIME;
    initHookCenter();
    Server.workers = createList();
    Server.pid = getpid();
    Server.relaunch_pid = 0;
    memcpy(Server.master_name, "master", 7);
    Server.configfile_path[0] = '\0';
    initMasterSignals();
    nonBlockCloseOnExecPipe(&Server.pipe_readfd, &Server.pipe_writefd);
}


void initServer()
{
    if (Server.port != 0) {
        Server.ipfd = wheatTcpServer(Server.neterr, Server.bind_addr, Server.port);
        if (Server.ipfd == NET_WRONG || Server.ipfd < 0) {
            wheatLog(WHEAT_WARNING, "Setup tcp server failed port: %d wrong: %s", Server.port, Server.neterr);
            halt(1);
        }
        wheatLog(WHEAT_NOTICE, "Server is listen port %d", Server.port);
    }
}

/* Worker Manage Function */

static void removeWorker(struct workerProcess *worker)
{
    struct listNode *worker_node = searchListKey(Server.workers, worker);
    if (worker_node)
        removeListNode(Server.workers, worker_node);
}

void wakeUp()
{
    int n;
    n = write(Server.pipe_writefd, ".", 1);
    if (n <= 0) {
        if (errno == EAGAIN || errno == EINTR)
            return ;
        wheatLog(WHEAT_WARNING, "wakeUp() can't write 1 byte to pipe: %s", strerror(errno));
        halt(1);
    }
    assert(n == 1);
}

void fakeSleep()
{
    struct timeval tvp;
    fd_set rset;
    tvp.tv_sec = 1;
    tvp.tv_usec = 0;

    FD_ZERO(&rset);
    FD_SET(Server.pipe_readfd, &rset);

    int ret = select(Server.pipe_readfd+1, &rset, NULL, NULL, &tvp);
    if (ret == 0)
        return ;
    else if (ret == -1) {
        if (errno == EAGAIN || errno == EINTR)
            return ;
        wheatLog(WHEAT_WARNING, "fakeSleep() select failed: %s", strerror(errno));
        halt(1);
    } else {
        int n;
        char buf[2];
        while ((n = read(Server.pipe_readfd, buf, 1)) <= 0){
            if (errno == EAGAIN || errno == EINTR)
                continue;
            wheatLog(WHEAT_WARNING, "fakeSleep() read failed: %s", strerror(errno));
            halt(1);
        }
        assert(buf[0] == '.');
    }
}

void adjustWorkerNumber()
{
    while (listLength(Server.workers) < Server.worker_number)
        spawnWorker("SyncWorker");
    while (listLength(Server.workers) > Server.worker_number) {
        /* worker is append to Server.workers, so first worker must be alive longer */
        struct listNode *max_age_worker = listFirst(Server.workers);
        killWorker(listNodeValue(max_age_worker), SIGQUIT);
    }
}

void spawnWorker(char *worker_name)
{
    pid_t pid;
    struct workerProcess *new_worker = malloc(sizeof(struct workerProcess));
    if (new_worker == NULL) {
        wheatLog(WHEAT_WARNING, "spawn new worker failed: %s", strerror(errno));
        halt(1);
    }

#ifdef WHEAT_DEBUG_WORKER
    pid = 0;
#else
    pid = fork();
#endif
    if (pid != 0) {
        new_worker->pid = pid;
        appendToListTail(Server.workers, new_worker);
        return ;
    } else {
        WorkerProcess = new_worker;
        wheatLog(WHEAT_NOTICE, "new worker spawned %d", getpid());
        initWorkerProcess(worker_name);
        wheatLog(WHEAT_NOTICE, "worker exit pid:%d", getpid());
        exit(0);
    }
}

void killAllWorkers(int sig)
{
    struct listIterator *iter = listGetIterator(Server.workers, START_HEAD);
    struct listNode *current;
    while ((current = listNext(iter)) != NULL) {
        struct workerProcess *w = current->value;
        killWorker(w, sig);
    }
    freeListIterator(iter);
}

void killWorker(struct workerProcess *worker, int sig)
{
    int result;
    if ((result = kill(worker->pid, sig)) != 0) {
        if (errno == ESRCH) {
            removeWorker(worker);
            return;
        }
        wheatLog(WHEAT_NOTICE, "kill %d pid failed: %s", strerror(errno));
    }
}

/* reap worker avoid zombie */
void reapWorkers()
{
    int status, result;
    pid_t pid;

    result = waitpid(-1, &status, WNOHANG);
    if (result == 0)
        return ;
    else if (result == -1) {
        if (errno == ECHILD)
            return ;
        else {
            wheatLog(WHEAT_NOTICE, "reapWorkers() waitpid failed: %s", strerror(errno));
        }
    } else {
        if (Server.relaunch_pid == result)
            return ;

        int exitcode = WEXITSTATUS(status);
        int bysignal = 0;

        if (WIFSIGNALED(status))
            bysignal = WTERMSIG(status);
        if (bysignal && exitcode != 0) {
            wheatLog(WHEAT_NOTICE, "reapWorkers() exitcode: %d  signal: %d", exitcode, bysignal);
        }
        if (exitcode == WORKER_BOOT_ERROR) {
            wheatLog(WHEAT_WARNING, "Worker failed to boot.");
            halt(1);
        }
    }
}

/* reload do some works below:
 * 1. reload config file
 * 2. spawn new workers
 * 3. kill old wokers */
void reload()
{
    char *old_addr = Server.bind_addr;
    int old_port = Server.port;
    loadConfigFile(Server.configfile_path, NULL);
    if (strlen(Server.bind_addr) != strlen(old_addr) ||
            strncmp(Server.bind_addr, old_addr, strlen(old_addr)) ||
            old_port != Server.port) {
        close(Server.ipfd);
        if (Server.port != 0) {
            Server.ipfd = wheatTcpServer(Server.neterr, Server.bind_addr, Server.port);
            if (Server.ipfd == NET_WRONG || Server.ipfd < 0) {
                wheatLog(WHEAT_WARNING, "Setup tcp server failed port: %d wrong: %s", Server.port, Server.neterr);
                halt(1);
            }
            wheatLog(WHEAT_NOTICE, "Server is listen port %d", Server.port);
        }
    }

    int i;
    for (i = 0; i < Server.worker_number; i++) {
        spawnWorker("SyncWorker");
    }

    adjustWorkerNumber();
}

void reexec()
{
}

void halt(int exitcode)
{
    stopWorkers(1);
    wheatLog(WHEAT_NOTICE, "shutdown %s", Server.master_name);
    exit(exitcode);
}

void stopWorkers(int graceful)
{
    int sig;
    close(Server.ipfd);

    if (graceful)
        sig = SIGQUIT;
    else
        sig = SIGTERM;
    int seconds = time(NULL) + Server.graceful_timeout;
    while (seconds < time(NULL)) {
        killAllWorkers(sig);
        sleep(0.2);
        reapWorkers();
    }
    killAllWorkers(SIGKILL);
}

void run()
{
    adjustWorkerNumber();
    while (1) {
        int *sig;

        reapWorkers();
        if (listLength(Server.signal_queue) == 0) {
            fakeSleep();
            adjustWorkerNumber();
            continue;
        }
        struct listNode *node = listFirst(Server.signal_queue);
        sig = listNodeValue(node);
        signalGenericHandle(*sig);
        removeListNode(Server.signal_queue, node);
        wakeUp();
    }
}

int main(int argc, const char *argv[])
{
    initGlobalServerConfig();

    if (argc >= 2) {
        int j = 1;
        wstr options = wstrEmpty();
        if (argv[j][0] != '-' || argv[j][1] != '-')
            strncpy(Server.configfile_path, argv[j], WHEATSERVER_MAX_NAMELEN);
        while(j != argc) {
            if (argv[j][0] == '-' && argv[j][1] == '-') {
                if (wstrlen(options))
                    options = wstrCat(options,"\n");
                options = wstrCat(options,argv[j]+2);
                options = wstrCat(options," ");
            } else {
               options = wstrCat(options, argv[j]);
               options = wstrCat(options," ");
            }
            j++;
        }
        loadConfigFile(Server.configfile_path, options);
    } else {
        wheatLog(WHEAT_NOTICE, "No config file specified, use the default settings");
    }

    initServer();
    run();
//    for (;;)
//    {
//        char ip[46]; //http://stackoverflow.com/questions/1076714/max-length-for-client-ip-address
//        int port;
//        int clifd = wheatTcpAccept(Server.neterr, Server.ipfd, ip, &port);
//        write(clifd, "hello from wheatserver", 23);
//        close(clifd);
//    }
    return 0;
}