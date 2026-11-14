/*
 * LikeOS boot loader -- the boot menu.
 *
 * Shown when the loader's own partition has \EFI\LIKEOS\bootmenu.conf (the
 * live medium does; an installed system does not and starts straight into
 * LikeOS).  The file, UTF-8 text, one setting per line, '#' starting a
 * comment line:
 *
 *   title=<heading>        the heading above the menu (default "LikeOS")
 *   timeout=<seconds>      start the default entry after this long; 0
 *                          waits for a choice (default 10)
 *   default=<index>        the entry selected at first, counted from 0
 *   entry=<title>|<words>  one menu entry (1 to 8 of them): its title and
 *                          the words it adds to the kernel command line
 *                          (after those of the boot entry or cmdline.txt)
 *
 * The live medium's file:
 *
 *   title=LikeOS
 *   timeout=10
 *   default=0
 *   entry=Run LikeOS Live from USB|
 *   entry=Install LikeOS from USB|likeos.mode=install
 *
 * Keys: Up/Down (wrapping), Home/End, Page Up/Page Down, 1-8 (select and
 * start that entry), Enter (start the selected one); Esc does nothing.
 * The countdown stops at the first key.
 *
 * Drawn on the Graphics Output Protocol in its current mode (gfx.c):
 * a dark gradient with a soft glow, the LikeOS logo, the heading, a
 * rounded panel with a shadow holding the entries -- the selected one on
 * an accent-coloured bar -- a countdown bar and a line of key hints, all
 * sized from the screen height.  Without graphics, a text menu on the
 * console with the selected entry in inverse video.  Either way the menu
 * is mirrored to the serial log as plain text.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "bootcommon.h"
#include "gfx.h"
#include "bootlogo.h"

#define MENU_TITLE_MAX 128
#define MENU_CONF_MAX (16 * 1024)
#define MENU_DEFAULT_TIMEOUT 10
#define MENU_TICK_100NS 1000000ULL /* the countdown's step, 100 ms */

/* EFI_INPUT_KEY scan codes */
#define KEY_SCAN_UP 0x01
#define KEY_SCAN_DOWN 0x02
#define KEY_SCAN_HOME 0x05
#define KEY_SCAN_END 0x06
#define KEY_SCAN_PAGE_UP 0x09
#define KEY_SCAN_PAGE_DOWN 0x0a
#define KEY_SCAN_ESC 0x17

struct menu_entry {
	char title[MENU_TITLE_MAX];
	char args[BOOTMENU_ARGS_MAX];
};

struct menu {
	char heading[MENU_TITLE_MAX];
	UINT32 timeout;  /* seconds, 0 = wait */
	UINT32 sel;      /* selected entry */
	UINT32 n;        /* entries */
	struct menu_entry e[BOOTMENU_MAX_ENTRIES];
	/* the countdown */
	BOOLEAN counting;
	UINT32 ticks_total, ticks_left;
};

/* ---- small string helpers ------------------------------------------------- */

static UINTN m_strlen(const char *s)
{
	UINTN n = 0;

	while (s[n])
		n++;
	return n;
}

static void m_strcpy(char *d, UINTN cap, const char *s)
{
	UINTN i = 0;

	if (!cap)
		return;
	for (; s[i] && i + 1 < cap; i++)
		d[i] = s[i];
	d[i] = 0;
}

/* Copy [s, e) trimmed of blanks into d (cap bytes, cut at a UTF-8
 * character boundary). */
static void m_trimcpy(char *d, UINTN cap, const char *s, const char *e)
{
	UINTN n;

	while (s < e && (*s == ' ' || *s == '\t'))
		s++;
	while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
		e--;
	n = (UINTN)(e - s);
	if (n + 1 > cap) {
		n = cap - 1;
		while (n > 0 && ((unsigned char)s[n] & 0xc0) == 0x80)
			n--;
	}
	for (UINTN i = 0; i < n; i++)
		d[i] = s[i];
	d[n] = 0;
}

static BOOLEAN m_keyeq(const char *k, UINTN klen, const char *name)
{
	UINTN i = 0;

	for (; i < klen && name[i]; i++) {
		char c = k[i];

		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
		if (c != name[i])
			return FALSE;
	}
	return i == klen && !name[i];
}

static BOOLEAN m_parse_uint(const char *s, UINT32 *out)
{
	UINT32 v = 0;
	UINTN digits = 0;

	while (*s == ' ' || *s == '\t')
		s++;
	while (*s >= '0' && *s <= '9') {
		if (v > 100000000u)
			return FALSE;
		v = v * 10 + (UINT32)(*s - '0');
		s++;
		digits++;
	}
	while (*s == ' ' || *s == '\t')
		s++;
	if (!digits || *s)
		return FALSE;
	*out = v;
	return TRUE;
}

