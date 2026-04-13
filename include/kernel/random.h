// LikeOS-64 - Cryptographically Secure Random Number Generator
// ChaCha20-based CSPRNG with entropy pool

#ifndef _KERNEL_RANDOM_H_
#define _KERNEL_RANDOM_H_

#include "types.h"

// Initialize the CSPRNG (call early in boot, before networking)
void random_init(void);

// Add entropy to the pool (from IRQ handlers, timers, devices)
void random_add_entropy(const void* data, size_t len);

// Get random bytes. blocking=1 waits for sufficient entropy (/dev/random),
// blocking=0 returns immediately once initially seeded (/dev/urandom).
// Returns number of bytes written, or -1 on error.
int random_get_bytes(void* buf, size_t len, int blocking);

// Convenience: get a random 32-bit value (non-blocking)
uint32_t random_u32(void);

// Convenience: get a random 64-bit value (non-blocking)
uint64_t random_u64(void);

// Called from timer IRQ to collect jitter entropy
void entropy_add_timer_jitter(void);

// Called from device IRQ handlers to collect timing entropy
void entropy_add_interrupt_timing(uint64_t extra);

// SipHash-2-4: keyed hash for TCP ISN generation (RFC 6528)
uint64_t siphash_2_4(const uint8_t key[16], const void* data, size_t len);

#endif // _KERNEL_RANDOM_H_
