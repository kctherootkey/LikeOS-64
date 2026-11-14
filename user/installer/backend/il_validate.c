/* libinstaller -- checking what a person typed, and suggestions.
 *
 *   host names     RFC 1123 labels: letters, digits, '-', 1..63 characters,
 *                  not starting or ending with '-', not all digits
 *   user names     [a-z_][a-z0-9_-]{0,31}, not a name the system uses
 *   passwords      1..255 characters, no line ends or other control
 *                  characters (a shorter one than IL_PASSWORD_SHORT is
 *                  valid, with a warning); a strength from 0 (very weak)
 *                  to 4 (strong)
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "il_internal.h"

static int il_bad(char *why, size_t len, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

static int il_bad(char *why, size_t len, const char *fmt, ...)
{
	va_list ap;

	if (why && len) {
		va_start(ap, fmt);
		vsnprintf(why, len, fmt, ap);
		va_end(ap);
	}
	return -EINVAL;
}

static void il_ok(char *why, size_t len)
{
	if (why && len)
		why[0] = '\0';
}

/* ---- host name ---------------------------------------------------------- */

int il_validate_hostname(const char *name, char *why, size_t len)
{
	size_t n;
	int digits_only = 1;

	if (!name || !name[0])
		return il_bad(why, len, "Enter a name for this computer");
	n = strlen(name);
	if (n > IL_HOSTNAME_MAX)
		return il_bad(why, len, "The name is too long: at most %d "
			      "characters", IL_HOSTNAME_MAX);
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)name[i];

		if (c == '.')
			return il_bad(why, len, "Use a single name without dots "
				      "(no domain)");
		if (c == ' ')
			return il_bad(why, len, "The name cannot contain blanks; "
				      "use '-' instead");
		if (!(isalnum(c) || c == '-') || c >= 0x80)
			return il_bad(why, len, "The name can only contain the "
				      "letters a-z, digits and '-'");
		if (!isdigit(c) && c != '-')
			digits_only = 0;
	}
	if (name[0] == '-')
		return il_bad(why, len, "The name cannot start with '-'");
	if (name[n - 1] == '-')
		return il_bad(why, len, "The name cannot end with '-'");
	if (digits_only)
		return il_bad(why, len, "The name needs at least one letter");
	if (!strcasecmp(name, "localhost"))
		return il_bad(why, len, "\"localhost\" is reserved; choose "
			      "another name");
	il_ok(why, len);
	return 0;
}

void il_suggest_hostname(char *buf, size_t len)
{
	unsigned char b[2] = { 0, 0 };
	char hex[5];

	if (il_random_bytes(b, sizeof(b)) != 0) {
		uint64_t t = il_now_ms();

		b[0] = (unsigned char)t;
		b[1] = (unsigned char)(t >> 8);
	}
	il_hex(b, sizeof(b), hex);
	snprintf(buf, len, "likeos-%s", hex);
}

/* ---- user name ------------------------------------------------------------ */

/* Names of system accounts and groups (the image's and the customary
 * ones): a person's account must not take them. */
static const char *const g_reserved[] = {
	"root", "daemon", "bin", "sys", "sync", "games", "man", "lp", "mail",
	"news", "uucp", "proxy", "www-data", "backup", "list", "irc", "gnats",
	"nobody", "nogroup", "sshd", "audiod", "messagebus", "dbus", "polkitd",
	"tty", "disk", "cdrom", "audio", "video", "input", "render", "users",
	"wheel", "adm", "operator", "kmem", "dialout", "floppy", "tape",
	"plugdev", "netdev", "staff", "shadow", "utmp", "kvm", "sgx", "lpadmin",
	"admin", "sudo", "halt", "shutdown", "ftp", "postfix", "systemd",
	"likeos", "install", "installer", NULL,
};

static int il_reserved_name(const char *name)
{
	for (int i = 0; g_reserved[i]; i++)
		if (!strcmp(g_reserved[i], name))
			return 1;
	return 0;
}

