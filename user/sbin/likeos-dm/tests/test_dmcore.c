// test_dmcore.c - host tests for likeos-dm's decisions and request format
// (dmcore.c): user names, requests both ways, pid files, /etc/shells,
// which request fits which mode, and what follows a child's exit --
// restarts with backoff, giving the graphical login up after three failed
// starts, a getty held off after five quick exits.
//
// Copyright (C) 2026 The LikeOS Project

#include "dmcore.h"

#include <stdio.h>
#include <string.h>

static int checks, failures;

#define CHECK(cond)                                                          \
	do {                                                                 \
		checks++;                                                    \
		if (!(cond)) {                                               \
			failures++;                                          \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,        \
				__LINE__, #cond);                            \
		}                                                            \
	} while (0)

static void test_user_names(void)
{
	char longname[64];

	CHECK(dm_user_name_valid("alice"));
	CHECK(dm_user_name_valid("root"));
	CHECK(dm_user_name_valid("a"));
	CHECK(dm_user_name_valid("Bob_2.x-y"));
	CHECK(!dm_user_name_valid(""));
	CHECK(!dm_user_name_valid(NULL));
	CHECK(!dm_user_name_valid("-f"));
	CHECK(!dm_user_name_valid(".hidden"));
	CHECK(!dm_user_name_valid("al ice"));
	CHECK(!dm_user_name_valid("al\nice"));
	CHECK(!dm_user_name_valid("a/b"));
	CHECK(!dm_user_name_valid("a;b"));
	CHECK(!dm_user_name_valid("\xc3\xa4"));
	memset(longname, 'x', sizeof(longname));
	longname[DM_USER_MAX] = '\0';
	CHECK(dm_user_name_valid(longname));
	longname[DM_USER_MAX] = 'x';
	longname[DM_USER_MAX + 1] = '\0';
	CHECK(!dm_user_name_valid(longname));
}

static void test_requests(void)
{
	char buf[128], user[DM_USER_MAX + 1], small[8];
	int n;

	CHECK(strcmp(dm_req_name(DM_REQ_CONSOLE), "console") == 0);
	CHECK(strcmp(dm_req_name(DM_REQ_GRAPHICAL), "graphical") == 0);
	CHECK(dm_req_name(DM_REQ_NONE) == NULL);

	n = dm_format_request(buf, sizeof(buf), DM_REQ_CONSOLE, "alice");
	CHECK(n == 14 && strcmp(buf, "console alice\n") == 0);
	CHECK(dm_parse_request(buf, (size_t)n, user, sizeof(user)) ==
	      DM_REQ_CONSOLE);
	CHECK(strcmp(user, "alice") == 0);

	n = dm_format_request(buf, sizeof(buf), DM_REQ_GRAPHICAL, "bob");
	CHECK(n > 0 && strcmp(buf, "graphical bob\n") == 0);
	CHECK(dm_parse_request(buf, (size_t)n, user, sizeof(user)) ==
	      DM_REQ_GRAPHICAL);
	CHECK(strcmp(user, "bob") == 0);

	// Refused when formatting.
	CHECK(dm_format_request(buf, sizeof(buf), DM_REQ_NONE, "alice") < 0);
	CHECK(dm_format_request(buf, sizeof(buf), DM_REQ_CONSOLE, "-x") < 0);
	CHECK(dm_format_request(buf, sizeof(buf), DM_REQ_CONSOLE, NULL) < 0);
	CHECK(dm_format_request(small, sizeof(small), DM_REQ_CONSOLE,
				"alice") < 0);

	// Malformed requests are no request.
#define BAD(s) CHECK(dm_parse_request(s, sizeof(s) - 1, user, \
				      sizeof(user)) == DM_REQ_NONE)
	BAD("console alice");     // no newline
	BAD("console \n");        // no user
	BAD("console\n");
	BAD("consolealice\n");
	BAD("Console alice\n");
	BAD("console alice bob\n");
	BAD("console alice\n\n");
	BAD("console -f\n");
	BAD("console al/ice\n");
	BAD("reboot alice\n");
	BAD("graphical  alice\n");
	BAD("\n");
	BAD("");
	BAD("console xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n"); // 33 characters