/* ---- the configuration ---------------------------------------------------- */

/* The file's bytes, or NULL (no file, or not readable). *len its length. */
static char *menu_read_conf(EFI_HANDLE image, UINTN *len)
{
	EFI_LOADED_IMAGE *li = NULL;
	EFI_FILE_IO_INTERFACE *fs = NULL;
	EFI_FILE_HANDLE root = NULL, file = NULL;
	char *buf = NULL;
	UINTN got = 0;

	*len = 0;
	if (EFI_ERROR(uefi_call_wrapper(BS->HandleProtocol, 3, image,
					&LoadedImageProtocol, (VOID **)&li)) ||
	    !li || !li->DeviceHandle)
		return NULL;
	if (EFI_ERROR(uefi_call_wrapper(BS->HandleProtocol, 3, li->DeviceHandle,
					&FileSystemProtocol, (VOID **)&fs)) ||
	    !fs)
		return NULL;
	if (EFI_ERROR(uefi_call_wrapper(fs->OpenVolume, 2, fs, &root)) || !root)
		return NULL;
	if (EFI_ERROR(uefi_call_wrapper(root->Open, 5, root, &file,
					BOOTMENU_CONF_PATH, EFI_FILE_MODE_READ,
					0ULL)) ||
	    !file) {
		uefi_call_wrapper(root->Close, 1, root);
		return NULL;
	}
	if (!EFI_ERROR(uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData,
					 (UINTN)MENU_CONF_MAX + 1,
					 (VOID **)&buf)) &&
	    buf) {
		/* Read() may return less than asked: keep going until EOF. */
		while (got < MENU_CONF_MAX) {
			UINTN chunk = MENU_CONF_MAX - got;

			if (EFI_ERROR(uefi_call_wrapper(file->Read, 3, file,
							&chunk, buf + got)) ||
			    chunk == 0)
				break;
			got += chunk;
		}
		buf[got] = 0;
	}
	uefi_call_wrapper(file->Close, 1, file);
	uefi_call_wrapper(root->Close, 1, root);
	*len = got;
	return buf;
}

/* Only printable ASCII goes onto the kernel command line, one blank
 * between words. */
static void menu_clean_args(char *a)
{
	UINTN o = 0;
	BOOLEAN space = FALSE;

	for (UINTN i = 0; a[i]; i++) {
		unsigned char c = (unsigned char)a[i];

		if (c == ' ' || c == '\t') {
			space = o > 0;
			continue;
		}
		if (c < 0x21 || c > 0x7e)
			continue;
		if (space) {
			a[o++] = ' ';
			space = FALSE;
		}
		a[o++] = (char)c;
	}
	a[o] = 0;
}

static void menu_parse(struct menu *m, const char *text, UINTN len)
{
	const char *p = text, *end = text + len;
	UINT32 def = 0;
	BOOLEAN have_def = FALSE;

	m_strcpy(m->heading, sizeof(m->heading), "LikeOS");
	m->timeout = MENU_DEFAULT_TIMEOUT;
	m->n = 0;
	/* a UTF-8 byte order mark */
	if (len >= 3 && (unsigned char)p[0] == 0xef &&
	    (unsigned char)p[1] == 0xbb && (unsigned char)p[2] == 0xbf)
		p += 3;
	while (p < end) {
		const char *line = p, *le, *eq, *ks;
		char val[MENU_TITLE_MAX + BOOTMENU_ARGS_MAX];

		while (p < end && *p != '\n')
			p++;
		le = p;
		if (p < end)
			p++;
		if (le > line && le[-1] == '\r')
			le--;
		ks = line;
		while (ks < le && (*ks == ' ' || *ks == '\t'))
			ks++;
		if (ks == le || *ks == '#')
			continue;
		for (eq = ks; eq < le && *eq != '='; eq++)
			;
		if (eq == le) {
			bl_log("bootmenu: line without '=' ignored\n");
			continue;
		}
		{
			const char *ke = eq;

			while (ke > ks && (ke[-1] == ' ' || ke[-1] == '\t'))
				ke--;
			m_trimcpy(val, sizeof(val), eq + 1, le);
			if (m_keyeq(ks, (UINTN)(ke - ks), "title")) {
				if (val[0])
					m_strcpy(m->heading, sizeof(m->heading), val);
			} else if (m_keyeq(ks, (UINTN)(ke - ks), "timeout")) {
				UINT32 t;

				if (m_parse_uint(val, &t))
					m->timeout = t > 86400 ? 86400 : t;
				else
					bl_log("bootmenu: bad timeout ignored\n");
			} else if (m_keyeq(ks, (UINTN)(ke - ks), "default")) {
				if (m_parse_uint(val, &def))
					have_def = TRUE;
				else
					bl_log("bootmenu: bad default ignored\n");
			} else if (m_keyeq(ks, (UINTN)(ke - ks), "entry")) {
				struct menu_entry *e;
				const char *bar = val;

				if (m->n >= BOOTMENU_MAX_ENTRIES) {
					bl_log("bootmenu: more than 8 entries, the rest ignored\n");
					continue;
				}
				while (*bar && *bar != '|')
					bar++;
				e = &m->e[m->n];
				m_trimcpy(e->title, sizeof(e->title), val, bar);
				if (*bar)
					m_trimcpy(e->args, sizeof(e->args), bar + 1,
						  bar + m_strlen(bar));
				else
					e->args[0] = 0;
				menu_clean_args(e->args);
				if (!e->title[0]) {
					bl_log("bootmenu: entry without a title ignored\n");
					continue;
				}
				m->n++;
			} else {
				bl_log("bootmenu: unknown setting ignored\n");
			}
		}
	}
	m->sel = have_def && def < m->n ? def : 0;
}

