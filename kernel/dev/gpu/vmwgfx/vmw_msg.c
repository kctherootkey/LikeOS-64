// LikeOS-64 -- vmwgfx: the guest-to-host message channel (the "backdoor").
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
// The protocol, as the open specification and the reference driver describe
// it:
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
// Failure is normal and not an error: on a hypervisor that does not
// implement RPCI (or on real hardware, where port 0x5658 reads back all
// ones) the OPEN simply fails, and everything above is told so.  Nothing in
// the graphics path depends on a message getting through.

#include <kernel/dev/gpu/vmwgfx/vmw_gb.h>
#include <kernel/dev/gpu/drm_internal.h>
#include <kernel/uapi/drm/vmwgfx_drm.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

#define VMW_PORT_MAGIC 0x564D5868u /* 'VMXh' */
#define VMW_PORT_CMD_PORT 0x5658
#define VMW_PORT_HB_PORT 0x5659

#define VMW_PORT_CMD_MSG 30
#define VMW_PORT_CMD_HB_MSG 0
#define VMW_PORT_CMD_OPEN_CHANNEL (VMW_PORT_CMD_MSG | (0 << 16))
#define VMW_PORT_CMD_CLOSE_CHANNEL (VMW_PORT_CMD_MSG | (6 << 16))
#define VMW_PORT_CMD_SENDSIZE (VMW_PORT_CMD_MSG | (1 << 16))
#define VMW_PORT_CMD_RECVSIZE (VMW_PORT_CMD_MSG | (3 << 16))
#define VMW_PORT_CMD_RECVSTATUS (VMW_PORT_CMD_MSG | (5 << 16))

#define VMW_HB_PORT_CMD_MSG 0
#define VMW_HB_PORT_CMD_SEND (VMW_HB_PORT_CMD_MSG | (0 << 16))
#define VMW_HB_PORT_CMD_RECV (VMW_HB_PORT_CMD_MSG | (1 << 16))

#define VMW_RPCI_PROTOCOL_NUM 0x49435052u /* 'RPCI' */
#define VMW_MESSAGE_STATUS_SUCCESS 0x0001
#define VMW_MESSAGE_STATUS_DORECV 0x0002
#define VMW_MESSAGE_STATUS_CPT 0x0010
#define VMW_MESSAGE_STATUS_HB 0x0080

#define VMW_MSG_MAX 8192 /* what one call may carry, either way */

struct vmw_msg_channel {
	uint32_t channel_id;
	uint32_t proto_num;
	uint32_t cookie_high;
	uint32_t cookie_low;
};

/* The one instruction the whole protocol is built on: IN from the command
 * port with the magic in EAX, the command in ECX and the port (with the
 * channel in its high half) in EDX.  Every register comes back changed --
 * including ESI and EDI, which is where the channel's cookie is handed over
 * on open and expected back on every later call. */
static inline void vmw_port(uint32_t cmd, uint32_t in_ebx, uint32_t channel,
			    uint32_t cookie_hi, uint32_t cookie_lo,
			    uint32_t *out_eax, uint32_t *out_ebx,
			    uint32_t *out_ecx, uint32_t *out_edx,
			    uint32_t *out_esi, uint32_t *out_edi)
{
	uint32_t eax = VMW_PORT_MAGIC, ebx = in_ebx, ecx = cmd;
	uint32_t edx = (channel << 16) | VMW_PORT_CMD_PORT;
	uint32_t esi = cookie_hi, edi = cookie_lo;

	__asm__ __volatile__("inl %%dx, %%eax"
			     : "+a"(eax), "+b"(ebx), "+c"(ecx), "+d"(edx),
			       "+S"(esi), "+D"(edi)
			     :
			     : "memory");
	if (out_eax)
		*out_eax = eax;
	if (out_ebx)
		*out_ebx = ebx;
	if (out_ecx)
		*out_ecx = ecx;
	if (out_edx)
		*out_edx = edx;
	if (out_esi)
		*out_esi = esi;
	if (out_edi)
		*out_edi = edi;
}

