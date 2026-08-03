/*
 * locale.c - locale selection.
 *
 * One locale is implemented, and its character encoding is UTF-8.  The name is
 * still remembered and reported back, because that is what programs test: a
 * great deal of software calls setlocale(LC_ALL, "") and then checks whether
 * nl_langinfo(CODESET) says UTF-8 before it will enable multibyte handling at
 * all.  Returning "ANSI_X3.4-1968" there is what kept those code paths off.
 *
 * Category-specific behaviour beyond the character set -- collation orders,
 * currency formats, translated month names -- is that of the C locale, and
 * setlocale reports honestly which name it settled on rather than pretending a
 * request for de_DE.UTF-8 changed how strcoll() orders strings.
 */
#include <locale.h>
#include <langinfo.h>
#include <stdlib.h>
#include <string.h>

#define LOCALE_NAME_MAX 64

/* Per-category names.  LC_ALL is not stored: it is the composite. */
static char cat_name[7][LOCALE_NAME_MAX] = {
	"C", "C", "C", "C", "C", "C", "C",
};

/* Buffer for the composite name setlocale(LC_ALL, ...) returns.  A single
 * string when every category agrees, otherwise the standard
 * "LC_CTYPE=x;LC_NUMERIC=y;..." form. */
static char composite[7 * (LOCALE_NAME_MAX + 16)];

static const char *const cat_label[7] = {
	"LC_ALL",      "LC_COLLATE",  "LC_CTYPE", "LC_MESSAGES",
	"LC_MONETARY", "LC_NUMERIC",  "LC_TIME",
};

/* Resolve "" against the environment the way POSIX specifies: LC_ALL wins over
 * the category variable, which wins over LANG, which falls back to C. */
static const char *env_locale(int category)
{
	const char *v = getenv("LC_ALL");
	if (v && *v)
		return v;
	if (category > 0 && category < 7) {
		v = getenv(cat_label[category]);
		if (v && *v)
			return v;
	}
	v = getenv("LANG");
	if (v && *v)
		return v;
	return "C";
}

/* Every locale name is accepted.  There is no locale database to consult, and
 * refusing names would only push programs down a "no locale support" path that
 * is strictly worse than running the one locale under the caller's preferred
 * name. */
static void store(int category, const char *name)
{
	size_t n = strlen(name);
	if (n >= LOCALE_NAME_MAX)
		n = LOCALE_NAME_MAX - 1;
	if (category == LC_ALL) {
		for (int i = 1; i < 7; i++) {
			memcpy(cat_name[i], name, n);
			cat_name[i][n] = '\0';
		}
	} else {
		memcpy(cat_name[category], name, n);
		cat_name[category][n] = '\0';
	}
}

static char *report(int category)
{
	if (category != LC_ALL)
		return cat_name[category];

	int uniform = 1;
	for (int i = 2; i < 7; i++) {
		if (strcmp(cat_name[i], cat_name[1]) != 0) {
			uniform = 0;
			break;
		}
	}
	if (uniform) {
		strcpy(composite, cat_name[1]);
		return composite;
	}

	composite[0] = '\0';
	for (int i = 1; i < 7; i++) {
		if (i > 1)
			strcat(composite, ";");
		strcat(composite, cat_label[i]);
		strcat(composite, "=");
		strcat(composite, cat_name[i]);
	}
	return composite;
}

char *setlocale(int category, const char *locale)
{
	if (category < 0 || category > 6)
		return 0;
	if (!locale)
		return report(category); /* query only */
	if (*locale == '\0')
		locale = env_locale(category);
	store(category, locale);
	return report(category);
}

