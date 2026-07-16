// teststress - stress test program that runs random commands in a loop
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>

#define DEFAULT_MINUTES 10
#define MAX_MINUTES 100000 // ~69 days; guards against overflow in minutes*60

// Simple linear congruential generator for random numbers
static unsigned int seed = 12345;

static unsigned int rand_simple(void)
{
	seed = seed * 1103515245 + 12345;
	return (seed >> 16) & 0x7FFF;
}

// Commands to run (use full paths since execve doesn't search PATH).
// Default mix — exercises libc/syscalls/memory (no network tests).
// teststress "all"     — also runs network_commands[] in addition to this.
// teststress "network" — runs only network_commands[].
static const char *commands[] = {
	"/bin/ls",
	"/usr/local/bin/testlibc",
	"/usr/local/bin/tests",
	"/usr/local/bin/testmem 100",
	"/usr/local/bin/hello",
	"/usr/local/bin/memstat",
	"/bin/cat /HELLO.TXT",
};

#define NUM_COMMANDS (sizeof(commands) / sizeof(commands[0]))

// Network-only command set, used both as the entire mix in `teststress
// network` and as an extension of the default mix in `teststress` (no arg).
static const char *network_commands[] = {
	"/usr/local/bin/testlibc network",
	"/ping -c 3 www.google.com",
	"/route",
	"/netstat",
	"/dig www.google.com",
	"/host www.google.com",
};

#define NUM_NETWORK_COMMANDS \
	(sizeof(network_commands) / sizeof(network_commands[0]))

// Try to execute with path search
static int try_exec(char *argv[])
{
	static char path_buf[256];
	const char *paths[] = { "", "/", "/bin/" };

	for (int i = 0; i < 3; i++) {
		// Build full path
		int j = 0;
		const char *p = paths[i];
		while (*p && j < 250)
			path_buf[j++] = *p++;
		p = argv[0];
		// Skip leading / in argv[0] if path already has one
		if (path_buf[j - 1] == '/' && *p == '/')
			p++;
		while (*p && j < 255)
			path_buf[j++] = *p++;
		path_buf[j] = '\0';

		// Try this path
		execve(path_buf, argv, NULL);
	}
	return -1; // All failed
}

// Parse command string into argv array
static int parse_command(const char *cmd, char *argv[], int max_args)
{
	static char buf[256];
	int argc = 0;
	int i = 0, j = 0;
	int in_word = 0;

	// Copy and parse
	while (cmd[i] && argc < max_args - 1) {
		if (cmd[i] == ' ' || cmd[i] == '\t') {
			if (in_word) {
				buf[j++] = '\0';
				in_word = 0;
			}
		} else {
			if (!in_word) {
				argv[argc++] = &buf[j];
				in_word = 1;
			}
			buf[j++] = cmd[i];
		}
		i++;
	}
	if (in_word) {
		buf[j] = '\0';
	}
	argv[argc] = NULL;
	return argc;
}

static void usage(const char *prog)
{
	printf("usage: %s [-t minutes] [all|network]\n", prog);
	printf("  -t minutes  run for this many minutes (default %d)\n",
	       DEFAULT_MINUTES);
	printf("  all         default command mix plus network commands\n");
	printf("  network     network commands only\n");
	printf("  (no mode)   default command mix, no network commands\n");
}