/* ---- serial mirror ------------------------------------------------------- */

static void menu_log_state(const struct menu *m)
{
	bl_log("\n=== ");
	bl_log(m->heading);
	bl_log(" boot menu ===\n");
	for (UINT32 i = 0; i < m->n; i++) {
		bl_log(i == m->sel ? " > " : "   ");
		bl_log_dec(i + 1);
		bl_log(") ");
		bl_log(m->e[i].title);
		if (m->e[i].args[0]) {
			bl_log("  [");
			bl_log(m->e[i].args);
			bl_log("]");
		}
		bl_log("\n");
	}
	if (m->timeout) {
		bl_log("Starting entry ");
		bl_log_dec(m->sel + 1);
		bl_log(" in ");
		bl_log_dec(m->timeout);
		bl_log(" s; a key on the console stops the countdown\n");
	} else {
		bl_log("Waiting for a choice on the console\n");
	}
}

/* ---- graphical menu ---------------------------------------------------------- */

/* Palette (matching the desktop's dark shell colours). */
#define COL_BG_TOP GFX_RGB(0x1d, 0x27, 0x40)
#define COL_BG_BOTTOM GFX_RGB(0x08, 0x0b, 0x13)
#define COL_GLOW GFX_ARGB(0x46, 0x3f, 0x74, 0xf2)
#define COL_GLOW2 GFX_ARGB(0x22, 0x2f, 0xb8, 0xc9)
#define COL_TITLE GFX_RGB(0xf4, 0xf6, 0xfb)
#define COL_SUBTITLE GFX_RGB(0x93, 0x9d, 0xb3)
#define COL_PANEL GFX_ARGB(0xe0, 0x18, 0x1f, 0x30)
#define COL_PANEL_LINE GFX_ARGB(0x1c, 0xff, 0xff, 0xff)
#define COL_SHADOW GFX_ARGB(0x8c, 0x00, 0x00, 0x00)
#define COL_ACCENT GFX_RGB(0x3f, 0x74, 0xf2)
#define COL_ACCENT_LINE GFX_ARGB(0x38, 0xff, 0xff, 0xff)
#define COL_ACCENT_LIGHT GFX_RGB(0x8f, 0xb2, 0xff)
#define COL_TEXT GFX_RGB(0xc9, 0xd0, 0xdc)
#define COL_TEXT_SEL GFX_RGB(0xff, 0xff, 0xff)
#define COL_BADGE GFX_RGB(0x26, 0x31, 0x4b)
#define COL_BADGE_SEL GFX_ARGB(0x33, 0xff, 0xff, 0xff)
#define COL_ICON GFX_RGB(0x7f, 0xa2, 0xff)
#define COL_ICON_SEL GFX_RGB(0xff, 0xff, 0xff)
#define COL_KEY_LINE GFX_ARGB(0x3a, 0xff, 0xff, 0xff)
#define COL_KEY_LINE_SEL GFX_ARGB(0x99, 0xff, 0xff, 0xff)
#define COL_KEY_TEXT GFX_RGB(0x8a, 0x94, 0xa8)
#define COL_HINT GFX_RGB(0x8a, 0x94, 0xa8)
#define COL_HINT_KEY GFX_RGB(0xe6, 0xea, 0xf2)
#define COL_HINT_DOT GFX_RGB(0x4d, 0x57, 0x6c)
#define COL_TRACK GFX_ARGB(0x1e, 0xff, 0xff, 0xff)

/* The layout, worked out once from the screen size. */
struct layout {
	INT32 W, H;
	float s; /* scale: 1.0 at 1920x1080 */
	const struct bootfont_face *f_title, *f_sub, *f_entry, *f_entry_sel,
		*f_hint;
	INT32 logo, logo_x, logo_y;
	INT32 title_y, sub_y; /* baselines */
	INT32 px, py, pw, ph; /* the panel */
	INT32 pad, rh, rgap, radius, row_radius;
	INT32 shadow, shadow_dy;
	INT32 bar_y, bar_h, bar_w;
	INT32 hint_y; /* baseline */
	INT32 dyn_x, dyn_y, dyn_w, dyn_h; /* what a selection change repaints */
	INT32 cnt_y, cnt_h;               /* what a countdown step repaints */
};

