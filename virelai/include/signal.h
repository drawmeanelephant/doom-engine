#ifndef VIRELAI_SIGNAL_H
#define VIRELAI_SIGNAL_H

typedef void (*sig_t)(int);
#define SIG_DFL ((sig_t)0)
#define SIG_IGN ((sig_t)1)
#define SIG_ERR ((sig_t)-1)

#define SIGABRT 6
#define SIGSEGV 11
#define SIGTERM 15

sig_t signal(int sig, sig_t handler);
int   raise(int sig);

#endif
