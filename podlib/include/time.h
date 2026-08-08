#ifndef _POD_TIME_H
#define _POD_TIME_H
#include <stddef.h>
typedef long time_t;
typedef long clock_t;
struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; };
time_t time(time_t *t);
struct tm *localtime(const time_t *t);
struct tm *localtime_r(const time_t *t, struct tm *r);
struct tm *gmtime(const time_t *t);
struct tm *gmtime_r(const time_t *t, struct tm *r);
time_t     mktime(struct tm *tm);
double     difftime(time_t a, time_t b);
size_t     strftime(char *s, size_t max, const char *fmt, const struct tm *tm);
char      *asctime(const struct tm *tm);
char      *asctime_r(const struct tm *tm, char *buf);
char      *ctime(const time_t *t);
char      *ctime_r(const time_t *t, char *buf);
clock_t clock(void);
#define CLOCKS_PER_SEC 100
#endif
