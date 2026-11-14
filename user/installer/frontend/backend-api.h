/* LikeOS installer -- calling the backend chosen at start.
 *
 * Included (through app.h) by every part of the wizard: after it, a call
 * written il_plan_view(plan, &view) goes to il_be->plan_view, the function
 * of the backend main() chose (see backend-ops.h).  The code reads as calls
 * of installer.h; which library answers is decided once, at run time.
 *
 * il_log is the exception: the wizard logs through app_log() (app.h),
 * which keeps to the backend's rule that a context is used by one thread at
 * a time and holds lines back while a job runs on the worker thread.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_INSTALLER_BACKEND_API_H
#define LIKEOS_INSTALLER_BACKEND_API_H

#include "backend-ops.h"

/* The backend in use; set by main() before anything else runs. */
extern const IlBackend *il_be;

#define il_ctx_new (il_be->ctx_new)
#define il_ctx_free (il_be->ctx_free)
#define il_last_error (il_be->last_error)
#define il_version (il_be->version)
#define il_disks_scan (il_be->disks_scan)
#define il_disk_describe (il_be->disk_describe)
#define il_format_size (il_be->format_size)
#define il_source_probe (il_be->source_probe)
#define il_source_verify (il_be->source_verify)
#define il_plan_new (il_be->plan_new)
#define il_plan_free (il_be->plan_free)
#define il_plan_use_whole_disk (il_be->plan_use_whole_disk)
#define il_plan_new_table (il_be->plan_new_table)
#define il_plan_delete (il_be->plan_delete)
#define il_plan_create (il_be->plan_create)
#define il_plan_extend (il_be->plan_extend)
#define il_plan_format (il_be->plan_format)
#define il_plan_select_root (il_be->plan_select_root)
#define il_plan_root (il_be->plan_root)
#define il_plan_esp (il_be->plan_esp)
#define il_plan_view (il_be->plan_view)
#define il_plan_nops (il_be->plan_nops)
#define il_plan_op (il_be->plan_op)
#define il_op_describe (il_be->op_describe)
#define il_plan_validate (il_be->plan_validate)
#define il_plan_disk (il_be->plan_disk)
#define il_config_defaults (il_be->config_defaults)
#define il_validate_hostname (il_be->validate_hostname)
#define il_validate_username (il_be->validate_username)
#define il_validate_password (il_be->validate_password)
#define il_password_strength (il_be->password_strength)
#define il_suggest_username (il_be->suggest_username)
#define il_suggest_hostname (il_be->suggest_hostname)
#define il_timezone_count (il_be->timezone_count)
#define il_timezone_name (il_be->timezone_name)
#define il_timezone_label (il_be->timezone_label)
#define il_locale_count (il_be->locale_count)
#define il_locale_name (il_be->locale_name)
#define il_locale_label (il_be->locale_label)
#define il_step_title (il_be->step_title)
#define il_step_weight (il_be->step_weight)
#define il_install (il_be->install)
#define il_cancel (il_be->cancel)
#define il_can_cancel (il_be->can_cancel)
#define il_reboot (il_be->reboot)
#define il_poweroff (il_be->poweroff)
#define il_answers_load (il_be->answers_load)
#define il_answers_save (il_be->answers_save)
#define il_timezone_find (il_be->timezone_find)
#define il_locale_find (il_be->locale_find)
#define il_timezone_offset_minutes (il_be->timezone_offset_minutes)
#define il_disks_find (il_be->disks_find)
#define il_disk_part (il_be->disk_part)
#define il_config_validate (il_be->config_validate)
#define il_answers_plan (il_be->answers_plan)
#define il_plan_summary (il_be->plan_summary)
#define il_log_path (il_be->log_path)
#define il_plan_can_select_root (il_be->plan_can_select_root)
#define il_plan_clear_root (il_be->plan_clear_root)
#define il_log il_log_is_called_through_app_log

#endif