int il_validate_username(const char *name, char *why, size_t len)
{
	size_t n;

	if (!name || !name[0])
		return il_bad(why, len, "Enter a user name");
	n = strlen(name);
	if (n > IL_USERNAME_MAX)
		return il_bad(why, len, "The user name is too long: at most %d "
			      "characters", IL_USERNAME_MAX);
	if (!(islower((unsigned char)name[0]) || name[0] == '_')) {
		if (isupper((unsigned char)name[0]))
			return il_bad(why, len, "Use lower-case letters only");
		return il_bad(why, len, "The user name must start with a "
			      "letter a-z");
	}
	for (size_t i = 1; i < n; i++) {
		unsigned char c = (unsigned char)name[i];

		if (isupper(c))
			return il_bad(why, len, "Use lower-case letters only");
		if (!(islower(c) || isdigit(c) || c == '_' || c == '-') ||
		    c >= 0x80)
			return il_bad(why, len, "The user name can only contain "
				      "a-z, digits, '_' and '-'");
	}
	if (il_reserved_name(name))
		return il_bad(why, len, "\"%s\" is used by the system; choose "
			      "another name", name);
	il_ok(why, len);
	return 0;
}

/* Common accented Latin letters (UTF-8) to plain ones for a login name. */
static const struct {
	const char *utf8;
	const char *ascii;
} g_fold[] = {
	{ "\xc3\xa4", "ae" }, { "\xc3\xb6", "oe" }, { "\xc3\xbc", "ue" },
	{ "\xc3\x84", "ae" }, { "\xc3\x96", "oe" }, { "\xc3\x9c", "ue" },
	{ "\xc3\x9f", "ss" }, { "\xc3\xa0", "a" },  { "\xc3\xa1", "a" },
	{ "\xc3\xa2", "a" },  { "\xc3\xa3", "a" },  { "\xc3\xa5", "a" },
	{ "\xc3\xa6", "ae" }, { "\xc3\xa7", "c" },  { "\xc3\xa8", "e" },
	{ "\xc3\xa9", "e" },  { "\xc3\xaa", "e" },  { "\xc3\xab", "e" },
	{ "\xc3\xac", "i" },  { "\xc3\xad", "i" },  { "\xc3\xae", "i" },
	{ "\xc3\xaf", "i" },  { "\xc3\xb1", "n" },  { "\xc3\xb2", "o" },
	{ "\xc3\xb3", "o" },  { "\xc3\xb4", "o" },  { "\xc3\xb5", "o" },
	{ "\xc3\xb8", "o" },  { "\xc3\xb9", "u" },  { "\xc3\xba", "u" },
	{ "\xc3\xbb", "u" },  { "\xc3\xbd", "y" },  { "\xc3\xbf", "y" },
	{ "\xc3\x80", "a" },  { "\xc3\x81", "a" },  { "\xc3\x82", "a" },
	{ "\xc3\x85", "a" },  { "\xc3\x86", "ae" }, { "\xc3\x87", "c" },
	{ "\xc3\x88", "e" },  { "\xc3\x89", "e" },  { "\xc3\x8a", "e" },
	{ "\xc3\x8d", "i" },  { "\xc3\x91", "n" },  { "\xc3\x93", "o" },
	{ "\xc3\x98", "o" },  { "\xc3\x9a", "u" },  { "\xc5\x82", "l" },
	{ "\xc5\x81", "l" },  { "\xc5\xa1", "s" },  { "\xc5\xa0", "s" },
	{ "\xc5\xbe", "z" },  { "\xc5\xbd", "z" },  { "\xc4\x8d", "c" },
	{ "\xc4\x8c", "c" },  { "\xc5\x99", "r" },  { "\xc4\x9b", "e" },
	{ NULL, NULL },
};

