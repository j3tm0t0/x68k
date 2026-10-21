#ifndef _X68K_SYSLOG_H_
#define _X68K_SYSLOG_H_
/* syslog は無い。全部 no-op。 */
#define LOG_EMERG 0
#define LOG_ALERT 1
#define LOG_CRIT 2
#define LOG_ERR 3
#define LOG_WARNING 4
#define LOG_NOTICE 5
#define LOG_INFO 6
#define LOG_DEBUG 7
#define LOG_PID 0
#define LOG_AUTHPRIV 0
#define LOG_USER 0
#define LOG_DAEMON 0
static inline void openlog(const char *a, int b, int c) { (void)a;(void)b;(void)c; }
static inline void closelog(void) {}
static inline void syslog(int a, const char *b, ...) { (void)a;(void)b; }
#endif