/* The high-bandwidth transfers: one string instruction moves the whole
 * message, with the buffer in ESI (out) or EDI (in) and the other half of
 * the cookie in RBP -- which is why these cannot be written as a plain
 * inline asm with a memory clobber and no frame pointer saved by hand.
 *
 * The register assignment is the protocol's, not a choice:
 *   EAX  the magic          EBX  status | HB message command
 *   ECX  the byte count     EDX  the high-bandwidth port, channel in the
 *                                high half, plus the direction bit
 *   ESI  source (out) / cookie high (in)
 *   EDI  cookie low (out) / destination (in)
 *   RBP  the other half of the cookie
 */
#define VMW_HB_DIR_OUT (1u << 1)

static inline void vmw_port_hb_out(const struct vmw_msg_channel *ch, uint32_t bytes,
				   const void *buf, uint32_t *out_ebx)
{
	uint32_t eax = VMW_PORT_MAGIC;
	uint32_t ebx = (VMW_MESSAGE_STATUS_SUCCESS << 16) | VMW_HB_PORT_CMD_MSG;
	uint32_t ecx = bytes;
	uint32_t edx = (ch->channel_id << 16) | VMW_PORT_HB_PORT | VMW_HB_DIR_OUT;
	uint64_t rsi = (uint64_t)(uintptr_t)buf;
	uint64_t rdi = ch->cookie_low;
	uint64_t rbp = ch->cookie_high;

	__asm__ __volatile__("push %%rbp\n\t"
			     "mov %[bp], %%rbp\n\t"
			     "cld\n\t"
			     "rep outsb\n\t"
			     "pop %%rbp"
			     : "+a"(eax), "+b"(ebx), "+c"(ecx), "+d"(edx),
			       "+S"(rsi), "+D"(rdi)
			     : [bp] "r"(rbp)
			     : "memory", "cc");
	if (out_ebx)
		*out_ebx = ebx;
}

static inline void vmw_port_hb_in(const struct vmw_msg_channel *ch, uint32_t bytes,
				  void *buf, uint32_t *out_ebx)
{
	uint32_t eax = VMW_PORT_MAGIC;
	uint32_t ebx = (VMW_MESSAGE_STATUS_SUCCESS << 16) | VMW_HB_PORT_CMD_MSG;
	uint32_t ecx = bytes;
	uint32_t edx = (ch->channel_id << 16) | VMW_PORT_HB_PORT;
	uint64_t rsi = ch->cookie_high;
	uint64_t rdi = (uint64_t)(uintptr_t)buf;
	uint64_t rbp = ch->cookie_low;

	__asm__ __volatile__("push %%rbp\n\t"
			     "mov %[bp], %%rbp\n\t"
			     "cld\n\t"
			     "rep insb\n\t"
			     "pop %%rbp"
			     : "+a"(eax), "+b"(ebx), "+c"(ecx), "+d"(edx),
			       "+S"(rsi), "+D"(rdi)
			     : [bp] "r"(rbp)
			     : "memory", "cc");
	if (out_ebx)
		*out_ebx = ebx;
}

static int vmw_msg_open(struct vmw_msg_channel *ch, uint32_t proto)
{
	uint32_t eax, ebx, ecx, edx;

	uint32_t esi, edi;

	mm_memset(ch, 0, sizeof(*ch));
	vmw_port(VMW_PORT_CMD_OPEN_CHANNEL, proto | 0x80000000u, 0, 0, 0, &eax,
		 &ebx, &ecx, &edx, &esi, &edi);
	if ((ecx & (VMW_MESSAGE_STATUS_SUCCESS << 16)) == 0)
		return -ENODEV;
	ch->channel_id = (edx >> 16) & 0xFFFF;
	ch->proto_num = proto;
	/* The cookie the host handed back: every later call on this channel
	 * has to present it, and a channel opened without noticing it would
	 * fail at the first send with nothing to say why. */
	ch->cookie_high = esi;
	ch->cookie_low = edi;
	return 0;
}