static INT32 iround(float v)
{
	return (INT32)(v < 0 ? v - 0.5f : v + 0.5f);
}

static INT32 imax(INT32 a, INT32 b)
{
	return a > b ? a : b;
}

static INT32 imin(INT32 a, INT32 b)
{
	return a < b ? a : b;
}

static void menu_layout(const struct menu *m, struct layout *L)
{
	float sw, sh;
	INT32 maxtext = 0, need, block, top, gap1, gap2, gap3, gap4, gap5;

	L->W = gfx_width();
	L->H = gfx_height();
	sw = (float)L->W / 1440.0f;
	sh = (float)L->H / 1080.0f;
	L->s = sw < sh ? sw : sh;
	if (L->s < 0.42f)
		L->s = 0.42f;
	if (L->s > 2.2f)
		L->s = 2.2f;
	L->f_title = gfx_face(1, imax(20, iround(46.0f * L->s)));
	L->f_sub = gfx_face(0, imax(14, iround(20.0f * L->s)));
	L->f_entry = gfx_face(0, imax(16, iround(25.0f * L->s)));
	L->f_entry_sel = gfx_face(1, imax(16, iround(25.0f * L->s)));
	L->f_hint = gfx_face(0, imax(14, iround(17.0f * L->s)));
	/* the semibold faces start at a larger size: on a small screen the
	 * selected entry would jump in size, so it keeps the regular face */
	if (L->f_entry_sel->px > L->f_entry->px + 2)
		L->f_entry_sel = L->f_entry;

	L->logo = imin(BOOTLOGO_W, imax(48, iround(124.0f * L->s)));
	L->pad = imax(6, iround(12.0f * L->s));
	L->rh = imax(iround(66.0f * L->s), (INT32)L->f_entry->line + 2 * L->pad);
	L->rgap = imax(2, iround(6.0f * L->s));
	L->radius = imax(8, iround(18.0f * L->s));
	L->row_radius = imax(6, iround(12.0f * L->s));
	L->shadow = imax(10, iround(34.0f * L->s));
	L->shadow_dy = imax(4, iround(12.0f * L->s));
	L->bar_h = imax(3, iround(4.0f * L->s));

	/* wide enough for the longest title, its badge and its key cap */
	for (UINT32 i = 0; i < m->n; i++) {
		INT32 w = gfx_text_width(L->f_entry_sel, m->e[i].title);

		if (w > maxtext)
			maxtext = w;
	}
	need = 2 * L->pad + 2 * L->rh + maxtext + iround(40.0f * L->s);
	L->pw = imax(iround(720.0f * L->s), need);
	L->pw = imin(L->pw, L->W - imax(16, L->W / 20) * 2);
	L->ph = (INT32)m->n * L->rh + ((INT32)m->n - 1) * L->rgap + 2 * L->pad;
	L->bar_w = L->pw - 2 * L->radius;

	gap1 = iround(26.0f * L->s);
	gap2 = iround(10.0f * L->s);
	gap3 = iround(40.0f * L->s);
	gap4 = iround(30.0f * L->s);
	gap5 = iround(20.0f * L->s);
	block = L->logo + gap1 + L->f_title->line + gap2 + L->f_sub->line + gap3 +
		L->ph + gap4 + L->bar_h + gap5 + L->f_hint->line;
	/* a little above the middle, where the eye expects it */
	top = (L->H - block) * 9 / 20;
	if (top < L->pad)
		top = L->pad;

	L->logo_x = (L->W - L->logo) / 2;
	L->logo_y = top;
	L->title_y = L->logo_y + L->logo + gap1 + L->f_title->ascent;
	L->sub_y = L->title_y + L->f_title->descent + gap2 + L->f_sub->ascent;
	L->px = (L->W - L->pw) / 2;
	L->py = L->sub_y + L->f_sub->descent + gap3;
	L->bar_y = L->py + L->ph + gap4;
	L->hint_y = L->bar_y + L->bar_h + gap5 + L->f_hint->ascent;

	L->dyn_x = L->px - L->shadow - 2;
	L->dyn_y = L->py - L->shadow - 2;
	L->dyn_w = L->pw + 2 * L->shadow + 4;
	L->dyn_h = L->ph + 2 * L->shadow + L->shadow_dy + 4;
	L->cnt_y = L->bar_y - 2;
	L->cnt_h = L->hint_y + L->f_hint->descent + 4 - L->cnt_y;
}

