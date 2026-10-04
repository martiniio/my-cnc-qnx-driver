/*
 * cnc_log.h - timestamped log lines on stderr.
 *
 *   CNC_LOG("INFO", "link up (%s)", url);
 *   -> 2026-10-02 14:03:11.482 INFO  link up (opc.tcp://...)
 */
#ifndef CNC_LOG_H
#define CNC_LOG_H

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static inline void cnc_log(const char *level, const char *fmt, ...)
{
    struct timespec ts;
    struct tm       tm;
    char            when[32];
    va_list         ap;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);

    flockfile(stderr);                     /* one line at a time from many threads */
    fprintf(stderr, "%s.%03ld %-5s ", when, ts.tv_nsec / 1000000L, level);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    funlockfile(stderr);
}

#define CNC_LOG(level, ...) cnc_log(level, __VA_ARGS__)

#endif