static void vmw_msg_close(struct vmw_msg_channel *ch)
{
	vmw_port(VMW_PORT_CMD_CLOSE_CHANNEL, 0, ch->channel_id, ch->cookie_high,
		 ch->cookie_low, NULL, NULL, NULL, NULL, NULL, NULL);
}

/* Send one message (the RPCI command string) on an open channel. */
static int vmw_msg_send(struct vmw_msg_channel *ch, const char *msg, uint32_t len)
{
	uint32_t eax, ebx, ecx, edx;

	vmw_port(VMW_PORT_CMD_SENDSIZE, len, ch->channel_id, ch->cookie_high,
		 ch->cookie_low, &eax, &ebx, &ecx, &edx, NULL, NULL);
	if ((ecx & (VMW_MESSAGE_STATUS_SUCCESS << 16)) == 0)
		return -EIO;

	if (len == 0)
		return 0;

	if (ecx & (VMW_MESSAGE_STATUS_HB << 16)) {
		uint32_t status = 0;
		vmw_port_hb_out(ch, len, msg, &status);
		/* The result of a high-bandwidth transfer comes back in the
		 * HIGH half of EBX, unlike the low-bandwidth calls whose
		 * status is in the high half of ECX. */
		return ((status >> 16) & VMW_MESSAGE_STATUS_SUCCESS) ? 0 : -EIO;
	}

	/* Low bandwidth: four bytes per exchange. */
	for (uint32_t off = 0; off < len; off += 4) {
		uint32_t word = 0;
		uint32_t n = len - off;

		if (n > 4)
			n = 4;
		for (uint32_t i = 0; i < n; i++)
			word |= (uint32_t)(uint8_t)msg[off + i] << (8 * i);
		vmw_port(VMW_PORT_CMD_MSG | (2 << 16) /* SEND PAYLOAD */, word,
			 ch->channel_id, ch->cookie_high, ch->cookie_low, &eax,
			 &ebx, &ecx, &edx, NULL, NULL);
		if ((ecx & (VMW_MESSAGE_STATUS_SUCCESS << 16)) == 0)
			return -EIO;
	}
	return 0;
}

/* Read the reply, if the host left one. */
static int vmw_msg_recv(struct vmw_msg_channel *ch, char *buf, uint32_t cap,
			uint32_t *out_len)
{
	uint32_t eax, ebx, ecx, edx;
	uint32_t len;

	*out_len = 0;
	vmw_port(VMW_PORT_CMD_RECVSIZE, 0, ch->channel_id, ch->cookie_high,
		 ch->cookie_low, &eax, &ebx, &ecx, &edx, NULL, NULL);
	if ((ecx & (VMW_MESSAGE_STATUS_SUCCESS << 16)) == 0)
		return -EIO;
	if ((ecx & (VMW_MESSAGE_STATUS_DORECV << 16)) == 0)
		return 0; /* nothing waiting */
	len = ebx;
	if (len == 0)
		return 0;
	if (len >= cap)
		len = cap - 1;

	if (ecx & (VMW_MESSAGE_STATUS_HB << 16)) {
		uint32_t status = 0;
		vmw_port_hb_in(ch, len, buf, &status);
		if (!((status >> 16) & VMW_MESSAGE_STATUS_SUCCESS))
			return -EIO;
	} else {
		for (uint32_t off = 0; off < len; off += 4) {
			vmw_port(VMW_PORT_CMD_MSG | (4 << 16) /* RECV PAYLOAD */,
				 VMW_MESSAGE_STATUS_SUCCESS, ch->channel_id,
				 ch->cookie_high, ch->cookie_low, &eax, &ebx,
				 &ecx, &edx, NULL, NULL);
			if ((ecx & (VMW_MESSAGE_STATUS_SUCCESS << 16)) == 0)
				return -EIO;
			for (uint32_t i = 0; i < 4 && off + i < len; i++)
				buf[off + i] = (char)((ebx >> (8 * i)) & 0xFF);
		}
	}
	buf[len] = '\0';
	*out_len = len;

	/* Acknowledge, or the host keeps the reply queued. */
	vmw_port(VMW_PORT_CMD_RECVSTATUS, VMW_MESSAGE_STATUS_SUCCESS,
		 ch->channel_id, ch->cookie_high, ch->cookie_low, &eax, &ebx,
		 &ecx, &edx, NULL, NULL);
	return 0;
}

