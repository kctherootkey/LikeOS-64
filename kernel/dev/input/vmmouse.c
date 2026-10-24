// LikeOS -- VMware absolute pointing device ("vmmouse"): the protocol.
//
// Four backdoor commands: ABSPOINTER_COMMAND switches the device (enable,
// disable, absolute or relative reports), ABSPOINTER_STATUS answers how many
// words are queued (or an error in the high half), ABSPOINTER_DATA hands over
// that many words -- four per report: status, x, y, z -- and
// ABSPOINTER_RESTRICT limits who may use the device.  See vmmouse.h.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/input/vmmouse.h>
#include <kernel/hal/vmware_backdoor.h>

#define VMMOUSE_CMD_ENABLE 0x45414552U
#define VMMOUSE_CMD_DISABLE 0x000000f5U
#define VMMOUSE_CMD_REQUEST_ABSOLUTE 0x53424152U

// ABSPOINTER_STATUS: the high half all ones is an error.
#define VMMOUSE_ERROR 0xffff0000U
// The first word an enabled device queues.
#define VMMOUSE_VERSION_ID 0x3442554aU

#define VMMOUSE_RESTRICT_CPL0 0x01U

// Words per report.
#define VMMOUSE_REPORT_WORDS 4U

static uint32_t vmmouse_cmd(uint32_t cmd, uint32_t arg)
{
	return vmware_backdoor_cmd(cmd, arg, NULL, NULL, NULL);
}

// Is the backdoor there to talk to?  The hypervisor first, then GETVERSION,
// which answers with the magic in EBX.
static int vmmouse_present(void)
{
	uint32_t ebx = 0, version;
	struct vmware_backdoor_regs r = {
		.ebx = ~VMWARE_BACKDOOR_MAGIC,
		.ecx = VMWARE_BACKDOOR_CMD_GETVERSION,
		.edx = VMWARE_BACKDOOR_PORT,
	};

	if (!vmware_backdoor_hypervisor_present())
		return 0;
	vmware_backdoor_call(&r);
	version = r.eax;
	ebx = r.ebx;
	return ebx == VMWARE_BACKDOOR_MAGIC && version != 0xffffffffU;
}

void vmmouse_disable(void)
{
	(void)vmmouse_cmd(VMWARE_BACKDOOR_CMD_ABSPOINTER_COMMAND,
			  VMMOUSE_CMD_DISABLE);
	(void)vmmouse_cmd(VMWARE_BACKDOOR_CMD_ABSPOINTER_STATUS, 0);
}

int vmmouse_enable(void)
{
	uint32_t status, version;

	if (!vmmouse_present())
		return -1;
	/* An enabled device queues its version id first: no id, no device
	 * (vmmouse.present = FALSE, or a host without one). */
	(void)vmmouse_cmd(VMWARE_BACKDOOR_CMD_ABSPOINTER_COMMAND,
			  VMMOUSE_CMD_ENABLE);
	status = vmmouse_cmd(VMWARE_BACKDOOR_CMD_ABSPOINTER_STATUS, 0);
	if ((status & 0x0000ffffU) == 0)
		return -1;
	version = vmmouse_cmd(VMWARE_BACKDOOR_CMD_ABSPOINTER_DATA, 1);
	if (version != VMMOUSE_VERSION_ID) {
		vmmouse_disable();
		return -1;
	}
	/* The kernel's alone: user space cannot read or steer the pointer
	 * through the backdoor behind the input devices' permissions. */
	(void)vmmouse_cmd(VMWARE_BACKDOOR_CMD_ABSPOINTER_RESTRICT,
			  VMMOUSE_RESTRICT_CPL0);
	(void)vmmouse_cmd(VMWARE_BACKDOOR_CMD_ABSPOINTER_COMMAND,
			  VMMOUSE_CMD_REQUEST_ABSOLUTE);
	return 0;
}

int vmmouse_read(struct vmmouse_packet *p)
{
	uint32_t status, queued;
	uint32_t x = 0, y = 0, z = 0;

	status = vmmouse_cmd(VMWARE_BACKDOOR_CMD_ABSPOINTER_STATUS, 0);
	if ((status & VMMOUSE_ERROR) == VMMOUSE_ERROR)
		return -1;
	queued = status & 0xffffU;
	if (queued == 0)
		return 0;
	/* Whole reports only; anything else and the words no longer line up
	 * with the fields. */
	if (queued % VMMOUSE_REPORT_WORDS)
		return -1;
	p->status = vmware_backdoor_cmd(VMWARE_BACKDOOR_CMD_ABSPOINTER_DATA,
					VMMOUSE_REPORT_WORDS, &x, &y, &z);
	p->x = x;
	p->y = y;
	p->z = z;
	return 1;
}