/* The play symbol: a triangle in the badge. */
static void icon_play(float cx, float cy, float d, gfx_color c)
{
	float h = d * 0.34f, w = h * 0.9f;
	float pts[6] = {
		cx - w * 0.38f, cy - h * 0.5f,
		cx + w * 0.62f, cy,
		cx - w * 0.38f, cy + h * 0.5f,
	};

	gfx_polygon(pts, 3, c);
}

/* The install symbol: an arrow down into a tray. */
static void icon_install(float cx, float cy, float d, gfx_color c)
{
	float u = d / 100.0f;
	INT32 shaft_w = imax(2, iround(9.0f * u));
	INT32 t = imax(2, iround(7.0f * u));
	float head[6] = {
		cx - 17.0f * u, cy - 2.0f * u,
		cx + 17.0f * u, cy - 2.0f * u,
		cx, cy + 16.0f * u,
	};

	/* shaft and head */
	gfx_round_rect(iround(cx - (float)shaft_w / 2.0f), iround(cy - 24.0f * u),
		       shaft_w, iround(24.0f * u), imax(1, shaft_w / 3), c);
	gfx_polygon(head, 3, c);
	/* tray: base and two uprights */
	gfx_round_rect(iround(cx - 22.0f * u), iround(cy + 20.0f * u),
		       iround(44.0f * u), t, imax(1, t / 2), c);
	gfx_round_rect(iround(cx - 22.0f * u), iround(cy + 9.0f * u), t,
		       iround(18.0f * u), imax(1, t / 2), c);
	gfx_round_rect(iround(cx + 22.0f * u) - t, iround(cy + 9.0f * u), t,
		       iround(18.0f * u), imax(1, t / 2), c);
}

static BOOLEAN entry_installs(const struct menu_entry *e)
{
	/* the word likeos.mode=install, or any word naming an installation */
	const char *a = e->args;

	for (UINTN i = 0; a[i]; i++) {
		const char *k = "install";
		UINTN j = 0;

		while (k[j] && a[i + j] == k[j])
			j++;
		if (!k[j])
			return TRUE;
	}
	return FALSE;
}

static void draw_panel(const struct menu *m, const struct layout *L)
{
	gfx_restore_background(L->dyn_x, L->dyn_y, L->dyn_w, L->dyn_h);
	gfx_shadow(L->px, L->py + L->shadow_dy, L->pw, L->ph, L->radius,
		   L->shadow, COL_SHADOW);
	gfx_round_rect(L->px, L->py, L->pw, L->ph, L->radius, COL_PANEL);
	gfx_round_rect_line(L->px, L->py, L->pw, L->ph, L->radius, 1,
			    COL_PANEL_LINE);

	for (UINT32 i = 0; i < m->n; i++) {
		BOOLEAN sel = i == m->sel;
		INT32 rx = L->px + L->pad;
		INT32 ry = L->py + L->pad + (INT32)i * (L->rh + L->rgap);
		INT32 rw = L->pw - 2 * L->pad;
		float bd = (float)L->rh * 0.60f; /* badge diameter */
		float bcx = (float)rx + (float)L->rh * 0.5f + 2.0f * L->s;
		float bcy = (float)ry + (float)L->rh * 0.5f;
		const struct bootfont_face *f = sel ? L->f_entry_sel : L->f_entry;
		INT32 key = imax(iround((float)L->f_hint->line * 1.25f),
				 iround(26.0f * L->s));
		INT32 kx = rx + rw - L->pad - key, ky = ry + (L->rh - key) / 2;
		INT32 tx = rx + L->rh + iround(10.0f * L->s);
		INT32 ty = ry + (L->rh - (INT32)f->line) / 2 + f->ascent;
		char num[2];

		if (sel) {
			gfx_round_rect(rx, ry, rw, L->rh, L->row_radius, COL_ACCENT);
			gfx_round_rect_line(rx, ry, rw, L->rh, L->row_radius, 1,
					    COL_ACCENT_LINE);
		}
		gfx_disc(bcx, bcy, bd * 0.5f, sel ? COL_BADGE_SEL : COL_BADGE);
		if (entry_installs(&m->e[i]))
			icon_install(bcx, bcy, bd, sel ? COL_ICON_SEL : COL_ICON);
		else
			icon_play(bcx, bcy, bd, sel ? COL_ICON_SEL : COL_ICON);
		gfx_text_fit(f, tx, ty, m->e[i].title, kx - iround(12.0f * L->s) - tx,
			     sel ? COL_TEXT_SEL : COL_TEXT);
		/* the number key that starts this entry */
		num[0] = (char)('1' + i);
		num[1] = 0;
		gfx_round_rect_line(kx, ky, key, key, imax(3, key / 4), 1,
				    sel ? COL_KEY_LINE_SEL : COL_KEY_LINE);
		gfx_text(L->f_hint,
			 kx + (key - gfx_text_width(L->f_hint, num)) / 2,
			 ky + (key - (INT32)L->f_hint->line) / 2 + L->f_hint->ascent,
			 num, sel ? COL_TEXT_SEL : COL_KEY_TEXT);
	}
}

