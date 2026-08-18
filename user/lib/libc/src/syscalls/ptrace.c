/* ptrace(2) — process tracing.
 *
 * A thin pass-through: the kernel does all the interpreting, because what the
 * addr and data arguments mean depends on the request.  See <sys/ptrace.h> for
 * the interface and the access-control rules.
 */
#include <sys/ptrace.h>
#include <errno.h>
#include "syscall.h"

long ptrace(int request, pid_t pid, void *addr, long data)
{
	long ret = syscall4(SYS_PTRACE, (long)request, (long)pid, (long)addr, data);

	/* The PEEK requests return the word they read, and that word can
	 * legitimately be any value including -1, so there is no return value
	 * left over to mean "failed".  The kernel still reports errors the
	 * usual way -- a negative errno -- and a caller that needs to tell a
	 * read of -1 from a failure clears errno first and checks it after,
	 * which is what the header says to do.
	 *
	 * That does mean a successful PEEK of a word in the range
	 * [-4095, -1] is reported as an error here.  Nothing can be done about
	 * that at this layer: distinguishing them requires an out-parameter the
	 * interface does not have, and every system with this call has the same
	 * corner. */
	if (ret < 0 && ret > -4096) {
		errno = (int)-ret;
		return -1;
	}
	return ret;
}
