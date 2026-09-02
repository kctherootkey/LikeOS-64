/* <sys/futex.h> -- the fast user-space mutex primitive.
 *
 * futex() waits on / wakes a 32-bit word shared between threads or, in
 * shared memory, between processes.  The operations and flags are the
 * conventional ones; see the kernel's include/kernel/ke/futex.h. */
#ifndef _SYS_FUTEX_H
#define _SYS_FUTEX_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_REQUEUE 3
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAKE_OP 5
#define FUTEX_LOCK_PI 6
#define FUTEX_UNLOCK_PI 7
#define FUTEX_TRYLOCK_PI 8
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10

#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_BITSET_MATCH_ANY 0xffffffffu

#define FUTEX_OP_SET 0
#define FUTEX_OP_ADD 1
#define FUTEX_OP_OR 2
#define FUTEX_OP_ANDN 3
#define FUTEX_OP_XOR 4
#define FUTEX_OP_OPARG_SHIFT 8
#define FUTEX_OP_CMP_EQ 0
#define FUTEX_OP_CMP_NE 1
#define FUTEX_OP_CMP_LT 2
#define FUTEX_OP_CMP_LE 3
#define FUTEX_OP_CMP_GT 4
#define FUTEX_OP_CMP_GE 5
#define FUTEX_OP(op, oparg, cmp, cmparg) \
	(((op) & 0xf) << 28 | ((cmp) & 0xf) << 24 | ((oparg) & 0xfff) << 12 | ((cmparg) & 0xfff))

/* Returns what the kernel returns (>= 0), or -1 with errno set. */
long futex(uint32_t *uaddr, int op, uint32_t val, const struct timespec *timeout,
	   uint32_t *uaddr2, uint32_t val3);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_FUTEX_H */
