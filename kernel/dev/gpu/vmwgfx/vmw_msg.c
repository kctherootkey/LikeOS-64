// LikeOS -- vmwgfx: the guest-to-host message channel (the "backdoor").
//
// The device is only half of what the hypervisor exposes.  The other half is
// a port-I/O protocol on port 0x5658, entered with a magic value in EAX, and
// it carries the things that are not pixels: the guest's log lines, the
// tools' version handshake, the answers to "what resolution is the window
// now".  The graphics stack uses one channel of it -- RPCI, the remote
// procedure call interface -- to send host log messages, and the display
// manager exposes that to userspace as DRM_VMW_MSG.  Mesa writes its
// renderer string and driver errors there, which is where they show up in
// the hypervisor's own log next to everything else about the virtual
// machine.
//
// The protocol, as the open specification describes it:
//
//   OPEN(protocol)   -> a channel number and a cookie
//   SENDSIZE(len)    -> the channel is ready for a message of len bytes
//   SEND             -> the bytes, either four at a time through the
//                       low-bandwidth port or in one block through the
//                       high-bandwidth one
//   RECVSIZE         -> how many bytes are waiting, if any
//   RECV / RECVSTATUS -> the reply, then an acknowledgement
//   CLOSE
//
// Every exchange opens a channel of its own and closes it again, so there is
// no channel state shared between callers and nothing to lock: two CPUs
// talking to the host at once simply hold two channels.
//
// A transfer can be interrupted by the virtual machine being checkpointed
// (suspended or snapshotted) in the middle of it.  The host then answers
// with the CPT status bit instead of success, and the whole step -- size
// and payload -- is sent again, up to VMW_MSG_RETRIES times.
//
// Failure is normal and not an error: on a hypervisor that does not
// implement RPCI (or on real hardware, where port 0x5658 reads back all
// ones) the OPEN simply fails, and everything above is told so.  Nothing in
// the graphics path depends on a message getting through.
//
// The plain command-port call is the shared one in
// <kernel/hal/vmware_backdoor.h>; the high-bandwidth string transfers, which
// only this channel uses, are here next to the protocol state they need.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from VMware's and Broadcom's code: GPL-2.0 OR MIT
// Portions Copyright 2016 VMware, Inc., Palo Alto, CA., USA
// Portions Copyright (c) 2009-2025 Broadcom. All Rights Reserved. The term
// “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.

#include <kernel/dev/gpu/vmwgfx/vmw_gb.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/uapi/drm/vmwgfx_drm.h>
#include <kernel/hal/vmware_backdoor.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

#define VMW_MESSAGE_STATUS_SUCCESS VMWARE_BACKDOOR_MSG_STATUS_SUCCESS
#define VMW_MESSAGE_STATUS_DORECV VMWARE_BACKDOOR_MSG_STATUS_DORECV
#define VMW_MESSAGE_STATUS_CPT VMWARE_BACKDOOR_MSG_STATUS_CPT
#define VMW_MESSAGE_STATUS_HB VMWARE_BACKDOOR_MSG_STATUS_HB

#define VMW_RPCI_PROTOCOL_NUM 0x49435052u /* 'RPCI' */
#define VMW_GUESTMSG_FLAG_COOKIE 0x80000000u
#define VMW_MSG_RETRIES 3

#define VMW_PORT_CMD_OPEN_CHANNEL VMWARE_BACKDOOR_MSG_CMD(VMWARE_BACKDOOR_MSG_OPEN)
#define VMW_PORT_CMD_CLOSE_CHANNEL VMWARE_BACKDOOR_MSG_CMD(VMWARE_BACKDOOR_MSG_CLOSE)
#define VMW_PORT_CMD_SENDSIZE VMWARE_BACKDOOR_MSG_CMD(VMWARE_BACKDOOR_MSG_SENDSIZE)
#define VMW_PORT_CMD_SENDPAYLOAD VMWARE_BACKDOOR_MSG_CMD(VMWARE_BACKDOOR_MSG_SENDPAYLOAD)
#define VMW_PORT_CMD_RECVSIZE VMWARE_BACKDOOR_MSG_CMD(VMWARE_BACKDOOR_MSG_RECVSIZE)
#define VMW_PORT_CMD_RECVPAYLOAD VMWARE_BACKDOOR_MSG_CMD(VMWARE_BACKDOOR_MSG_RECVPAYLOAD)
#define VMW_PORT_CMD_RECVSTATUS VMWARE_BACKDOOR_MSG_CMD(VMWARE_BACKDOOR_MSG_RECVSTATUS)

