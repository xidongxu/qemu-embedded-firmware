/* nosys.c - minimal newlib syscall stubs so gcov's libc references link on
 * bare metal.  Only linked by board_test.py in --coverage mode.  Nothing here
 * is called on the happy path (the exporter streams .gcda over the UART). */
#include <sys/stat.h>
#include <errno.h>

void _exit(int s)
{
    (void)s;
    for (;;) {
    }
}

int _close(int fd) { (void)fd; return -1; }
int _open(const char *p, int f, ...) { (void)p; (void)f; return -1; }
int _unlink(const char *p) { (void)p; return -1; }
int _lseek(int fd, int off, int whence) { (void)fd; (void)off; (void)whence; return -1; }
int _read(int fd, char *buf, int n) { (void)fd; (void)buf; (void)n; return 0; }
int _write(int fd, const char *buf, int n) { (void)fd; (void)buf; (void)n; return -1; }

static char s_heap[16384];
static int  s_heap_used = 0;

void *_sbrk(int incr)
{
    if (s_heap_used + incr > (int)sizeof(s_heap)) {
        errno = ENOMEM;
        return (void *)-1;
    }
    void *p = s_heap + s_heap_used;
    s_heap_used += incr;
    return p;
}

int _fstat(int fd, struct stat *st) { (void)fd; st->st_mode = 0; return 0; }
int _isatty(int fd) { (void)fd; return 1; }
int _kill(int pid, int sig) { (void)pid; (void)sig; return -1; }
int _getpid(void) { return 1; }
