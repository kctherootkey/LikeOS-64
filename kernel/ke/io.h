// LikeOS - memory-mapped I/O accessors and memory barriers
//
// A device register is not memory: a read may have side effects (clear a
// status bit), a write may start the device, and the order of the accesses
// is the protocol.  The compiler must therefore neither merge, reorder,
// elide nor split them -- every accessor here is one volatile access of
// exactly the stated width, followed by a compiler barrier so the
// surrounding plain loads and stores stay on their side of it.
//
// x86-64 ordering, and what each barrier is for:
//   * MMIO mapped uncached (MM_MMIO_UC) is strongly ordered against other
//     uncached accesses and against ordinary memory: a store to a doorbell
//     cannot pass an earlier store to a queue entry in write-back memory,
//     and a load of a register cannot be satisfied early.  So readl/writel
//     need only the compiler barrier.
//   * wmb()/rmb()/mb(): full ordering also against write-combining
//     mappings (a CMB or PMR mapped WC) and non-temporal stores -- sfence,
//     lfence, mfence.  Use them where a driver mixes WC and UC.
//   * dma_wmb()/dma_rmb(): ordering of accesses to COHERENT DMA memory (the
//     host-memory rings a device reads and writes) among themselves.  x86
//     keeps loads ordered with loads and stores with stores, so these are
//     compiler barriers -- they still must be written, because the
//     compiler does reorder.  Typical use: read a completion entry's phase
//     bit, dma_rmb(), then read the rest of the entry.
//   * smp_mb()/smp_wmb()/smp_rmb(): CPU-to-CPU ordering of ordinary
//     memory.  smp_mb() is a locked instruction (store->load ordering is the
//     one x86 does not give), the other two are compiler barriers.
//
// 64-bit registers: a device that implements an 8-byte register may still
// accept only 4-byte accesses (NVMe allows either for CAP, ASQ, ACQ, BPMBL,
// CMBMSC, PMRMSC).  lo_hi_readq/lo_hi_writeq do two dword accesses, low half
// first; hi_lo_* the other way round.  readq/writeq are single 8-byte
// accesses.
//
// Port I/O (inb/outb ...) lives in <kernel/hal/...>; this header is MMIO only.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_KE_IO_H
#define KERNEL_KE_IO_H

#include <kernel/uapi/types.h>

/* ---- barriers ------------------------------------------------------------ */

#ifndef barrier
#define barrier() __asm__ volatile("" ::: "memory")
#endif

static inline void mb(void)
{
	__asm__ volatile("mfence" ::: "memory");
}
static inline void rmb(void)
{
	__asm__ volatile("lfence" ::: "memory");
}
static inline void wmb(void)
{
	__asm__ volatile("sfence" ::: "memory");
}

/* Coherent DMA memory ordering (see above): compiler barriers on x86. */
static inline void dma_rmb(void)
{
	barrier();
}
static inline void dma_wmb(void)
{
	barrier();
}
/* Ordering of a store to DMA memory before a later MMIO store (a queue
 * entry before its doorbell) is given by x86 for UC MMIO; kept as a named
 * barrier so the intent is visible. */
static inline void dma_mb(void)
{
	mb();
}

static inline void smp_mb(void)
{
	__asm__ volatile("lock; addl $0,-4(%%rsp)" ::: "memory", "cc");
}
static inline void smp_rmb(void)
{
	barrier();
}
static inline void smp_wmb(void)
{
	barrier();
}

/* Single-copy-atomic accesses to ordinary shared memory (a field another
 * CPU or the device writes): one access, not torn, not cached by the
 * compiler. */
#define READ_ONCE(x) (*(const volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, v)                                          \
	do {                                                      \
		*(volatile __typeof__(x) *)&(x) = (v);            \
	} while (0)

/* ---- MMIO ---------------------------------------------------------------- */

