// LikeOS HAL - AMD Platform Security Processor: the platform access
// mailbox and doorbell
//
// The PSP is a PCI function of AMD's secure processor block (the CCP
// device, BAR 2).  Besides its security services it offers "platform
// access": a command/response mailbox (C2PMSG_28..30: command register
// with a response-ready bit, and the physical address of a request
// buffer the PSP reads and writes) and, on newer parts, a doorbell (a
// command register and a button register) for short requests without a
// buffer.  The I2C bus arbitration between the OS and the PSP
// (i2c-designware-amdpsp.c) is the user.
//
// Only what that needs is here: finding the device (only when a driver
// asks: amd_psp_init), checking it is reachable, and the two message
// paths.  The PSP's interrupts stay disabled (both paths are polled) and
// nothing else of the device is touched.
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2013, 2019, 2023 Advanced Micro Devices, Inc.

#ifndef KERNEL_HAL_AMD_PSP_H
#define KERNEL_HAL_AMD_PSP_H

#include <kernel/uapi/types.h>

/* Command/response register fields (most mailboxes). */
#define PSP_CMDRESP_STS 0x0000ffffu /* bits 15:0 */
#define PSP_CMDRESP_CMD 0x00ff0000u /* bits 23:16 */
#define PSP_CMDRESP_CMD_SHIFT 16
#define PSP_CMDRESP_RESERVED 0x3f000000u
#define PSP_CMDRESP_RECOVERY (1u << 30)
#define PSP_CMDRESP_RESP (1u << 31)
#define PSP_DRBL_RING (1u << 0)
#define PSP_DOORBELL_CMDRESP_STS 0x000000ffu /* bits 7:0 */

/* How long the PSP may take to take/answer a command (us). */
#ifndef PSP_CMD_TIMEOUT_US
#define PSP_CMD_TIMEOUT_US (500u * 1000u)
#endif

enum psp_platform_access_msg {
	PSP_CMD_NONE = 0x0,
	PSP_SFS_GET_FW_VERSIONS,
	PSP_SFS_UPDATE,
	PSP_CMD_HSTI_QUERY = 0x14,
	PSP_I2C_REQ_BUS_CMD = 0x64,
	PSP_DYNAMIC_BOOST_GET_NONCE,
	PSP_DYNAMIC_BOOST_SET_UID,
	PSP_DYNAMIC_BOOST_GET_PARAMETER,
	PSP_DYNAMIC_BOOST_SET_PARAMETER,
};

/* Every request buffer starts with this header; payload_size is the
 * whole buffer's size, status is written by the PSP. */
struct psp_req_buffer_hdr {
	uint32_t payload_size;
	uint32_t status;
} __attribute__((packed));

struct psp_request {
	struct psp_req_buffer_hdr header;
	uint8_t buf[];
} __attribute__((packed));

/* The register offsets of one PSP generation (0: not present). */
struct amd_psp_vdata {
	uint32_t feature_reg;
	uint32_t inten_reg;
	uint32_t intsts_reg;
	/* platform access */
	uint32_t cmdresp_reg;
	uint32_t cmdbuff_addr_lo_reg;
	uint32_t cmdbuff_addr_hi_reg;
	uint32_t doorbell_button_reg;
	uint32_t doorbell_cmd_reg;
};

/* The largest request psp_send_platform_access_msg carries. */
#define PSP_REQ_MAX 1024u

/* Find the PSP with platform access among the PCI functions and set it
 * up (once; process context): 0, -ENODEV (none), -EIO (registers not
 * reachable: a firmware that hides the device), -ENOMEM. */
int amd_psp_init(void);
/* Use `regs' (the mapped BAR) with `vd' as the PSP -- the second half of
 * amd_psp_init, and the tests' way in; NULL detaches (tests).  0 or
 * -errno as amd_psp_init. */
int amd_psp_attach(volatile uint8_t *regs, const struct amd_psp_vdata *vd,
		   const char *name);
/* The register layout of a PSP PCI device id, NULL when that PSP has no
 * platform access. */
const struct amd_psp_vdata *amd_psp_lookup_vdata(uint16_t device_id);

/* 0 when platform access is ready, -ENODEV otherwise. */
int psp_check_platform_access_status(void);
/* Send `msg' with the request `req' (req->header.payload_size bytes, at
 * most PSP_REQ_MAX; the answer is copied back into it): 0, -EBUSY
 * (mailbox in recovery or still busy), -ENODEV, -ETIMEDOUT, -EIO (the
 * PSP reported a status, in req->header.status), -EINVAL.  May sleep. */
int psp_send_platform_access_msg(enum psp_platform_access_msg msg,
				 struct psp_request *req);
/* Ring the doorbell with `msg': 0, -EBUSY, -ENODEV, -ETIMEDOUT, -EIO
 * (the status the PSP answered is in *result).  May sleep. */
int psp_ring_platform_doorbell(int msg, uint32_t *result);

#endif /* KERNEL_HAL_AMD_PSP_H */
