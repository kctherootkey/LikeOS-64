/* LikeOS installer -- the names the stand-in backend is compiled under.
 *
 * Included by mock/il-mock.c before installer.h: every function of
 * installer.h is declared and defined there as ilmock_<name>, so the
 * stand-in and libinstaller can be linked into one program.  The stand-in
 * is reached through il_backend_mock (backend-ops.h) only.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_INSTALLER_IL_MOCK_NAMES_H
#define LIKEOS_INSTALLER_IL_MOCK_NAMES_H

#define il_ctx_new ilmock_ctx_new
#define il_ctx_free ilmock_ctx_free
#define il_last_error ilmock_last_error
#define il_version ilmock_version
#define il_log ilmock_log
#define il_disks_scan ilmock_disks_scan
#define il_disk_describe ilmock_disk_describe
#define il_format_size ilmock_format_size
#define il_source_probe ilmock_source_probe
#define il_source_verify ilmock_source_verify
#define il_plan_new ilmock_plan_new
#define il_plan_free ilmock_plan_free
#define il_plan_use_whole_disk ilmock_plan_use_whole_disk
#define il_plan_new_table ilmock_plan_new_table
#define il_plan_delete ilmock_plan_delete
#define il_plan_create ilmock_plan_create
#define il_plan_extend ilmock_plan_extend
#define il_plan_format ilmock_plan_format
#define il_plan_select_root ilmock_plan_select_root
#define il_plan_root ilmock_plan_root
#define il_plan_esp ilmock_plan_esp
#define il_plan_view ilmock_plan_view
#define il_plan_nops ilmock_plan_nops
#define il_plan_op ilmock_plan_op
#define il_op_describe ilmock_op_describe
#define il_plan_validate ilmock_plan_validate
#define il_plan_disk ilmock_plan_disk
#define il_config_defaults ilmock_config_defaults
#define il_validate_hostname ilmock_validate_hostname
#define il_validate_username ilmock_validate_username
#define il_validate_password ilmock_validate_password
#define il_password_strength ilmock_password_strength
#define il_suggest_username ilmock_suggest_username
#define il_suggest_hostname ilmock_suggest_hostname
#define il_timezone_count ilmock_timezone_count
#define il_timezone_name ilmock_timezone_name
#define il_timezone_label ilmock_timezone_label
#define il_locale_count ilmock_locale_count
#define il_locale_name ilmock_locale_name
#define il_locale_label ilmock_locale_label
#define il_step_title ilmock_step_title
#define il_step_weight ilmock_step_weight
#define il_install ilmock_install
#define il_cancel ilmock_cancel
#define il_can_cancel ilmock_can_cancel
#define il_reboot ilmock_reboot
#define il_poweroff ilmock_poweroff
#define il_answers_load ilmock_answers_load
#define il_answers_save ilmock_answers_save
#define il_timezone_find ilmock_timezone_find
#define il_locale_find ilmock_locale_find
#define il_timezone_offset_minutes ilmock_timezone_offset_minutes
#define il_disks_find ilmock_disks_find
#define il_disk_part ilmock_disk_part
#define il_config_validate ilmock_config_validate
#define il_answers_plan ilmock_answers_plan
#define il_plan_summary ilmock_plan_summary
#define il_log_path ilmock_log_path
#define il_plan_can_select_root ilmock_plan_can_select_root
#define il_plan_clear_root ilmock_plan_clear_root

#endif
