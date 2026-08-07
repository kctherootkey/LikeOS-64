/*
 * kdump -- ask the kernel to print its diagnostic tables.
 *
 * Emits the TCP connection table, the AF_UNIX socket table, the PTY table and
 * the scheduler's task list, through SYS_DEBUG_DUMP.  Root only, because the
 * tables name every process and every connection on the system.
 *
 * The same tables are on the Ctrl+D and Ctrl+N debug hotkeys, but those need a
 * keypress on the machine's own keyboard and go through the interrupt handler.
 * This does not: it works from any shell, including one over ssh, at exactly
 * the moment you choose -- which is what you want when something has hung and
 * you need to see what it is waiting on before touching it.
 *
 * Output goes to the kernel log, so it appears on the console and can be read
 * back afterwards with dmesg.  Nothing is printed on this program's own stdout;
 * "kdump && dmesg | tail -60" is the usual invocation.
 *
 * The task list is the interesting part for a hang: each blocked task carries
 * the kernel address it went to sleep at, which is the same value ps reports in
 * its WCHAN column.  Resolve it on the build host with
 *
 *     rm build/kernel.elf && make NO_STRIP=1
 *     addr2line -f -e build/kernel.elf <address>
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

static void usage(FILE *out)
{
	fprintf(out,
		"Usage: kdump\n"
		"\n"
		"Print the kernel's diagnostic tables (TCP, AF_UNIX, PTY,\n"
		"tasks) to the kernel log.  Must be run as root.\n"
		"\n"
		"The output is not written here -- read it with dmesg:\n"
		"    kdump && dmesg | tail -60\n");
}

int main(int argc, char **argv)
{
	if (argc > 1) {
		if (strcmp(argv[1], "--help") == 0 ||
		    strcmp(argv[1], "-h") == 0) {
			usage(stdout);
			return 0;
		}
		fprintf(stderr, "kdump: unexpected argument '%s'\n", argv[1]);
		usage(stderr);
		return 2;
	}

	if (debug_dump() != 0) {
		if (errno == EPERM)
			fprintf(stderr,
				"kdump: permission denied (must be root)\n");
		else
			fprintf(stderr, "kdump: %s\n", strerror(errno));
		return 1;
	}

	/* Say where it went: the tables are in the log, not on this stream, and
	 * a program that appears to do nothing invites being run again. */
	fprintf(stderr, "kdump: tables written to the kernel log"
			" (read them with: dmesg | tail -60)\n");
	return 0;
}
