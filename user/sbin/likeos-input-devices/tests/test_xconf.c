// likeos-input-devices host tests: the touchpads found in each fixture
// device description and the configuration text written for them, compared
// with tests/expected/<fixture>.conf; settings parsing; identifiers.
//
//   test_xconf <fixture directory> <expected directory>
//
// UPDATE_EXPECTED=1 in the environment writes the expected files instead of
// comparing (review the diff before keeping it).
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ludev.h"
#include "xconf.h"

static int g_fail, g_pass;

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (cond) {                                                    \
			g_pass++;                                              \
		} else {                                                       \
			g_fail++;                                              \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
		}                                                              \
	} while (0)

static char *slurp(const char *path)
{
	char *text = NULL;
	size_t len;

	if (ludev_read_file(path, &text, &len, 1 << 20) != 0)
		return NULL;
	return text;
}

// Collect from one fixture and compare the rendered text with the golden
// file; `want' touchpads are expected.
static void golden(const char *fixtures, const char *expected, const char *name,
		   size_t want)
{
	char proc[1024], exp[1024];
	struct udev *u = udev_new();
	struct lid_options o;
	struct lid_touchpad *tp;
	size_t n;
	char *text, *gold;
	int rc;

	snprintf(proc, sizeof(proc), "%s/%s.devices", fixtures, name);
	snprintf(exp, sizeof(exp), "%s/%s.conf", expected, name);
	ludev_test_set_paths(proc, NULL, NULL, NULL);
	lid_options_defaults(&o);
	rc = lid_collect(u, &tp, &n);
	CHECK(rc == 0, "%s: collect %d", name, rc);
	CHECK(n == want, "%s: %zu touchpads, want %zu", name, n, want);
	if (rc || n == 0) {
		CHECK(n != 0 || tp == NULL, "%s: no list without touchpads", name);
		free(tp);
		udev_unref(u);
		return;
	}
	text = lid_render(tp, n, &o);
	CHECK(text != NULL, "%s: render", name);
	if (text && getenv("UPDATE_EXPECTED")) {
		FILE *f = fopen(exp, "w");

		if (f) {
			fputs(text, f);
			fclose(f);
		}
		printf("wrote %s\n", exp);
	} else if (text) {
		gold = slurp(exp);
		CHECK(gold && strcmp(gold, text) == 0, "%s: differs from %s:\n%s", name,
		      exp, text);
		free(gold);
	}
	free(text);
	free(tp);
	udev_unref(u);
}

static void test_options(void)
{
	struct lid_options o;
	char err[128];
	int bad;

	lid_options_defaults(&o);
	CHECK(o.tapping == 1 && o.tapping_drag == 1 && o.natural_scrolling == 0 &&
		      o.disable_while_typing == 1 && strcmp(o.accel_profile, "adaptive") == 0 &&
		      o.scroll_method[0] == '\0' && o.click_method[0] == '\0',
	      "defaults");

	bad = lid_options_parse(&o,
				"# comment\n"
				"\n"
				"Tapping = off\n"
				"NaturalScrolling=yes\n"
				"ScrollMethod=edge\n"
				"ClickMethod=buttonareas\n"
				"AccelProfile=flat\n"
				"AccelSpeed=-0.25\n"
				"LeftHanded=1\n"
				"MiddleEmulation=on\r\n"
				"HorizontalScrolling=false\n",
				err, sizeof(err));
	CHECK(bad == 0, "valid settings (%d: %s)", bad, err);
	CHECK(o.tapping == 0 && o.natural_scrolling == 1 && o.left_handed == 1 &&
		      o.middle_emulation == 1 && o.horizontal_scrolling == 0,
	      "booleans");
	CHECK(strcmp(o.scroll_method, "edge") == 0 && strcmp(o.click_method, "buttonareas") == 0 &&
		      strcmp(o.accel_profile, "flat") == 0 && strcmp(o.accel_speed, "-0.25") == 0,
	      "words");

	lid_options_defaults(&o);
	bad = lid_options_parse(&o,
				"Tapping=maybe\n"
				"ScrollMethod=circle\n"
				"AccelSpeed=2\n"
				"AccelSpeed=1e0\n"
				"AccelSpeed=0.5\" \"x\n"
				"Device=/dev/mem\n"
				"no equals sign\n"
				"ClickMethod=clickfinger\" Option \"x\n",
				err, sizeof(err));
	CHECK(bad == 8, "every invalid line skipped (%d)", bad);
	CHECK(strstr(err, "line 1") != NULL, "first error reported: %s", err);
	CHECK(o.tapping == 1 && o.scroll_method[0] == '\0' && o.accel_speed[0] == '\0' &&
		      o.click_method[0] == '\0',
	      "defaults kept");
}

static void test_render_edge_cases(void)
{
	struct lid_options o;
	struct lid_touchpad tp[3];
	char *text;

	lid_options_defaults(&o);
	memset(tp, 0, sizeof(tp));
	tp[0].event = 4;
	snprintf(tp[0].name, sizeof(tp[0].name), "Same Pad");
	tp[0].buttonpad = true;
	tp[0].multifinger = true;
	tp[1].event = 9;
	snprintf(tp[1].name, sizeof(tp[1].name), "same_pad");
	tp[2].event = 12;
	snprintf(tp[2].name, sizeof(tp[2].name), "mouse 0");

	text = lid_render(tp, 3, &o);
	CHECK(text != NULL, "render");
	if (text) {
		CHECK(strstr(text, "Identifier\t\"Same Pad (event4)\"") != NULL,
		      "clash made unique (1)");
		CHECK(strstr(text, "Identifier\t\"same_pad (event9)\"") != NULL,
		      "clash made unique (2)");
		CHECK(strstr(text, "Identifier\t\"mouse 0 (event12)\"") != NULL,
		      "no clash with Mouse0");
		// Single-finger touchpad: edge scrolling, no click method.
		CHECK(strstr(text, "\"edge\"") != NULL, "edge scrolling");
		{
			const char *s9 = strstr(text, "same_pad (event9)\"");
			const char *end = s9 ? strstr(s9, "EndSection") : NULL;
			const char *cm = s9 ? strstr(s9, "ClickMethod") : NULL;

			CHECK(s9 && end && (!cm || cm > end), "no ClickMethod without a clickpad");
		}
		free(text);
	}

	// Explicit two-finger scrolling on a one-finger pad still edge.
	snprintf(o.scroll_method, sizeof(o.scroll_method), "twofinger");
	text = lid_render(&tp[1], 1, &o);
	CHECK(text && strstr(text, "\"twofinger\"") == NULL, "twofinger needs two fingers");
	free(text);

	CHECK(lid_render(tp, 0, &o) == NULL && errno == EINVAL, "nothing to render");
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s <fixtures> <expected>\n", argv[0]);
		return 2;
	}
	ludev_test_set_paths(NULL, "/nonexistent-lid-test", "/nonexistent-lid-test",
			     "/nonexistent-lid-test");

	golden(argv[1], argv[2], "hp-probook-650-g8", 1);
	golden(argv[1], argv[2], "dell-pro16-plus", 1);
	golden(argv[1], argv[2], "likeos-hp-probook", 1);
	golden(argv[1], argv[2], "thinkpad-p50", 1);
	// Machines without a touchpad: no configuration at all.
	golden(argv[1], argv[2], "vm-absolute-mouse", 0);
	golden(argv[1], argv[2], "malformed", 0);
	test_options();
	test_render_edge_cases();

	printf("likeos-input-devices host tests: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
