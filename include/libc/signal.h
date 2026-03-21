#ifndef _AIOS_SIGNAL_H
#define _AIOS_SIGNAL_H

typedef volatile int sig_atomic_t;
typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

#define SIGINT  2
#define SIGTERM 15
#define SIGABRT 6

sighandler_t signal(int sig, sighandler_t handler);

#endif