static void draw_countdown(const struct menu *m, const struct layout *L)
{
	/* hint segments: key names bright, words dim, separators dimmer */
	struct seg {
		const char *t;
		gfx_color c;
	} segs[8];
	char cnt[48];
	UINTN n = 0;
	INT32 total = 0, x;

	gfx_restore_background(0, L->cnt_y, L->W, L->cnt_h);
	if (m->counting && m->ticks_total) {
		INT32 bx = (L->W - L->bar_w) / 2;
		INT32 fill = (INT32)((UINT64)L->bar_w * m->ticks_left /
				     m->ticks_total);

		gfx_round_rect(bx, L->bar_y, L->bar_w, L->bar_h, L->bar_h / 2,
			       COL_TRACK);
		if (fill > 0)
			gfx_round_rect(bx + (L->bar_w - fill) / 2, L->bar_y, fill,
				       L->bar_h, L->bar_h / 2, COL_ACCENT);
	}
	segs[n].t = "\xe2\x86\x91 \xe2\x86\x93"; /* up, down arrows */
	segs[n++].c = COL_HINT_KEY;
	segs[n].t = "  to choose";
	segs[n++].c = COL_HINT;
	segs[n].t = "     \xc2\xb7     "; /* middle dot */
	segs[n++].c = COL_HINT_DOT;
	segs[n].t = "Enter";
	segs[n++].c = COL_HINT_KEY;
	segs[n].t = "  to start";
	segs[n++].c = COL_HINT;
	if (m->counting) {
		UINT32 secs = (m->ticks_left + 9) / 10;
		UINTN k = 0;
		const char *pre = "Starting in ";
		char dec[12];
		INT32 dn = 0;

		while (pre[k]) {
			cnt[k] = pre[k];
			k++;
		}
		do {
			dec[dn++] = (char)('0' + secs % 10);
			secs /= 10;
		} while (secs && dn < 10);
		while (dn > 0)
			cnt[k++] = dec[--dn];
		cnt[k++] = ' ';
		cnt[k++] = 's';
		cnt[k] = 0;
		segs[n].t = "     \xc2\xb7     ";
		segs[n++].c = COL_HINT_DOT;
		segs[n].t = cnt;
		segs[n++].c = COL_ACCENT_LIGHT;
	}
	for (UINTN i = 0; i < n; i++)
		total += gfx_text_width(L->f_hint, segs[i].t);
	x = (L->W - total) / 2;
	for (UINTN i = 0; i < n; i++)
		x = gfx_text(L->f_hint, x, L->hint_y, segs[i].t, segs[i].c);
}

static void draw_static(const struct menu *m, const struct layout *L)
{
	const char *sub = "Choose how to start";

	gfx_vgradient(0, 0, L->W, L->H, COL_BG_TOP, COL_BG_BOTTOM);
	gfx_glow(L->W / 2, L->logo_y + L->logo / 2, iround((float)L->W * 0.42f),
		 iround((float)L->H * 0.42f), COL_GLOW);
	gfx_glow(L->W / 6, L->H * 7 / 8, iround((float)L->W * 0.35f),
		 iround((float)L->H * 0.30f), COL_GLOW2);
	gfx_image(bootlogo_argb, BOOTLOGO_W, BOOTLOGO_H, L->logo_x, L->logo_y,
		  L->logo, L->logo);
	gfx_text(L->f_title, (L->W - gfx_text_width(L->f_title, m->heading)) / 2,
		 L->title_y, m->heading, COL_TITLE);
	gfx_text(L->f_sub, (L->W - gfx_text_width(L->f_sub, sub)) / 2, L->sub_y,
		 sub, COL_SUBTITLE);
}

/* ---- text menu ----------------------------------------------------------- */

/* Up to maxcols characters of UTF-8 text, one column per code point (a
 * multi-byte sequence is shown as one '?', never cut in the middle); the
 * number of columns written. */
static UINTN con_utf8(const char *s, UINTN maxcols)
{
	CHAR16 out[MENU_TITLE_MAX + 1];
	UINTN o = 0;
	const unsigned char *p = (const unsigned char *)s;

	while (*p && o < MENU_TITLE_MAX && o < maxcols) {
		UINT32 c = *p++;

		if (c >= 0x80) {
			/* not every console font has more than ASCII */
			UINT32 need = (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : 3;

			while (need-- && (*p & 0xc0) == 0x80)
				p++;
			c = '?';
		}
		out[o++] = (CHAR16)c;
	}
	out[o] = 0;
	uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, out);
	return o;
}