void il_suggest_username(const char *fullname, char *buf, size_t len)
{
	char out[IL_USERNAME_MAX + 1];
	size_t n = 0;
	const char *p = fullname ? fullname : "";

	if (!buf || !len)
		return;
	/* The first word of the name. */
	while (*p == ' ' || *p == '\t')
		p++;
	while (*p && *p != ' ' && *p != '\t' && n < IL_USERNAME_MAX) {
		unsigned char c = (unsigned char)*p;
		int folded = 0;

		if (c >= 0x80) {
			for (int i = 0; g_fold[i].utf8; i++) {
				size_t l = strlen(g_fold[i].utf8);

				if (!strncmp(p, g_fold[i].utf8, l)) {
					for (const char *a = g_fold[i].ascii;
					     *a && n < IL_USERNAME_MAX; a++)
						out[n++] = *a;
					p += l;
					folded = 1;
					break;
				}
			}
			if (!folded) {
				/* Another character: skip all its bytes. */
				p++;
				while (((unsigned char)*p & 0xC0) == 0x80)
					p++;
			}
			continue;
		}
		if (isalnum(c))
			out[n++] = (char)tolower(c);
		else if (c == '-' || c == '_' || c == '.')
			out[n++] = c == '.' ? '_' : (char)c;
		p++;
	}
	out[n] = '\0';
	/* It has to start with a letter. */
	while (out[0] && !islower((unsigned char)out[0]))
		memmove(out, out + 1, strlen(out));
	if (!out[0])
		il_strlcpy(out, "user", sizeof(out));
	if (il_reserved_name(out)) {
		size_t l = strlen(out);

		if (l + 1 <= IL_USERNAME_MAX) {
			out[l] = '1';
			out[l + 1] = '\0';
		}
	}
	il_strlcpy(buf, out, len);
}

/* ---- passwords ------------------------------------------------------------ */

int il_validate_password(const char *pw, char *why, size_t len)
{
	size_t n;

	if (!pw || !pw[0])
		return il_bad(why, len, "Enter a password");
	n = strlen(pw);
	if (n > IL_PASSWORD_MAX)
		return il_bad(why, len, "The password is too long: at most %d "
			      "characters", IL_PASSWORD_MAX);
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)pw[i];

		if (c < 0x20 || c == 0x7f)
			return il_bad(why, len, "The password cannot contain line "
				      "ends or other control characters");
	}
	/* Valid, but worth a word (the caller may show it as a warning). */
	if (n < IL_PASSWORD_SHORT) {
		il_bad(why, len, "This password is short; %d characters or more "
		       "are safer", IL_PASSWORD_SHORT);
		return 0;
	}
	il_ok(why, len);
	return 0;
}

/* Passwords everyone tries first. */
static const char *const g_common[] = {
	"password", "passw0rd", "123456", "1234567", "12345678", "123456789",
	"1234567890", "qwerty", "qwertz", "azerty", "abc123", "letmein",
	"welcome", "admin", "root", "toor", "likeos", "iloveyou", "monkey",
	"dragon", "football", "baseball", "master", "sunshine", "princess",
	"login", "secret", "111111", "000000", "trustno1", "hallo", "passwort",
	"changeme", "default", "test", "guest", NULL,
};

int il_password_strength(const char *pw)
{
	size_t n;
	int lower = 0, upper = 0, digit = 0, other = 0, classes;
	int score, runs = 0, seq = 0;

	if (!pw || !pw[0])
		return 0;
	n = strlen(pw);
	for (int i = 0; g_common[i]; i++)
		if (!strcasecmp(pw, g_common[i]))
			return 0;
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)pw[i];

		if (islower(c))
			lower = 1;
		else if (isupper(c))
			upper = 1;
		else if (isdigit(c))
			digit = 1;
		else
			other = 1;
		if (i >= 2 && pw[i] == pw[i - 1] && pw[i] == pw[i - 2])
			runs++;
		if (i >= 2 && (unsigned char)pw[i] == (unsigned char)pw[i - 1] + 1 &&
		    (unsigned char)pw[i - 1] == (unsigned char)pw[i - 2] + 1)
			seq++;
	}
	classes = lower + upper + digit + other;
	if (n < 6)
		score = 0;
	else if (n < 8)
		score = 1;
	else if (n < 12)
		score = 2;
	else if (n < 16)
		score = 3;
	else
		score = 4;
	if (classes >= 3)
		score++;
	if (classes >= 4 && n >= 10)
		score++;
	if (classes == 1)
		score--;
	/* aaa, 123, abc: the length counts less. */
	if ((size_t)(runs + seq) * 2 >= n)
		score -= 2;
	else if (runs + seq > 0)
		score--;
	if (score < 0)
		score = 0;
	if (score > 4)
		score = 4;
	return score;
}