/* The high-bandwidth port's own command word (EBX, low half). */
#define VMW_PORT_CMD_HB_MSG 0

#define VMW_HIGH_WORD(x) (((uint32_t)(x) & 0xFFFF0000u) >> 16)

/* The longest string a client may hand DRM_VMW_MSG, terminator included. */
#define VMW_MAX_USER_MSG_LENGTH PAGE_SIZE
/* The longest host log line vmw_host_printf() formats; longer is cut. */
#define VMW_HOST_LOG_MAX PAGE_SIZE
/* The largest reply accepted from the host.  The size comes from the other
 * side of the channel, and nothing this driver asks for is anywhere near
 * this, so a larger number is a confused host, not a message to allocate
 * for. */
#define VMW_MSG_REPLY_MAX (1024u * 1024u)

struct rpc_channel {
	uint16_t channel_id;
	uint32_t cookie_high;
	uint32_t cookie_low;
};

/* Whether the host answers at all: 0 not asked yet, 1 it does, -1 it does
 * not.  Asked once; a host does not grow or lose the protocol at run time. */
static volatile int vmw_msg_state;

/* One command-port call on a channel.  The channel number rides in the high
 * half of EDX, the cookie in ESI/EDI -- every call after OPEN has to present
 * the cookie the host handed out, and every register comes back changed. */
static void vmw_rpc_call(uint32_t cmd, uint32_t in_ebx,
			 const struct rpc_channel *channel,
			 struct vmware_backdoor_regs *r)
{
	r->eax = 0;
	r->ebx = in_ebx;
	r->ecx = cmd;
	r->edx = ((uint32_t)channel->channel_id << 16) | VMWARE_BACKDOOR_PORT;
	r->esi = channel->cookie_high;
	r->edi = channel->cookie_low;
	vmware_backdoor_call(r);
}

/* The high-bandwidth transfers: one string instruction moves the whole
 * message, with the buffer in RSI (out) or RDI (in) and the other half of
 * the cookie in RBP -- which is why these cannot be written as a plain
 * inline asm with a memory clobber and no frame pointer saved by hand.
 *
 * The register assignment is the protocol's, not a choice:
 *   EAX  the magic          EBX  status | HB message command
 *   ECX  the byte count     EDX  the high-bandwidth port, channel in the
 *                                high half
 *   RSI  source (out) / cookie high (in)
 *   RDI  cookie low (out) / destination (in)
 *   RBP  cookie high (out) / cookie low (in)
 *
 * The low half of EDX is the port number itself, exactly
 * VMWARE_BACKDOOR_HB_PORT, for both directions: the string instruction
 * already says which way the bytes go.  A direction flag OR'ed into it --
 * as this file once did for the send -- names port 0x565B instead, and an
 * access to a port the hypervisor does not watch leaves EBX exactly as it
 * went in, SUCCESS bit included: the send would look delivered whether or
 * not the host ever saw it.
 *
 * The result comes back in the HIGH half of EBX, unlike the low-bandwidth
 * calls whose status is in the high half of ECX. */
static uint32_t vmw_hypercall_hb_out(const struct rpc_channel *channel,
				     const void *buf, uint32_t bytes)
{
	uint32_t eax = VMWARE_BACKDOOR_MAGIC;
	uint32_t ebx = (VMW_MESSAGE_STATUS_SUCCESS << 16) | VMW_PORT_CMD_HB_MSG;
	uint32_t ecx = bytes;
	uint32_t edx = ((uint32_t)channel->channel_id << 16) | VMWARE_BACKDOOR_HB_PORT;
	uint64_t rsi = (uint64_t)(uintptr_t)buf;
	uint64_t rdi = channel->cookie_low;
	uint64_t rbp = channel->cookie_high;

	__asm__ __volatile__("push %%rbp\n\t"
			     "mov %[bp], %%rbp\n\t"
			     "cld\n\t"
			     "rep outsb\n\t"
			     "pop %%rbp"
			     : "+a"(eax), "+b"(ebx), "+c"(ecx), "+d"(edx),
			       "+S"(rsi), "+D"(rdi)
			     : [bp] "r"(rbp)
			     : "memory", "cc");
	return ebx;
}