static void con_at(UINTN col, UINTN row)
{
	uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, col, row);
}

static void con_attr(UINTN a)
{
	uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, a);
}

static void con_spaces(UINTN n)
{
	CHAR16 sp[2] = { ' ', 0 };

	while (n--)
		uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, sp);
}

static void text_draw(const struct menu *m, BOOLEAN full)
{
	UINTN cols = 80, rows = 25, top, w = 60, left;

	uefi_call_wrapper(ST->ConOut->QueryMode, 4, ST->ConOut,
			  (UINTN)ST->ConOut->Mode->Mode, &cols, &rows);
	if (cols < 40)
		cols = 40;
	if (w + 4 > cols)
		w = cols - 4;
	left = (cols - w) / 2;
	top = rows > m->n + 8 ? (rows - (m->n + 8)) / 2 : 0;
	if (full) {
		con_attr(EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK));
		uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);
		con_attr(EFI_TEXT_ATTR(EFI_WHITE, EFI_BLACK));
		con_at(left, top);
		con_utf8(m->heading, w);
		con_attr(EFI_TEXT_ATTR(EFI_DARKGRAY, EFI_BLACK));
		con_at(left, top + 1);
		con_utf8("Choose how to start", w);
	}
	for (UINT32 i = 0; i < m->n; i++) {
		char lead[5] = { ' ', (char)('1' + i), '.', ' ', 0 };

		con_at(left, top + 3 + i);
		con_attr(i == m->sel ? EFI_TEXT_ATTR(EFI_BLACK, EFI_LIGHTGRAY) :
				       EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK));
		{
			UINTN len = con_utf8(lead, w);

			len += con_utf8(m->e[i].title, w - 5);
			con_spaces(len < w ? w - len : 0);
		}
	}
	con_attr(EFI_TEXT_ATTR(EFI_DARKGRAY, EFI_BLACK));
	con_at(left, top + 4 + m->n);
	if (m->counting) {
		char line[80];
		UINT32 secs = (m->ticks_left + 9) / 10;
		const char *pre = "Up/Down to choose, Enter to start.  Starting in ";
		UINTN k = 0;
		char dec[12];
		INT32 dn = 0;

		while (pre[k] && k < 60) {
			line[k] = pre[k];
			k++;
		}
		do {
			dec[dn++] = (char)('0' + secs % 10);
			secs /= 10;
		} while (secs && dn < 10);
		while (dn > 0)
			line[k++] = dec[--dn];
		line[k++] = ' ';
		line[k++] = 's';
		line[k] = 0;
		k = con_utf8(line, w);
		con_spaces(k < w ? w - k : 0);
	} else {
		const char *t = "Up/Down to choose, Enter to start.";

		UINTN len = con_utf8(t, w);

		con_spaces(len < w ? w - len : 0);
	}
	con_attr(EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK));
}

/* ---- the loop ---------------------------------------------------------------- */

enum { ACT_NONE, ACT_MOVED, ACT_START };

static int menu_key(struct menu *m, const EFI_INPUT_KEY *k)
{
	UINT32 old = m->sel;

	switch (k->ScanCode) {
	case KEY_SCAN_UP:
		m->sel = m->sel ? m->sel - 1 : m->n - 1;
		break;
	case KEY_SCAN_DOWN:
		m->sel = m->sel + 1 < m->n ? m->sel + 1 : 0;
		break;
	case KEY_SCAN_HOME:
	case KEY_SCAN_PAGE_UP:
		m->sel = 0;
		break;
	case KEY_SCAN_END:
	case KEY_SCAN_PAGE_DOWN:
		m->sel = m->n - 1;
		break;
	case KEY_SCAN_ESC:
		break;
	default:
		if (k->UnicodeChar == '\r' || k->UnicodeChar == '\n')
			return ACT_START;
		if (k->UnicodeChar >= '1' && k->UnicodeChar < '1' + m->n) {
			m->sel = (UINT32)(k->UnicodeChar - '1');
			return ACT_START;
		}
		break;
	}
	return m->sel != old ? ACT_MOVED : ACT_NONE;
}

