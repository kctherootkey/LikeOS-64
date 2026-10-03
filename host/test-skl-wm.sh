#!/bin/sh
# Host test for the Skylake-and-later watermark arithmetic
# (kernel/dev/gpu/i915/display/intel_skl_wm.c): the per-plane parameters,
# the two latency methods and the level computation, run against hand
# worked numbers for the panel of the machine this was written for and
# for a display version 12 part.  A tiled surface is fetched in whole
# tile rows and needs many times the buffered data a linear one does;
# an X tiled one on display version 9 carries the bandwidth workaround's
# extra latency.  The functions are pure integer arithmetic over a small
# global (g_wm), so they are taken out of the driver file and built on
# the host with a stand-in for that global.  Run from the repository
# root.
#
# Copyright (C) 2026 The LikeOS Project

set -e
SRC=kernel/dev/gpu/i915/display/intel_skl_wm.c
TMP=${TMPDIR:-/tmp}/likeos-sklwm-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
fn() { sed -n "/^$1/,/^}/p" "$SRC"; }
{
	cat <<'PRE'
#include <stdint.h>
#include <string.h>
struct i915_device { int unused; };
struct intel_wm_pipe_cfg { uint32_t htotal; };
static struct {
	struct i915_device *i915;
	int ver;
	int num_levels;
	uint16_t latency[8];
	int ipc_enabled;
} g_wm;
static int is_kbl_cfl(struct i915_device *i915) { (void)i915; return 0; }
static int has_sagv_wm(struct i915_device *i915) { (void)i915; return g_wm.ver >= 12; }
static uint32_t intel_sagv_block_time_us(struct i915_device *i915) { (void)i915; return 0; }
#define mm_memset memset
#define DRM_FORMAT_MOD_LINEAR 0ULL
#define I915_FORMAT_MOD_X_TILED ((1ULL << 56) | 1)
#define I915_FORMAT_MOD_Y_TILED ((1ULL << 56) | 2)
PRE
	sed -n '/^#define U16_MAX_/p; /^#define U32_MAX_/p' "$SRC"
	sed -n '/^static uint32_t umin(/p; /^static uint32_t umax(/p' "$SRC"
	fn 'static uint32_t div_round_up(uint64_t'
	sed -n '/^#define FP_MAX/p' "$SRC"
	for f in fp_clamp u32_to_fp fp_to_u32_round_up div_fp mul_u32_fp mul_round_up_u32_fp \
		 add_fp_u32 div_round_up_fp; do
		fn "static uint32_t $f("
	done
	sed -n '/^struct skl_wm_level {/,/^};/p' "$SRC"
	sed -n '/^struct skl_wm_params {/,/^};/p' "$SRC"
	fn 'static uint32_t skl_wm_latency('
	fn 'static void skl_compute_wm_params('
	fn 'static uint32_t skl_wm_method1('
	fn 'static uint32_t skl_wm_method2('
	fn 'static int skl_wm_has_lines('
	fn 'static uint32_t skl_wm_max_lines('
	fn 'static void skl_compute_plane_wm('
	cat host/test-skl-wm.c
} > "$TMP/test.c"
cc -std=gnu11 -Wall -Wextra -Werror -O1 -o "$TMP/test" "$TMP/test.c"
"$TMP/test"
