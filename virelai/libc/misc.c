/* virelai/libc/misc.c — unistd/stat/time glue over the vl seam. */

#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <stdio.h>

#include "../sys.h"

int isatty(int fd) { (void)fd; return 0; }

int unlink(const char *path) { return remove(path); }

int access(const char *path, int mode)
{
    (void)mode;
    int64_t h = vl_file_open(path, strlen(path), VL_O_READ);
    if (h < 0) { errno = ENOENT; return -1; }
    vl_file_close((uint32_t)h);
    return 0;
}

int mkdir(const char *path, mode_t mode)
{
    (void)path;
    (void)mode;
    /* The mkdir seam is the slot-79 pinned-directory op — heavier than the
     * skeleton card needs; config/save dirs degrade to cwd. */
    errno = ENOSYS;
    return -1;
}

int stat(const char *path, struct stat *st)
{
    int64_t h = vl_file_open(path, strlen(path), VL_O_READ);
    if (h < 0) { errno = ENOENT; return -1; }
    vl_file_close((uint32_t)h);
    if (st) {
        st->st_size = -1;
        st->st_mode = S_IFREG;
    }
    return 0;
}

time_t time(time_t *t)
{
    int64_t r = vl_time();
    time_t v = r < 0 ? (time_t)0 : (time_t)r;
    if (t) *t = v;
    return v;
}

clock_t clock(void) { return 0; }
double difftime(time_t a, time_t b) { return (double)(a - b); }

int gettimeofday(struct timeval *tv, struct timezone *tz)
{
    (void)tz;
    if (tv) {
        tv->tv_sec = time(NULL);
        tv->tv_usec = 0;
    }
    return 0;
}

unsigned int sleep(unsigned int seconds)
{
    vl_sleep_ticks(seconds);
    return 0;
}

int usleep(unsigned long usec)
{
    /* Scheduler ticks are 1 s granularity; sub-second waits are the
     * caller's job (DG_SleepMs busy-waits on the hardware counter). */
    (void)usec;
    return 0;
}

sig_t signal(int sig, sig_t handler)
{
    (void)sig;
    (void)handler;
    return SIG_DFL;
}

int raise(int sig)
{
    (void)sig;
    return 0;
}
