/*
 * sock_select_lwip.c
 *
 * PJLIB socket select() abstraction backed by lwIP.
 *
 * Uses newlib's fd_set/FD_* macros (pulled in via <sys/select.h>, which
 * newlib's <sys/types.h> includes anyway) and lwIP's lwip_select().
 * We deliberately do NOT shadow <sys/select.h> with an lwIP compat header,
 * because newlib's own <sys/types.h> includes it and that broke type
 * ordering (u_int/sbintime_t undefined).
 */
#include <pj/sock_select.h>
#include <pj/assert.h>
#include <pj/errno.h>
#include <pj/os.h>

#include <sys/types.h>
#include <sys/select.h>   /* newlib fd_set / FD_* macros */
#include <sys/time.h>     /* struct timeval */
#include <lwip/sockets.h> /* lwip_select() */

#define PART_FDSET(ps)          ((fd_set *)&(ps)->data[1])
#define PART_COUNT(ps)          ((ps)->data[0])

PJ_DEF(void) PJ_FD_ZERO(pj_fd_set_t *fdsetp)
{
    pj_assert(sizeof(pj_fd_set_t) - sizeof(pj_sock_t) >= sizeof(fd_set));

    FD_ZERO(PART_FDSET(fdsetp));
    PART_COUNT(fdsetp) = 0;
}

PJ_DEF(void) PJ_FD_SET(pj_sock_t fd, pj_fd_set_t *fdsetp)
{
    pj_assert(sizeof(pj_fd_set_t) - sizeof(pj_sock_t) >= sizeof(fd_set));

    if (!PJ_FD_ISSET(fd, fdsetp))
        ++PART_COUNT(fdsetp);
    FD_SET(fd, PART_FDSET(fdsetp));
}

PJ_DEF(void) PJ_FD_CLR(pj_sock_t fd, pj_fd_set_t *fdsetp)
{
    pj_assert(sizeof(pj_fd_set_t) - sizeof(pj_sock_t) >= sizeof(fd_set));

    if (PJ_FD_ISSET(fd, fdsetp))
        --PART_COUNT(fdsetp);
    FD_CLR(fd, PART_FDSET(fdsetp));
}

PJ_DEF(pj_bool_t) PJ_FD_ISSET(pj_sock_t fd, const pj_fd_set_t *fdsetp)
{
    pj_assert(sizeof(pj_fd_set_t) - sizeof(pj_sock_t) >= sizeof(fd_set));

    return FD_ISSET(fd, PART_FDSET(fdsetp)) ? PJ_TRUE : PJ_FALSE;
}

PJ_DEF(pj_size_t) PJ_FD_COUNT(const pj_fd_set_t *fdsetp)
{
    return PART_COUNT(fdsetp);
}

PJ_DEF(int) pj_sock_select(int n,
                           pj_fd_set_t *readfds,
                           pj_fd_set_t *writefds,
                           pj_fd_set_t *exceptfds,
                           const pj_time_val *timeout)
{
    struct timeval tv, *ptv = NULL;
    fd_set *pr = NULL, *pw = NULL, *pe = NULL;

    if (timeout) {
        tv.tv_sec  = timeout->sec;
        tv.tv_usec = timeout->msec * 1000;
        ptv = &tv;
    }

    if (readfds)
        pr = PART_FDSET(readfds);
    if (writefds)
        pw = PART_FDSET(writefds);
    if (exceptfds)
        pe = PART_FDSET(exceptfds);

    return lwip_select(n, pr, pw, pe, ptv);
}
