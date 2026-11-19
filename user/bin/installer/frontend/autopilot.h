/* LikeOS installer -- the autopilot: a whole installation without a
 * person, for tests.
 *
 * likeos-installer --autopilot=FILE (or LIKEOS_INSTALLER_AUTOPILOT=FILE)
 * reads an answer file (likeos-install.conf(5), the format of the
 * text-mode installer) and goes through the wizard as a person would: on
 * every page it fills in the answers through the page's own controls
 * (Page.autofill), presses Next, answers the questions that come up with
 * their main button, waits for the installation and presses Restart now at
 * the end (or leaves setup when the file says reboot=no).  A page that does
 * not move on, a failed installation or an answer that cannot be given
 * ends the program with status 3 (APP_EXIT_AUTOPILOT).
 *
 * Further environment:
 *   LIKEOS_INSTALLER_AUTOPILOT_DELAY=MS  the pause between actions
 *                                        (default 400)
 *   LIKEOS_INSTALLER_AUTOPILOT_SHOTS=DIR a PNG of the window on every page
 *                                        and step, before Next is pressed
 *   LIKEOS_INSTALLER_AUTOPILOT_RETRIES=N on the error page, press Try again
 *                                        N times before giving up
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_INSTALLER_AUTOPILOT_H
#define LIKEOS_INSTALLER_AUTOPILOT_H

#include "app.h"

/* FALSE (with a message on stderr) when the file cannot be used. */
gboolean autopilot_start(App *app, const char *answers_path);
void autopilot_free(App *app);
/* The wizard shows another page (wizard_go). */
void autopilot_page_entered(App *app, int page);

#endif
