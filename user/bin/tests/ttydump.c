/* ttydump - show exactly what the terminal sends, and in what reads.
 *
 * Every attempt to answer "what bytes did the program actually receive"
 * with the tools already on this image failed for the same reason: they all
 * run in canonical mode.  The line discipline holds input in canon_buf until
 * a newline arrives, so anything without one -- a cursor-position report, a
 * bracketed-paste marker, a function key -- is invisible until something
 * else flushes the line, and an interrupt discards it entirely.  Piping a
 * canonical-mode reader through a hex dumper does not help: the bytes never
 * reach the reader in the first place.  There is no stty here to change that
 * from a shell, so it has to be a program that changes its own mode.
 *
 * This does that and nothing else.  It reports:
 *
 *   - every byte, in hex and as a printable rendering (^[ for escape, the
 *     character itself when it is one),
 *   - the READ BOUNDARIES, which is the other half of the question: a
 *     sequence that arrives whole but split across two reads is a completely
 *     different fault from one that arrives with bytes missing, and the two
 *     are indistinguishable from inside an editor.
 *
 * Usage:  ttydump [-p]      Ctrl+D twice in a row to quit.
 *
 *   -p   ask the terminal to bracket pastes first (what a full-screen
 *        editor does on startup), so a paste is framed by the markers that
 *        editor would see.  Off by default, because half the point is to
 *        compare the two.
 *
 * It also asks for a cursor-position report at startup: a terminal answers
 * that unprompted, so the first line of output tells you the return path
 * carries escape bytes at all before you test anything else with it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>

static struct termios g_saved;
static int g_saved_ok;

static void restore(void)
{
	if (g_saved_ok)
		tcsetattr(STDIN_FILENO, TCSANOW, &g_saved);
}

/* Render one byte the way a terminal log should: control characters as the
 * caret form a person can compare against a manual, everything else as
 * itself. */
static void render(unsigned char c, char *out, size_t n)
{
	if (c == 0x1B)
		snprintf(out, n, "^[");
	else if (c < 0x20)
		snprintf(out, n, "^%c", c + '@');
	else if (c == 0x7F)
		snprintf(out, n, "^?");
	else if (c < 0x7F)
		snprintf(out, n, "%c", c);
	else
		snprintf(out, n, ".");
}

int main(int argc, char **argv)
{
	struct termios raw;
	unsigned char buf[512];
	int bracket = (argc > 1 && strcmp(argv[1], "-p") == 0);
	int last_was_eot = 0;
	unsigned long n_reads = 0, n_bytes = 0;

	if (!isatty(STDIN_FILENO)) {
		fprintf(stderr, "ttydump: stdin is not a terminal\n");
		return 2;
	}
	if (tcgetattr(STDIN_FILENO, &g_saved) != 0) {
		perror("ttydump: tcgetattr");
		return 1;
	}
	g_saved_ok = 1;
	atexit(restore);

	/* Raw: no line buffering, no echo, no signal characters.  Ctrl+D is
	 * then an ordinary 0x04 byte and this program decides what it means,
	 * which is why quitting takes two of them -- one on its own has to
	 * stay visible in the dump, since it is exactly the kind of byte a
	 * paste might contain. */
	raw = g_saved;
	cfmakeraw(&raw);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
		perror("ttydump: tcsetattr");
		return 1;
	}

	printf("ttydump: raw mode, bracketed paste %s.\r\n",
	       bracket ? "ON" : "off");
	printf("         each line is one read(); Ctrl+D twice to quit.\r\n");
	/* A terminal answers this by itself.  If nothing comes back, the
	 * return path is not carrying escape bytes and there is no point
	 * testing anything more complicated. */
	printf("         asking for a cursor report -- a reply should follow.\r\n");
	fflush(stdout);
	write(STDOUT_FILENO, "\033[6n", 4);
	if (bracket)
		write(STDOUT_FILENO, "\033[?2004h", 8);

	for (;;) {
		ssize_t got = read(STDIN_FILENO, buf, sizeof(buf));
		char pretty[4];
		ssize_t i;

		if (got < 0) {
			if (errno == EINTR)
				continue;
			printf("read: %s\r\n", strerror(errno));
			break;
		}
		if (got == 0) {
			printf("read: EOF\r\n");
			break;
		}
		n_reads++;
		n_bytes += (unsigned long)got;

		printf("read %2ld:", (long)got);
		for (i = 0; i < got; i++)
			printf(" %02x", buf[i]);
		printf("   \"");
		for (i = 0; i < got; i++) {
			render(buf[i], pretty, sizeof(pretty));
			fputs(pretty, stdout);
		}
		printf("\"\r\n");
		fflush(stdout);

		/* Quit on two consecutive Ctrl+D, and only when the second is
		 * alone in its read -- so a paste that happens to contain one
		 * cannot end the session mid-dump. */
		if (got == 1 && buf[0] == 0x04) {
			if (last_was_eot)
				break;
			last_was_eot = 1;
		} else {
			last_was_eot = 0;
		}
	}

	if (bracket)
		write(STDOUT_FILENO, "\033[?2004l", 8);
	printf("\r\nttydump: %lu reads, %lu bytes\r\n", n_reads, n_bytes);
	return 0;
}
