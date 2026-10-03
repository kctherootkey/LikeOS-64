// LikeOS -- the VMware hypervisor "backdoor" port protocol.
//
// A guest talks to a VMware-compatible hypervisor by executing a port
// instruction on a reserved I/O port with a magic value in EAX.  The
// hypervisor intercepts the access, reads the request out of the general
// purpose registers and hands its answer back in the same registers.  No
// device has to be probed first, but nothing answers on bare metal either:
// there the port is unclaimed and reads back all ones, so callers should only
// use this once something (the SVGA II adapter, the hypervisor CPUID leaf)
// has said they are running under such a hypervisor.
//
// Two ports:
//   VMWARE_BACKDOOR_PORT     the low-bandwidth command port; one IN carries a
//                            command in ECX and up to four dwords of
//                            arguments, and returns up to six dwords.
//   VMWARE_BACKDOOR_HB_PORT  the high-bandwidth port; a REP INSB/OUTSB moves
//                            a whole buffer (used by the RPC message channel).
//
// Register convention of a command-port call:
//   in:  EAX magic, EBX argument, ECX command (sub-command in the high half
//        for VMWARE_BACKDOOR_CMD_MESSAGE), EDX port (channel id in the high
//        half for message calls), ESI/EDI further arguments (message cookie)
//   out: every one of EAX..EDI may have been rewritten by the hypervisor.
//
// Only the generic call and the one query the display driver makes are here;
// the message channel keeps its own high-bandwidth helpers next to its
// protocol state (kernel/dev/gpu/vmwgfx/vmw_msg.c).
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_HAL_VMWARE_BACKDOOR_H
#define KERNEL_HAL_VMWARE_BACKDOOR_H

#include <kernel/uapi/types.h>

#define VMWARE_BACKDOOR_MAGIC 0x564D5868U /* 'VMXh' */
#define VMWARE_BACKDOOR_PORT 0x5658
#define VMWARE_BACKDOOR_HB_PORT 0x5659

/* Command numbers (ECX, low half). */
#define VMWARE_BACKDOOR_CMD_GETMHZ 1
#define VMWARE_BACKDOOR_CMD_GETVERSION 10
#define VMWARE_BACKDOOR_CMD_GETSCREENSIZE 15
#define VMWARE_BACKDOOR_CMD_GETHWVERSION 17
#define VMWARE_BACKDOOR_CMD_GETMEMSIZE 20
#define VMWARE_BACKDOOR_CMD_MESSAGE 30
#define VMWARE_BACKDOOR_CMD_GETHZ 45
#define VMWARE_BACKDOOR_CMD_GET_VCPU_INFO 68

/* VMWARE_BACKDOOR_CMD_MESSAGE sub-commands (ECX, high half). */
#define VMWARE_BACKDOOR_MSG_OPEN 0
#define VMWARE_BACKDOOR_MSG_SENDSIZE 1
#define VMWARE_BACKDOOR_MSG_SENDPAYLOAD 2
#define VMWARE_BACKDOOR_MSG_RECVSIZE 3
#define VMWARE_BACKDOOR_MSG_RECVPAYLOAD 4
#define VMWARE_BACKDOOR_MSG_RECVSTATUS 5
#define VMWARE_BACKDOOR_MSG_CLOSE 6
#define VMWARE_BACKDOOR_MSG_CMD(sub) \
	(VMWARE_BACKDOOR_CMD_MESSAGE | ((uint32_t)(sub) << 16))

/* Message status bits (ECX high half on return from a message call). */
#define VMWARE_BACKDOOR_MSG_STATUS_SUCCESS 0x0001
#define VMWARE_BACKDOOR_MSG_STATUS_DORECV 0x0002
#define VMWARE_BACKDOOR_MSG_STATUS_CPT 0x0010
#define VMWARE_BACKDOOR_MSG_STATUS_HB 0x0080

/* The register file of one command-port call.  eax is overwritten with the
 * magic on entry; everything else goes in as given and comes back as the
 * hypervisor left it. */
struct vmware_backdoor_regs {
	uint32_t eax, ebx, ecx, edx, esi, edi;
};

static inline void vmware_backdoor_call(struct vmware_backdoor_regs *r)
{
	uint32_t eax = VMWARE_BACKDOOR_MAGIC;
	uint32_t ebx = r->ebx, ecx = r->ecx, edx = r->edx;
	uint32_t esi = r->esi, edi = r->edi;

	__asm__ __volatile__("inl %%dx, %%eax"
			     : "+a"(eax), "+b"(ebx), "+c"(ecx), "+d"(edx),
			       "+S"(esi), "+D"(edi)
			     :
			     : "memory", "cc");
	r->eax = eax;
	r->ebx = ebx;
	r->ecx = ecx;
	r->edx = edx;
	r->esi = esi;
	r->edi = edi;
}

/* One plain command on the command port, channel 0: returns EAX, and EBX..EDX
 * through the optional out pointers. */
static inline uint32_t vmware_backdoor_cmd(uint32_t cmd, uint32_t in_ebx,
					   uint32_t *out_ebx, uint32_t *out_ecx,
					   uint32_t *out_edx)
{
	struct vmware_backdoor_regs r = {
		.ebx = in_ebx,
		.ecx = cmd,
		.edx = VMWARE_BACKDOOR_PORT,
	};

	vmware_backdoor_call(&r);
	if (out_ebx)
		*out_ebx = r.ebx;
	if (out_ecx)
		*out_ecx = r.ecx;
	if (out_edx)
		*out_edx = r.edx;
	return r.eax;
}

/* The host's screen size: width in the high half of EAX, height in the low.
 *
 * Returns -1 when there was no answer.  All ones is the protocol's refusal
 * and also what an unclaimed port reads back; the magic handed back
 * unchanged is a hypervisor that speaks the protocol but does not implement
 * this command.  Anything else is returned as is -- whether it is a
 * plausible display is the caller's judgement. */
static inline int vmware_backdoor_screen_size(uint32_t *w, uint32_t *h)
{
	uint32_t eax = vmware_backdoor_cmd(VMWARE_BACKDOOR_CMD_GETSCREENSIZE, 0,
					   NULL, NULL, NULL);

	if (eax == 0xFFFFFFFFU || eax == VMWARE_BACKDOOR_MAGIC)
		return -1;
	*w = eax >> 16;
	*h = eax & 0xFFFFU;
	return 0;
}

#endif
