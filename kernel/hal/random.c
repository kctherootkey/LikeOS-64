// LikeOS-64 - ChaCha20-based CSPRNG with Entropy Pool
//
// Implements:
//   - ChaCha20 block cipher (RFC 7539)
//   - Entropy pool with mixing from timer jitter, interrupts, devices
//   - /dev/random (blocking) and /dev/urandom (non-blocking) support
//   - SipHash-2-4 for TCP ISN generation (RFC 6528)

#include "../../include/kernel/random.h"
#include "../../include/kernel/timer.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/sched.h"   // spinlock_t

// ============================================================================
// ChaCha20 Core (RFC 7539)
// ============================================================================

// ChaCha20 state: 16 x 32-bit words
// Layout: constants[4] | key[8] | counter[1] | nonce[3]

static inline uint32_t rotl32(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

#define CHACHA_QR(a, b, c, d) do { \
    a += b; d ^= a; d = rotl32(d, 16); \
    c += d; b ^= c; b = rotl32(b, 12); \
    a += b; d ^= a; d = rotl32(d, 8);  \
    c += d; b ^= c; b = rotl32(b, 7);  \
} while(0)

static void chacha20_block(const uint32_t input[16], uint32_t output[16]) {
    uint32_t x[16];
    for (int i = 0; i < 16; i++) x[i] = input[i];

    // 20 rounds = 10 double-rounds
    for (int i = 0; i < 10; i++) {
        // Column rounds
        CHACHA_QR(x[0], x[4], x[ 8], x[12]);
        CHACHA_QR(x[1], x[5], x[ 9], x[13]);
        CHACHA_QR(x[2], x[6], x[10], x[14]);
        CHACHA_QR(x[3], x[7], x[11], x[15]);
        // Diagonal rounds
        CHACHA_QR(x[0], x[5], x[10], x[15]);
        CHACHA_QR(x[1], x[6], x[11], x[12]);
        CHACHA_QR(x[2], x[7], x[ 8], x[13]);
        CHACHA_QR(x[3], x[4], x[ 9], x[14]);
    }

    for (int i = 0; i < 16; i++) output[i] = x[i] + input[i];
}

// ============================================================================
// Entropy Pool
// ============================================================================

// "expand 32-byte k" as 4 little-endian uint32_t
#define CHACHA_CONST0 0x61707865
#define CHACHA_CONST1 0x3320646e
#define CHACHA_CONST2 0x79622d32
#define CHACHA_CONST3 0x6b206574

typedef struct {
    uint32_t key[8];        // 256-bit key
    uint64_t counter;       // Block counter
    uint32_t nonce[3];      // 96-bit nonce
    int entropy_bits;       // Estimated entropy collected
    int seeded;             // Has been initially seeded
    uint64_t last_tsc;      // Last TSC value for jitter measurement
    uint64_t reseed_count;  // Number of reseeds
    spinlock_t lock;
} entropy_pool_t;

static entropy_pool_t g_pool;

// Read TSC (Time Stamp Counter)
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// Mix data into entropy pool by XOR-folding into key
static void pool_mix(const uint8_t* data, size_t len) {
    uint8_t* key_bytes = (uint8_t*)g_pool.key;
    for (size_t i = 0; i < len; i++) {
        key_bytes[i % 32] ^= data[i];
    }
}

// Re-key the pool: generate a new key from current state using ChaCha20
static void pool_rekey(void) {
    uint32_t state[16];
    state[0] = CHACHA_CONST0;
    state[1] = CHACHA_CONST1;
    state[2] = CHACHA_CONST2;
    state[3] = CHACHA_CONST3;
    for (int i = 0; i < 8; i++) state[4 + i] = g_pool.key[i];
    state[12] = (uint32_t)g_pool.counter;
    state[13] = g_pool.nonce[0];
    state[14] = g_pool.nonce[1];
    state[15] = g_pool.nonce[2];

    uint32_t output[16];
    chacha20_block(state, output);

    // Use first 8 words as new key, next 3 as new nonce
    for (int i = 0; i < 8; i++) g_pool.key[i] = output[i];
    for (int i = 0; i < 3; i++) g_pool.nonce[i] = output[8 + i];
    g_pool.counter++;
    g_pool.reseed_count++;
}

