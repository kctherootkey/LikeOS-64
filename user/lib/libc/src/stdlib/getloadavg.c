/*
 * getloadavg - retrieve the system load averages.
 *
 * The BSD interface, also provided by glibc, and the one programs reach for
 * first when they want load figures: xload prefers it over every other backend
 * in its get_load.c, and only falls back to reading /proc/loadavg or poking
 * /dev/kmem when it is missing.
 *
 * The kernel already maintains the three exponentially-weighted averages and
 * reports them through sysinfo(2) as fixed-point values scaled by 1 << 16, so
 * this is a conversion rather than a new mechanism.
 */
#include <stdlib.h>
#include <errno.h>
#include <sys/sysinfo.h>

/* The scale the kernel reports loads[] in (LOADAVG_FSHIFT in the scheduler).
 * Kept as a shift rather than a literal so the relationship to the kernel's
 * fixed-point format stays visible. */
#define LOADAVG_FSHIFT 16
#define LOADAVG_SCALE ((double)(1UL << LOADAVG_FSHIFT))

int getloadavg(double loadavg[], int nelem)
{
	struct sysinfo si;
	int i;

	/* A negative count is a caller bug; zero is a no-op that trivially
	 * succeeds, which is what glibc does (its fill loop simply does not
	 * run).  Both are decided before the syscall so a pointless one is
	 * never made. */
	if (nelem < 0) {
		errno = EINVAL;
		return -1;
	}
	if (nelem == 0)
		return 0;
	if (loadavg == NULL) {
		errno = EFAULT;
		return -1;
	}

	/* Only three intervals exist (1, 5 and 15 minutes).  Asking for more is
	 * not an error: the documented contract is that the RETURN VALUE says
	 * how many were actually stored, so callers that pass a larger array
	 * still behave correctly. */
	if (nelem > 3)
		nelem = 3;

	if (sysinfo(&si) != 0)
		return -1; /* sysinfo set errno */

	for (i = 0; i < nelem; i++)
		loadavg[i] = (double)si.loads[i] / LOADAVG_SCALE;

	return nelem;
}
