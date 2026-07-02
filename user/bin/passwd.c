/*
 * passwd - change user password and password-aging information
 *
 * Usage: passwd [options] [LOGIN]
 *
 * A normal user may change only their own password; the superuser may change
 * any account and its aging attributes.  New passwords are hashed with
 * yescrypt using a cryptographically random salt (crypt_gensalt).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <getopt.h>
#include <termios.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pwd.h>
#include <shadow.h>
#include <crypt.h>

#define VERSION_STRING "passwd (LikeOS shadow utils) 0.2"

static const char *root_dir = "";
static int opt_quiet;

static char shadow_path[512];
static char shadow_tmp[512];

static void build_paths(void)
{
	snprintf(shadow_path, sizeof(shadow_path), "%s/etc/shadow", root_dir);
	snprintf(shadow_tmp, sizeof(shadow_tmp), "%s/etc/shadow+", root_dir);
}

static long today_days(void)
{
	time_t t = time(NULL);
	if (t <= 0)
		return 0;
	return (long)(t / 86400);
}

/* Read a line with echo disabled. */
static char *read_secret(const char *prompt)
{
	struct termios old, raw;
	int have = (tcgetattr(0, &old) == 0);
	static char buf[256];
	size_t len = 0;

	fputs(prompt, stdout);
	fflush(stdout);
	if (have) {
		raw = old;
		raw.c_lflag &= ~ECHO;
		tcsetattr(0, TCSAFLUSH, &raw);
	}
	for (;;) {
		char ch;
		ssize_t n = read(0, &ch, 1);
		if (n <= 0 || ch == '\n')
			break;
		if (len + 1 < sizeof(buf))
			buf[len++] = ch;
	}
	buf[len] = '\0';
	if (have) {
		tcsetattr(0, TCSAFLUSH, &old);
		fputc('\n', stdout);
	}
	return buf;
}

struct sp_change {
	int set_pwd;
	const char *pwd;
	int lock, unlock, del, expire;
	int set_min, set_max, set_warn, set_inact;
	long min, max, warn, inact;
	int touch_lstchg;
};

static void apply_change(struct spwd *sp, const struct sp_change *ch,
                         char *pwbuf, size_t pwbufsz)
{
	if (ch->set_pwd) {
		snprintf(pwbuf, pwbufsz, "%s", ch->pwd);
		sp->sp_pwdp = pwbuf;
	}
	if (ch->del) {
		pwbuf[0] = '\0';
		sp->sp_pwdp = pwbuf;
	}
	if (ch->lock && sp->sp_pwdp && sp->sp_pwdp[0] != '!') {
		snprintf(pwbuf, pwbufsz, "!%s", sp->sp_pwdp);
		sp->sp_pwdp = pwbuf;
	}
	if (ch->unlock && sp->sp_pwdp && sp->sp_pwdp[0] == '!') {
		snprintf(pwbuf, pwbufsz, "%s", sp->sp_pwdp + 1);
		sp->sp_pwdp = pwbuf;
	}
	if (ch->expire)
		sp->sp_lstchg = 0;
	if (ch->touch_lstchg)
		sp->sp_lstchg = today_days();
	if (ch->set_min)
		sp->sp_min = ch->min;
	if (ch->set_max)
		sp->sp_max = ch->max;
	if (ch->set_warn)
		sp->sp_warn = ch->warn;
	if (ch->set_inact)
		sp->sp_inact = ch->inact;
}

