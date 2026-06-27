/* openssltest.c - TLS connection stress test
 *
 * Repeatedly runs: openssl s_client -crlf -connect www.google.com:443
 * Detects "Verify return code: 20" in the output (TLS handshake complete),
 * waits KILL_DELAY_MS milliseconds, kills the child, and loops.
 * Exits after 10 minutes and prints a summary.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <fcntl.h>

#define RUN_DURATION (10 * 60) /* total runtime: 10 minutes */
#define KILL_DELAY_MS 200 /* wait after detection before SIGTERM */
#define DETECT_LINE "Verify return code: 20"
#define OPENSSL_PATH "/bin/openssl"
#define TARGET "www.google.com:443"

static long monotonic_sec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec;
}

int main(void)
{
	long start = monotonic_sec();
	int iteration = 0, connected = 0, failed = 0;

	printf("=== openssltest: TLS handshake stress test ===\n");
	printf("Duration : 10 minutes\n");
	printf("Target   : %s\n", TARGET);
	printf("Detect   : \"%s\"\n\n", DETECT_LINE);
	fflush(NULL);

	while (1) {
		long elapsed = monotonic_sec() - start;
		if (elapsed >= RUN_DURATION) {
			printf("\n=== 10 minutes elapsed. Test complete. ===\n");
			break;
		}

		iteration++;
		printf("[%d] t=%lds  Connecting to %s ...\n", iteration,
		       elapsed, TARGET);
		fflush(NULL);

		/* Pipe: parent reads child's stdout+stderr */
		int pfd[2];
		if (pipe(pfd) < 0) {
			printf("[%d] pipe() failed\n", iteration);
			failed++;
			continue;
		}

		pid_t pid = fork();
		if (pid < 0) {
			printf("[%d] fork() failed\n", iteration);
			close(pfd[0]);
			close(pfd[1]);
			failed++;
			continue;
		}

		if (pid == 0) {
			/* ---- child ---- */
			close(pfd[0]);

			/* stdin  → /dev/null (causes openssl to exit after handshake) */
			int null_fd = open("/dev/null", O_RDONLY);
			if (null_fd >= 0) {
				dup2(null_fd, STDIN_FILENO);
				close(null_fd);
			}

			/* stdout + stderr → pipe write end */
			dup2(pfd[1], STDOUT_FILENO);
			dup2(pfd[1], STDERR_FILENO);
			close(pfd[1]);

			char *argv[] = { "openssl",  "s_client", "-crlf",
					 "-connect", TARGET,     NULL };
			execvp(OPENSSL_PATH, argv);
			_exit(127);
		}

		/* ---- parent ---- */
		close(pfd[1]);

		/* Read child output line-by-line, detect the target string */
		char buf[256];
		char line[512];
		int llen = 0;
		int detected = 0;
		ssize_t n;

		while (!detected) {
			/* Honor the global 10-minute deadline inside the read loop */
			if (monotonic_sec() - start >= RUN_DURATION)
				break;

			n = read(pfd[0], buf, sizeof(buf));
			if (n <= 0)
				break;

			for (ssize_t i = 0; i < n; i++) {
				char c = buf[i];

				/* Skip bare CR */
				if (c == '\r')
					continue;

				if (c == '\n') {
					if (llen > 0) {
						line[llen] = '\0';
						printf("  %s\n", line);
						fflush(NULL);

						if (strstr(line, DETECT_LINE))
							detected = 1;

						llen = 0;
					}
					if (detected)
						break;
				} else {
					if (llen < (int)(sizeof(line) - 1))
						line[llen++] = c;
				}
			}
		}

		close(pfd[0]);

		if (detected) {
			connected++;
			printf("[%d] Connected! Waiting %dms then sending SIGTERM.\n",
			       iteration, KILL_DELAY_MS);
			fflush(NULL);

			struct timespec ts = { 0,
					       (long)KILL_DELAY_MS * 1000000L };
			nanosleep(&ts, NULL);
			kill(pid, SIGTERM);
		} else {
			failed++;
			printf("[%d] Connection not detected (failed or no output).\n",
			       iteration);
			fflush(NULL);
			kill(pid, SIGTERM);
		}

		/* Reap child */
		int status;
		waitpid(pid, &status, 0);

		if (WIFEXITED(status))
			printf("[%d] child exited %d\n", iteration,
			       WEXITSTATUS(status));
		else if (WIFSIGNALED(status))
			printf("[%d] child killed by signal %d\n", iteration,
			       WTERMSIG(status));
		fflush(NULL);
	}

	printf("\nIterations : %d\n", iteration);
	printf("Connected  : %d\n", connected);
	printf("Failed     : %d\n", failed);
	return 0;
}