struct lconv *localeconv(void)
{
	/* C locale values.  The monetary fields are the "unspecified" markers
	 * CHAR_MAX, which is how a program tells that no currency information
	 * is available rather than reading zeros as real formatting rules. */
	static struct lconv lc = {
		.decimal_point = ".",
		.thousands_sep = "",
		.grouping = "",
		.int_curr_symbol = "",
		.currency_symbol = "",
		.mon_decimal_point = "",
		.mon_thousands_sep = "",
		.mon_grouping = "",
		.positive_sign = "",
		.negative_sign = "-",
		.int_frac_digits = 127,
		.frac_digits = 127,
		.p_cs_precedes = 127,
		.p_sep_by_space = 127,
		.n_cs_precedes = 127,
		.n_sep_by_space = 127,
		.p_sign_posn = 127,
		.n_sign_posn = 127,
		.int_p_cs_precedes = 127,
		.int_p_sep_by_space = 127,
		.int_n_cs_precedes = 127,
		.int_n_sep_by_space = 127,
		.int_p_sign_posn = 127,
		.int_n_sign_posn = 127,
	};
	return &lc;
}

char *nl_langinfo(nl_item item)
{
	switch (item) {
	case CODESET:
		return "UTF-8";
	case D_T_FMT:
		return "%a %b %e %H:%M:%S %Y";
	case D_FMT:
		return "%m/%d/%y";
	case T_FMT:
		return "%H:%M:%S";
	case T_FMT_AMPM:
		return "%I:%M:%S %p";
	case AM_STR:
		return "AM";
	case PM_STR:
		return "PM";
	case DAY_1:
		return "Sunday";
	case DAY_2:
		return "Monday";
	case DAY_3:
		return "Tuesday";
	case DAY_4:
		return "Wednesday";
	case DAY_5:
		return "Thursday";
	case DAY_6:
		return "Friday";
	case DAY_7:
		return "Saturday";
	case ABDAY_1:
		return "Sun";
	case ABDAY_2:
		return "Mon";
	case ABDAY_3:
		return "Tue";
	case ABDAY_4:
		return "Wed";
	case ABDAY_5:
		return "Thu";
	case ABDAY_6:
		return "Fri";
	case ABDAY_7:
		return "Sat";
	case MON_1:
		return "January";
	case MON_2:
		return "February";
	case MON_3:
		return "March";
	case MON_4:
		return "April";
	case MON_5:
		return "May";
	case MON_6:
		return "June";
	case MON_7:
		return "July";
	case MON_8:
		return "August";
	case MON_9:
		return "September";
	case MON_10:
		return "October";
	case MON_11:
		return "November";
	case MON_12:
		return "December";
	case ABMON_1:
		return "Jan";
	case ABMON_2:
		return "Feb";
	case ABMON_3:
		return "Mar";
	case ABMON_4:
		return "Apr";
	case ABMON_5:
		return "May";
	case ABMON_6:
		return "Jun";
	case ABMON_7:
		return "Jul";
	case ABMON_8:
		return "Aug";
	case ABMON_9:
		return "Sep";
	case ABMON_10:
		return "Oct";
	case ABMON_11:
		return "Nov";
	case ABMON_12:
		return "Dec";
	case RADIXCHAR:
		return ".";
	case THOUSEP:
		return "";
	case YESEXPR:
		return "^[yY]";
	case NOEXPR:
		return "^[nN]";
	case CRNCYSTR:
		return "";
	case ERA:
	case ERA_D_FMT:
	case ERA_D_T_FMT:
	case ERA_T_FMT:
	case ALT_DIGITS:
		return "";
	default:
		return "";
	}
}

/* ---- Per-thread locale objects (uselocale/newlocale) --------------------
 *
 * There is one locale, so a locale_t is a token rather than a description.
 * The interface is still provided because software uses it to make formatting
 * independent of the global locale -- which, here, it already is.           */

static struct __locale_struct the_locale = { 1 };

locale_t newlocale(int mask, const char *locale, locale_t base)
{
	(void)mask;
	(void)locale;
	(void)base;
	return &the_locale;
}

locale_t duplocale(locale_t loc)
{
	(void)loc;
	return &the_locale;
}

void freelocale(locale_t loc)
{
	(void)loc; /* never allocated, so never released */
}

locale_t uselocale(locale_t loc)
{
	(void)loc;
	return &the_locale;
}