/* One RPCI exchange: open, send, optionally read the reply, close. */
static int vmw_msg_rpci(const char *msg, uint32_t len, char *reply,
			uint32_t reply_cap, uint32_t *reply_len)
{
	struct vmw_msg_channel ch;
	int rc;

	rc = vmw_msg_open(&ch, VMW_RPCI_PROTOCOL_NUM);
	if (rc)
		return rc;
	rc = vmw_msg_send(&ch, msg, len);
	if (rc == 0 && reply && reply_cap)
		rc = vmw_msg_recv(&ch, reply, reply_cap, reply_len);
	vmw_msg_close(&ch);
	return rc;
}

/* Is the channel there at all?  Asked once, at probe. */
int vmw_msg_probe(void)
{
	struct vmw_msg_channel ch;

	if (vmw_msg_open(&ch, VMW_RPCI_PROTOCOL_NUM) != 0)
		return 0;
	vmw_msg_close(&ch);
	return 1;
}

/* A log line from the kernel side. */
int vmw_host_log(const char *line)
{
	char buf[256];
	uint32_t n = 0;

	if (!line)
		return -EINVAL;
	const char *pfx = "log ";
	while (*pfx && n < sizeof(buf) - 1)
		buf[n++] = *pfx++;
	while (*line && n < sizeof(buf) - 1)
		buf[n++] = *line++;
	buf[n] = '\0';
	return vmw_msg_rpci(buf, n, NULL, 0, NULL);
}

/* DRM_VMW_MSG: userspace sends an RPCI string and may read the reply.
 *
 * The string and the reply buffer are user pointers, so both are copied
 * rather than touched in place -- the port sequence runs with the message in
 * kernel memory, and a fault in the middle of it would leave the channel
 * half-open. */
long vmw_ioctl_msg(struct vmw_device *v, struct drm_vmw_msg_arg *a)
{
	char *msg, *reply = NULL;
	uint32_t reply_len = 0;
	long rc;

	if (!v->has_msg)
		return -ENODEV;
	if (!a->send)
		return -EINVAL;

	msg = kalloc(VMW_MSG_MAX);
	if (!msg)
		return -ENOMEM;
	/* Bounded copy: the string is NUL-terminated in user memory and must
	 * be no longer than the buffer. */
	uint32_t n = 0;
	for (;;) {
		char c;
		if (drm_copy_from_user(&c, (const void *)(uintptr_t)(a->send + n), 1) != 0) {
			kfree(msg);
			return -EFAULT;
		}
		msg[n] = c;
		if (!c)
			break;
		if (++n >= VMW_MSG_MAX - 1) {
			msg[n] = '\0';
			break;
		}
	}

	if (!a->send_only && a->receive && a->receive_len) {
		uint32_t cap = a->receive_len;
		if (cap > VMW_MSG_MAX)
			cap = VMW_MSG_MAX;
		reply = kalloc(cap);
		if (!reply) {
			kfree(msg);
			return -ENOMEM;
		}
		rc = vmw_msg_rpci(msg, n, reply, cap, &reply_len);
		if (rc == 0 && reply_len) {
			if (drm_copy_to_user((void *)(uintptr_t)a->receive, reply,
					 reply_len + 1) != 0)
				rc = -EFAULT;
		}
		kfree(reply);
	} else {
		rc = vmw_msg_rpci(msg, n, NULL, 0, NULL);
	}
	kfree(msg);
	return rc;
}
