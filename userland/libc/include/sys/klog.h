#ifndef _SYS_KLOG_H
#define _SYS_KLOG_H

/* klogctl operations (same as syslog(2) type argument) */
#define SYSLOG_ACTION_READ       2
#define SYSLOG_ACTION_READ_ALL   3
#define SYSLOG_ACTION_READ_CLEAR 4
#define SYSLOG_ACTION_CLEAR      5
#define SYSLOG_ACTION_SIZE_BUFFER 10

int klogctl(int type, char *bufp, int len);

#endif /* _SYS_KLOG_H */
