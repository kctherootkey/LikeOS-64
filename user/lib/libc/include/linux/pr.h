// LikeOS persistent reservation UAPI - the IOC_PR_* block ioctls
//
// Persistent reservations let hosts that share a storage device (two
// servers on one dual-ported NVMe namespace or SCSI LUN) register keys and
// take shared or exclusive reservations that survive resets.  These are
// the established, transport-neutral requests a block device node
// answers; an NVMe namespace maps them onto Reservation Register/Acquire/
// Release/Report.  The structures and numbers ('p' group, 200..207) are
// fixed by that interface.
//
//   IOC_PR_REGISTER          struct pr_registration: register new_key
//                            (replacing old_key; PR_FL_IGNORE_KEY skips
//                            the old-key check), new_key 0 unregisters
//   IOC_PR_RESERVE           struct pr_reservation: acquire a reservation
//                            of `type' with `key'
//   IOC_PR_RELEASE           struct pr_reservation: release it
//   IOC_PR_PREEMPT           struct pr_preempt: preempt old_key's
//                            reservation with new_key
//   IOC_PR_PREEMPT_ABORT     the same, aborting the preempted host's
//                            commands
//   IOC_PR_CLEAR             struct pr_clear: clear every registration and
//                            reservation
//   IOC_PR_READ_KEYS         struct pr_read_keys: the generation and up to
//                            num_keys registered keys into keys_ptr
//   IOC_PR_READ_RESERVATION  struct pr_read_reservation: the holder's key,
//                            the generation and the type (0 = none)
//
// A positive return value is a pr_status (the device refused the request);
// -1 with errno is a failure of the request itself.
//
// Mirrored byte-for-byte into the libc include tree, at the established
// path programs include it by; keep both copies identical.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef _KERNEL_UAPI_PR_H_
#define _KERNEL_UAPI_PR_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __LIKEOS_KERNEL__
#include <kernel/uapi/types.h>
#include <kernel/uapi/ioctl.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
#endif

enum pr_status {
	PR_STS_SUCCESS = 0x0,
	// The codes below follow SCSI, for which the interface was made.
	PR_STS_IOERR = 0x2, // generic device failure
	PR_STS_RESERVATION_CONFLICT = 0x18,
	PR_STS_RETRY_PATH_FAILURE = 0xe0000, // temporary path failure
	PR_STS_PATH_FAST_FAILED = 0xf0000, // failed by a fast-fail timer
	PR_STS_PATH_FAILED = 0x10000, // path unreachable, marked failed
};

enum pr_type {
	PR_WRITE_EXCLUSIVE = 1,
	PR_EXCLUSIVE_ACCESS = 2,
	PR_WRITE_EXCLUSIVE_REG_ONLY = 3,
	PR_EXCLUSIVE_ACCESS_REG_ONLY = 4,
	PR_WRITE_EXCLUSIVE_ALL_REGS = 5,
	PR_EXCLUSIVE_ACCESS_ALL_REGS = 6,
};

struct pr_reservation {
	uint64_t key;
	uint32_t type; // enum pr_type
	uint32_t flags; // PR_FL_*
};

struct pr_registration {
	uint64_t old_key;
	uint64_t new_key;
	uint32_t flags;
	uint32_t __pad;
};

struct pr_preempt {
	uint64_t old_key;
	uint64_t new_key;
	uint32_t type;
	uint32_t flags;
};

struct pr_clear {
	uint64_t key;
	uint32_t flags;
	uint32_t __pad;
};

struct pr_read_keys {
	uint32_t generation; // out
	uint32_t num_keys; // in: room at keys_ptr; out: keys registered
	uint64_t keys_ptr; // user pointer to uint64_t[num_keys]
};

struct pr_read_reservation {
	uint64_t key;
	uint32_t generation;
	uint32_t type; // 0: no reservation
};

#define PR_FL_IGNORE_KEY (1 << 0) // ignore the existing key

#define IOC_PR_REGISTER _IOW('p', 200, struct pr_registration)
#define IOC_PR_RESERVE _IOW('p', 201, struct pr_reservation)
#define IOC_PR_RELEASE _IOW('p', 202, struct pr_reservation)
#define IOC_PR_PREEMPT _IOW('p', 203, struct pr_preempt)
#define IOC_PR_PREEMPT_ABORT _IOW('p', 204, struct pr_preempt)
#define IOC_PR_CLEAR _IOW('p', 205, struct pr_clear)
#define IOC_PR_READ_KEYS _IOWR('p', 206, struct pr_read_keys)
#define IOC_PR_READ_RESERVATION _IOR('p', 207, struct pr_read_reservation)

// The most keys IOC_PR_READ_KEYS returns.
#define PR_KEYS_MAX (1u << 16)

#ifdef __cplusplus
}
#endif

#endif // _KERNEL_UAPI_PR_H_