static uint32_t vmw_hypercall_hb_in(const struct rpc_channel *channel,
				    void *buf, uint32_t bytes)
{
	uint32_t eax = VMWARE_BACKDOOR_MAGIC;
	uint32_t ebx = (VMW_MESSAGE_STATUS_SUCCESS << 16) | VMW_PORT_CMD_HB_MSG;
	uint32_t ecx = bytes;
	uint32_t edx = ((uint32_t)channel->channel_id << 16) | VMWARE_BACKDOOR_HB_PORT;
	uint64_t rsi = channel->cookie_high;
	uint64_t rdi = (uint64_t)(uintptr_t)buf;
	uint64_t rbp = channel->cookie_low;

	__asm__ __volatile__("push %%rbp\n\t"
			     "mov %[bp], %%rbp\n\t"
			     "cld\n\t"
			     "rep insb\n\t"
			     "pop %%rbp"
			     : "+a"(eax), "+b"(ebx), "+c"(ecx), "+d"(edx),
			       "+S"(rsi), "+D"(rdi)
			     : [bp] "r"(rbp)
			     : "memory", "cc");
	return ebx;
}

static int vmw_open_channel(struct rpc_channel *channel, uint32_t protocol)
{
	static const struct rpc_channel none;
	struct vmware_backdoor_regs r;

	mm_memset(channel, 0, sizeof(*channel));
	vmw_rpc_call(VMW_PORT_CMD_OPEN_CHANNEL,
		     protocol | VMW_GUESTMSG_FLAG_COOKIE, &none, &r);
	if ((VMW_HIGH_WORD(r.ecx) & VMW_MESSAGE_STATUS_SUCCESS) == 0)
		return -EINVAL;

	channel->channel_id = (uint16_t)VMW_HIGH_WORD(r.edx);
	/* The cookie the host handed back: every later call on this channel
	 * has to present it, and a channel opened without noticing it would
	 * fail at the first send with nothing to say why. */
	channel->cookie_high = r.esi;
	channel->cookie_low = r.edi;
	return 0;
}

static int vmw_close_channel(struct rpc_channel *channel)
{
	struct vmware_backdoor_regs r;

	vmw_rpc_call(VMW_PORT_CMD_CLOSE_CHANNEL, 0, channel, &r);
	if ((VMW_HIGH_WORD(r.ecx) & VMW_MESSAGE_STATUS_SUCCESS) == 0)
		return -EINVAL;
	return 0;
}

/* Send the payload, through the high-bandwidth port when the host offered
 * it for this message, four bytes per call otherwise.  Returns the status
 * word of the last call (status in its high half). */
static uint32_t vmw_port_hb_out(struct rpc_channel *channel, const char *msg,
				uint32_t msg_len, int hb)
{
	struct vmware_backdoor_regs r;
	uint32_t ecx;

	if (hb)
		return vmw_hypercall_hb_out(channel, msg, msg_len);

	/* High-bandwidth port not available: four bytes at a time. */
	ecx = VMW_MESSAGE_STATUS_SUCCESS << 16;
	while (msg_len && (VMW_HIGH_WORD(ecx) & VMW_MESSAGE_STATUS_SUCCESS)) {
		uint32_t bytes = min_t(uint32_t, msg_len, 4);
		uint32_t word = 0;

		mm_memcpy(&word, msg, bytes);
		msg_len -= bytes;
		msg += bytes;
		vmw_rpc_call(VMW_PORT_CMD_SENDPAYLOAD, word, channel, &r);
		ecx = r.ecx;
	}
	return ecx;
}

/* Receive the payload, the same two ways.  Returns the status word of the
 * last call. */
static uint32_t vmw_port_hb_in(struct rpc_channel *channel, char *reply,
			       uint32_t reply_len, int hb)
{
	struct vmware_backdoor_regs r;
	uint32_t ecx;

	if (hb)
		return vmw_hypercall_hb_in(channel, reply, reply_len);

	/* High-bandwidth port not available: four bytes at a time. */
	ecx = VMW_MESSAGE_STATUS_SUCCESS << 16;
	while (reply_len) {
		uint32_t bytes = min_t(uint32_t, reply_len, 4);

		vmw_rpc_call(VMW_PORT_CMD_RECVPAYLOAD, VMW_MESSAGE_STATUS_SUCCESS,
			     channel, &r);
		ecx = r.ecx;
		if ((VMW_HIGH_WORD(ecx) & VMW_MESSAGE_STATUS_SUCCESS) == 0)
			break;
		mm_memcpy(reply, &r.ebx, bytes);
		reply_len -= bytes;
		reply += bytes;
	}
	return ecx;
}

