#ifndef VIRELAI_TIME_H
#define VIRELAI_TIME_H

#include <stddef.h>

typedef long time_t;
typedef long clock_t;

struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year;
    int tm_wday, tm_yday, tm_isdst;
};

#define CLOCKS_PER_SEC 1000000

time_t  time(time_t *t);
clock_t clock(void);
double  difftime(time_t a, time_t b);

#endif