// Generate random bytes from the pool
static void pool_generate(uint8_t* buf, size_t len) {
    uint32_t state[16];
    state[0] = CHACHA_CONST0;
    state[1] = CHACHA_CONST1;
    state[2] = CHACHA_CONST2;
    state[3] = CHACHA_CONST3;
    for (int i = 0; i < 8; i++) state[4 + i] = g_pool.key[i];
    state[13] = g_pool.nonce[0];
    state[14] = g_pool.nonce[1];
    state[15] = g_pool.nonce[2];

    size_t offset = 0;
    while (offset < len) {
        state[12] = (uint32_t)(g_pool.counter++);
        uint32_t output[16];
        chacha20_block(state, output);

        size_t chunk = len - offset;
        if (chunk > 64) chunk = 64;
        uint8_t* out_bytes = (uint8_t*)output;
        for (size_t i = 0; i < chunk; i++)
            buf[offset + i] = out_bytes[i];
        offset += chunk;
    }

    // Forward secrecy: re-key after generation
    pool_rekey();
}

// ============================================================================
// Public API
// ============================================================================

void random_init(void) {
    // Zero-initialize pool
    for (int i = 0; i < 8; i++) g_pool.key[i] = 0;
    g_pool.counter = 0;
    g_pool.nonce[0] = 0;
    g_pool.nonce[1] = 0;
    g_pool.nonce[2] = 0;
    g_pool.entropy_bits = 0;
    g_pool.seeded = 0;
    g_pool.last_tsc = 0;
    g_pool.reseed_count = 0;
    g_pool.lock = (spinlock_t)SPINLOCK_INIT("entropy");

    // Initial seeding from TSC and boot time
    uint64_t tsc = rdtsc();
    pool_mix((const uint8_t*)&tsc, sizeof(tsc));

    uint64_t epoch = timer_get_epoch();
    pool_mix((const uint8_t*)&epoch, sizeof(epoch));

    uint64_t ticks = timer_ticks();
    pool_mix((const uint8_t*)&ticks, sizeof(ticks));

    // Mix TSC again for more bits
    tsc = rdtsc();
    pool_mix((const uint8_t*)&tsc, sizeof(tsc));

    // Re-key with initial entropy
    pool_rekey();

    // Mark as seeded with initial entropy estimate
    g_pool.entropy_bits = 64;
    g_pool.seeded = 1;
    g_pool.last_tsc = tsc;

    kprintf("RANDOM: ChaCha20 CSPRNG initialized\n");
}

void random_add_entropy(const void* data, size_t len) {
    uint64_t flags;
    spin_lock_irqsave(&g_pool.lock, &flags);

    pool_mix((const uint8_t*)data, len);

    // Also mix in current TSC for additional jitter
    uint64_t tsc = rdtsc();
    pool_mix((const uint8_t*)&tsc, sizeof(tsc));

    // Estimate: 1 bit of entropy per byte of input (conservative)
    g_pool.entropy_bits += (int)len;
    if (g_pool.entropy_bits > 4096) g_pool.entropy_bits = 4096;

    // Re-key periodically
    if (g_pool.entropy_bits >= 256) {
        pool_rekey();
    }

    spin_unlock_irqrestore(&g_pool.lock, flags);
}

void entropy_add_timer_jitter(void) {
    uint64_t tsc = rdtsc();
    uint64_t delta = tsc - g_pool.last_tsc;
    g_pool.last_tsc = tsc;

    // The lower bits of TSC delta contain jitter from interrupt timing
    uint64_t flags;
    spin_lock_irqsave(&g_pool.lock, &flags);

    pool_mix((const uint8_t*)&delta, sizeof(delta));

    // Conservative: 1 bit per timer jitter sample
    g_pool.entropy_bits++;
    if (g_pool.entropy_bits > 4096) g_pool.entropy_bits = 4096;

    spin_unlock_irqrestore(&g_pool.lock, flags);
}

void entropy_add_interrupt_timing(uint64_t extra) {
    uint64_t tsc = rdtsc();
    uint64_t combined[2] = { tsc, extra };

    uint64_t flags;
    spin_lock_irqsave(&g_pool.lock, &flags);

    pool_mix((const uint8_t*)combined, sizeof(combined));
    g_pool.entropy_bits += 2;
    if (g_pool.entropy_bits > 4096) g_pool.entropy_bits = 4096;

    spin_unlock_irqrestore(&g_pool.lock, flags);
}

