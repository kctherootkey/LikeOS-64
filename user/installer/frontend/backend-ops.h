/* LikeOS installer -- the backend as a table of functions.
 *
 * likeos-installer carries two backends: libinstaller, which really
 * installs (linked in by the LikeOS build and by the host build made with
 * BACKEND=real), and the stand-in of mock/il-mock.c, which makes up disks
 * and pretends to install (--mock; the only backend of a build without
 * libinstaller).  Both implement installer.h.  The stand-in is compiled
 * under other names (mock/il-mock-names.h), so the two never clash, and
 * each fills one IlBackend with its functions; main() picks one at start
 * (il_be, backend-api.h) and every call of the wizard goes through it.
 *
 * Each table is filled with IL_BACKEND_FUNCTIONS, the list of every
 * function of installer.h under its il_ name: in backend-real.c those are
 * libinstaller's, in il-mock.c the renamed stand-ins.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_INSTALLER_BACKEND_OPS_H
#define LIKEOS_INSTALLER_BACKEND_OPS_H

#include "installer.h"

typedef struct IlBackend {
	const char *name;        /* for --version and the log */
	int is_mock;             /* the stand-in: nothing is ever written */

	/* installer.h, in its order (without the il_ prefix) */
	il_ctx *(*ctx_new)(const struct il_options *opt);
	void (*ctx_free)(il_ctx *ctx);
	const char *(*last_error)(const il_ctx *ctx);
	const char *(*version)(void);
	void (*log)(il_ctx *ctx, int level, const char *fmt, ...);
	int (*disks_scan)(il_ctx *ctx, struct il_disk_list *out);
	void (*disk_describe)(const struct il_disk *d, int index, char *buf,
			size_t len);
	void (*format_size)(uint64_t bytes, char *buf, size_t len);
	int (*source_probe)(il_ctx *ctx, struct il_source_info *out);
	int (*source_verify)(il_ctx *ctx, void (*progress)(double fraction,
			void *user), void *user);
	il_plan *(*plan_new)(il_ctx *ctx, const struct il_disk *disk);
	void (*plan_free)(il_plan *plan);
	int (*plan_use_whole_disk)(il_plan *plan);
	int (*plan_new_table)(il_plan *plan);
	int (*plan_delete)(il_plan *plan, int partno);
	int (*plan_create)(il_plan *plan, int free_index, uint64_t size_bytes,
			int *partno_out);
	int (*plan_extend)(il_plan *plan, int partno, uint64_t new_size_bytes);
	int (*plan_format)(il_plan *plan, int partno);
	int (*plan_select_root)(il_plan *plan, int partno);
	int (*plan_root)(const il_plan *plan);
	int (*plan_esp)(const il_plan *plan);
	int (*plan_view)(const il_plan *plan, struct il_disk *out);
	int (*plan_nops)(const il_plan *plan);
	const struct il_op *(*plan_op)(const il_plan *plan, int i);
	void (*op_describe)(const struct il_op *op, char *buf, size_t len);
	int (*plan_validate)(const il_plan *plan,
			const struct il_source_info *src, char *why,
			size_t len);
	const struct il_disk *(*plan_disk)(const il_plan *plan);
	void (*config_defaults)(struct il_config *cfg);
	int (*validate_hostname)(const char *name, char *why, size_t len);
	int (*validate_username)(const char *name, char *why, size_t len);
	int (*validate_password)(const char *pw, char *why, size_t len);
	int (*password_strength)(const char *pw);
	void (*suggest_username)(const char *fullname, char *buf, size_t len);
	void (*suggest_hostname)(char *buf, size_t len);
	int (*timezone_count)(void);
	const char *(*timezone_name)(int i);
	const char *(*timezone_label)(int i);
	int (*locale_count)(void);
	const char *(*locale_name)(int i);
	const char *(*locale_label)(int i);
	const char *(*step_title)(int step);
	double (*step_weight)(int step);
	int (*install)(il_ctx *ctx, il_plan *plan, const struct il_config *cfg,
			il_progress_cb cb, void *user);
	void (*cancel)(il_ctx *ctx);
	int (*can_cancel)(const il_ctx *ctx);
	int (*reboot)(il_ctx *ctx);
	int (*poweroff)(il_ctx *ctx);
	int (*answers_load)(il_ctx *ctx, const char *path,
			struct il_answers *out);
	int (*answers_save)(il_ctx *ctx, const char *path,
			const struct il_answers *a, int include_passwords);
	int (*timezone_find)(const char *name);
	int (*locale_find)(const char *name);
	int (*timezone_offset_minutes)(int i);
	const struct il_disk *(*disks_find)(const struct il_disk_list *l,
			const char *which);
	const struct il_partition *(*disk_part)(const struct il_disk *d,
			int partno);
	int (*config_validate)(const struct il_config *cfg, char *why,
			size_t len);
	int (*answers_plan)(il_ctx *ctx, const struct il_answers *a,
			const struct il_disk_list *disks, il_plan **out);
	void (*plan_summary)(const il_plan *plan, char *buf, size_t len);
	const char *(*log_path)(const il_ctx *ctx);
	int (*plan_can_select_root)(const il_plan *plan, int partno,
			const struct il_source_info *src, char *why,
			size_t len);
	int (*plan_clear_root)(il_plan *plan);
} IlBackend;

