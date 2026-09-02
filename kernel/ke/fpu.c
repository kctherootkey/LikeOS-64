// LikeOS-64 -- extended processor state: discovery and image helpers.
#include <kernel/ke/fpu.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

uint32_t g_fpu_state_size = 512;
uint64_t g_fpu_xcr0 = 0;
int g_fpu_use_xsave = 0;

static inline void cpuid_count(uint32_t leaf, uint32_t sub, uint32_t *a,
			       uint32_t *b, uint32_t *c, uint32_t *d)
{
	__asm__ volatile("cpuid"
			 : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
			 : "a"(leaf), "c"(sub));
}

static inline void xsetbv(uint32_t idx, uint64_t val)
{
	__asm__ volatile("xsetbv"
			 :
			 : "c"(idx), "a"((uint32_t)val), "d"((uint32_t)(val >> 32))
			 : "memory");
}

static inline void cr4_set_osxsave(void)
{
	uint64_t cr4;

	__asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
	cr4 |= (1ULL << 18); /* OSXSAVE */
	__asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
}

void fpu_init_bsp(void)
{
	uint32_t a, b, c, d;

	cpuid_count(1, 0, &a, &b, &c, &d);
	if (!(c & (1u << 26))) { /* no XSAVE: FXSAVE it is */
		kprintf("fpu: FXSAVE, 512-byte state\n");
		return;
	}

	/* Supported components, then the subset this kernel is willing to
	 * carry per task.  AVX-512 comes only as a trio, and only on top of
	 * AVX; anything else advertised (PKRU, AMX, ...) stays disabled. */
	cpuid_count(0xD, 0, &a, &b, &c, &d);
	uint64_t supported = ((uint64_t)d << 32) | a;
	uint64_t want = supported & (XFEATURE_MASK_X87 | XFEATURE_MASK_SSE |
				     XFEATURE_MASK_AVX);
	const uint64_t avx512 = XFEATURE_MASK_OPMASK | XFEATURE_MASK_ZMM_HI256 |
				XFEATURE_MASK_HI16_ZMM;
	if ((want & XFEATURE_MASK_AVX) && (supported & avx512) == avx512)
		want |= avx512;

	cr4_set_osxsave();
	for (;;) {
		xsetbv(0, want);
		cpuid_count(0xD, 0, &a, &b, &c, &d);
		/* EBX: bytes needed for the components currently in XCR0. */
		if (b <= FPU_STATE_MAX)
			break;
		/* Too large for the per-task area: shed the widest component
		 * and measure again. */
		if (want & avx512)
			want &= ~avx512;
		else
			want &= ~XFEATURE_MASK_AVX;
	}
	g_fpu_xcr0 = want;
	g_fpu_state_size = b;
	g_fpu_use_xsave = 1;
	kprintf("fpu: XSAVE, XCR0=0x%llx, %u-byte state%s\n",
		(unsigned long long)want, b,
		(want & avx512) ? " (AVX-512)" :
		(want & XFEATURE_MASK_AVX) ? " (AVX)" : "");
}

void fpu_init_cpu(void)
{
	if (!g_fpu_use_xsave)
		return;
	cr4_set_osxsave();
	xsetbv(0, g_fpu_xcr0);
}

void fpu_init_state(void *area)
{
	uint8_t *p = (uint8_t *)area;

	mm_memset(p, 0, g_fpu_state_size);
	*(uint16_t *)(p + 0) = 0x037F; /* FCW: all exceptions masked */
	*(uint32_t *)(p + 24) = 0x1F80; /* MXCSR: all SSE exceptions masked */
	if (g_fpu_use_xsave) {
		/* XSAVE header: the legacy region above is valid data for
		 * x87 and SSE; every other component starts in its initial
		 * state.  xcomp_bv = 0 selects the standard (non-compacted)
		 * format, which is the only one XRSTOR accepts here. */
		*(uint64_t *)(p + 512) = XFEATURE_MASK_X87 | XFEATURE_MASK_SSE;
		*(uint64_t *)(p + 520) = 0;
	}
}

void fpu_sanitize_state(void *area)
{
	uint8_t *p = (uint8_t *)area;

	/* MXCSR: the low 16 bits are the architected control/status bits and
	 * are all legal; reserved high bits set make FXRSTOR/XRSTOR #GP. */
	*(uint32_t *)(p + 24) &= 0xFFFF;
	if (g_fpu_use_xsave) {
		/* Header: only components in XCR0 may be named, the compacted
		 * form is not used, and the reserved bytes must be zero. */
		*(uint64_t *)(p + 512) &= g_fpu_xcr0;
		*(uint64_t *)(p + 520) = 0;
		mm_memset(p + 528, 0, 48);
	}
}