int random_get_bytes(void* buf, size_t len, int blocking) {
    if (!buf || len == 0) return 0;

    uint64_t flags;
    spin_lock_irqsave(&g_pool.lock, &flags);

    if (blocking) {
        // /dev/random behavior: wait for sufficient entropy
        // In practice we spin-wait (with unlock/relock) until entropy available
        while (g_pool.entropy_bits < (int)(len * 8) && g_pool.entropy_bits < 256) {
            spin_unlock_irqrestore(&g_pool.lock, flags);
            // Yield to allow entropy collection
            __asm__ volatile("pause");
            spin_lock_irqsave(&g_pool.lock, &flags);
        }
    }

    if (!g_pool.seeded) {
        // Not yet seeded — shouldn't happen after random_init, but be safe
        spin_unlock_irqrestore(&g_pool.lock, flags);
        return -1;
    }

    pool_generate((uint8_t*)buf, len);

    // Deplete entropy estimate for blocking mode
    if (blocking) {
        g_pool.entropy_bits -= (int)(len * 8);
        if (g_pool.entropy_bits < 0) g_pool.entropy_bits = 0;
    }

    spin_unlock_irqrestore(&g_pool.lock, flags);
    return (int)len;
}

uint32_t random_u32(void) {
    uint32_t val;
    random_get_bytes(&val, sizeof(val), 0);
    return val;
}

uint64_t random_u64(void) {
    uint64_t val;
    random_get_bytes(&val, sizeof(val), 0);
    return val;
}

// ============================================================================
// SipHash-2-4 — Keyed hash for TCP ISN generation
// Reference: https://131002.net/siphash/
// ============================================================================

static inline uint64_t sip_rotl(uint64_t x, int b) {
    return (x << b) | (x >> (64 - b));
}

#define SIPROUND do { \
    v0 += v1; v1 = sip_rotl(v1, 13); v1 ^= v0; v0 = sip_rotl(v0, 32); \
    v2 += v3; v3 = sip_rotl(v3, 16); v3 ^= v2; \
    v0 += v3; v3 = sip_rotl(v3, 21); v3 ^= v0; \
    v2 += v1; v1 = sip_rotl(v1, 17); v1 ^= v2; v2 = sip_rotl(v2, 32); \
} while(0)

uint64_t siphash_2_4(const uint8_t key[16], const void* data, size_t len) {
    uint64_t k0 = 0, k1 = 0;
    for (int i = 0; i < 8; i++) {
        k0 |= (uint64_t)key[i]   << (i * 8);
        k1 |= (uint64_t)key[8+i] << (i * 8);
    }

    uint64_t v0 = k0 ^ 0x736f6d6570736575ULL;
    uint64_t v1 = k1 ^ 0x646f72616e646f6dULL;
    uint64_t v2 = k0 ^ 0x6c7967656e657261ULL;
    uint64_t v3 = k1 ^ 0x7465646279746573ULL;

    const uint8_t* p = (const uint8_t*)data;
    size_t left = len;

    // Process full 8-byte blocks
    while (left >= 8) {
        uint64_t m = 0;
        for (int i = 0; i < 8; i++) m |= (uint64_t)p[i] << (i * 8);
        v3 ^= m;
        SIPROUND; SIPROUND;
        v0 ^= m;
        p += 8;
        left -= 8;
    }

    // Last block with length byte
    uint64_t b = ((uint64_t)len) << 56;
    switch (left) {
    case 7: b |= (uint64_t)p[6] << 48; __attribute__((fallthrough));
    case 6: b |= (uint64_t)p[5] << 40; __attribute__((fallthrough));
    case 5: b |= (uint64_t)p[4] << 32; __attribute__((fallthrough));
    case 4: b |= (uint64_t)p[3] << 24; __attribute__((fallthrough));
    case 3: b |= (uint64_t)p[2] << 16; __attribute__((fallthrough));
    case 2: b |= (uint64_t)p[1] << 8;  __attribute__((fallthrough));
    case 1: b |= (uint64_t)p[0];        break;
    case 0: break;
    }
    v3 ^= b;
    SIPROUND; SIPROUND;
    v0 ^= b;

    // Finalization
    v2 ^= 0xff;
    SIPROUND; SIPROUND; SIPROUND; SIPROUND;

    return v0 ^ v1 ^ v2 ^ v3;
}
