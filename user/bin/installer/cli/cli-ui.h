/* likeos-install -- the text terminal: prompts, menus, tables, the
 * progress bar.
 *
 * Everything reads the standard input and writes the standard output.  On
 * a terminal the prompts edit a line, passwords are read without echo and
 * the progress bar redraws itself in place; on a pipe or file the same
 * questions read one line each and the progress is printed as plain lines,
 * so the program can be driven by another program or logged.
 *
 * Ctrl-C (SIGINT) and the end of the input make every prompt return
 * UI_CANCEL.  Typing "back" (or "<") returns UI_BACK where going back makes
 * sense.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_INSTALL_CLI_UI_H
#define LIKEOS_INSTALL_CLI_UI_H

#include <signal.h>
#include <stddef.h>
#include <stdint.h>

#define UI_OK      0
#define UI_BACK    1
#define UI_CANCEL  2

/* Set by the SIGINT handler (count of Ctrl-C presses). */
extern volatile sig_atomic_t ui_interrupted;
/* Set by ui_error(); a caller about to redraw the screen clears it after
 * letting the message be read. */
extern int ui_error_shown;

/* Sets up the terminal: width, colours, the SIGINT handler (without
 * SA_RESTART, so a prompt sees it).  `claim_tty': a program started
 * straight from init on the console has no controlling terminal and gets
 * no Ctrl-C; this makes the terminal on standard input its own. */
void ui_init(int claim_tty);
/* Puts the terminal back (echo, line mode).  ui_init() also arranges this
 * for exit() and for SIGTERM, SIGHUP and SIGQUIT. */
void ui_restore(void);
/* Standard input to a sane line mode (ICANON, ECHO, ECHOE, ECHOK, ISIG,
 * VMIN 1, VTIME 0, ICRNL, OPOST, ONLCR), whatever it was before: 0, or
 * -1 with errno (not a terminal). */
int ui_reset_tty(void);

int ui_is_tty(void);         /* standard input AND output are terminals */
int ui_out_tty(void);        /* standard output is a terminal */
int ui_cols(void);           /* width of the terminal (80 when unknown) */

/* ---- text ------------------------------------------------------------ */

void ui_clear(void);                       /* a fresh screen on a terminal */
void ui_header(const char *title, int step, int nsteps);
void ui_para(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ui_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ui_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ui_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ui_blank(void);
/* Bold / dim when colours are on, plain otherwise. */
const char *ui_bold(void);
const char *ui_dim(void);
const char *ui_reset(void);

/* Display width of a UTF-8 string (one column per character). */
int ui_width(const char *s);
/* `s' cut to `cols' columns (with "..." when cut) and padded to it. */
void ui_fit(char *out, size_t len, const char *s, int cols, int pad);

/* ---- input ----------------------------------------------------------- */

/* A line of input after `prompt' (and "[def]" when `def' is given; an empty
 * answer is the default then).  UI_OK, UI_BACK ("back", "<") or UI_CANCEL. */
int ui_line(const char *prompt, const char *def, char *buf, size_t len);
/* The same without echo (passwords).  No default, no "back". */
int ui_secret(const char *prompt, char *buf, size_t len);
/* yes / no; `def' is 1 or 0 (Enter), -1 for no default.  *answer is set. */
int ui_yes_no(const char *prompt, int def, int *answer);
/* A number from lo to hi, `def' (or -1 for none) on Enter. */
int ui_number(const char *prompt, int lo, int hi, int def, int *out);
/* A numbered menu of `n' items (shown 1..n); *choice gets the index.
 * `def' is the index preselected on Enter, -1 for none. */
int ui_menu(const char *prompt, const char *const *items, int n, int def,
	    int *choice);

/* ---- progress ------------------------------------------------------------ */

struct ui_progress_state {
	int last_step;
	int last_pct;         /* plain output: last percentage printed */
	int drawn;            /* a bar is on the current line */
	long long last_draw_ms;
};

void ui_progress_begin(struct ui_progress_state *s);
/* `step_title' of step `step' (0-based) of `nsteps'; `percent' 0..100, the
 * backend's one percentage (il_progress.percent), shown as it is;
 * `counts' ("5,396 of 16,624 files") and `detail' may be NULL; `eta'
 * seconds (already smoothed by the backend) or -1 when not known yet. */
void ui_progress(struct ui_progress_state *s, int step, int nsteps,
		 const char *step_title, int percent, const char *counts,
		 const char *detail, int eta);
/* "less than a minute left", "about 5 minutes left"; "" for -1. */
void ui_eta_text(int eta, char *buf, size_t len);
/* 16624 -> "16,624" */
void ui_group_digits(unsigned long long v, char *buf, size_t len);
/* Step `step' finished (a tick line on a terminal). */
void ui_progress_step_done(struct ui_progress_state *s, const char *title);
/* Clears the bar before other output; the next ui_progress() redraws. */
void ui_progress_clear(struct ui_progress_state *s);
void ui_progress_end(struct ui_progress_state *s);

long long ui_now_ms(void);

#endif /* LIKEOS_INSTALL_CLI_UI_H */
