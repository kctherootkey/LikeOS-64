/*
 * test_pam.c - host tests for libpam's API (make host-test).
 *
 * pam.c compiled for the build machine and driven through its public API:
 * items (strings, the X authorization copy, the failure-delay function), the
 * session environment (pam_putenv/pam_getenv/pam_getenvlist), data kept with
 * the handle and its cleanups, the failure delay, the per-service policies
 * that need no password database (lightdm-greeter, lightdm-autologin for an
 * unknown account), and the error cases of each.  The password check itself
 * reads /etc/shadow and is tested on LikeOS by test_libc.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <security/pam_appl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>

static int failures, checks;

#define CHECK(cond)                                                          \
	do {                                                                 \
		checks++;                                                    \
		if (!(cond)) {                                               \
			failures++;                                          \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,        \
				__LINE__, #cond);                            \
		}                                                            \
	} while (0)

static int conv_calls;
static const char *conv_answer;

static int test_conv(int n, const struct pam_message **msg,
		     struct pam_response **resp, void *appdata)
{
	struct pam_response *r = calloc((size_t)n, sizeof(*r));

	(void)msg;
	(void)appdata;
	if (!r)
		return PAM_BUF_ERR;
	conv_calls++;
	for (int i = 0; i < n; i++)
		r[i].resp = conv_answer ? strdup(conv_answer) : NULL;
	*resp = r;
	return PAM_SUCCESS;
}

static int cleanup_calls;
static int cleanup_last_status;
static void *cleanup_last_data;

static void data_cleanup(pam_handle_t *pamh, void *data, int status)
{
	(void)pamh;
	cleanup_calls++;
	cleanup_last_status = status;
	cleanup_last_data = data;
}

static int delay_calls;
static unsigned delay_usec;
static int delay_retval;
static void *delay_appdata;

static void delay_fn(int retval, unsigned usec, void *appdata)
{
	delay_calls++;
	delay_retval = retval;
	delay_usec = usec;
	delay_appdata = appdata;
}

static void test_start_end(void)
{
	struct pam_conv conv = { test_conv, NULL };
	pam_handle_t *h = NULL;
	const void *v = NULL;

	CHECK(pam_start("svc", "alice", NULL, &h) == PAM_SYSTEM_ERR);
	CHECK(pam_start("svc", "alice", &conv, NULL) == PAM_SYSTEM_ERR);
	CHECK(pam_start("svc", "alice", &conv, &h) == PAM_SUCCESS);
	CHECK(pam_get_item(h, PAM_SERVICE, &v) == PAM_SUCCESS &&
	      strcmp(v, "svc") == 0);
	CHECK(pam_get_item(h, PAM_USER, &v) == PAM_SUCCESS &&
	      strcmp(v, "alice") == 0);
	CHECK(pam_get_item(h, PAM_CONV, &v) == PAM_SUCCESS && v == &conv);
	CHECK(pam_end(h, PAM_AUTH_ERR) == PAM_AUTH_ERR);
	CHECK(pam_end(NULL, 0) == PAM_SYSTEM_ERR);
}

static void test_items(void)
{
	struct pam_conv conv = { test_conv, NULL };
	pam_handle_t *h = NULL;
	const void *v = NULL;
	struct pam_xauth_data x;
	const struct pam_xauth_data *got;
	char name[] = "MIT-MAGIC-COOKIE-1";
	char cookie[] = { 1, 0, 2, 3 };

	CHECK(pam_start("lightdm", NULL, &conv, &h) == PAM_SUCCESS);
	CHECK(pam_get_item(h, PAM_USER, &v) == PAM_SUCCESS && v == NULL);

	CHECK(pam_set_item(h, PAM_XDISPLAY, ":0") == PAM_SUCCESS);
	CHECK(pam_get_item(h, PAM_XDISPLAY, &v) == PAM_SUCCESS &&
	      strcmp(v, ":0") == 0);
	CHECK(pam_set_item(h, PAM_TTY, ":0") == PAM_SUCCESS);
	CHECK(pam_set_item(h, PAM_AUTHTOK_TYPE, "UNIX") == PAM_SUCCESS);
	CHECK(pam_get_item(h, PAM_AUTHTOK_TYPE, &v) == PAM_SUCCESS &&
	      strcmp(v, "UNIX") == 0);
	CHECK(pam_set_item(h, PAM_OLDAUTHTOK, "old") == PAM_SUCCESS);
	CHECK(pam_get_item(h, PAM_OLDAUTHTOK, &v) == PAM_SUCCESS &&
	      strcmp(v, "old") == 0);
	CHECK(pam_set_item(h, PAM_OLDAUTHTOK, NULL) == PAM_SUCCESS);
	CHECK(pam_get_item(h, PAM_OLDAUTHTOK, &v) == PAM_SUCCESS && !v);

	/* The X authorization is copied, not referenced. */
	x.name = name;
	x.namelen = (int)strlen(name);
	x.data = cookie;
	x.datalen = (int)sizeof(cookie);
	CHECK(pam_get_item(h, PAM_XAUTHDATA, &v) == PAM_SUCCESS && !v);
	CHECK(pam_set_item(h, PAM_XAUTHDATA, &x) == PAM_SUCCESS);
	name[0] = 'X';
	cookie[0] = 9;
	CHECK(pam_get_item(h, PAM_XAUTHDATA, &v) == PAM_SUCCESS && v);
	got = v;
	CHECK(got && got != &x && got->namelen == 18 &&
	      memcmp(got->name, "MIT-MAGIC-COOKIE-1", 18) == 0 &&
	      got->datalen == 4 && got->data[0] == 1 && got->data[1] == 0 &&
	      got->data[3] == 3);
	/* Malformed ones are refused and leave the old one in place. */
	x.namelen = -1;
	CHECK(pam_set_item(h, PAM_XAUTHDATA, &x) == PAM_BAD_ITEM);
	x.namelen = 3;
	x.name = NULL;
	CHECK(pam_set_item(h, PAM_XAUTHDATA, &x) == PAM_BAD_ITEM);
	CHECK(pam_get_item(h, PAM_XAUTHDATA, &v) == PAM_SUCCESS && v == got);
	/* An empty one is an authorization with no name and no data. */
	x.name = NULL;
	x.namelen = 0;
	x.data = NULL;
	x.datalen = 0;
	CHECK(pam_set_item(h, PAM_XAUTHDATA, &x) == PAM_SUCCESS);
	CHECK(pam_get_item(h, PAM_XAUTHDATA, &v) == PAM_SUCCESS && v &&
	      ((const struct pam_xauth_data *)v)->namelen == 0);
	CHECK(pam_set_item(h, PAM_XAUTHDATA, NULL) == PAM_SUCCESS);
	CHECK(pam_get_item(h, PAM_XAUTHDATA, &v) == PAM_SUCCESS && !v);

	CHECK(pam_set_item(h, PAM_FAIL_DELAY, (const void *)delay_fn) ==
	      PAM_SUCCESS);
	CHECK(pam_get_item(h, PAM_FAIL_DELAY, &v) == PAM_SUCCESS &&
	      v == (const void *)delay_fn);

	/* Unknown items, NULL handles and outputs. */
	CHECK(pam_set_item(h, 999, "x") == PAM_BAD_ITEM);
	CHECK(pam_get_item(h, 999, &v) == PAM_BAD_ITEM);
	CHECK(pam_get_item(h, PAM_USER, NULL) == PAM_SYSTEM_ERR);
	CHECK(pam_get_item(NULL, PAM_USER, &v) == PAM_SYSTEM_ERR);
	CHECK(pam_set_item(NULL, PAM_USER, "x") == PAM_SYSTEM_ERR);
	CHECK(pam_set_item(h, PAM_CONV, NULL) == PAM_PERM_DENIED);
	pam_end(h, PAM_SUCCESS);
}