INT32 bootmenu_run(EFI_HANDLE image, char *args, UINTN cap)
{
	static struct menu m; /* 3 KiB: kept off the firmware's stack */
	struct layout L;
	EFI_EVENT timer = NULL, events[2];
	BOOLEAN gfx = FALSE;
	UINTN len = 0;
	char *conf;

	if (cap)
		args[0] = 0;
	conf = menu_read_conf(image, &len);
	if (!conf) {
		bl_log("bootmenu: no \\EFI\\LIKEOS\\bootmenu.conf, no menu\n");
		return -1;
	}
	menu_parse(&m, conf, len);
	uefi_call_wrapper(BS->FreePool, 1, conf);
	if (m.n == 0) {
		bl_log("bootmenu: bootmenu.conf has no entries, no menu\n");
		return -1;
	}

	/* No firmware watchdog while somebody reads the menu (it resets the
	 * machine after five minutes); nothing typed before the menu counts. */
	uefi_call_wrapper(BS->SetWatchdogTimer, 4, 0, 0, 0, NULL);
	uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
	uefi_call_wrapper(ST->ConOut->EnableCursor, 2, ST->ConOut, FALSE);

	m.counting = m.timeout > 0;
	m.ticks_total = m.ticks_left = m.timeout * 10;
	menu_log_state(&m);

	if (gfx_init() == 0) {
		menu_layout(&m, &L);
		draw_static(&m, &L);
		gfx = gfx_save_background();
		if (gfx) {
			draw_panel(&m, &L);
			draw_countdown(&m, &L);
			gfx_present(0, 0, L.W, L.H);
			bl_log("bootmenu: graphical, ");
			bl_log_dec((UINT64)L.W);
			bl_log("x");
			bl_log_dec((UINT64)L.H);
			bl_log("\n");
		} else {
			gfx_shutdown();
		}
	}
	if (!gfx) {
		bl_log("bootmenu: text console\n");
		text_draw(&m, TRUE);
	}

	if (m.counting &&
	    (EFI_ERROR(uefi_call_wrapper(BS->CreateEvent, 5, EVT_TIMER, 0, NULL,
					 NULL, &timer)) ||
	     EFI_ERROR(uefi_call_wrapper(BS->SetTimer, 3, timer, TimerPeriodic,
					 MENU_TICK_100NS)))) {
		/* no timer: start the default entry rather than wait for ever
		 * on a menu that promised to start by itself */
		bl_log("bootmenu: no timer available, starting the default entry\n");
		if (timer)
			uefi_call_wrapper(BS->CloseEvent, 1, timer);
		timer = NULL;
		m.ticks_left = 0;
	}

	while (!(m.counting && m.ticks_left == 0)) {
		UINTN idx = 0, nev = 0;
		EFI_INPUT_KEY key;
		int act = ACT_NONE;

		events[nev++] = ST->ConIn->WaitForKey;
		if (timer && m.counting)
			events[nev++] = timer;
		if (EFI_ERROR(uefi_call_wrapper(BS->WaitForEvent, 3, nev, events,
						&idx)))
			idx = 0;
		if (nev == 2 && idx == 1) {
			if (m.ticks_left)
				m.ticks_left--;
			if (m.ticks_left == 0)
				break;
			if (m.ticks_left % 10 == 0 || gfx) {
				if (gfx) {
					draw_countdown(&m, &L);
					gfx_present(0, L.cnt_y, L.W, L.cnt_h);
				} else {
					text_draw(&m, FALSE);
				}
			}
			continue;
		}
		if (EFI_ERROR(uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2,
						ST->ConIn, &key)))
			continue;
		if (m.counting) {
			/* the first key stops the countdown */
			m.counting = FALSE;
			if (timer) {
				uefi_call_wrapper(BS->SetTimer, 3, timer,
						  TimerCancel, 0);
			}
			bl_log("bootmenu: countdown stopped\n");
			if (gfx) {
				draw_countdown(&m, &L);
				gfx_present(0, L.cnt_y, L.W, L.cnt_h);
			}
		}
		act = menu_key(&m, &key);
		if (act == ACT_MOVED || act == ACT_START) {
			if (gfx) {
				draw_panel(&m, &L);
				gfx_present(L.dyn_x, L.dyn_y, L.dyn_w, L.dyn_h);
			}
			if (act == ACT_MOVED) {
				bl_log("bootmenu: entry ");
				bl_log_dec(m.sel + 1);
				bl_log(" selected\n");
			}
		}
		if (!gfx)
			text_draw(&m, FALSE);
		if (act == ACT_START)
			break;
	}
	if (timer)
		uefi_call_wrapper(BS->CloseEvent, 1, timer);

	bl_log("bootmenu: starting entry ");
	bl_log_dec(m.sel + 1);
	bl_log(": ");
	bl_log(m.e[m.sel].title);
	if (m.e[m.sel].args[0]) {
		bl_log(" (");
		bl_log(m.e[m.sel].args);
		bl_log(")");
	}
	bl_log("\n");

	/* Back to a plain console for the loader's own messages. */
	if (gfx) {
		gfx_clear_screen();
		gfx_shutdown();
	}
	uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut,
			  EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK));
	uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);
	uefi_call_wrapper(ST->ConOut->EnableCursor, 2, ST->ConOut, TRUE);

	m_strcpy(args, cap, m.e[m.sel].args);
	return (INT32)m.sel;
}
