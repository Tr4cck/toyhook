#ifndef _ANDROID_LOG_H_MOCK
#define _ANDROID_LOG_H_MOCK

#define ANDROID_LOG_DEBUG 3
#define ANDROID_LOG_ERROR 6
#define ANDROID_LOG_INFO  4
#define ANDROID_LOG_WARN  5

static inline int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    (void)prio; (void)tag; (void)fmt;
    return 0;
}

#endif