static void test_env(void)
{
	struct pam_conv conv = { test_conv, NULL };
	pam_handle_t *h = NULL;
	char **list;
	int n;

	CHECK(pam_start("lightdm", "alice", &conv, &h) == PAM_SUCCESS);
	list = pam_getenvlist(h);
	CHECK(list && list[0] == NULL);
	free(list);

	CHECK(pam_getenv(h, "PATH") == NULL);
	CHECK(pam_putenv(h, "PATH=/usr/bin:/bin") == PAM_SUCCESS);
	CHECK(pam_putenv(h, "USER=alice") == PAM_SUCCESS);
	CHECK(pam_putenv(h, "EMPTY=") == PAM_SUCCESS);
	CHECK(strcmp(pam_getenv(h, "PATH"), "/usr/bin:/bin") == 0);
	CHECK(strcmp(pam_getenv(h, "EMPTY"), "") == 0);
	/* A prefix of a name is not that name. */
	CHECK(pam_getenv(h, "PAT") == NULL);
	CHECK(pam_getenv(h, "PATH=") == NULL);
	CHECK(pam_getenv(h, "") == NULL);
	CHECK(pam_getenv(h, NULL) == NULL);
	CHECK(pam_getenv(NULL, "PATH") == NULL);

	/* Replacing keeps one entry. */
	CHECK(pam_putenv(h, "PATH=/bin") == PAM_SUCCESS);
	CHECK(strcmp(pam_getenv(h, "PATH"), "/bin") == 0);
	list = pam_getenvlist(h);
	CHECK(list != NULL);
	for (n = 0; list && list[n]; n++)
		;
	CHECK(n == 3);
	/* The list is a copy: changing the handle does not change it. */
	CHECK(pam_putenv(h, "PATH=/sbin") == PAM_SUCCESS);
	{
		int seen = 0;

		for (int i = 0; list && list[i]; i++) {
			if (strcmp(list[i], "PATH=/bin") == 0)
				seen = 1;
			free(list[i]);
		}
		CHECK(seen);
	}
	free(list);

	/* Removal. */
	CHECK(pam_putenv(h, "USER") == PAM_SUCCESS);
	CHECK(pam_getenv(h, "USER") == NULL);
	CHECK(pam_putenv(h, "USER") == PAM_BAD_ITEM);
	CHECK(strcmp(pam_getenv(h, "PATH"), "/sbin") == 0);
	CHECK(strcmp(pam_getenv(h, "EMPTY"), "") == 0);

	/* Bad input. */
	CHECK(pam_putenv(h, "=value") == PAM_BAD_ITEM);
	CHECK(pam_putenv(h, "") == PAM_BAD_ITEM);
	CHECK(pam_putenv(h, NULL) == PAM_PERM_DENIED);
	CHECK(pam_putenv(NULL, "A=b") == PAM_PERM_DENIED);
	CHECK(pam_getenvlist(NULL) == NULL);

	/* Growth past the first allocation. */
	for (int i = 0; i < 100; i++) {
		char buf[32];

		snprintf(buf, sizeof(buf), "V%d=%d", i, i * 7);
		CHECK(pam_putenv(h, buf) == PAM_SUCCESS);
	}
	CHECK(strcmp(pam_getenv(h, "V99"), "693") == 0);
	CHECK(strcmp(pam_getenv(h, "V0"), "0") == 0);
	list = pam_getenvlist(h);
	for (n = 0; list && list[n]; n++)
		free(list[n]);
	free(list);
	CHECK(n == 102);
	pam_end(h, PAM_SUCCESS);
}

