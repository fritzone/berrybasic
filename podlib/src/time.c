// time.c - the calendar half of <time.h>: convert a time_t (seconds) to and from
// a broken-down struct tm, and format it. The epoch here is whatever time()
// counts from (seconds since boot until the machine gains a real clock), so
// these are exact relative to that; difftime and elapsed-time formatting work
// today, wall-clock dates once a clock source is set. No pointer tables, so it
// links into a seed. Civil<->days uses Howard Hinnant's algorithm.
#include <time.h>
#include <stdio.h>

static const char wdays[] = "SunMonTueWedThuFriSat";
static const char mons[]  = "JanFebMarAprMayJunJulAugSepOctNovDec";
static const char wfull[] = "Sunday\0Monday\0Tuesday\0Wednesday\0Thursday\0Friday\0Saturday";
static const char mfull[] = "January\0February\0March\0April\0May\0June\0July\0"
                            "August\0September\0October\0November\0December";
static const char *nth_str(const char *p, int n) { while (n--) { while (*p) p++; p++; } return p; }

struct tm *gmtime_r(const time_t *tp, struct tm *r) {
    long t = *tp;
    long days = t / 86400, secs = t % 86400;
    if (secs < 0) { secs += 86400; days--; }
    r->tm_hour = (int)(secs / 3600);
    r->tm_min  = (int)((secs / 60) % 60);
    r->tm_sec  = (int)(secs % 60);
    r->tm_wday = (int)(((days % 7) + 4 + 7000000L) % 7);     // 1970-01-01 was a Thursday
    long z = days + 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);
    r->tm_year = (int)(y - 1900);
    r->tm_mon  = (int)m - 1;
    r->tm_mday = (int)d;
    int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    static const int cum[] = { 0,31,59,90,120,151,181,212,243,273,304,334 };
    r->tm_yday = cum[r->tm_mon] + (r->tm_mon > 1 && leap ? 1 : 0) + (int)d - 1;
    r->tm_isdst = 0;
    return r;
}

static struct tm g_tm;
struct tm *gmtime(const time_t *t)    { return gmtime_r(t, &g_tm); }
struct tm *localtime(const time_t *t) { return gmtime_r(t, &g_tm); }   // no timezone: local == UTC
struct tm *localtime_r(const time_t *t, struct tm *r) { return gmtime_r(t, r); }

time_t mktime(struct tm *tm) {
    int y = tm->tm_year + 1900, m = tm->tm_mon + 1, d = tm->tm_mday;
    y -= (m <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (unsigned)(m > 2 ? m - 3 : m + 9) + 2) / 5 + (unsigned)d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097 + (long)doe - 719468;
    time_t t = days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
    gmtime_r(&t, tm);                                        // normalise wday/yday/...
    return t;
}

double difftime(time_t a, time_t b) { return (double)(a - b); }

size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm) {
    size_t n = 0;
    #define PUT(c) do { if (n + 1 < max) s[n] = (c); n++; } while (0)
    for (; *fmt; fmt++) {
        if (*fmt != '%') { PUT(*fmt); continue; }
        fmt++;
        char buf[32]; int len = 0;
        switch (*fmt) {
            case 'Y': len = snprintf(buf, sizeof buf, "%d", tm->tm_year + 1900); break;
            case 'y': len = snprintf(buf, sizeof buf, "%02d", (tm->tm_year + 1900) % 100); break;
            case 'm': len = snprintf(buf, sizeof buf, "%02d", tm->tm_mon + 1); break;
            case 'd': len = snprintf(buf, sizeof buf, "%02d", tm->tm_mday); break;
            case 'e': len = snprintf(buf, sizeof buf, "%2d", tm->tm_mday); break;
            case 'H': len = snprintf(buf, sizeof buf, "%02d", tm->tm_hour); break;
            case 'I': len = snprintf(buf, sizeof buf, "%02d", (tm->tm_hour % 12) ? tm->tm_hour % 12 : 12); break;
            case 'M': len = snprintf(buf, sizeof buf, "%02d", tm->tm_min); break;
            case 'S': len = snprintf(buf, sizeof buf, "%02d", tm->tm_sec); break;
            case 'j': len = snprintf(buf, sizeof buf, "%03d", tm->tm_yday + 1); break;
            case 'w': len = snprintf(buf, sizeof buf, "%d", tm->tm_wday); break;
            case 'p': len = snprintf(buf, sizeof buf, "%s", tm->tm_hour < 12 ? "AM" : "PM"); break;
            case 'a': len = snprintf(buf, sizeof buf, "%.3s", wdays + 3 * (tm->tm_wday & 7)); break;
            case 'A': len = snprintf(buf, sizeof buf, "%s", nth_str(wfull, tm->tm_wday)); break;
            case 'b':
            case 'h': len = snprintf(buf, sizeof buf, "%.3s", mons + 3 * (tm->tm_mon % 12)); break;
            case 'B': len = snprintf(buf, sizeof buf, "%s", nth_str(mfull, tm->tm_mon)); break;
            case 'T': len = snprintf(buf, sizeof buf, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); break;
            case 'F': len = snprintf(buf, sizeof buf, "%04d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday); break;
            case '%': buf[0] = '%'; len = 1; break;
            case 0:   fmt--; buf[0] = 0; len = 0; break;
            default:  buf[0] = '%'; buf[1] = *fmt; len = 2; break;
        }
        for (int i = 0; i < len; i++) PUT(buf[i]);
    }
    if (max) s[n < max ? n : max - 1] = 0;
    #undef PUT
    return n;
}

char *asctime_r(const struct tm *tm, char *buf) {
    snprintf(buf, 26, "%.3s %.3s %2d %02d:%02d:%02d %d\n",
             wdays + 3 * (tm->tm_wday & 7), mons + 3 * (tm->tm_mon % 12),
             tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year + 1900);
    return buf;
}
static char g_asc[32];
char *asctime(const struct tm *tm) { return asctime_r(tm, g_asc); }
char *ctime(const time_t *t)       { return asctime(gmtime(t)); }
char *ctime_r(const time_t *t, char *buf) { struct tm tmp; return asctime_r(gmtime_r(t, &tmp), buf); }