/* Send one message (an RPCI command string, no terminator) on an open
 * channel.  A checkpoint in the middle sends the whole message again. */
static int vmw_send_msg(struct rpc_channel *channel, const char *msg,
			uint32_t msg_len)
{
	struct vmware_backdoor_regs r;
	uint32_t ebx;
	int retries = 0;

	while (retries < VMW_MSG_RETRIES) {
		retries++;

		vmw_rpc_call(VMW_PORT_CMD_SENDSIZE, msg_len, channel, &r);
		if ((VMW_HIGH_WORD(r.ecx) & VMW_MESSAGE_STATUS_SUCCESS) == 0) {
			/* Expected success.  Give up. */
			return -EINVAL;
		}

		/* Send msg */
		ebx = vmw_port_hb_out(channel, msg, msg_len,
				      !!(VMW_HIGH_WORD(r.ecx) & VMW_MESSAGE_STATUS_HB));
		if ((VMW_HIGH_WORD(ebx) & VMW_MESSAGE_STATUS_SUCCESS) != 0)
			return 0;
		if ((VMW_HIGH_WORD(ebx) & VMW_MESSAGE_STATUS_CPT) != 0)
			continue; /* a checkpoint occurred: retry */
		break;
	}
	return -EINVAL;
}

/* Receive the reply, if the host left one.  On success *msg is a
 * NUL-terminated kalloc() buffer the caller frees (NULL when there was no
 * reply, which is not an error) and *msg_len its length. */
static int vmw_recv_msg(struct rpc_channel *channel, char **msg,
			uint32_t *msg_len)
{
	struct vmware_backdoor_regs r;
	char *reply = NULL;
	uint32_t reply_len = 0;
	uint32_t ebx;
	int retries = 0;

	*msg_len = 0;
	*msg = NULL;

	while (retries < VMW_MSG_RETRIES) {
		retries++;

		vmw_rpc_call(VMW_PORT_CMD_RECVSIZE, 0, channel, &r);
		if ((VMW_HIGH_WORD(r.ecx) & VMW_MESSAGE_STATUS_SUCCESS) == 0)
			return -EINVAL;

		/* No reply available.  This is okay. */
		if ((VMW_HIGH_WORD(r.ecx) & VMW_MESSAGE_STATUS_DORECV) == 0)
			return 0;

		reply_len = r.ebx;
		if (reply_len > VMW_MSG_REPLY_MAX)
			return -ENOMEM;
		reply = kalloc(reply_len + 1);
		if (!reply)
			return -ENOMEM;
		mm_memset(reply, 0, reply_len + 1);

		/* Receive buffer */
		ebx = vmw_port_hb_in(channel, reply, reply_len,
				     !!(VMW_HIGH_WORD(r.ecx) & VMW_MESSAGE_STATUS_HB));
		if ((VMW_HIGH_WORD(ebx) & VMW_MESSAGE_STATUS_SUCCESS) == 0) {
			kfree(reply);
			reply = NULL;
			if ((VMW_HIGH_WORD(ebx) & VMW_MESSAGE_STATUS_CPT) != 0)
				continue; /* a checkpoint occurred: retry */
			return -EINVAL;
		}

		reply[reply_len] = '\0';

		/* Acknowledge, or the host keeps the reply queued. */
		vmw_rpc_call(VMW_PORT_CMD_RECVSTATUS, VMW_MESSAGE_STATUS_SUCCESS,
			     channel, &r);
		if ((VMW_HIGH_WORD(r.ecx) & VMW_MESSAGE_STATUS_SUCCESS) == 0) {
			kfree(reply);
			reply = NULL;
			if ((VMW_HIGH_WORD(r.ecx) & VMW_MESSAGE_STATUS_CPT) != 0)
				continue; /* a checkpoint occurred: retry */
			return -EINVAL;
		}
		break;
	}

	if (!reply)
		return -EINVAL;

	*msg_len = reply_len;
	*msg = reply;
	return 0;
}

static uint32_t vmw_msg_strlen(const char *s)
{
	uint32_t n = 0;

	while (s[n])
		n++;
	return n;
}

/* Failures after the channel was found to exist are worth a line, but a
 * client logging in a loop must not turn them into a flood. */
static void vmw_msg_report_failure(const char *what)
{
	static int budget = 8;

	if (budget > 0) {
		budget--;
		kprintf("[drm] vmwgfx: %s\n", what);
	}
}