static void test_data(void)
{
	struct pam_conv conv = { test_conv, NULL };
	pam_handle_t *h = NULL;
	const void *v = NULL;
	int a = 1, b = 2, c = 3;

	CHECK(pam_start("svc", "alice", &conv, &h) == PAM_SUCCESS);
	CHECK(pam_get_data(h, "x", &v) == PAM_NO_MODULE_DATA);
	CHECK(pam_set_data(h, "x", &a, data_cleanup) == PAM_SUCCESS);
	CHECK(pam_set_data(h, "y", &c, NULL) == PAM_SUCCESS);
	CHECK(pam_get_data(h, "x", &v) == PAM_SUCCESS && v == &a);
	CHECK(pam_get_data(h, "y", &v) == PAM_SUCCESS && v == &c);

	/* Replacing calls the old entry's cleanup with PAM_DATA_REPLACE. */
	cleanup_calls = 0;
	CHECK(pam_set_data(h, "x", &b, data_cleanup) == PAM_SUCCESS);
	CHECK(cleanup_calls == 1 && cleanup_last_data == &a &&
	      (cleanup_last_status & PAM_DATA_REPLACE));
	CHECK(pam_get_data(h, "x", &v) == PAM_SUCCESS && v == &b);

	CHECK(pam_set_data(h, NULL, &a, NULL) == PAM_SYSTEM_ERR);
	CHECK(pam_set_data(NULL, "x", &a, NULL) == PAM_SYSTEM_ERR);
	CHECK(pam_get_data(h, NULL, &v) == PAM_SYSTEM_ERR);
	CHECK(pam_get_data(h, "x", NULL) == PAM_SYSTEM_ERR);
	CHECK(pam_get_data(NULL, "x", &v) == PAM_SYSTEM_ERR);

	/* pam_end calls the cleanups with its status. */
	cleanup_calls = 0;
	pam_end(h, PAM_ABORT);
	CHECK(cleanup_calls == 1 && cleanup_last_data == &b &&
	      cleanup_last_status == PAM_ABORT);
}