static inline uint8_t readb(const volatile void *addr)
{
	uint8_t v = *(const volatile uint8_t *)addr;

	barrier();
	return v;
}
static inline uint16_t readw(const volatile void *addr)
{
	uint16_t v = *(const volatile uint16_t *)addr;

	barrier();
	return v;
}
static inline uint32_t readl(const volatile void *addr)
{
	uint32_t v = *(const volatile uint32_t *)addr;

	barrier();
	return v;
}
static inline uint64_t readq(const volatile void *addr)
{
	uint64_t v = *(const volatile uint64_t *)addr;

	barrier();
	return v;
}
static inline void writeb(uint8_t v, volatile void *addr)
{
	barrier();
	*(volatile uint8_t *)addr = v;
	barrier();
}
static inline void writew(uint16_t v, volatile void *addr)
{
	barrier();
	*(volatile uint16_t *)addr = v;
	barrier();
}
static inline void writel(uint32_t v, volatile void *addr)
{
	barrier();
	*(volatile uint32_t *)addr = v;
	barrier();
}
static inline void writeq(uint64_t v, volatile void *addr)
{
	barrier();
	*(volatile uint64_t *)addr = v;
	barrier();
}

/* The "relaxed" forms: the same on x86 (UC MMIO is ordered anyway). */
#define readl_relaxed(a) readl(a)
#define readq_relaxed(a) readq(a)
#define writel_relaxed(v, a) writel((v), (a))
#define writeq_relaxed(v, a) writeq((v), (a))

/* 64-bit register through two 32-bit accesses. */
static inline uint64_t lo_hi_readq(const volatile void *addr)
{
	const volatile uint32_t *p = (const volatile uint32_t *)addr;
	uint32_t lo = readl(p);
	uint32_t hi = readl(p + 1);

	return ((uint64_t)hi << 32) | lo;
}
static inline uint64_t hi_lo_readq(const volatile void *addr)
{
	const volatile uint32_t *p = (const volatile uint32_t *)addr;
	uint32_t hi = readl(p + 1);
	uint32_t lo = readl(p);

	return ((uint64_t)hi << 32) | lo;
}
static inline void lo_hi_writeq(uint64_t v, volatile void *addr)
{
	volatile uint32_t *p = (volatile uint32_t *)addr;

	writel((uint32_t)v, p);
	writel((uint32_t)(v >> 32), p + 1);
}
static inline void hi_lo_writeq(uint64_t v, volatile void *addr)
{
	volatile uint32_t *p = (volatile uint32_t *)addr;

	writel((uint32_t)(v >> 32), p + 1);
	writel((uint32_t)v, p);
}

/* Copy to / from an MMIO window (a CMB, a PMR), in 32-bit units where the
 * alignment allows, bytes otherwise.  Never uses string instructions or
 * SSE (the kernel is built integer-only). */
static inline void memcpy_toio(volatile void *dst, const void *src,
			       size_t len)
{
	volatile uint8_t *d = (volatile uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;

	while (len && ((uint64_t)d & 3)) {
		writeb(*s++, d++);
		len--;
	}
	while (len >= 4) {
		uint32_t v = (uint32_t)s[0] | ((uint32_t)s[1] << 8) |
			     ((uint32_t)s[2] << 16) | ((uint32_t)s[3] << 24);

		writel(v, d);
		d += 4;
		s += 4;
		len -= 4;
	}
	while (len--)
		writeb(*s++, d++);
}
static inline void memcpy_fromio(void *dst, const volatile void *src,
				 size_t len)
{
	uint8_t *d = (uint8_t *)dst;
	const volatile uint8_t *s = (const volatile uint8_t *)src;

	while (len && ((uint64_t)s & 3)) {
		*d++ = readb(s++);
		len--;
	}
	while (len >= 4) {
		uint32_t v = readl(s);

		d[0] = (uint8_t)v;
		d[1] = (uint8_t)(v >> 8);
		d[2] = (uint8_t)(v >> 16);
		d[3] = (uint8_t)(v >> 24);
		d += 4;
		s += 4;
		len -= 4;
	}
	while (len--)
		*d++ = readb(s++);
}
static inline void memset_io(volatile void *dst, int c, size_t len)
{
	volatile uint8_t *d = (volatile uint8_t *)dst;
	uint32_t v = (uint8_t)c;

	v |= v << 8;
	v |= v << 16;
	while (len && ((uint64_t)d & 3)) {
		writeb((uint8_t)c, d++);
		len--;
	}
	while (len >= 4) {
		writel(v, d);
		d += 4;
		len -= 4;
	}
	while (len--)
		writeb((uint8_t)c, d++);
}

#endif /* KERNEL_KE_IO_H */
