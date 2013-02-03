#include "wheatserver.h"


/* because readBulkFrom call wstrMakeRoom realloc memory pointer,
 * pass a pointer point pointer is needed.
 * return -1 means `fd` occurs error or closed, it should be closed
 * return 0 means EAGAIN */
int readBulkFrom(int fd, wstr *clientbuf)
{
    ssize_t nread, readlen;
    int qblen;
    wstr buf = *clientbuf;

    readlen = WHEAT_IOBUF_LEN;

    qblen = wstrlen(buf);
    if (wstrfree(buf) * 2 > readlen)
        readlen = wstrfree(buf);
    buf = wstrMakeRoom(buf, readlen);

    // Important ! buf may changed
    *clientbuf = buf;

    nread = read(fd, buf+qblen, readlen);
    if (nread == -1) {
        if (errno == EAGAIN) {
            nread = 0;
        } else {
            wheatLog(WHEAT_VERBOSE, "Reading from fd %d: %s", fd, strerror(errno));
            return -1;
        }
    } else if (nread == 0) {
        return -1;
    }
    if (nread) {
        wstrupdatelen(buf, (int)(qblen+nread));
    }
    return (int)nread;
}

int writeBulkTo(int fd, wstr *clientbuf)
{
    wstr buf = *clientbuf;
    ssize_t bufpos = 0, nwritten = 0, totalwritten = 0;
    while(bufpos <= wstrlen(buf)) {
        nwritten = write(fd, buf+bufpos, wstrlen(buf)-bufpos);
        if (nwritten <= 0)
            break;
        bufpos += nwritten;
        totalwritten += nwritten;
    }
    if (nwritten == -1) {
        if (errno == EAGAIN) {
            nwritten = 0;
        } else {
            wheatLog(WHEAT_VERBOSE,
                "Error writing to client: %s", strerror(errno));
            return -1;
        }
    }
    return (int)totalwritten;
}