static void test_policies_and_delay(void)
{
	struct pam_conv conv = { test_conv, (void *)&conv_calls };
	pam_handle_t *h = NULL;
	const char *u = NULL;

	/* The greeter's service: no password, no account check -- even for
	 * a name that is not an account (the greeter account is a system one
	 * with a locked password). */
	conv_calls = 0;
	CHECK(pam_start("lightdm-greeter", "no-such-user-xyz", &conv, &h) ==
	      PAM_SUCCESS);
	CHECK(pam_authenticate(h, 0) == PAM_SUCCESS);
	CHECK(pam_acct_mgmt(h, 0) == PAM_SUCCESS);
	CHECK(conv_calls == 0);
	pam_end(h, PAM_SUCCESS);

	/* Autologin: no password, but the account has to exist. */
	CHECK(pam_start("lightdm-autologin", "no-such-user-xyz", &conv, &h) ==
	      PAM_SUCCESS);
	CHECK(pam_authenticate(h, 0) == PAM_USER_UNKNOWN);
	CHECK(conv_calls == 0);
	pam_end(h, PAM_SUCCESS);

	/* The password services: an unknown account fails without a
	 * password prompt. */
	CHECK(pam_start("lightdm", "no-such-user-xyz", &conv, &h) ==
	      PAM_SUCCESS);
	CHECK(pam_authenticate(h, 0) == PAM_USER_UNKNOWN);
	CHECK(pam_acct_mgmt(h, 0) == PAM_USER_UNKNOWN);
	CHECK(conv_calls == 0);

	/* The failure delay: handed to PAM_FAIL_DELAY with the
	 * conversation's appdata, the largest request winning ... */
	delay_calls = 0;
	CHECK(pam_set_item(h, PAM_FAIL_DELAY, (const void *)delay_fn) ==
	      PAM_SUCCESS);
	CHECK(pam_fail_delay(h, 2000) == PAM_SUCCESS);
	CHECK(pam_fail_delay(h, 500000) == PAM_SUCCESS);
	CHECK(pam_fail_delay(h, 1000) == PAM_SUCCESS);
	CHECK(pam_authenticate(h, 0) == PAM_USER_UNKNOWN);
	CHECK(delay_calls == 1 && delay_usec == 500000 &&
	      delay_retval == PAM_USER_UNKNOWN &&
	      delay_appdata == (void *)&conv_calls);
	/* ... used up by that attempt ... */
	CHECK(pam_authenticate(h, 0) == PAM_USER_UNKNOWN);
	CHECK(delay_calls == 1);
	pam_end(h, PAM_SUCCESS);

	/* ... and not applied to a success. */
	CHECK(pam_start("lightdm-greeter", "x", &conv, &h) == PAM_SUCCESS);
	CHECK(pam_set_item(h, PAM_FAIL_DELAY, (const void *)delay_fn) ==
	      PAM_SUCCESS);
	CHECK(pam_fail_delay(h, 100) == PAM_SUCCESS);
	CHECK(pam_authenticate(h, 0) == PAM_SUCCESS);
	CHECK(delay_calls == 1);
	pam_end(h, PAM_SUCCESS);
	CHECK(pam_fail_delay(NULL, 1) == PAM_SYSTEM_ERR);

	/* Without a user the name is asked for through the conversation. */
	conv_calls = 0;
	conv_answer = "no-such-user-xyz";
	CHECK(pam_start("lightdm-greeter", NULL, &conv, &h) == PAM_SUCCESS);
	CHECK(pam_get_user(h, &u, NULL) == PAM_SUCCESS && u &&
	      strcmp(u, "no-such-user-xyz") == 0 && conv_calls == 1);
	pam_end(h, PAM_SUCCESS);
	conv_answer = NULL;

	/* Opening a session as a user who is not root records nothing and
	 * succeeds (only root starts logins); su and other services never
	 * record one. */
	if (geteuid() != 0) {
		struct passwd *pw = getpwuid(getuid());

		if (pw) {
			CHECK(pam_start("login", pw->pw_name, &conv, &h) ==
			      PAM_SUCCESS);
			CHECK(pam_open_session(h, 0) == PAM_SUCCESS);
			CHECK(pam_close_session(h, 0) == PAM_SUCCESS);
			pam_end(h, PAM_SUCCESS);
		}
	}
	CHECK(pam_start("su", "no-such-user-xyz", &conv, &h) == PAM_SUCCESS);
	CHECK(pam_open_session(h, 0) == PAM_SUCCESS);
	pam_end(h, PAM_SUCCESS);
	CHECK(pam_open_session(NULL, 0) == PAM_SESSION_ERR);
}

static void test_strerror(void)
{
	CHECK(strcmp(pam_strerror(NULL, PAM_SUCCESS), "Success") == 0);
	CHECK(strcmp(pam_strerror(NULL, PAM_NO_MODULE_DATA),
		     "Unknown PAM error") != 0);
	CHECK(strcmp(pam_strerror(NULL, PAM_BAD_ITEM), "Unknown PAM error") !=
	      0);
	CHECK(strcmp(pam_strerror(NULL, 12345), "Unknown PAM error") == 0);
}

int main(void)
{
	test_start_end();
	test_items();
	test_env();
	test_data();
	test_policies_and_delay();
	test_strerror();
	printf("libpam host tests: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