#undef BAD
	CHECK(dm_parse_request("console a\0b\n", 12, user, sizeof(user)) ==
	      DM_REQ_NONE);
	CHECK(dm_parse_request("console a\n", 10, user, 8) == DM_REQ_NONE);
	CHECK(dm_parse_request(NULL, 0, user, sizeof(user)) == DM_REQ_NONE);
	CHECK(dm_parse_request("console xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n",
			       41, user, sizeof(user)) == DM_REQ_CONSOLE);
}

static void test_pids(void)
{
	CHECK(dm_parse_pid("      1234\n", 11) == 1234);
	CHECK(dm_parse_pid("42\n", 3) == 42);
	CHECK(dm_parse_pid("42", 2) == 42);
	CHECK(dm_parse_pid("0\n", 2) == -1);
	CHECK(dm_parse_pid("-5\n", 3) == -1);
	CHECK(dm_parse_pid("\n", 1) == -1);
	CHECK(dm_parse_pid("", 0) == -1);
	CHECK(dm_parse_pid("12x\n", 4) == -1);
	CHECK(dm_parse_pid("12\n\n", 4) == -1);
	CHECK(dm_parse_pid("99999999999\n", 12) == -1);
	CHECK(dm_parse_pid(NULL, 3) == -1);
}

static void test_shells(void)
{
	static const char shells[] =
		"# /etc/shells - the login shells\n"
		"\n"
		"/bin/sh\n"
		"  /bin/bash  \n"
		"#/bin/zsh\n"
		"usr/bin/fish\n"
		"/usr/bin/tmux";
	size_t len = sizeof(shells) - 1;

	CHECK(dm_shell_listed(shells, len, "/bin/sh"));
	CHECK(dm_shell_listed(shells, len, "/bin/bash"));
	CHECK(dm_shell_listed(shells, len, "/usr/bin/tmux"));
	CHECK(dm_shell_listed(shells, len, "")); // empty is /bin/sh
	CHECK(dm_shell_listed(shells, len, NULL));
	CHECK(!dm_shell_listed(shells, len, "/bin/zsh"));
	CHECK(!dm_shell_listed(shells, len, "usr/bin/fish"));
	CHECK(!dm_shell_listed(shells, len, "/sbin/nologin"));
	CHECK(!dm_shell_listed(shells, len, "/bin/bas"));
	CHECK(!dm_shell_listed(shells, len, "/bin/bash2"));
	CHECK(!dm_shell_listed(NULL, 0, "/bin/sh"));
	CHECK(!dm_shell_listed("", 0, "/bin/sh"));
}

static void test_request_actions(void)
{
	struct dm_state s;

	dm_init(&s);
	CHECK(s.mode == DM_MODE_GRAPHICAL);
	CHECK(dm_request_action(&s, DM_REQ_CONSOLE, "alice") ==
	      DM_ACT_LEAVE_GRAPHICAL);
	CHECK(dm_request_action(&s, DM_REQ_CONSOLE, "-x") == DM_ACT_IGNORE);
	CHECK(dm_request_action(&s, DM_REQ_GRAPHICAL, "alice") ==
	      DM_ACT_IGNORE);
	CHECK(dm_request_action(&s, DM_REQ_NONE, "alice") == DM_ACT_IGNORE);

	dm_enter_console_user(&s, "alice");
	CHECK(s.mode == DM_MODE_CONSOLE_USER && strcmp(s.user, "alice") == 0);
	// A second "console" while already there is a double click.
	CHECK(dm_request_action(&s, DM_REQ_CONSOLE, "alice") ==
	      DM_ACT_IGNORE);
	CHECK(dm_request_action(&s, DM_REQ_GRAPHICAL, "alice") ==
	      DM_ACT_LEAVE_CONSOLE);

	s.mode = DM_MODE_CONSOLE_GETTY;
	CHECK(dm_request_action(&s, DM_REQ_GRAPHICAL, "bob") ==
	      DM_ACT_LEAVE_CONSOLE);
	CHECK(dm_request_action(&s, DM_REQ_CONSOLE, "bob") == DM_ACT_IGNORE);

	dm_enter_graphical(&s);
	CHECK(s.mode == DM_MODE_GRAPHICAL && s.user[0] == '\0' &&
	      s.graphical_failures == 0);
}