/* Is the channel there at all?  The answer is asked for once and kept.
 *
 * Only asked where the SVGA adapter is present: that is what says this is a
 * hypervisor speaking the protocol, and on bare metal the port belongs to
 * nobody (or to somebody else). */
int vmw_msg_probe(void)
{
	struct rpc_channel channel;
	int state = vmw_msg_state;

	if (state)
		return state > 0;
	if (!vmsvga2_hw_present()) {
		vmw_msg_state = -1;
		return 0;
	}
	if (vmw_open_channel(&channel, VMW_RPCI_PROTOCOL_NUM) != 0) {
		vmw_msg_state = -1;
		return 0;
	}
	vmw_close_channel(&channel);
	vmw_msg_state = 1;
	return 1;
}

/* Bring-up of the channel for the driver: record whether the host answers
 * and introduce the driver in the host's log, as the first thing the log
 * shows of this guest's graphics stack. */
void vmw_msg_init(struct vmw_device *v)
{
	const struct drm_driver *drv = v->drm.drv;

	v->has_msg = vmw_msg_probe();
	if (!v->has_msg || !drv)
		return;
#ifdef LIKEOS_VERSION
	vmw_host_printf("vmwgfx: Module Version: %d.%d.%d (kernel: LikeOS %s)",
			drv->major, drv->minor, drv->patch, LIKEOS_VERSION);
#else
	vmw_host_printf("vmwgfx: Module Version: %d.%d.%d (kernel: LikeOS)",
			drv->major, drv->minor, drv->patch);
#endif
}

/* vmw_host_get_guestinfo - the value of a guestinfo.* variable of the
 * virtual machine's configuration (e.g. "guestinfo.svga.gl3"), as the
 * string the host keeps; parsing it is the caller's business.
 *
 * @buffer may be NULL to ask only for the length.  On entry *@length is the
 * size of @buffer, on return the number of bytes stored (no terminator is
 * added).  A variable the host does not know comes back as its error text,
 * the way the host phrases it. */
int vmw_host_get_guestinfo(const char *guest_info_param, char *buffer,
			   size_t *length)
{
	struct rpc_channel channel;
	char *msg, *reply = NULL;
	uint32_t reply_len = 0;
	size_t out_len;
	uint32_t plen, n;
	static const char cmd[] = "info-get ";

	if (!vmw_msg_probe())
		return -ENODEV;
	if (!guest_info_param || !length)
		return -EINVAL;

	plen = vmw_msg_strlen(guest_info_param);
	if (plen > VMW_HOST_LOG_MAX)
		return -EINVAL;
	n = (uint32_t)(sizeof(cmd) - 1) + plen;
	msg = kalloc(n + 1);
	if (!msg)
		return -ENOMEM;
	mm_memcpy(msg, cmd, sizeof(cmd) - 1);
	mm_memcpy(msg + sizeof(cmd) - 1, guest_info_param, plen);
	msg[n] = '\0';

	if (vmw_open_channel(&channel, VMW_RPCI_PROTOCOL_NUM))
		goto out_open;

	if (vmw_send_msg(&channel, msg, n) ||
	    vmw_recv_msg(&channel, &reply, &reply_len))
		goto out_msg;

	vmw_close_channel(&channel);

	/* The first two characters of the reply are its status code ("1 "
	 * for found, "0 " for not), not part of the value. */
	out_len = reply_len > 2 ? reply_len - 2 : 0;
	if (buffer && reply && out_len > 0) {
		out_len = min_t(size_t, out_len, *length);
		if (out_len > 0)
			mm_memcpy(buffer, reply + 2, out_len);
	}
	*length = out_len;

	kfree(reply);
	kfree(msg);
	return 0;

out_msg:
	vmw_close_channel(&channel);
	kfree(reply);
out_open:
	*length = 0;
	kfree(msg);
	vmw_msg_report_failure("failed to get guest info from the host");
	return -EINVAL;
}

/* vmw_host_printf - a line for the hypervisor's log of this virtual machine.
 * Longer than VMW_HOST_LOG_MAX is cut. */