int main(int argc, char **argv)
{
	// Three modes:
	//   teststress              — default mix only (no network commands;
	//                             testlibc is run WITHOUT "network").
	//   teststress all          — default mix + network commands.
	//   teststress network      — only network commands (testlibc "network").
	// Each optionally preceded by -t <minutes> to override the run length.
	int network_only = 0;
	int run_all = 0;
	long minutes = DEFAULT_MINUTES;

	for (int i = 1; i < argc; i++) {
		if (!argv[i])
			continue;
		if (strcmp(argv[i], "-t") == 0) {
			if (i + 1 >= argc) {
				printf("%s: -t requires a value in minutes\n",
				       argv[0]);
				usage(argv[0]);
				return 2;
			}
			char *end = NULL;
			minutes = strtol(argv[++i], &end, 10);
			// Reject trailing garbage ("30x"), empty strings and
			// out-of-range values rather than silently running for
			// some other length than asked for.
			if (!end || *end != '\0' || minutes <= 0 ||
			    minutes > MAX_MINUTES) {
				printf("%s: invalid duration '%s' (expected 1..%d minutes)\n",
				       argv[0], argv[i], MAX_MINUTES);
				return 2;
			}
			continue;
		}
		if (strcmp(argv[i], "network") == 0) {
			network_only = 1;
			continue;
		}
		if (strcmp(argv[i], "all") == 0) {
			run_all = 1;
			continue;
		}
		// Reject unknown arguments instead of ignoring them: a typo'd
		// flag would otherwise silently run the default mix for the
		// default duration, which looks like a passing test.
		printf("%s: unknown argument '%s'\n", argv[0], argv[i]);
		usage(argv[0]);
		return 2;
	}

	const time_t timeout_seconds = (time_t)minutes * 60;

	// Build the active command pool.
	const char *pool[NUM_COMMANDS + NUM_NETWORK_COMMANDS];
	int pool_count = 0;
	if (network_only) {
		for (unsigned i = 0; i < NUM_NETWORK_COMMANDS; i++)
			pool[pool_count++] = network_commands[i];
	} else if (run_all) {
		for (unsigned i = 0; i < NUM_COMMANDS; i++)
			pool[pool_count++] = commands[i];
		for (unsigned i = 0; i < NUM_NETWORK_COMMANDS; i++)
			pool[pool_count++] = network_commands[i];
	} else {
		// Default: commands[] only, no network.
		for (unsigned i = 0; i < NUM_COMMANDS; i++)
			pool[pool_count++] = commands[i];
	}

	int iteration = 0;
	time_t start_time = time(NULL);

	printf("=== STRESS TEST STARTED ===\n");
	printf("Mode: %s\n", network_only ? "network only" :
			     run_all      ? "default + network" :
					    "default only");
	printf("Running random commands for up to %ld minute%s...\n", minutes,
	       minutes == 1 ? "" : "s");
	printf("Press Ctrl+C to stop early\n\n");

	// Use pid as part of seed for some variation
	seed = (unsigned int)getpid() * 31337;

	while (1) {
		// Check if the requested run length has elapsed
		time_t now = time(NULL);
		if (now - start_time >= timeout_seconds) {
			printf("\n=== %ld MINUTE%s ELAPSED - STRESS TEST COMPLETE ===\n",
			       minutes, minutes == 1 ? "" : "S");
			printf("Total iterations: %d\n", iteration);
			break;
		}

		// Pick a random command
		int cmd_idx = rand_simple() % pool_count;
		const char *cmd = pool[cmd_idx];

		iteration++;
		printf("[%d] Running: %s\n", iteration, cmd);

		// Fork and exec
		int pid = fork();
		if (pid < 0) {
			printf("fork failed!\n");
			// Wait a bit and retry
			for (volatile int i = 0; i < 1000000; i++)
				;
			continue;
		}

		if (pid == 0) {
			// Child process
			char *child_argv[16];
			parse_command(cmd, child_argv, 16);

			if (child_argv[0]) {
				try_exec(child_argv);
				// If try_exec returns, all paths failed
				printf("execve failed for: %s\n",
				       child_argv[0]);
			}
			exit(1);
		}

		// Parent: wait for child and check exit status
		int status;
		waitpid(pid, &status, 0);

		// Check if child exited normally
		if (WIFEXITED(status)) {
			int exit_code = WEXITSTATUS(status);
			if (exit_code != 0) {
				printf("[FAIL] Command '%s' exited with code %d\n",
				       cmd, exit_code);
				printf("=== STRESS TEST FAILED after %d iterations ===\n",
				       iteration);
				return 1;
			}
		} else if (WIFSIGNALED(status)) {
			int sig = WTERMSIG(status);
			printf("[FAIL] Command '%s' killed by signal %d\n", cmd,
			       sig);
			printf("=== STRESS TEST FAILED after %d iterations ===\n",
			       iteration);
			return 1;
		}

		// Small delay between commands
		for (volatile int i = 0; i < 100000; i++)
			;
	}

	printf("=== STRESS TEST PASSED ===\n");
	return 0;
}
