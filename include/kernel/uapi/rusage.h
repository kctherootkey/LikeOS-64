/* The rusage block reported by getrusage() and wait4(). */
#ifndef _KERNEL_UAPI_RUSAGE_H
#define _KERNEL_UAPI_RUSAGE_H

#include <kernel/uapi/types.h>

struct k_rusage_compat {
	int64_t ru_utime_sec;
	int64_t ru_utime_usec;
	int64_t ru_stime_sec;
	int64_t ru_stime_usec;
	int64_t ru_maxrss;
	int64_t ru_ixrss;
	int64_t ru_idrss;
	int64_t ru_isrss;
	int64_t ru_minflt;
	int64_t ru_majflt;
	int64_t ru_nswap;
	int64_t ru_inblock;
	int64_t ru_oublock;
	int64_t ru_msgsnd;
	int64_t ru_msgrcv;
	int64_t ru_nsignals;
	int64_t ru_nvcsw;
	int64_t ru_nivcsw;
};

/* If this fires, <sys/wait.h> and the struct above have drifted apart, and
 * whichever of them is larger is writing over the other one's memory. */
_Static_assert(sizeof(struct k_rusage_compat) == 144,
	       "struct rusage size must match user/lib/libc/include/sys/wait.h");

#endif /* _KERNEL_UAPI_RUSAGE_H */