/* Rewrite /etc/shadow, applying `ch` to `user`.  Returns 0 ok, 1 not found. */
static int rewrite_shadow(const char *user, const struct sp_change *ch)
{
	FILE *in = fopen(shadow_path, "r");
	if (!in) {
		fprintf(stderr, "passwd: cannot open %s: %s\n", shadow_path,
		        strerror(errno));
		return -1;
	}
	/* Create the temporary shadow file mode 0600 from the start so its
	 * contents are never briefly world-readable. */
	int ofd = open(shadow_tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	FILE *out = ofd >= 0 ? fdopen(ofd, "w") : NULL;
	if (!out) {
		fprintf(stderr, "passwd: cannot create %s: %s\n", shadow_tmp,
		        strerror(errno));
		if (ofd >= 0)
			close(ofd);
		fclose(in);
		return -1;
	}
	struct spwd *sp;
	int found = 0;
	char pwbuf[512];
	while ((sp = fgetspent(in)) != NULL) {
		if (strcmp(sp->sp_namp, user) == 0) {
			apply_change(sp, ch, pwbuf, sizeof(pwbuf));
			found = 1;
		}
		if (putspent(sp, out) != 0) {
			fprintf(stderr, "passwd: write error: %s\n",
			        strerror(errno));
			fclose(in);
			fclose(out);
			unlink(shadow_tmp);
			return -1;
		}
	}
	fclose(in);
	if (fclose(out) != 0) {
		unlink(shadow_tmp);
		return -1;
	}
	if (!found) {
		unlink(shadow_tmp);
		return 1;
	}
	chmod(shadow_tmp, 0600);
	if (rename(shadow_tmp, shadow_path) != 0) {
		fprintf(stderr, "passwd: cannot replace %s: %s\n", shadow_path,
		        strerror(errno));
		unlink(shadow_tmp);
		return -1;
	}
	return 0;
}

static void print_status(const char *user, int all)
{
	setspent();
	struct spwd *sp;
	while ((sp = getspent()) != NULL) {
		if (!all && strcmp(sp->sp_namp, user) != 0)
			continue;
		const char *st = "P";
		if (!sp->sp_pwdp || sp->sp_pwdp[0] == '\0')
			st = "NP";
		else if (sp->sp_pwdp[0] == '!' || sp->sp_pwdp[0] == '*')
			st = "L";
		printf("%s %s %ld %ld %ld %ld %ld\n", sp->sp_namp, st,
		       sp->sp_lstchg, sp->sp_min, sp->sp_max, sp->sp_warn,
		       sp->sp_inact);
		if (!all)
			break;
	}
	endspent();
}

static void usage(int status) __attribute__((noreturn));
static void usage(int status)
{
	FILE *o = status ? stderr : stdout;
	fprintf(o,
	    "Usage: passwd [options] [LOGIN]\n\n"
	    "Change user password.\n\n"
	    "  -a, --all               with -S, show status for all users\n"
	    "  -d, --delete            delete the password (make it empty)\n"
	    "  -e, --expire            expire the password (force a change)\n"
	    "  -i, --inactive DAYS     set inactivity period after expiry\n"
	    "  -k, --keep-tokens       change only expired passwords\n"
	    "  -l, --lock              lock the account's password\n"
	    "  -n, --mindays DAYS      set minimum days between changes\n"
	    "  -q, --quiet             quiet mode\n"
	    "  -r, --repository REPO   change password in REPO (only 'files')\n"
	    "  -R, --root DIR          apply changes under the DIR directory\n"
	    "  -S, --status            report password status\n"
	    "  -u, --unlock            unlock the account's password\n"
	    "  -w, --warndays DAYS     set expiry warning days\n"
	    "  -x, --maxdays DAYS      set maximum days a password stays valid\n"
	    "  -h, --help              display this help and exit\n");
	exit(status);
}

int main(int argc, char **argv)
{
	int c;
	int do_status = 0, do_all = 0, keep_tokens = 0;
	struct sp_change ch = { 0 };
	int aging_op = 0; /* any option that only root may use */

	static struct option lo[] = {
		{ "all", no_argument, 0, 'a' },
		{ "delete", no_argument, 0, 'd' },
		{ "expire", no_argument, 0, 'e' },
		{ "inactive", required_argument, 0, 'i' },
		{ "keep-tokens", no_argument, 0, 'k' },
		{ "lock", no_argument, 0, 'l' },
		{ "mindays", required_argument, 0, 'n' },
		{ "quiet", no_argument, 0, 'q' },
		{ "repository", required_argument, 0, 'r' },
		{ "root", required_argument, 0, 'R' },
		{ "status", no_argument, 0, 'S' },
		{ "unlock", no_argument, 0, 'u' },
		{ "warndays", required_argument, 0, 'w' },
		{ "maxdays", required_argument, 0, 'x' },
		{ "help", no_argument, 0, 'h' },
		{ "version", no_argument, 0, 1 },
		{ 0, 0, 0, 0 }
	};

	while ((c = getopt_long(argc, argv, "adei:kln:qr:R:Suw:x:h", lo,
	                        NULL)) != -1) {
		switch (c) {
		case 'a': do_all = 1; break;
		case 'd': ch.del = 1; aging_op = 1; break;
		case 'e': ch.expire = 1; aging_op = 1; break;
		case 'i': ch.set_inact = 1; ch.inact = atol(optarg); aging_op = 1; break;
		case 'k': keep_tokens = 1; break;
		case 'l': ch.lock = 1; aging_op = 1; break;
		case 'n': ch.set_min = 1; ch.min = atol(optarg); aging_op = 1; break;
		case 'q': opt_quiet = 1; break;
		case 'r':
			if (strcmp(optarg, "files") != 0) {
				fprintf(stderr,
				    "passwd: only the 'files' repository is supported\n");
				return 1;
			}
			break;
		case 'R': root_dir = optarg; break;
		case 'S': do_status = 1; break;
		case 'u': ch.unlock = 1; aging_op = 1; break;
		case 'w': ch.set_warn = 1; ch.warn = atol(optarg); aging_op = 1; break;
		case 'x': ch.set_max = 1; ch.max = atol(optarg); aging_op = 1; break;
		case 'h': usage(0);
		case 1: printf("%s\n", VERSION_STRING); return 0;
		default: usage(1);
		}
	}

	uid_t uid = getuid();

	/* -R/--root relocates the account databases; since passwd is
	 * setuid-root, only the real superuser may redirect where it writes. */
	if (root_dir[0] != '\0' && uid != 0) {
		fprintf(stderr, "passwd: only root may use --root\n");
		return 1;
	}

	build_paths();

	/* Determine the target account. */
	const char *login = NULL;
	if (optind < argc) {
		login = argv[optind];
	} else {
		struct passwd *self = getpwuid(uid);
		if (!self) {
			fprintf(stderr, "passwd: cannot determine your user name: %s\n",
			        strerror(errno));
			return 1;
		}
		login = self->pw_name;
	}

	/* -S status report. */
	if (do_status) {
		if (do_all && uid != 0) {
			fprintf(stderr, "passwd: only root can display all users\n");
			return 1;
		}
		print_status(login, do_all);
		return 0;
	}

	/* Only root may change another user or use aging options. */
	struct passwd *pw = getpwnam(login);
	if (!pw) {
		fprintf(stderr, "passwd: user '%s' does not exist\n", login);
		return 1;
	}
	if (uid != 0 && pw->pw_uid != uid) {
		fprintf(stderr, "passwd: you may not change the password for %s\n",
		        login);
		return 1;
	}
	if (aging_op && uid != 0) {
		fprintf(stderr, "passwd: only root can change password aging or "
		                "lock/unlock accounts\n");
		return 1;
	}

	/* Pure aging/lock/unlock/delete operation (no new password). */
	if (aging_op) {
		int r = rewrite_shadow(login, &ch);
		if (r != 0) {
			if (r == 1)
				fprintf(stderr, "passwd: no shadow entry for %s\n",
				        login);
			return 1;
		}
		if (!opt_quiet)
			printf("passwd: password aging information changed.\n");
		return 0;
	}

	/* ---- Interactive password change. ---- */
	struct spwd *sp = getspnam(login);
	const char *oldhash = sp ? sp->sp_pwdp : "";

	if (sp && (sp->sp_pwdp[0] == '!' || sp->sp_pwdp[0] == '*') && uid != 0) {
		fprintf(stderr, "passwd: the account is locked\n");
		return 1;
	}

	if (uid != 0 && oldhash && oldhash[0]) {
		char *old = read_secret("Current password: ");
		char oldcopy[256];
		snprintf(oldcopy, sizeof(oldcopy), "%s", old);
		struct crypt_data cd;
		cd.initialized = 0;
		char *chk = crypt_r(oldcopy, oldhash, &cd);
		if (!chk || strcmp(chk, oldhash) != 0) {
			fprintf(stderr, "passwd: Authentication token manipulation error\n");
			return 1;
		}
	}

	if (keep_tokens && sp && sp->sp_lstchg != 0) {
		/* Password is not expired; --keep-tokens leaves it unchanged. */
		if (!opt_quiet)
			printf("passwd: password unchanged (not expired)\n");
		return 0;
	}

	printf("Changing password for %s.\n", login);
	char *p1 = read_secret("New password: ");
	char newpass[256];
	snprintf(newpass, sizeof(newpass), "%s", p1);
	if (newpass[0] == '\0') {
		fprintf(stderr, "passwd: No password supplied\n");
		return 1;
	}
	if (strlen(newpass) < 6)
		fprintf(stderr,
		    "passwd: warning: password is shorter than 6 characters\n");
	char *p2 = read_secret("Retype new password: ");
	if (strcmp(newpass, p2) != 0) {
		fprintf(stderr, "passwd: passwords do not match\n");
		return 1;
	}

	/* Hash with a fresh random salt from the kernel CSPRNG. */
	char *salt = crypt_gensalt(NULL, 0, NULL, 0);
	if (!salt) {
		fprintf(stderr, "passwd: cannot generate salt: %s\n",
		        strerror(errno));
		return 1;
	}
	struct crypt_data cd;
	cd.initialized = 0;
	char *hash = crypt_r(newpass, salt, &cd);
	if (!hash) {
		fprintf(stderr, "passwd: hashing failed: %s\n", strerror(errno));
		return 1;
	}

	ch.set_pwd = 1;
	ch.pwd = hash;
	ch.touch_lstchg = 1;
	int r = rewrite_shadow(login, &ch);
	if (r != 0) {
		if (r == 1)
			fprintf(stderr, "passwd: no shadow entry for %s\n", login);
		return 1;
	}
	printf("passwd: password updated successfully\n");
	return 0;
}
