// LikeOS NVMe UAPI - the pass-through ioctls of NVMe device nodes
//
// What programs that talk to NVMe devices directly (nvme-cli and libnvme,
// smartctl, fio's passthrough engine) issue against
//   /dev/nvmeX     the controller character device (admin commands, reset,
//                  rescan, subsystem reset; I/O commands to the one
//                  namespace when the controller has exactly one),
//   /dev/nvmeXnY   a namespace block device,
//   /dev/ngXnY     a namespace generic character device.
// The structures and request numbers are fixed by that established
// interface ('N' ioctl group) and must never change.
//
// Requests:
//   NVME_IOCTL_ID           returns the namespace ID of the node (namespace
//                           nodes; -ENOTTY on a controller node)
//   NVME_IOCTL_ADMIN_CMD    struct nvme_admin_cmd: an admin command; the
//                           completion's dword 0 comes back in `result'
//   NVME_IOCTL_SUBMIT_IO    struct nvme_user_io: a read/write/compare
//   NVME_IOCTL_IO_CMD       struct nvme_passthru_cmd: an I/O command
//   NVME_IOCTL_RESET        reset the controller
//   NVME_IOCTL_SUBSYS_RESET NVM subsystem reset (NSSR)
//   NVME_IOCTL_RESCAN       rescan the namespaces
//   NVME_IOCTL_ADMIN64_CMD  struct nvme_passthru_cmd64 (64-bit result)
//   NVME_IOCTL_IO64_CMD     the same for I/O commands
//   NVME_IOCTL_IO64_CMD_VEC the same with `addr' pointing to an iovec
//                           array of `vec_cnt' entries
//   NVME_URING_CMD_*        the command layouts of the asynchronous
//                           (io_uring) pass-through; defined for source
//                           compatibility.
//
// Return value of the command ioctls: 0 when the command completed
// successfully, a positive NVMe status (status code | type << 8, with DNR
// and MORE as the controller reported them) when the controller failed it,
// or -1 with errno for a failure before or around the command.
//
// The `addr' and `metadata' fields are user pointers carried as 64-bit
// integers; data_len/metadata_len are byte counts; timeout_ms 0 means the
// driver's default.
//
// Mirrored byte-for-byte into the libc include tree, at the established
// path programs include it by; keep both copies identical.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef _KERNEL_UAPI_NVME_IOCTL_H_
#define _KERNEL_UAPI_NVME_IOCTL_H_

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

struct nvme_user_io {
	uint8_t opcode;
	uint8_t flags;
	uint16_t control;
	uint16_t nblocks; // 0-based count of logical blocks
	uint16_t rsvd;
	uint64_t metadata; // user pointer
	uint64_t addr; // user pointer
	uint64_t slba;
	uint32_t dsmgmt;
	uint32_t reftag;
	uint16_t apptag;
	uint16_t appmask;
};

struct nvme_passthru_cmd {
	uint8_t opcode;
	uint8_t flags;
	uint16_t rsvd1;
	uint32_t nsid;
	uint32_t cdw2;
	uint32_t cdw3;
	uint64_t metadata; // user pointer
	uint64_t addr; // user pointer
	uint32_t metadata_len;
	uint32_t data_len;
	uint32_t cdw10;
	uint32_t cdw11;
	uint32_t cdw12;
	uint32_t cdw13;
	uint32_t cdw14;
	uint32_t cdw15;
	uint32_t timeout_ms;
	uint32_t result; // completion dword 0, written back
};

struct nvme_passthru_cmd64 {
	uint8_t opcode;
	uint8_t flags;
	uint16_t rsvd1;
	uint32_t nsid;
	uint32_t cdw2;
	uint32_t cdw3;
	uint64_t metadata; // user pointer
	uint64_t addr; // user pointer (an iovec array for the _VEC form)
	uint32_t metadata_len;
	union {
		uint32_t data_len; // non-vectored
		uint32_t vec_cnt; // vectored: number of iovecs
	};
	uint32_t cdw10;
	uint32_t cdw11;
	uint32_t cdw12;
	uint32_t cdw13;
	uint32_t cdw14;
	uint32_t cdw15;
	uint32_t timeout_ms;
	uint32_t rsvd2;
	uint64_t result; // completion dwords 0-1, written back
};

// The asynchronous pass-through command (no result field: the result
// comes with the completion).
struct nvme_uring_cmd {
	uint8_t opcode;
	uint8_t flags;
	uint16_t rsvd1;
	uint32_t nsid;
	uint32_t cdw2;
	uint32_t cdw3;
	uint64_t metadata;
	uint64_t addr;
	uint32_t metadata_len;
	uint32_t data_len;
	uint32_t cdw10;
	uint32_t cdw11;
	uint32_t cdw12;
	uint32_t cdw13;
	uint32_t cdw14;
	uint32_t cdw15;
	uint32_t timeout_ms;
	uint32_t rsvd2;
};

#define nvme_admin_cmd nvme_passthru_cmd

#define NVME_IOCTL_ID _IO('N', 0x40)
#define NVME_IOCTL_ADMIN_CMD _IOWR('N', 0x41, struct nvme_admin_cmd)
#define NVME_IOCTL_SUBMIT_IO _IOW('N', 0x42, struct nvme_user_io)
#define NVME_IOCTL_IO_CMD _IOWR('N', 0x43, struct nvme_passthru_cmd)
#define NVME_IOCTL_RESET _IO('N', 0x44)
#define NVME_IOCTL_SUBSYS_RESET _IO('N', 0x45)
#define NVME_IOCTL_RESCAN _IO('N', 0x46)
#define NVME_IOCTL_ADMIN64_CMD _IOWR('N', 0x47, struct nvme_passthru_cmd64)
#define NVME_IOCTL_IO64_CMD _IOWR('N', 0x48, struct nvme_passthru_cmd64)
#define NVME_IOCTL_IO64_CMD_VEC _IOWR('N', 0x49, struct nvme_passthru_cmd64)

#define NVME_URING_CMD_IO _IOWR('N', 0x80, struct nvme_uring_cmd)
#define NVME_URING_CMD_IO_VEC _IOWR('N', 0x81, struct nvme_uring_cmd)
#define NVME_URING_CMD_ADMIN _IOWR('N', 0x82, struct nvme_uring_cmd)
#define NVME_URING_CMD_ADMIN_VEC _IOWR('N', 0x83, struct nvme_uring_cmd)

#ifdef __cplusplus
}
#endif

#endif // _KERNEL_UAPI_NVME_IOCTL_H_