static void test_exits(void)
{
	struct dm_state s;
	char why[160];
	unsigned d;

	// The login screen failing to come up: 2 s, 5 s, then a getty and
	// the reason.
	dm_init(&s);
	d = dm_child_exited(&s, 1, why, sizeof(why));
	CHECK(d == 2 && s.mode == DM_MODE_GRAPHICAL && why[0] == '\0');
	d = dm_child_exited(&s, 0, why, sizeof(why));
	CHECK(d == 5 && s.mode == DM_MODE_GRAPHICAL);
	d = dm_child_exited(&s, 3, why, sizeof(why));
	CHECK(d == 0 && s.mode == DM_MODE_CONSOLE_GETTY);
	CHECK(strstr(why, "3 times") != NULL);
	CHECK(s.graphical_failures == 0);

	// One that ran a while and then crashed: started again after 2 s,
	// and the count starts over each time it ran long enough.
	dm_init(&s);
	d = dm_child_exited(&s, 5, why, sizeof(why));
	CHECK(d == 2 && s.graphical_failures == 1);
	d = dm_child_exited(&s, 3600, why, sizeof(why));
	CHECK(d == 2 && s.graphical_failures == 1 &&
	      s.mode == DM_MODE_GRAPHICAL);
	d = dm_child_exited(&s, 4000, why, sizeof(why));
	CHECK(d == 2 && s.mode == DM_MODE_GRAPHICAL);
	d = dm_child_exited(&s, 1, why, sizeof(why));
	CHECK(d == 5 && s.mode == DM_MODE_GRAPHICAL);
	d = dm_child_exited(&s, 1, why, sizeof(why));
	CHECK(d == 0 && s.mode == DM_MODE_CONSOLE_GETTY);

	// A successful switch back starts the count afresh.
	dm_enter_graphical(&s);
	d = dm_child_exited(&s, 0, why, sizeof(why));
	CHECK(d == 2 && s.mode == DM_MODE_GRAPHICAL);

	// The console user's shell exited: a text login prompt.
	dm_enter_console_user(&s, "alice");
	d = dm_child_exited(&s, 100, why, sizeof(why));
	CHECK(d == 0 && s.mode == DM_MODE_CONSOLE_GETTY && s.user[0] == '\0');
	// Even when it exited at once (login failed).
	dm_enter_console_user(&s, "alice");
	d = dm_child_exited(&s, 0, why, sizeof(why));
	CHECK(d == 0 && s.mode == DM_MODE_CONSOLE_GETTY);

	// getty: logouts respawn it at once; five quick exits in a row hold
	// it off, and a normal run resets the count.
	s.getty_quick_exits = 0;
	for (int i = 0; i < DM_GETTY_TRIES - 1; i++) {
		d = dm_child_exited(&s, 0, why, sizeof(why));
		CHECK(d == 0 && s.mode == DM_MODE_CONSOLE_GETTY);
	}
	d = dm_child_exited(&s, 0, why, sizeof(why));
	CHECK(d == DM_GETTY_HOLD_SECS);
	d = dm_child_exited(&s, 0, why, sizeof(why));
	CHECK(d == 0);
	d = dm_child_exited(&s, 600, why, sizeof(why));
	CHECK(d == 0 && s.getty_quick_exits == 0);
	// why may be NULL.
	CHECK(dm_child_exited(&s, 600, NULL, 0) == 0);
}

int main(void)
{
	test_user_names();
	test_requests();
	test_pids();
	test_shells();
	test_request_actions();
	test_exits();
	printf("likeos-dm host tests: %d checks, %d failed\n", checks,
	       failures);
	return failures ? 1 : 0;
}