/* Fills the function members of an IlBackend with the il_ functions this
 * translation unit sees. */
#define IL_BACKEND_FUNCTIONS \
	.ctx_new = il_ctx_new, \
	.ctx_free = il_ctx_free, \
	.last_error = il_last_error, \
	.version = il_version, \
	.log = il_log, \
	.disks_scan = il_disks_scan, \
	.disk_describe = il_disk_describe, \
	.format_size = il_format_size, \
	.source_probe = il_source_probe, \
	.source_verify = il_source_verify, \
	.plan_new = il_plan_new, \
	.plan_free = il_plan_free, \
	.plan_use_whole_disk = il_plan_use_whole_disk, \
	.plan_new_table = il_plan_new_table, \
	.plan_delete = il_plan_delete, \
	.plan_create = il_plan_create, \
	.plan_extend = il_plan_extend, \
	.plan_format = il_plan_format, \
	.plan_select_root = il_plan_select_root, \
	.plan_root = il_plan_root, \
	.plan_esp = il_plan_esp, \
	.plan_view = il_plan_view, \
	.plan_nops = il_plan_nops, \
	.plan_op = il_plan_op, \
	.op_describe = il_op_describe, \
	.plan_validate = il_plan_validate, \
	.plan_disk = il_plan_disk, \
	.config_defaults = il_config_defaults, \
	.validate_hostname = il_validate_hostname, \
	.validate_username = il_validate_username, \
	.validate_password = il_validate_password, \
	.password_strength = il_password_strength, \
	.suggest_username = il_suggest_username, \
	.suggest_hostname = il_suggest_hostname, \
	.timezone_count = il_timezone_count, \
	.timezone_name = il_timezone_name, \
	.timezone_label = il_timezone_label, \
	.locale_count = il_locale_count, \
	.locale_name = il_locale_name, \
	.locale_label = il_locale_label, \
	.step_title = il_step_title, \
	.step_weight = il_step_weight, \
	.install = il_install, \
	.cancel = il_cancel, \
	.can_cancel = il_can_cancel, \
	.reboot = il_reboot, \
	.poweroff = il_poweroff, \
	.answers_load = il_answers_load, \
	.answers_save = il_answers_save, \
	.timezone_find = il_timezone_find, \
	.locale_find = il_locale_find, \
	.timezone_offset_minutes = il_timezone_offset_minutes, \
	.disks_find = il_disks_find, \
	.disk_part = il_disk_part, \
	.config_validate = il_config_validate, \
	.answers_plan = il_answers_plan, \
	.plan_summary = il_plan_summary, \
	.log_path = il_log_path, \
	.plan_can_select_root = il_plan_can_select_root, \
	.plan_clear_root = il_plan_clear_root

/* The stand-in (always there) and libinstaller (when it is linked in:
 * INSTALLER_HAVE_REAL). */
extern const IlBackend il_backend_mock;
#ifdef INSTALLER_HAVE_REAL
extern const IlBackend il_backend_real;
#endif

#endif