/* ---- a whole configuration -------------------------------------------- */

static int il_xkb_name_ok(const char *s, int allow_empty)
{
	if (!s[0])
		return allow_empty;
	for (const char *p = s; *p; p++)
		if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-' ||
		      *p == '.' || *p == '+' || *p == '@'))
			return 0;
	return 1;
}

static int il_xkb_options_ok(const char *s)
{
	for (const char *p = s; *p; p++)
		if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-' ||
		      *p == ':' || *p == ',' || *p == '(' || *p == ')' ||
		      *p == '+'))
			return 0;
	return 1;
}

int il_config_validate(const struct il_config *cfg, char *why, size_t len)
{
	char sub[256];
	int rc;

	if (!cfg)
		return il_bad(why, len, "No settings");
	if (il_locale_find(cfg->locale) < 0)
		return il_bad(why, len, "The language %s is not offered",
			      cfg->locale[0] ? cfg->locale : "(none)");
	if (cfg->time_format[0] && il_locale_find(cfg->time_format) < 0)
		return il_bad(why, len, "The format %s is not offered",
			      cfg->time_format);
	if (il_timezone_find(cfg->timezone) < 0)
		return il_bad(why, len, "The time zone %s is not known",
			      cfg->timezone[0] ? cfg->timezone : "(none)");
	if (cfg->nlayouts < 1 || cfg->nlayouts > IL_MAX_LAYOUTS)
		return il_bad(why, len, "Choose one to %d keyboard layouts",
			      IL_MAX_LAYOUTS);
	for (int i = 0; i < cfg->nlayouts; i++) {
		if (!il_xkb_name_ok(cfg->layouts[i], 0))
			return il_bad(why, len, "Keyboard layout %d (\"%s\") is "
				      "not a layout name", i + 1,
				      cfg->layouts[i]);
		if (!il_xkb_name_ok(cfg->variants[i], 1))
			return il_bad(why, len, "The variant of keyboard layout "
				      "%d is not a variant name", i + 1);
	}
	if (!il_xkb_options_ok(cfg->xkb_options))
		return il_bad(why, len, "The keyboard options are not valid XKB "
			      "options");
	rc = il_validate_hostname(cfg->hostname, sub, sizeof(sub));
	if (rc)
		return il_bad(why, len, "Computer name: %s", sub);
	if (!cfg->root_password_hash[0]) {
		rc = il_validate_password(cfg->root_password, sub, sizeof(sub));
		if (rc)
			return il_bad(why, len, "Root password: %s", sub);
	}
	if (cfg->user.username[0] || cfg->user.fullname[0]) {
		rc = il_validate_username(cfg->user.username, sub, sizeof(sub));
		if (rc)
			return il_bad(why, len, "User name: %s", sub);
		if (!cfg->user_password_hash[0]) {
			rc = il_validate_password(cfg->user.password, sub,
						  sizeof(sub));
			if (rc)
				return il_bad(why, len, "Password of %s: %s",
					      cfg->user.username, sub);
		}
		for (const char *p = cfg->user.fullname; *p; p++)
			if (*p == ':' || *p == '\n' || *p == '\r')
				return il_bad(why, len, "The full name cannot "
					      "contain ':' or line ends");
		if (strlen(cfg->user.fullname) > IL_FULLNAME_MAX)
			return il_bad(why, len, "The full name is too long");
	}
	il_ok(why, len);
	return 0;
}

void il_config_defaults(struct il_config *cfg)
{
	if (!cfg)
		return;
	memset(cfg, 0, sizeof(*cfg));
	il_strlcpy(cfg->locale, "en_US.UTF-8", sizeof(cfg->locale));
	il_strlcpy(cfg->time_format, "en_US.UTF-8", sizeof(cfg->time_format));
	il_strlcpy(cfg->timezone, "UTC", sizeof(cfg->timezone));
	cfg->nlayouts = 1;
	il_strlcpy(cfg->layouts[0], "us", sizeof(cfg->layouts[0]));
	il_suggest_hostname(cfg->hostname, sizeof(cfg->hostname));
	cfg->user_extra_groups = 1;
}