int vmw_host_printf(const char *fmt, ...)
{
	struct rpc_channel channel;
	__builtin_va_list ap;
	static const char pfx[] = "log ";
	char *msg;
	uint32_t n;

	if (!vmw_msg_probe())
		return -ENODEV;
	if (!fmt)
		return 0;

	msg = kalloc(VMW_HOST_LOG_MAX);
	if (!msg)
		return -ENOMEM;
	mm_memcpy(msg, pfx, sizeof(pfx));
	__builtin_va_start(ap, fmt);
	kvsnprintf(msg + sizeof(pfx) - 1, VMW_HOST_LOG_MAX - (sizeof(pfx) - 1),
		   fmt, ap);
	__builtin_va_end(ap);
	msg[VMW_HOST_LOG_MAX - 1] = '\0';
	n = vmw_msg_strlen(msg);

	if (vmw_open_channel(&channel, VMW_RPCI_PROTOCOL_NUM))
		goto out_open;

	if (vmw_send_msg(&channel, msg, n))
		goto out_msg;

	vmw_close_channel(&channel);
	kfree(msg);
	return 0;

out_msg:
	vmw_close_channel(&channel);
out_open:
	kfree(msg);
	vmw_msg_report_failure("failed to send host log message");
	return -EINVAL;
}

/* A log line from the kernel side. */
int vmw_host_log(const char *line)
{
	if (!line)
		return -EINVAL;
	return vmw_host_printf("%s", line);
}

/* The client's string, NUL included, into msg (VMW_MAX_USER_MSG_LENGTH
 * bytes).  Copied a page-bounded piece at a time: the string may end just
 * before an unmapped page, so the copy never reaches past the page it is
 * reading in, and stops at the first terminator.  Returns the length, or
 * -EINVAL when the pointer faults or no terminator came within the limit. */
static int vmw_msg_copy_user_string(char *msg, uint64_t user)
{
	uint32_t n = 0;

	while (n < VMW_MAX_USER_MSG_LENGTH) {
		uint64_t addr = user + n;
		uint32_t chunk = PAGE_SIZE - (uint32_t)(addr & (PAGE_SIZE - 1));

		if (chunk > VMW_MAX_USER_MSG_LENGTH - n)
			chunk = VMW_MAX_USER_MSG_LENGTH - n;
		if (drm_copy_from_user(msg + n, (const void *)(uintptr_t)addr,
				       chunk) != 0)
			return -EINVAL;
		for (uint32_t i = 0; i < chunk; i++)
			if (msg[n + i] == '\0')
				return (int)(n + i);
		n += chunk;
	}
	return -EINVAL;
}

/* DRM_VMW_MSG: userspace sends an RPCI string and may read the reply.
 *
 * The string and the reply buffer are user pointers, so both are copied
 * rather than touched in place -- the port sequence runs with the message in
 * kernel memory, and a fault in the middle of it would leave the channel
 * half-open.
 *
 * The reply is stored at `receive' and its length in `receive_len'.  A
 * caller that sets `receive_len' on entry gets no more than that many bytes
 * (and a terminator when there is room for one); `receive_len' 0 takes the
 * whole reply, without a terminator.  Without a `receive' pointer the reply is read and dropped. */
long vmw_ioctl_msg(struct vmw_device *v, struct drm_vmw_msg_arg *a)
{
	struct rpc_channel channel;
	char *msg;
	int length;

	(void)v;
	if (!vmw_msg_probe())
		return -EINVAL;

	msg = kalloc(VMW_MAX_USER_MSG_LENGTH);
	if (!msg)
		return -ENOMEM;

	length = vmw_msg_copy_user_string(msg, a->send);
	if (length < 0) {
		kfree(msg);
		return -EINVAL;
	}

	if (vmw_open_channel(&channel, VMW_RPCI_PROTOCOL_NUM))
		goto out_open;

	if (vmw_send_msg(&channel, msg, (uint32_t)length))
		goto out_msg;

	if (!a->send_only) {
		char *reply = NULL;
		uint32_t reply_len = 0;

		if (vmw_recv_msg(&channel, &reply, &reply_len))
			goto out_msg;

		if (reply && reply_len > 0 && a->receive) {
			uint32_t cap = a->receive_len;
			uint32_t n = reply_len;

			if (cap && n > cap)
				n = cap;
			/* the terminator too, when the caller left room */
			if (drm_copy_to_user((void *)(uintptr_t)a->receive, reply,
					     (cap && n < cap) ? n + 1 : n) != 0) {
				kfree(reply);
				goto out_msg;
			}
			a->receive_len = n;
		}
		kfree(reply);
	}

	vmw_close_channel(&channel);
	kfree(msg);
	return 0;

out_msg:
	vmw_close_channel(&channel);
out_open:
	kfree(msg);
	return -EINVAL;
}
