// LikeOS-64 -- extended processor state (x87 / SSE / AVX / AVX-512).
//
// One task's register file is saved and restored as a single opaque block.
// On a CPU with XSAVE the block is the XSAVE standard-format image, sized
// by CPUID for the components the kernel enabled in XCR0; on anything older
// it is the 512-byte FXSAVE image.  Every user of a save area -- the
// context switch, fork, kernel_fpu_begin(), the signal frame -- goes through
// these helpers so that not one of them has to know which.
//
// Enabling AVX in XCR0 is what makes AVX usable in user mode at all: without
// OSXSAVE a VEX-encoded instruction raises #UD, and a JIT that has read
// CPUID's AVX bit will emit exactly those.
#ifndef KERNEL_KE_FPU_H
#define KERNEL_KE_FPU_H

#include <kernel/uapi/types.h>

/* Largest state image this kernel will enable.  The x87+SSE+AVX+AVX-512
 * standard-format image is 2688 bytes; components past that (AMX tiles,
 * 8 KB+) are never put into XCR0. */
#define FPU_STATE_MAX 4096
/* XSAVE wants its area 64-byte aligned; FXSAVE only 16. */
#define FPU_STATE_ALIGN 64

/* XCR0 component bits. */
#define XFEATURE_MASK_X87 (1ULL << 0)
#define XFEATURE_MASK_SSE (1ULL << 1)
#define XFEATURE_MASK_AVX (1ULL << 2)
#define XFEATURE_MASK_OPMASK (1ULL << 5)
#define XFEATURE_MASK_ZMM_HI256 (1ULL << 6)
#define XFEATURE_MASK_HI16_ZMM (1ULL << 7)

/* Bytes in one save image (512 without XSAVE).  Fixed once fpu_init_bsp()
 * has run; every task's area is FPU_STATE_MAX bytes regardless. */
extern uint32_t g_fpu_state_size;
/* The XCR0 value in force on every CPU; 0 when XSAVE is not in use. */
extern uint64_t g_fpu_xcr0;
/* Non-zero when the XSAVE family of instructions is used. */
extern int g_fpu_use_xsave;

/* BSP: discover XSAVE, choose XCR0, size the image.  Before any task exists. */
void fpu_init_bsp(void);
/* Every CPU (BSP included, after fpu_init_bsp): OSXSAVE + XCR0. */
void fpu_init_cpu(void);

/* Save / load the live registers to / from a properly aligned area. */
static inline void fpu_save(void *area)
{
	if (g_fpu_use_xsave) {
		uint32_t lo = (uint32_t)g_fpu_xcr0, hi = (uint32_t)(g_fpu_xcr0 >> 32);
		__asm__ volatile("xsave (%0)" : : "r"(area), "a"(lo), "d"(hi)
				 : "memory");
	} else {
		__asm__ volatile("fxsave (%0)" : : "r"(area) : "memory");
	}
}

static inline void fpu_restore(const void *area)
{
	if (g_fpu_use_xsave) {
		uint32_t lo = (uint32_t)g_fpu_xcr0, hi = (uint32_t)(g_fpu_xcr0 >> 32);
		__asm__ volatile("xrstor (%0)" : : "r"(area), "a"(lo), "d"(hi)
				 : "memory");
	} else {
		__asm__ volatile("fxrstor (%0)" : : "r"(area) : "memory");
	}
}

/* Fill an area with the state a fresh thread expects (default control
 * words, every SSE exception masked, extended components in their initial
 * state). */
void fpu_init_state(void *area);

/* Make an image that came from user memory safe to load: reserved MXCSR
 * bits and an XSAVE header naming components the kernel never enabled both
 * make the restore instruction fault IN THE KERNEL. */
void fpu_sanitize_state(void *area);

#endif
