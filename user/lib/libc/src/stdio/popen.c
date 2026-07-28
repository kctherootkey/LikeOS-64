/*
 * popen() / pclose() — run a command through the shell with a pipe attached
 * to its standard input or output, exactly as POSIX specifies.
 *
 *   popen(cmd, "r")  parent reads the command's standard output
 *   popen(cmd, "w")  parent writes the command's standard input
 *
 * The child pid is remembered in a small table keyed by the stream so that
 * pclose() can reap the right process and return its exit status.  fds that
 * are themselves popen streams are marked close-on-exec so nested popen()
 * children do not inherit each other's pipe ends (the POSIX requirement).
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>

extern int fcntl(int fd, int cmd, ...);

/* stdin/stdout descriptor numbers without pulling in more headers. */
#define POPEN_STDIN_FILENO 0
#define POPEN_STDOUT_FILENO 1

/* Association between an open popen stream and its child pid. */
struct popen_node {
	FILE *fp;
	pid_t pid;
	struct popen_node *next;
};

static struct popen_node *popen_list;

FILE *popen(const char *command, const char *type)
{
	if (command == NULL || type == NULL ||
	    (type[0] != 'r' && type[0] != 'w')) {
		errno = EINVAL;
		return NULL;
	}
	int read_mode = (type[0] == 'r');

	struct popen_node *node = malloc(sizeof(*node));
	if (node == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	int pipefd[2];
	if (pipe(pipefd) < 0) {
		free(node);
		return NULL;
	}
	/* parent_end is the descriptor the caller keeps; child_end is handed
	 * to the shell as its stdin (write mode) or stdout (read mode). */
	int parent_end = read_mode ? pipefd[0] : pipefd[1];
	int child_end = read_mode ? pipefd[1] : pipefd[0];

	pid_t pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		free(node);
		return NULL;
	}

	if (pid == 0) {
		/* Child: wire child_end onto the right standard descriptor,
		 * then close every parent-side popen fd so a command like
		 * `sort | uniq` invoked through nested popen() does not keep a
		 * sibling's pipe open (which would prevent EOF). */
		int target = read_mode ? POPEN_STDOUT_FILENO : POPEN_STDIN_FILENO;
		if (child_end != target) {
			if (dup2(child_end, target) < 0)
				_exit(127);
			close(child_end);
		}
		close(parent_end);
		for (struct popen_node *p = popen_list; p != NULL; p = p->next)
			close(fileno(p->fp));
		execl("/bin/sh", "sh", "-c", command, (char *)NULL);
		_exit(127);
	}

	/* Parent: keep parent_end, drop child_end, wrap in a FILE. */
	close(child_end);
	FILE *fp = fdopen(parent_end, read_mode ? "r" : "w");
	if (fp == NULL) {
		int saved = errno;
		close(parent_end);
		/* Reap the child we can no longer talk to. */
		int st;
		waitpid(pid, &st, 0);
		free(node);
		errno = saved;
		return NULL;
	}

	/* A popen stream must not survive across the caller's own exec. */
	fcntl(parent_end, F_SETFD, FD_CLOEXEC);

	node->fp = fp;
	node->pid = pid;
	node->next = popen_list;
	popen_list = node;
	return fp;
}

int pclose(FILE *stream)
{
	struct popen_node **pp = &popen_list;
	while (*pp != NULL && (*pp)->fp != stream)
		pp = &(*pp)->next;
	if (*pp == NULL) {
		/* Not a stream we opened. */
		errno = EINVAL;
		return -1;
	}

	struct popen_node *node = *pp;
	*pp = node->next;
	pid_t pid = node->pid;
	free(node);

	/* Closing the stream sends EOF / SIGPIPE to the child as appropriate,
	 * then we wait for it and report its status like the shell would. */
	fclose(stream);

	int status = 0;
	pid_t r;
	do {
		r = waitpid(pid, &status, 0);
	} while (r < 0 && errno == EINTR);
	if (r < 0)
		return -1;
	return status;
}
