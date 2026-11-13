// LikeOS - NVMe sysfs: /sys/class/nvme/nvmeX, its namespaces, hwmon,
// power and fault injection directories, /sys/class/nvme-subsystem
//
// Controllers come and go (hot removal, delete_controller), and the
// pseudo filesystem cannot remove static nodes, so both class
// directories are DYNAMIC: their listings and lookups are produced from
// the live objects on every walk.  A node made by a lookup carries the
// object's instance number (and namespace ID) in arg2, never a pointer;
// reading or writing it resolves the object again under its registry
// lock and holds a reference for the duration, so a node outliving its
// controller only answers -ENODEV.
//
//   /sys/class/nvme/nvmeX/              controller attributes (the
//                                       established names), dev, uevent,
//                                       device -> PCI function,
//                                       subsystem -> /sys/class/nvme
//   /sys/class/nvme/nvmeX/nvmeXnY/      namespace attributes (nsid, wwid,
//                                       eui, nguid, uuid, csi, ...)
//   /sys/class/nvme/nvmeX/power/        pm_qos_latency_tolerance_us (APST)
//   /sys/class/nvme/nvmeX/hwmonX/       name, temp<N>_{input,max,min,
//                                       crit,alarm,label}
//   /sys/class/nvme/nvmeX/fault_inject  fault injection (P6's one-file
//                                       view, also per namespace)
//   /sys/class/nvme-subsystem/nvme-subsysX/  subsystem attributes, links
//                                       to its controllers, multipath
//                                       head directories
// plus, once per instance number, static links /sys/dev/char/MAJ:MIN and
// <PCI function>/nvme/nvmeX pointing at /sys/class/nvme/nvmeX.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/nvme/nvme_pci.h>
#include <kernel/fs/sysfs.h>
#include <kernel/fs/pseudofs.h>
#include <kernel/fs/vfs.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

#ifndef DT_DIR
#define DT_DIR 4
#endif
#ifndef DT_REG
#define DT_REG 8
#endif
#ifndef DT_LNK
#define DT_LNK 10
#endif

/* ==========================================================================
 * Registry of visible controllers (by instance)
 * ========================================================================== */

static struct nvme_ctrl *nvme_sysfs_ctrls[NVME_MAX_INSTANCES];
static spinlock_t nvme_sysfs_lock = SPINLOCK_INIT("nvme_sysfs");
/* Instances whose static links (dev/char, PCI) exist already: they are
 * never removed, and point at the dynamic directory, which answers for
 * whichever controller holds the number now. */
static uint8_t nvme_sysfs_linked[NVME_MAX_INSTANCES / 8];
static struct pfs *nvme_sysfs_fs;

static struct nvme_ctrl *nvme_sysfs_get_ctrl(unsigned int instance)
{
	struct nvme_ctrl *ctrl = NULL;
	uint64_t flags;

	if (instance >= NVME_MAX_INSTANCES)
		return NULL;
	spin_lock_irqsave(&nvme_sysfs_lock, &flags);
	ctrl = nvme_sysfs_ctrls[instance];
	if (ctrl && !refcount_inc_not_zero(&ctrl->ref))
		ctrl = NULL;
	spin_unlock_irqrestore(&nvme_sysfs_lock, flags);
	return ctrl;
}

static struct nvme_subsystem *nvme_sysfs_get_subsys(unsigned int instance)
{
	struct nvme_subsystem *subsys, *found = NULL;

	mutex_lock(&nvme_subsystems_lock);
	list_for_each_entry(subsys, &nvme_subsystems, entry) {
		if (subsys->instance == (int)instance &&
		    refcount_inc_not_zero(&subsys->ref)) {
			found = subsys;
			break;
		}
	}
	mutex_unlock(&nvme_subsystems_lock);
	return found;
}

/* ==========================================================================
 * Node encoding
 * ========================================================================== */

enum nvme_sysfs_kind {
	NK_NONE = 0,
	NK_CTRL_DIR,
	NK_CTRL_ATTR,
	NK_POWER_DIR,
	NK_POWER_ATTR,
	NK_HWMON_DIR,
	NK_HWMON_ATTR,
	NK_CFI_FILE, /* controller fault_inject (P6's one-file view) */
	NK_NS_DIR,
	NK_NS_ATTR,
	NK_NSFI_FILE, /* namespace fault_inject */
	NK_SUBSYS_DIR,
	NK_SUBSYS_ATTR,
	NK_HEAD_DIR,
	NK_HEAD_ATTR,
	NK_MPATH_DIR, /* <head>/multipath: one link per path */
};

static inline uint64_t nvme_sysfs_key(unsigned int instance, uint32_t nsid,
				      unsigned int kind, unsigned int attr)
{
	return ((uint64_t)(instance & 0xffff) << 48) |
	       ((uint64_t)nsid << 16) | ((uint64_t)(kind & 0xff) << 8) |
	       (attr & 0xff);
}
static inline unsigned int nk_instance(const struct pfs_node *n)
{
	return (unsigned int)(n->arg2 >> 48);
}
static inline uint32_t nk_nsid(const struct pfs_node *n)
{
	return (uint32_t)(n->arg2 >> 16);
}
static inline unsigned int nk_kind(const struct pfs_node *n)
{
	return (unsigned int)((n->arg2 >> 8) & 0xff);
}
static inline unsigned int nk_attr(const struct pfs_node *n)
{
	return (unsigned int)(n->arg2 & 0xff);
}

/* ==========================================================================
 * Small helpers
 * ========================================================================== */

/* Parse a boolean ("1/0/y/n/on/off/true/false", trailing newline ok). */
static int nvme_sysfs_parse_bool(const char *buf, long len, bool *out)
{
	char tmp[8];
	long n = len;

	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
		n--;
	if (n <= 0 || n >= (long)sizeof(tmp))
		return -EINVAL;
	kmemcpy(tmp, buf, (size_t)n);
	tmp[n] = '\0';
	if (!kstrcmp(tmp, "1") || !kstrcmp(tmp, "y") || !kstrcmp(tmp, "Y") ||
	    !kstrcmp(tmp, "on") || !kstrcmp(tmp, "true")) {
		*out = true;
		return 0;
	}
	if (!kstrcmp(tmp, "0") || !kstrcmp(tmp, "n") || !kstrcmp(tmp, "N") ||
	    !kstrcmp(tmp, "off") || !kstrcmp(tmp, "false")) {
		*out = false;
		return 0;
	}
	return -EINVAL;
}

/* Parse a signed integer (decimal or 0x hex). */
static int nvme_sysfs_parse_s64(const char *buf, long len, int64_t *out)
{
	int64_t v = 0;
	long i = 0;
	int neg = 0, base = 10, digits = 0;

	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == ' '))
		len--;
	if (i < len && buf[i] == '-') {
		neg = 1;
		i++;
	}
	if (i + 1 < len && buf[i] == '0' && (buf[i + 1] == 'x' ||
					     buf[i + 1] == 'X')) {
		base = 16;
		i += 2;
	}
	for (; i < len; i++) {
		char c = buf[i];
		int d;

		if (c >= '0' && c <= '9')
			d = c - '0';
		else if (base == 16 && c >= 'a' && c <= 'f')
			d = c - 'a' + 10;
		else if (base == 16 && c >= 'A' && c <= 'F')
			d = c - 'A' + 10;
		else
			return -EINVAL;
		v = v * base + d;
		digits++;
	}
	if (!digits)
		return -EINVAL;
	*out = neg ? -v : v;
	return 0;
}

static long nvme_sysfs_hex(char *buf, long cap, long p, const uint8_t *b,
			   int n, const char *sep)
{
	int i;

	for (i = 0; i < n; i++)
		p = pfs_printf(buf, cap, p, "%s%02x", i && sep ? sep : "",
			       b[i]);
	return p;
}

/* xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx */
static long nvme_sysfs_uuid(char *buf, long cap, long p, const uint8_t *u)
{
	p = nvme_sysfs_hex(buf, cap, p, u, 4, NULL);
	p = pfs_printf(buf, cap, p, "-");
	p = nvme_sysfs_hex(buf, cap, p, u + 4, 2, NULL);
	p = pfs_printf(buf, cap, p, "-");
	p = nvme_sysfs_hex(buf, cap, p, u + 6, 2, NULL);
	p = pfs_printf(buf, cap, p, "-");
	p = nvme_sysfs_hex(buf, cap, p, u + 8, 2, NULL);
	p = pfs_printf(buf, cap, p, "-");
	return nvme_sysfs_hex(buf, cap, p, u + 10, 6, NULL);
}

static bool nvme_mem_zero(const uint8_t *b, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (b[i])
			return false;
	return true;
}

/* A fixed-width identity field as the established attributes print it:
 * the whole field, padding included. */
static long nvme_sysfs_field(char *buf, long cap, const char *s, size_t n)
{
	long p = 0;
	size_t i;

	for (i = 0; i < n && s[i]; i++, p++)
		if (p < cap)
			buf[p] = s[i];
	return pfs_printf(buf, cap, p, "\n");
}

/* Does `dir' have a static child `name'?  (A character device directory
 * made by sysfs_add_char_device, P6, carries dev/uevent/device/subsystem
 * statically; the dynamic listing must not repeat them.)  The static tree
 * only grows, like pfs's own lookups assume. */
static bool nvme_pfs_static_child(const struct pfs_node *dir,
				  const char *name)
{
	const struct pfs_node *c;

	for (c = dir->children; c; c = c->sibling)
		if (!kstrcmp(c->name, name))
			return true;
	return false;
}

/* A PCI function's address as a non-zero number (0: no function). */
static uintptr_t nvme_sysfs_pci_id(const pci_device_t *pdev)
{
	if (!pdev)
		return 0;
	return 1 + (((uintptr_t)pdev->segment << 16) |
		    ((uintptr_t)pdev->bus << 8) |
		    ((uintptr_t)pdev->device << 3) | pdev->function);
}

static bool nvme_sysfs_is_pcie(const struct nvme_ctrl *ctrl)
{
	return ctrl->ops && ctrl->ops->name &&
	       !kstrcmp(ctrl->ops->name, "pcie");
}

/* ==========================================================================
 * Attribute tables
 * ========================================================================== */

enum nvme_ctrl_attr_id {
	CA_RESET,
	CA_RESCAN,
	CA_MODEL,
	CA_SERIAL,
	CA_FIRMWARE,
	CA_CNTLID,
	CA_DELETE,
	CA_TRANSPORT,
	CA_SUBSYSNQN,
	CA_ADDRESS,
	CA_STATE,
	CA_NUMA_NODE,
	CA_QUEUE_COUNT,
	CA_SQSIZE,
	CA_KATO,
	CA_CNTRLTYPE,
	CA_DCTYPE,
	CA_QUIRKS,
	CA_PASSTHRU_LOG,
	CA_DEV,
	CA_UEVENT,
	CA_CMB,
	CA_CMBLOC,
	CA_CMBSZ,
	CA_HMB,
	CA_DEVICE_LINK,
	CA_SUBSYSTEM_LINK,
	CA_POWER_DIR,
	CA_HWMON_DIR,
	CA_FI_FILE,
	CA_NR
};

struct nvme_sysfs_attr {
	const char *name;
	uint8_t id;
	uint8_t type; /* PFS_FILE / PFS_DIR / PFS_LINK */
	uint16_t mode;
};

static const struct nvme_sysfs_attr nvme_ctrl_attrs[] = {
	{ "reset_controller", CA_RESET, PFS_FILE, 0200 },
	{ "rescan_controller", CA_RESCAN, PFS_FILE, 0200 },
	{ "model", CA_MODEL, PFS_FILE, 0444 },
	{ "serial", CA_SERIAL, PFS_FILE, 0444 },
	{ "firmware_rev", CA_FIRMWARE, PFS_FILE, 0444 },
	{ "cntlid", CA_CNTLID, PFS_FILE, 0444 },
	{ "delete_controller", CA_DELETE, PFS_FILE, 0200 },
	{ "transport", CA_TRANSPORT, PFS_FILE, 0444 },
	{ "subsysnqn", CA_SUBSYSNQN, PFS_FILE, 0444 },
	{ "address", CA_ADDRESS, PFS_FILE, 0444 },
	{ "state", CA_STATE, PFS_FILE, 0444 },
	{ "numa_node", CA_NUMA_NODE, PFS_FILE, 0444 },
	{ "queue_count", CA_QUEUE_COUNT, PFS_FILE, 0444 },
	{ "sqsize", CA_SQSIZE, PFS_FILE, 0444 },
	{ "kato", CA_KATO, PFS_FILE, 0444 },
	{ "cntrltype", CA_CNTRLTYPE, PFS_FILE, 0444 },
	{ "dctype", CA_DCTYPE, PFS_FILE, 0444 },
	{ "quirks", CA_QUIRKS, PFS_FILE, 0444 },
	{ "passthru_err_log_enabled", CA_PASSTHRU_LOG, PFS_FILE, 0644 },
	{ "dev", CA_DEV, PFS_FILE, 0444 },
	{ "uevent", CA_UEVENT, PFS_FILE, 0444 },
	{ "cmb", CA_CMB, PFS_FILE, 0444 },
	{ "cmbloc", CA_CMBLOC, PFS_FILE, 0444 },
	{ "cmbsz", CA_CMBSZ, PFS_FILE, 0444 },
	{ "hmb", CA_HMB, PFS_FILE, 0644 },
	{ "device", CA_DEVICE_LINK, PFS_LINK, 0777 },
	{ "subsystem", CA_SUBSYSTEM_LINK, PFS_LINK, 0777 },
	{ "power", CA_POWER_DIR, PFS_DIR, 0555 },
	{ "hwmon", CA_HWMON_DIR, PFS_DIR, 0555 }, /* named hwmon<instance> */
	{ "fault_inject", CA_FI_FILE, PFS_FILE, 0644 },
};
#define NVME_NR_CTRL_ATTRS \
	((unsigned int)(sizeof(nvme_ctrl_attrs) / sizeof(nvme_ctrl_attrs[0])))

/* Namespace (and multipath head) attributes. */
enum nvme_ns_attr_id {
	NA_WWID,
	NA_UUID,
	NA_NGUID,
	NA_EUI,
	NA_CSI,
	NA_NSID,
	NA_METADATA_BYTES,
	NA_NUSE,
	NA_ANA_GRPID,
	NA_ANA_STATE,
	NA_PASSTHRU_LOG,
	NA_QUEUE_DEPTH,
	NA_NUMA_NODES,
	NA_DELAYED_REMOVAL,
	NA_DEV_LINK,
	NA_SIZE_LINK,
	NA_QUEUE_LINK,
	NA_DEVICE_LINK,
	NA_FI_FILE,
	NA_NR
};
static const struct nvme_sysfs_attr nvme_ns_attrs[] = {
	{ "wwid", NA_WWID, PFS_FILE, 0444 },
	{ "uuid", NA_UUID, PFS_FILE, 0444 },
	{ "nguid", NA_NGUID, PFS_FILE, 0444 },
	{ "eui", NA_EUI, PFS_FILE, 0444 },
	{ "csi", NA_CSI, PFS_FILE, 0444 },
	{ "nsid", NA_NSID, PFS_FILE, 0444 },
	{ "metadata_bytes", NA_METADATA_BYTES, PFS_FILE, 0444 },
	{ "nuse", NA_NUSE, PFS_FILE, 0444 },
	{ "ana_grpid", NA_ANA_GRPID, PFS_FILE, 0444 },
	{ "ana_state", NA_ANA_STATE, PFS_FILE, 0444 },
	{ "passthru_err_log_enabled", NA_PASSTHRU_LOG, PFS_FILE, 0644 },
	{ "queue_depth", NA_QUEUE_DEPTH, PFS_FILE, 0444 },
	{ "numa_nodes", NA_NUMA_NODES, PFS_FILE, 0444 },
	{ "delayed_removal_secs", NA_DELAYED_REMOVAL, PFS_FILE, 0644 },
	{ "dev", NA_DEV_LINK, PFS_LINK, 0777 },
	{ "size", NA_SIZE_LINK, PFS_LINK, 0777 },
	{ "queue", NA_QUEUE_LINK, PFS_LINK, 0777 },
	{ "device", NA_DEVICE_LINK, PFS_LINK, 0777 },
	{ "fault_inject", NA_FI_FILE, PFS_FILE, 0644 },
};
#define NVME_NR_NS_ATTRS \
	((unsigned int)(sizeof(nvme_ns_attrs) / sizeof(nvme_ns_attrs[0])))

enum nvme_subsys_attr_id {
	SA_MODEL,
	SA_SERIAL,
	SA_FIRMWARE,
	SA_SUBSYSNQN,
	SA_SUBSYSTYPE,
	SA_IOPOLICY,
	SA_NR
};
static const struct nvme_sysfs_attr nvme_subsys_attrs[] = {
	{ "model", SA_MODEL, PFS_FILE, 0444 },
	{ "serial", SA_SERIAL, PFS_FILE, 0444 },
	{ "firmware_rev", SA_FIRMWARE, PFS_FILE, 0444 },
	{ "subsysnqn", SA_SUBSYSNQN, PFS_FILE, 0444 },
	{ "subsystype", SA_SUBSYSTYPE, PFS_FILE, 0444 },
	{ "iopolicy", SA_IOPOLICY, PFS_FILE, 0644 },
};
#define NVME_NR_SUBSYS_ATTRS \
	((unsigned int)(sizeof(nvme_subsys_attrs) / sizeof(nvme_subsys_attrs[0])))

static const char *const nvme_hwmon_attr_names[] = {
	[NVME_HWMON_TEMP_INPUT] = "input", [NVME_HWMON_TEMP_MAX] = "max",
	[NVME_HWMON_TEMP_MIN] = "min",	   [NVME_HWMON_TEMP_CRIT] = "crit",
	[NVME_HWMON_TEMP_ALARM] = "alarm", [NVME_HWMON_TEMP_LABEL] = "label",
};
#define NVME_NR_HWMON_ATTRS 6

/* ==========================================================================
 * Visibility
 * ========================================================================== */

static bool nvme_ctrl_attr_visible(struct nvme_ctrl *ctrl, unsigned int id)
{
	switch (id) {
	case CA_DELETE:
		return ctrl->ops->delete_ctrl != NULL;
	case CA_ADDRESS:
		return ctrl->ops->get_address != NULL;
	case CA_CMB:
	case CA_CMBLOC:
	case CA_CMBSZ:
		return nvme_sysfs_is_pcie(ctrl) && to_nvme_dev(ctrl)->cmbsz;
	case CA_HMB:
		return nvme_sysfs_is_pcie(ctrl) &&
		       (ctrl->hmpre || to_nvme_dev(ctrl)->hmb);
	case CA_DEVICE_LINK:
		return ctrl->pdev != NULL;
	case CA_POWER_DIR:
		return ctrl->apst_enabled;
	case CA_HWMON_DIR:
		return ctrl->hwmon != NULL && nvme_hwmon_id(ctrl) >= 0;
	default:
		return true;
	}
}

/* ==========================================================================
 * Namespace / head lookup
 * ========================================================================== */

/* The namespace `nsid' of `ctrl' (one sysfs shows when `visible'), with
 * a reference. */
static struct nvme_ns *nvme_sysfs_find_ns(struct nvme_ctrl *ctrl,
					  uint32_t nsid, bool visible)
{
	struct nvme_ns *ns, *found = NULL;

	mutex_lock(&ctrl->namespaces_lock);
	list_for_each_entry(ns, &ctrl->namespaces, list) {
		if (!ns->head || ns->head->ns_id != nsid || !ns->disk ||
		    (visible && !ns->sysfs_base[0]))
			continue;
		if (nvme_get_ns(ns))
			found = ns;
		break;
	}
	mutex_unlock(&ctrl->namespaces_lock);
	return found;
}

static struct nvme_ns *nvme_sysfs_get_ns(struct nvme_ctrl *ctrl,
					 uint32_t nsid)
{
	return nvme_sysfs_find_ns(ctrl, nsid, true);
}

/* Any live path of a multipath head, with a reference (its namespace
 * holds its controller). */
static struct nvme_ns *nvme_sysfs_head_path(struct nvme_ns_head *head)
{
	struct nvme_subsystem *subsys = head->subsys;
	struct nvme_ns *ns = NULL;
	struct nvme_ctrl *ctrl;

	if (!subsys)
		return NULL;
	mutex_lock(&nvme_subsystems_lock);
	list_for_each_entry(ctrl, &subsys->ctrls, subsys_entry) {
		if (nvme_ctrl_state(ctrl) != NVME_CTRL_LIVE)
			continue;
		ns = nvme_sysfs_find_ns(ctrl, head->ns_id, false);
		if (ns)
			break;
	}
	mutex_unlock(&nvme_subsystems_lock);
	return ns;
}

static struct nvme_ns_head *nvme_sysfs_get_head(struct nvme_subsystem *subsys,
						uint32_t nsid)
{
	struct nvme_ns_head *head, *found = NULL;

	mutex_lock(&subsys->lock);
	list_for_each_entry(head, &subsys->nsheads, entry) {
		if (head->ns_id != nsid || !head->disk)
			continue;
		if (nvme_tryget_ns_head(head))
			found = head;
		break;
	}
	mutex_unlock(&subsys->lock);
	return found;
}

/* ==========================================================================
 * Show / store
 * ========================================================================== */

/* Namespace / head attributes: `ns' may be NULL for a head. */
static long nvme_show_ns_attr(struct nvme_ns_head *head, struct nvme_ns *ns,
			      unsigned int id, char *buf, long cap)
{
	struct nvme_ns_ids *ids = &head->ids;
	struct nvme_subsystem *subsys = head->subsys;
	long p = 0;

	switch (id) {
	case NA_WWID: {
		int serial_len, model_len;

		if (!nvme_mem_zero(ids->uuid, sizeof(ids->uuid))) {
			p = pfs_printf(buf, cap, p, "uuid.");
			p = nvme_sysfs_uuid(buf, cap, p, ids->uuid);
			return pfs_printf(buf, cap, p, "\n");
		}
		if (!nvme_mem_zero(ids->nguid, sizeof(ids->nguid))) {
			p = pfs_printf(buf, cap, p, "eui.");
			p = nvme_sysfs_hex(buf, cap, p, ids->nguid, 16, NULL);
			return pfs_printf(buf, cap, p, "\n");
		}
		if (!nvme_mem_zero(ids->eui64, sizeof(ids->eui64))) {
			p = pfs_printf(buf, cap, p, "eui.");
			p = nvme_sysfs_hex(buf, cap, p, ids->eui64, 8, NULL);
			return pfs_printf(buf, cap, p, "\n");
		}
		if (!subsys)
			return -ENODEV;
		serial_len = nvme_strlen(subsys->serial,
					 (int)sizeof(subsys->serial));
		model_len =
			nvme_strlen(subsys->model, (int)sizeof(subsys->model));
		p = pfs_printf(buf, cap, p, "nvme.%04x-", subsys->vendor_id);
		p = nvme_sysfs_hex(buf, cap, p,
				   (const uint8_t *)subsys->serial, serial_len,
				   NULL);
		p = pfs_printf(buf, cap, p, "-");
		p = nvme_sysfs_hex(buf, cap, p, (const uint8_t *)subsys->model,
				   model_len, NULL);
		return pfs_printf(buf, cap, p, "-%08x\n", head->ns_id);
	}
	case NA_UUID:
		/* No UUID: the NGUID, as the old interface did. */
		if (nvme_mem_zero(ids->uuid, sizeof(ids->uuid)))
			p = nvme_sysfs_uuid(buf, cap, p, ids->nguid);
		else
			p = nvme_sysfs_uuid(buf, cap, p, ids->uuid);
		return pfs_printf(buf, cap, p, "\n");
	case NA_NGUID:
		p = nvme_sysfs_uuid(buf, cap, p, ids->nguid);
		return pfs_printf(buf, cap, p, "\n");
	case NA_EUI:
		p = nvme_sysfs_hex(buf, cap, p, ids->eui64, 8, " ");
		return pfs_printf(buf, cap, p, "\n");
	case NA_CSI:
		return pfs_printf(buf, cap, 0, "%u\n", ids->csi);
	case NA_NSID:
		return pfs_printf(buf, cap, 0, "%d\n", (int)head->ns_id);
	case NA_METADATA_BYTES:
		return pfs_printf(buf, cap, 0, "%u\n", head->ms);
	case NA_NUSE: {
		uint64_t now = hrtimer_now_ns();
		struct nvme_ns *path = ns;

		/* Refresh at most every 5 s: Identify is not free. */
		if (!head->nuse_last_ns ||
		    now - head->nuse_last_ns > 5000000000ULL) {
			struct nvme_id_ns *id = NULL;
			int ret = -EWOULDBLOCK;

			if (!path)
				path = nvme_sysfs_head_path(head);
			if (path)
				ret = nvme_identify_ns(path->ctrl,
						       head->ns_id, &id);
			if (path && path != ns)
				nvme_put_ns(path);
			if (ret)
				return ret > 0 ? -EIO : ret;
			head->nuse = le64_to_cpu(id->nuse);
			head->nuse_last_ns = now;
			kfree(id);
		}
		return pfs_printf(buf, cap, 0, "%llu\n",
				  (unsigned long long)head->nuse);
	}
	case NA_ANA_GRPID:
		if (!ns)
			return -ENOENT;
		return pfs_printf(buf, cap, 0, "%u\n",
				  __atomic_load_n(&ns->ana_grpid,
						  __ATOMIC_RELAXED));
	case NA_ANA_STATE:
		if (!ns)
			return -ENOENT;
		return pfs_printf(buf, cap, 0, "%s\n",
				  nvme_ana_state_name(nvme_ns_ana_state(ns)));
	case NA_PASSTHRU_LOG:
		return pfs_printf(buf, cap, 0, "%s\n",
				  head->passthru_err_log_enabled ? "on" :
								   "off");
	case NA_QUEUE_DEPTH:
		/* In-flight I/O of the path's controller (queue-depth
		 * policy only, empty otherwise). */
		if (!ns || !subsys || subsys->iopolicy != NVME_IOPOLICY_QD)
			return 0;
		return pfs_printf(buf, cap, 0, "%d\n",
				  __atomic_load_n(&ns->ctrl->nr_active,
						  __ATOMIC_RELAXED));
	case NA_NUMA_NODES: {
		/* The nodes this path is the current path of (one node in
		 * LikeOS: node 0 when any CPU uses it).  Pointer compare
		 * only, nothing dereferenced. */
		unsigned int cpu;

		if (!ns)
			return -ENOENT;
		for (cpu = 0; cpu < MAX_CPUS; cpu++)
			if (__atomic_load_n(&head->current_path[cpu],
					    __ATOMIC_RELAXED) == ns)
				return pfs_printf(buf, cap, 0, "0\n");
		return pfs_printf(buf, cap, 0, "\n");
	}
	case NA_DELAYED_REMOVAL:
		return pfs_printf(buf, cap, 0, "%u\n",
				  head->delayed_removal_secs);
	default:
		return -ENOENT;
	}
}

static long nvme_show_ctrl_attr(struct nvme_ctrl *ctrl, unsigned int id,
				char *buf, long cap)
{
	struct nvme_subsystem *subsys = ctrl->subsys;
	long p = 0;

	switch (id) {
	case CA_MODEL:
		return subsys ? nvme_sysfs_field(buf, cap, subsys->model,
						 sizeof(subsys->model)) :
				pfs_printf(buf, cap, 0, "\n");
	case CA_SERIAL:
		return subsys ? nvme_sysfs_field(buf, cap, subsys->serial,
						 sizeof(subsys->serial)) :
				pfs_printf(buf, cap, 0, "\n");
	case CA_FIRMWARE:
		return subsys ? nvme_sysfs_field(buf, cap,
						 subsys->firmware_rev,
						 sizeof(subsys->firmware_rev)) :
				pfs_printf(buf, cap, 0, "\n");
	case CA_CNTLID:
		return pfs_printf(buf, cap, 0, "%d\n", ctrl->cntlid);
	case CA_TRANSPORT:
		return pfs_printf(buf, cap, 0, "%s\n", ctrl->ops->name);
	case CA_SUBSYSNQN:
		return pfs_printf(buf, cap, 0, "%s\n",
				  subsys ? subsys->subnqn : "");
	case CA_ADDRESS: {
		char addr[64];
		int n = ctrl->ops->get_address(ctrl, addr, (int)sizeof(addr));

		if (n < 0)
			return n;
		if (n >= (int)sizeof(addr))
			n = (int)sizeof(addr) - 1;
		addr[n] = '\0';
		while (n > 0 && addr[n - 1] == '\n')
			addr[--n] = '\0';
		return pfs_printf(buf, cap, 0, "%s\n", addr);
	}
	case CA_STATE:
		return pfs_printf(buf, cap, 0, "%s\n",
				  nvme_ctrl_state_name(nvme_ctrl_state(ctrl)));
	case CA_NUMA_NODE:
		return pfs_printf(buf, cap, 0, "%d\n", (int)ctrl->numa_node);
	case CA_QUEUE_COUNT:
		return pfs_printf(buf, cap, 0, "%d\n", (int)ctrl->queue_count);
	case CA_SQSIZE:
		return pfs_printf(buf, cap, 0, "%d\n", (int)ctrl->sqsize);
	case CA_KATO:
		return pfs_printf(buf, cap, 0, "%d\n", (int)ctrl->kato_s);
	case CA_CNTRLTYPE:
		switch (ctrl->cntrltype) {
		case NVME_CTRL_TYPE_IO:
			return pfs_printf(buf, cap, 0, "io\n");
		case NVME_CTRL_TYPE_DISC:
			return pfs_printf(buf, cap, 0, "discovery\n");
		case NVME_CTRL_TYPE_ADMIN:
			return pfs_printf(buf, cap, 0, "admin\n");
		default:
			return pfs_printf(buf, cap, 0, "reserved\n");
		}
	case CA_DCTYPE:
		switch (ctrl->dctype) {
		case NVME_DCTYPE_NOT_REPORTED:
			return pfs_printf(buf, cap, 0, "none\n");
		case NVME_DCTYPE_DDC:
			return pfs_printf(buf, cap, 0, "ddc\n");
		case NVME_DCTYPE_CDC:
			return pfs_printf(buf, cap, 0, "cdc\n");
		default:
			return pfs_printf(buf, cap, 0, "reserved\n");
		}
	case CA_QUIRKS: {
		unsigned long q = ctrl->quirks;
		unsigned int bit;

		if (!q)
			return pfs_printf(buf, cap, 0, "none\n");
		for (bit = 0; q; bit++, q >>= 1)
			if (q & 1)
				p = pfs_printf(buf, cap, p, "%s\n",
					       nvme_quirk_name(
						       (enum nvme_quirks)(1UL
									  << bit)));
		return p;
	}
	case CA_PASSTHRU_LOG:
		return pfs_printf(buf, cap, 0, "%s\n",
				  ctrl->passthru_err_log_enabled ? "on" :
								   "off");
	case CA_DEV:
		return pfs_printf(buf, cap, 0, "%u:%u\n", nvme_char_major,
				  (unsigned int)ctrl->instance);
	case CA_UEVENT:
		p = pfs_printf(buf, cap, p, "MAJOR=%u\n", nvme_char_major);
		p = pfs_printf(buf, cap, p, "MINOR=%u\n",
			       (unsigned int)ctrl->instance);
		p = pfs_printf(buf, cap, p, "DEVNAME=%s\n", ctrl->name);
		return pfs_printf(buf, cap, p, "NVME_TRTYPE=%s\n",
				  ctrl->ops->name);
	case CA_CMB:
		return pfs_printf(buf, cap, 0, "cmbloc : 0x%08x\ncmbsz  : 0x%08x\n",
				  to_nvme_dev(ctrl)->cmbloc,
				  to_nvme_dev(ctrl)->cmbsz);
	case CA_CMBLOC:
		return pfs_printf(buf, cap, 0, "%u\n",
				  to_nvme_dev(ctrl)->cmbloc);
	case CA_CMBSZ:
		return pfs_printf(buf, cap, 0, "%u\n", to_nvme_dev(ctrl)->cmbsz);
	case CA_HMB:
		return pfs_printf(buf, cap, 0, "%d\n",
				  to_nvme_dev(ctrl)->hmb ? 1 : 0);
	default:
		return -ENOENT;
	}
}

static long nvme_store_ctrl_attr(struct nvme_ctrl *ctrl, unsigned int id,
				 const char *buf, long len)
{
	bool v;
	int ret;

	switch (id) {
	case CA_RESET:
		ret = nvme_reset_ctrl_sync(ctrl);
		return ret < 0 ? ret : len;
	case CA_RESCAN:
		nvme_queue_scan(ctrl);
		return len;
	case CA_DELETE:
		if (!ctrl->ops->delete_ctrl)
			return -EACCES;
		if (!nvme_test_bit(NVME_CTRL_STARTED_ONCE, &ctrl->flags))
			return -EBUSY;
		nvme_delete_ctrl_sync(ctrl);
		return len;
	case CA_PASSTHRU_LOG:
		if (nvme_sysfs_parse_bool(buf, len, &v))
			return -EINVAL;
		ctrl->passthru_err_log_enabled = v;
		return len;
	case CA_HMB: {
		struct nvme_dev *dev = to_nvme_dev(ctrl);

		if (!nvme_sysfs_is_pcie(ctrl))
			return -EACCES;
		if (nvme_sysfs_parse_bool(buf, len, &v))
			return -EINVAL;
		/* One enable/disable at a time, and none against a reset's,
		 * a suspend's or the removal's: a buffer the controller was
		 * just given must never be freed by a second writer.  Only a
		 * live controller takes the command. */
		mutex_lock(&dev->hmb_lock);
		if (v == dev->hmb) {
			ret = 0;
		} else if (nvme_ctrl_state(ctrl) != NVME_CTRL_LIVE) {
			ret = -EBUSY;
		} else if (v) {
			ret = nvme_setup_host_mem(dev);
		} else {
			ret = nvme_set_host_mem(dev, 0);
			if (!ret)
				nvme_free_host_mem(dev);
		}
		mutex_unlock(&dev->hmb_lock);
		return ret < 0 ? ret : len;
	}
	default:
		return -EACCES;
	}
}

static long nvme_show_power(struct nvme_ctrl *ctrl, char *buf, long cap)
{
	if (ctrl->ps_max_latency_us == ~0ULL)
		return pfs_printf(buf, cap, 0, "any\n");
	return pfs_printf(buf, cap, 0, "%llu\n",
			  (unsigned long long)ctrl->ps_max_latency_us);
}

static long nvme_store_power(struct nvme_ctrl *ctrl, const char *buf,
			     long len)
{
	uint64_t latency;
	int64_t v;
	long n = len;

	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
		n--;
	if ((n == 3 && !kstrncmp(buf, "any", 3)) ||
	    (n == 4 && !kstrncmp(buf, "auto", 4))) {
		latency = ~0ULL; /* no constraint */
	} else {
		if (nvme_sysfs_parse_s64(buf, len, &v) || v < 0 ||
		    v > 0x7fffffffLL)
			return -EINVAL;
		latency = (uint64_t)v;
	}
	if (ctrl->ps_max_latency_us != latency) {
		ctrl->ps_max_latency_us = latency;
		if (nvme_ctrl_state(ctrl) == NVME_CTRL_LIVE)
			nvme_configure_apst(ctrl);
	}
	return len;
}

static long nvme_show_hwmon(struct nvme_ctrl *ctrl, unsigned int attr,
			    char *buf, long cap)
{
	int channel;
	long val = 0;
	int ret;

	if (attr == 0xff)
		return pfs_printf(buf, cap, 0, "nvme\n");
	channel = (int)(attr >> 3);
	attr &= 7;
	if (attr == NVME_HWMON_TEMP_LABEL)
		return pfs_printf(buf, cap, 0, "%s\n",
				  nvme_hwmon_label(channel));
	ret = nvme_hwmon_read(ctrl, (enum nvme_hwmon_attr)attr, channel, &val);
	if (ret)
		return ret;
	return pfs_printf(buf, cap, 0, "%ld\n", val);
}

static long nvme_sysfs_show(struct pfs_node *n, char *buf, long cap)
{
	unsigned int kind = nk_kind(n), attr = nk_attr(n);
	struct nvme_ctrl *ctrl;
	struct nvme_subsystem *subsys;
	struct nvme_ns *ns;
	struct nvme_ns_head *head;
	char name[DISK_NAME_LEN];
	long ret = -ENODEV;

	switch (kind) {
	case NK_CTRL_ATTR:
	case NK_POWER_ATTR:
	case NK_HWMON_ATTR:
		ctrl = nvme_sysfs_get_ctrl(nk_instance(n));
		if (!ctrl)
			return -ENODEV;
		if (kind == NK_CTRL_ATTR)
			ret = nvme_show_ctrl_attr(ctrl, attr, buf, cap);
		else if (kind == NK_POWER_ATTR)
			ret = nvme_show_power(ctrl, buf, cap);
		else
			ret = nvme_show_hwmon(ctrl, attr, buf, cap);
		nvme_put_ctrl(ctrl);
		return ret;
	case NK_CFI_FILE:
		ctrl = nvme_sysfs_get_ctrl(nk_instance(n));
		if (!ctrl)
			return -ENODEV;
		ksnprintf(name, sizeof(name), "%s", ctrl->name);
		nvme_put_ctrl(ctrl);
		/* P6 finds the object by name again (no pointer kept). */
		return nvme_fault_inject_show_name(name, buf, cap);
	case NK_NS_ATTR:
	case NK_NSFI_FILE:
		ctrl = nvme_sysfs_get_ctrl(nk_instance(n));
		if (!ctrl)
			return -ENODEV;
		ns = nvme_sysfs_get_ns(ctrl, nk_nsid(n));
		name[0] = '\0';
		if (ns) {
			if (kind == NK_NS_ATTR)
				ret = nvme_show_ns_attr(ns->head, ns, attr,
							buf, cap);
			else
				ksnprintf(name, sizeof(name), "%s",
					  ns->disk->disk_name);
			nvme_put_ns(ns);
		}
		nvme_put_ctrl(ctrl);
		if (kind == NK_NSFI_FILE && name[0])
			ret = nvme_fault_inject_show_name(name, buf, cap);
		return ret;
	case NK_SUBSYS_ATTR:
		subsys = nvme_sysfs_get_subsys(nk_instance(n));
		if (!subsys)
			return -ENODEV;
		switch (attr) {
		case SA_MODEL:
			ret = nvme_sysfs_field(buf, cap, subsys->model,
					       sizeof(subsys->model));
			break;
		case SA_SERIAL:
			ret = nvme_sysfs_field(buf, cap, subsys->serial,
					       sizeof(subsys->serial));
			break;
		case SA_FIRMWARE:
			ret = nvme_sysfs_field(buf, cap, subsys->firmware_rev,
					       sizeof(subsys->firmware_rev));
			break;
		case SA_SUBSYSNQN:
			ret = pfs_printf(buf, cap, 0, "%s\n", subsys->subnqn);
			break;
		case SA_SUBSYSTYPE:
			ret = pfs_printf(buf, cap, 0, "%s\n",
					 subsys->subtype == NVME_NQN_DISC ?
						 "discovery" :
					 subsys->subtype == NVME_NQN_NVME ?
						 "nvm" :
						 "reserved");
			break;
		case SA_IOPOLICY:
			ret = pfs_printf(buf, cap, 0, "%s\n",
					 nvme_iopolicy_name(subsys->iopolicy));
			break;
		default:
			ret = -ENOENT;
			break;
		}
		nvme_put_subsystem(subsys);
		return ret;
	case NK_HEAD_ATTR:
		subsys = nvme_sysfs_get_subsys(nk_instance(n));
		if (!subsys)
			return -ENODEV;
		head = nvme_sysfs_get_head(subsys, nk_nsid(n));
		if (head) {
			ret = nvme_show_ns_attr(head, NULL, attr, buf, cap);
			nvme_put_ns_head(head);
		}
		nvme_put_subsystem(subsys);
		return ret;
	default:
		return -ENOENT;
	}
}

static long nvme_sysfs_store(struct pfs_node *n, const char *buf, long len)
{
	unsigned int kind = nk_kind(n), attr = nk_attr(n);
	struct nvme_ctrl *ctrl;
	struct nvme_subsystem *subsys;
	struct nvme_ns *ns;
	struct nvme_ns_head *head;
	char name[DISK_NAME_LEN];
	long ret = -ENODEV;
	bool v;

	switch (kind) {
	case NK_CTRL_ATTR:
	case NK_POWER_ATTR:
	case NK_HWMON_ATTR:
		ctrl = nvme_sysfs_get_ctrl(nk_instance(n));
		if (!ctrl)
			return -ENODEV;
		if (kind == NK_CTRL_ATTR) {
			ret = nvme_store_ctrl_attr(ctrl, attr, buf, len);
		} else if (kind == NK_POWER_ATTR) {
			ret = nvme_store_power(ctrl, buf, len);
		} else {
			int64_t mc;

			if (attr == 0xff ||
			    nvme_sysfs_parse_s64(buf, len, &mc)) {
				ret = -EINVAL;
			} else {
				ret = nvme_hwmon_write(
					ctrl,
					(enum nvme_hwmon_attr)(attr & 7),
					(int)(attr >> 3), (long)mc);
				if (!ret)
					ret = len;
			}
		}
		nvme_put_ctrl(ctrl);
		return ret;
	case NK_CFI_FILE:
		ctrl = nvme_sysfs_get_ctrl(nk_instance(n));
		if (!ctrl)
			return -ENODEV;
		ksnprintf(name, sizeof(name), "%s", ctrl->name);
		nvme_put_ctrl(ctrl);
		return nvme_fault_inject_store_name(name, buf, len);
	case NK_NS_ATTR:
	case NK_NSFI_FILE:
		ctrl = nvme_sysfs_get_ctrl(nk_instance(n));
		if (!ctrl)
			return -ENODEV;
		ns = nvme_sysfs_get_ns(ctrl, nk_nsid(n));
		name[0] = '\0';
		if (ns) {
			if (kind == NK_NSFI_FILE) {
				ksnprintf(name, sizeof(name), "%s",
					  ns->disk->disk_name);
			} else if (attr == NA_PASSTHRU_LOG) {
				if (nvme_sysfs_parse_bool(buf, len, &v)) {
					ret = -EINVAL;
				} else {
					ns->head->passthru_err_log_enabled = v;
					ret = len;
				}
			} else {
				ret = -EACCES;
			}
			nvme_put_ns(ns);
		}
		nvme_put_ctrl(ctrl);
		if (kind == NK_NSFI_FILE && name[0])
			ret = nvme_fault_inject_store_name(name, buf, len);
		return ret;
	case NK_SUBSYS_ATTR: {
		char pol[32];
		long m = len;
		int policy;

		if (attr != SA_IOPOLICY)
			return -EACCES;
		while (m > 0 && (buf[m - 1] == '\n' || buf[m - 1] == ' '))
			m--;
		if (m <= 0 || m >= (long)sizeof(pol))
			return -EINVAL;
		kmemcpy(pol, buf, (size_t)m);
		pol[m] = '\0';
		policy = nvme_iopolicy_from_name(pol);
		if (policy < 0)
			return -EINVAL;
		subsys = nvme_sysfs_get_subsys(nk_instance(n));
		if (!subsys)
			return -ENODEV;
		/* Sets the policy and makes every path choice anew. */
		nvme_subsys_iopolicy_update(subsys, policy);
		nvme_put_subsystem(subsys);
		return len;
	}
	case NK_HEAD_ATTR:
		if (attr != NA_PASSTHRU_LOG && attr != NA_DELAYED_REMOVAL)
			return -EACCES;
		subsys = nvme_sysfs_get_subsys(nk_instance(n));
		if (!subsys)
			return -ENODEV;
		head = nvme_sysfs_get_head(subsys, nk_nsid(n));
		if (head && attr == NA_DELAYED_REMOVAL) {
			int64_t secs;

			if (nvme_sysfs_parse_s64(buf, len, &secs) || secs < 0 ||
			    secs > 0xffffffffLL) {
				ret = -EINVAL;
			} else {
				nvme_mpath_set_delayed_removal(
					head, (unsigned int)secs);
				ret = len;
			}
			nvme_put_ns_head(head);
		} else if (head) {
			if (nvme_sysfs_parse_bool(buf, len, &v)) {
				ret = -EINVAL;
			} else {
				head->passthru_err_log_enabled = v;
				ret = len;
			}
			nvme_put_ns_head(head);
		}
		nvme_put_subsystem(subsys);
		return ret;
	default:
		return -EACCES;
	}
}

/* ==========================================================================
 * Dynamic directories
 * ========================================================================== */

static int nvme_dir_list(struct pfs_node *dir, unsigned *cursor, char *name,
			 long cap, int *type);
static struct pfs_node *nvme_dir_lookup(struct pfs_node *dir,
					const char *name);

static struct pfs_node *nvme_new_file(struct pfs_node *dir, const char *name,
				      uint64_t key, unsigned int mode)
{
	struct pfs_node *n = pfs_node_new(dir->fs, dir, name, PFS_FILE);

	if (!n)
		return NULL;
	n->arg2 = key;
	n->mode = mode;
	/* A write-only attribute (reset_controller, rescan_controller,
	 * delete_controller) has no contents: pseudofs renders a file at
	 * every open, also an open for writing, and a show that has nothing
	 * to say for these would fail that open. */
	if (mode & 0444)
		n->show = nvme_sysfs_show;
	if (mode & 0222)
		n->store = nvme_sysfs_store;
	return n;
}

static struct pfs_node *nvme_new_dir(struct pfs_node *dir, const char *name,
				     uint64_t key)
{
	struct pfs_node *n = pfs_node_new(dir->fs, dir, name, PFS_DIR);

	if (!n)
		return NULL;
	/* The parent is a transient lookup node freed as soon as the walk
	 * moves on: a listing must not reach back into it. */
	n->parent = NULL;
	n->arg2 = key;
	pfs_set_dynamic(n, nvme_dir_list, nvme_dir_lookup);
	return n;
}

static struct pfs_node *nvme_new_link(struct pfs_node *dir, const char *name,
				      const char *target)
{
	struct pfs_node *n = pfs_node_new(dir->fs, dir, name, PFS_LINK);

	if (n)
		ksnprintf(n->link, sizeof(n->link), "%s", target);
	return n;
}

static void nvme_pci_sysfs_path(const struct nvme_ctrl *ctrl, char *out,
				size_t cap)
{
	sysfs_pci_path(ctrl->pdev, out, cap);
}

/* ---- controller directory ------------------------------------------------- */

#define NVME_CTRL_LIST_NS_BASE 256u
#define NVME_HWMON_LIST_NAME 0u
#define NVME_HEAD_LIST_BASE 4096u
#define NVME_SUBSYS_LIST_CTRL_BASE 64u

static int nvme_ctrl_dir_list(struct pfs_node *dir, struct nvme_ctrl *ctrl,
			      unsigned *cursor, char *name, long cap,
			      int *type)
{
	unsigned int c = *cursor;
	struct nvme_ns *ns;
	int found = 0;

	for (; c < NVME_NR_CTRL_ATTRS; c++) {
		const struct nvme_sysfs_attr *a = &nvme_ctrl_attrs[c];

		if (!nvme_ctrl_attr_visible(ctrl, a->id))
			continue;
		if (a->id == CA_HWMON_DIR)
			ksnprintf(name, (size_t)cap, "hwmon%d",
				  nvme_hwmon_id(ctrl));
		else
			ksnprintf(name, (size_t)cap, "%s", a->name);
		if (nvme_pfs_static_child(dir, name))
			continue;
		*type = a->type == PFS_DIR ? DT_DIR :
			a->type == PFS_LINK ? DT_LNK :
					      DT_REG;
		*cursor = c + 1;
		return 1;
	}
	/* Namespaces, keyed by nsid. */
	if (c < NVME_CTRL_LIST_NS_BASE)
		c = NVME_CTRL_LIST_NS_BASE;
	mutex_lock(&ctrl->namespaces_lock);
	{
		uint32_t best = 0xffffffffu;
		const char *best_name = NULL;

		list_for_each_entry(ns, &ctrl->namespaces, list) {
			uint32_t nsid;

			if (!ns->head || !ns->disk || !ns->sysfs_base[0])
				continue;
			nsid = ns->head->ns_id;
			if (nsid > 0xffffffffu - NVME_CTRL_LIST_NS_BASE - 1)
				continue;
			if (nsid + NVME_CTRL_LIST_NS_BASE < c || nsid >= best)
				continue;
			best = nsid;
			best_name = ns->disk->disk_name;
		}
		if (best_name) {
			ksnprintf(name, (size_t)cap, "%s", best_name);
			*type = DT_DIR;
			*cursor = best + NVME_CTRL_LIST_NS_BASE + 1;
			found = 1;
		}
	}
	mutex_unlock(&ctrl->namespaces_lock);
	return found;
}

static struct pfs_node *nvme_ctrl_dir_lookup(struct pfs_node *dir,
					     struct nvme_ctrl *ctrl,
					     const char *name)
{
	unsigned int inst = (unsigned int)ctrl->instance;
	char tmp[96];
	struct nvme_ns *ns;
	uint32_t nsid = 0;
	unsigned int i;
	int hit = 0;

	for (i = 0; i < NVME_NR_CTRL_ATTRS; i++) {
		const struct nvme_sysfs_attr *a = &nvme_ctrl_attrs[i];

		if (a->id == CA_HWMON_DIR) {
			ksnprintf(tmp, sizeof(tmp), "hwmon%d",
				  nvme_hwmon_id(ctrl));
			if (kstrcmp(tmp, name))
				continue;
		} else if (kstrcmp(a->name, name)) {
			continue;
		}
		if (!nvme_ctrl_attr_visible(ctrl, a->id))
			return NULL;
		switch (a->id) {
		case CA_DEVICE_LINK:
			nvme_pci_sysfs_path(ctrl, tmp, sizeof(tmp));
			return nvme_new_link(dir, name, tmp);
		case CA_SUBSYSTEM_LINK:
			return nvme_new_link(dir, name, "/sys/class/nvme");
		case CA_POWER_DIR:
			return nvme_new_dir(dir, name,
					    nvme_sysfs_key(inst, 0,
							   NK_POWER_DIR, 0));
		case CA_HWMON_DIR:
			return nvme_new_dir(dir, name,
					    nvme_sysfs_key(inst, 0,
							   NK_HWMON_DIR, 0));
		case CA_FI_FILE:
			return nvme_new_file(dir, name,
					     nvme_sysfs_key(inst, 0, NK_CFI_FILE,
							    0),
					     a->mode);
		default:
			return nvme_new_file(dir, name,
					     nvme_sysfs_key(inst, 0,
							    NK_CTRL_ATTR,
							    a->id),
					     a->mode);
		}
	}
	/* A namespace directory? */
	mutex_lock(&ctrl->namespaces_lock);
	list_for_each_entry(ns, &ctrl->namespaces, list) {
		if (!ns->head || !ns->disk || !ns->sysfs_base[0])
			continue;
		if (!kstrcmp(ns->disk->disk_name, name)) {
			nsid = ns->head->ns_id;
			hit = 1;
			break;
		}
	}
	mutex_unlock(&ctrl->namespaces_lock);
	if (!hit)
		return NULL;
	return nvme_new_dir(dir, name, nvme_sysfs_key(inst, nsid, NK_NS_DIR, 0));
}

/* ---- namespace / head directories ----------------------------------------- */

static bool nvme_ns_attr_visible(struct nvme_ctrl *ctrl, bool is_head,
				 bool multipath, const struct nvme_ns_ids *ids,
				 unsigned int id)
{
	switch (id) {
	case NA_UUID:
		return !nvme_mem_zero(ids->uuid, sizeof(ids->uuid)) ||
		       !nvme_mem_zero(ids->nguid, sizeof(ids->nguid));
	case NA_NGUID:
		return !nvme_mem_zero(ids->nguid, sizeof(ids->nguid));
	case NA_EUI:
		return !nvme_mem_zero(ids->eui64, sizeof(ids->eui64));
	case NA_ANA_GRPID:
	case NA_ANA_STATE:
		return !is_head && ctrl && nvme_ctrl_use_ana(ctrl);
	case NA_FI_FILE:
	case NA_DEVICE_LINK:
		return !is_head;
	case NA_QUEUE_DEPTH:
	case NA_NUMA_NODES:
		/* paths of a multipath head only */
		return !is_head && multipath;
	case NA_DELAYED_REMOVAL:
		return is_head;
	default:
		return true;
	}
}

/* The ids of the namespace / head a directory node names (copied). */
static int nvme_dir_ids(struct pfs_node *dir, struct nvme_ctrl **ctrlp,
			struct nvme_ns_ids *ids, bool *multipath, char *disk,
			size_t cap)
{
	unsigned int kind = nk_kind(dir);
	struct nvme_ctrl *ctrl;
	struct nvme_ns *ns;
	struct nvme_subsystem *subsys;
	struct nvme_ns_head *head;

	*ctrlp = NULL;
	if (kind == NK_NS_DIR) {
		ctrl = nvme_sysfs_get_ctrl(nk_instance(dir));
		if (!ctrl)
			return -ENODEV;
		ns = nvme_sysfs_get_ns(ctrl, nk_nsid(dir));
		if (!ns) {
			nvme_put_ctrl(ctrl);
			return -ENODEV;
		}
		*ids = ns->head->ids;
		*multipath = nvme_ns_head_multipath(ns->head);
		ksnprintf(disk, cap, "%s", ns->disk->disk_name);
		nvme_put_ns(ns);
		*ctrlp = ctrl; /* the caller puts it */
		return 0;
	}
	subsys = nvme_sysfs_get_subsys(nk_instance(dir));
	if (!subsys)
		return -ENODEV;
	head = nvme_sysfs_get_head(subsys, nk_nsid(dir));
	if (!head) {
		nvme_put_subsystem(subsys);
		return -ENODEV;
	}
	*ids = head->ids;
	*multipath = true;
	ksnprintf(disk, cap, "%s", head->disk->disk_name);
	nvme_put_ns_head(head);
	nvme_put_subsystem(subsys);
	return 0;
}

static int nvme_ns_dir_list(struct pfs_node *dir, unsigned *cursor,
			    char *name, long cap, int *type)
{
	bool is_head = nk_kind(dir) == NK_HEAD_DIR;
	struct nvme_ctrl *ctrl;
	struct nvme_ns_ids ids;
	bool multipath = false;
	char disk[DISK_NAME_LEN];
	unsigned int c;
	int found = 0;

	if (nvme_dir_ids(dir, &ctrl, &ids, &multipath, disk, sizeof(disk)))
		return 0;
	for (c = *cursor; c < NVME_NR_NS_ATTRS; c++) {
		const struct nvme_sysfs_attr *a = &nvme_ns_attrs[c];

		if (!nvme_ns_attr_visible(ctrl, is_head, multipath, &ids, a->id))
			continue;
		ksnprintf(name, (size_t)cap, "%s", a->name);
		*type = a->type == PFS_DIR ? DT_DIR :
			a->type == PFS_LINK ? DT_LNK :
					      DT_REG;
		*cursor = c + 1;
		found = 1;
		break;
	}
	/* A multipath head: multipath/ lists its paths. */
	if (!found && is_head && c == NVME_NR_NS_ATTRS) {
		ksnprintf(name, (size_t)cap, "multipath");
		*type = DT_DIR;
		*cursor = c + 1;
		found = 1;
	}
	if (ctrl)
		nvme_put_ctrl(ctrl);
	return found;
}

static struct pfs_node *nvme_ns_dir_lookup(struct pfs_node *dir,
					   const char *name)
{
	bool is_head = nk_kind(dir) == NK_HEAD_DIR;
	unsigned int inst = nk_instance(dir);
	uint32_t nsid = nk_nsid(dir);
	struct pfs_node *r = NULL;
	struct nvme_ctrl *ctrl;
	struct nvme_ns_ids ids;
	bool multipath = false;
	char disk[DISK_NAME_LEN], target[128];
	unsigned int i;

	if (nvme_dir_ids(dir, &ctrl, &ids, &multipath, disk, sizeof(disk)))
		return NULL;
	if (is_head && !kstrcmp(name, "multipath")) {
		if (ctrl)
			nvme_put_ctrl(ctrl);
		return nvme_new_dir(dir, name,
				    nvme_sysfs_key(inst, nsid, NK_MPATH_DIR, 0));
	}
	for (i = 0; i < NVME_NR_NS_ATTRS; i++) {
		const struct nvme_sysfs_attr *a = &nvme_ns_attrs[i];

		if (kstrcmp(a->name, name))
			continue;
		if (!nvme_ns_attr_visible(ctrl, is_head, multipath, &ids, a->id))
			break;
		switch (a->id) {
		case NA_DEV_LINK:
		case NA_SIZE_LINK:
		case NA_QUEUE_LINK:
			ksnprintf(target, sizeof(target), "/sys/block/%s/%s",
				  disk, name);
			r = nvme_new_link(dir, name, target);
			break;
		case NA_DEVICE_LINK:
			ksnprintf(target, sizeof(target), "/sys/class/nvme/nvme%u",
				  inst);
			r = nvme_new_link(dir, name, target);
			break;
		case NA_FI_FILE:
			r = nvme_new_file(dir, name,
					  nvme_sysfs_key(inst, nsid, NK_NSFI_FILE,
							 0),
					  a->mode);
			break;
		default:
			r = nvme_new_file(dir, name,
					  nvme_sysfs_key(inst, nsid,
							 is_head ?
								 NK_HEAD_ATTR :
								 NK_NS_ATTR,
							 a->id),
					  a->mode);
			break;
		}
		break;
	}
	if (ctrl)
		nvme_put_ctrl(ctrl);
	return r;
}

/* ---- <head>/multipath ------------------------------------------------------ */

/* Path `idx' (in head->list order) that has its multipath link
 * (NVME_NS_SYSFS_ATTR_LINK), or the one named `want': its disk name and
 * the path's directory under its controller.  0 / -ENOENT. */
static int nvme_mpath_link_find(struct pfs_node *dir, unsigned int idx,
				const char *want, char *name, size_t ncap,
				char *target, size_t tcap)
{
	struct nvme_subsystem *subsys;
	struct nvme_ns_head *head;
	struct nvme_ns *ns;
	unsigned int i = 0;
	int ret = -ENOENT, srcu_idx;

	subsys = nvme_sysfs_get_subsys(nk_instance(dir));
	if (!subsys)
		return -ENOENT;
	head = nvme_sysfs_get_head(subsys, nk_nsid(dir));
	if (!head) {
		nvme_put_subsystem(subsys);
		return -ENOENT;
	}
	srcu_idx = nvme_srcu_read_lock(&head->srcu);
	nvme_list_for_each_entry_srcu(ns, &head->list, siblings) {
		if (!ns->disk || !ns->ctrl ||
		    !nvme_test_bit(NVME_NS_SYSFS_ATTR_LINK, &ns->flags))
			continue;
		if (want ? !kstrcmp(ns->disk->disk_name, want) : i == idx) {
			ksnprintf(name, ncap, "%s", ns->disk->disk_name);
			ksnprintf(target, tcap, "/sys/class/nvme/%s/%s",
				  ns->ctrl->name, ns->disk->disk_name);
			ret = 0;
			break;
		}
		i++;
	}
	nvme_srcu_read_unlock(&head->srcu, srcu_idx);
	nvme_put_ns_head(head);
	nvme_put_subsystem(subsys);
	return ret;
}

static int nvme_mpath_dir_list(struct pfs_node *dir, unsigned *cursor,
			       char *name, long cap, int *type)
{
	char target[128];

	if (nvme_mpath_link_find(dir, *cursor, NULL, name, (size_t)cap,
				 target, sizeof(target)))
		return 0;
	*type = DT_LNK;
	*cursor += 1;
	return 1;
}

static struct pfs_node *nvme_mpath_dir_lookup(struct pfs_node *dir,
					      const char *name)
{
	char found[DISK_NAME_LEN], target[128];

	if (nvme_mpath_link_find(dir, 0, name, found, sizeof(found), target,
				 sizeof(target)))
		return NULL;
	return nvme_new_link(dir, name, target);
}

/* ---- subsystem directory --------------------------------------------------- */

static int nvme_subsys_dir_list(struct nvme_subsystem *subsys,
				unsigned *cursor, char *name, long cap,
				int *type)
{
	unsigned int c = *cursor;
	struct nvme_ctrl *ctrl;
	struct nvme_ns_head *head;
	int found = 0;

	if (c < NVME_NR_SUBSYS_ATTRS) {
		ksnprintf(name, (size_t)cap, "%s", nvme_subsys_attrs[c].name);
		*type = DT_REG;
		*cursor = c + 1;
		return 1;
	}
	/* Links to the controllers, keyed by instance. */
	if (c < NVME_SUBSYS_LIST_CTRL_BASE)
		c = NVME_SUBSYS_LIST_CTRL_BASE;
	if (c < NVME_SUBSYS_LIST_CTRL_BASE + NVME_MAX_INSTANCES) {
		int best = -1;

		mutex_lock(&nvme_subsystems_lock);
		list_for_each_entry(ctrl, &subsys->ctrls, subsys_entry) {
			unsigned int key = (unsigned int)ctrl->instance +
					   NVME_SUBSYS_LIST_CTRL_BASE;

			if (key < c || (best >= 0 && ctrl->instance >= best))
				continue;
			best = ctrl->instance;
		}
		mutex_unlock(&nvme_subsystems_lock);
		if (best >= 0) {
			ksnprintf(name, (size_t)cap, "nvme%d", best);
			*type = DT_LNK;
			*cursor = (unsigned int)best +
				  NVME_SUBSYS_LIST_CTRL_BASE + 1;
			return 1;
		}
		c = NVME_HEAD_LIST_BASE;
	}
	/* Multipath head disks, keyed by nsid. */
	if (c < NVME_HEAD_LIST_BASE)
		c = NVME_HEAD_LIST_BASE;
	mutex_lock(&subsys->lock);
	{
		uint32_t best = 0xffffffffu;
		const char *best_name = NULL;

		list_for_each_entry(head, &subsys->nsheads, entry) {
			if (!head->disk ||
			    head->ns_id > 0xffffffffu - NVME_HEAD_LIST_BASE - 1)
				continue;
			if (head->ns_id + NVME_HEAD_LIST_BASE < c ||
			    head->ns_id >= best)
				continue;
			best = head->ns_id;
			best_name = head->disk->disk_name;
		}
		if (best_name) {
			ksnprintf(name, (size_t)cap, "%s", best_name);
			*type = DT_DIR;
			*cursor = best + NVME_HEAD_LIST_BASE + 1;
			found = 1;
		}
	}
	mutex_unlock(&subsys->lock);
	return found;
}

static struct pfs_node *nvme_subsys_dir_lookup(struct pfs_node *dir,
					       struct nvme_subsystem *subsys,
					       const char *name)
{
	unsigned int inst = (unsigned int)subsys->instance;
	struct nvme_ctrl *ctrl;
	struct nvme_ns_head *head;
	char tmp[64];
	uint32_t nsid = 0;
	unsigned int i;
	int hit = 0;

	for (i = 0; i < NVME_NR_SUBSYS_ATTRS; i++)
		if (!kstrcmp(nvme_subsys_attrs[i].name, name))
			return nvme_new_file(dir, name,
					     nvme_sysfs_key(inst, 0,
							    NK_SUBSYS_ATTR,
							    nvme_subsys_attrs[i]
								    .id),
					     nvme_subsys_attrs[i].mode);

	mutex_lock(&nvme_subsystems_lock);
	list_for_each_entry(ctrl, &subsys->ctrls, subsys_entry) {
		if (!kstrcmp(ctrl->name, name)) {
			hit = 1;
			break;
		}
	}
	mutex_unlock(&nvme_subsystems_lock);
	if (hit) {
		ksnprintf(tmp, sizeof(tmp), "/sys/class/nvme/%s", name);
		return nvme_new_link(dir, name, tmp);
	}

	mutex_lock(&subsys->lock);
	list_for_each_entry(head, &subsys->nsheads, entry) {
		if (head->disk && !kstrcmp(head->disk->disk_name, name)) {
			nsid = head->ns_id;
			hit = 1;
			break;
		}
	}
	mutex_unlock(&subsys->lock);
	if (!hit)
		return NULL;
	return nvme_new_dir(dir, name,
			    nvme_sysfs_key(inst, nsid, NK_HEAD_DIR, 0));
}

/* ---- hwmon directory ---------------------------------------------------------- */

static int nvme_hwmon_dir_list(struct nvme_ctrl *ctrl, unsigned *cursor,
			       char *name, long cap, int *type)
{
	unsigned int c = *cursor;

	*type = DT_REG;
	if (c == 0) {
		ksnprintf(name, (size_t)cap, "name");
		*cursor = 1;
		return 1;
	}
	for (; c < 1 + NVME_HWMON_CHANNELS * 8; c++) {
		unsigned int channel = (c - 1) / 8, attr = (c - 1) % 8;

		if (attr >= NVME_NR_HWMON_ATTRS)
			continue;
		if (!nvme_hwmon_is_visible(ctrl, (enum nvme_hwmon_attr)attr,
					   (int)channel))
			continue;
		ksnprintf(name, (size_t)cap, "temp%u_%s", channel + 1,
			  nvme_hwmon_attr_names[attr]);
		*cursor = c + 1;
		return 1;
	}
	/* The class device's links: its parent (the controller) and its
	 * class, which is how sensor tools tell what the monitor is. */
	if (c == 1 + NVME_HWMON_CHANNELS * 8) {
		ksnprintf(name, (size_t)cap, "device");
		*type = DT_LNK;
		*cursor = c + 1;
		return 1;
	}
	if (c == 2 + NVME_HWMON_CHANNELS * 8) {
		ksnprintf(name, (size_t)cap, "subsystem");
		*type = DT_LNK;
		*cursor = c + 1;
		return 1;
	}
	return 0;
}

static struct pfs_node *nvme_hwmon_dir_lookup(struct pfs_node *dir,
					      struct nvme_ctrl *ctrl,
					      const char *name)
{
	unsigned int inst = (unsigned int)ctrl->instance;
	unsigned int channel, attr, mode;
	char tmp[32];

	if (!kstrcmp(name, "name"))
		return nvme_new_file(dir, name,
				     nvme_sysfs_key(inst, 0, NK_HWMON_ATTR,
						    0xff),
				     0444);
	if (!kstrcmp(name, "device")) {
		ksnprintf(tmp, sizeof(tmp), "/sys/class/nvme/nvme%u", inst);
		return nvme_new_link(dir, name, tmp);
	}
	if (!kstrcmp(name, "subsystem"))
		return nvme_new_link(dir, name, "/sys/class/hwmon");
	for (channel = 0; channel < NVME_HWMON_CHANNELS; channel++) {
		for (attr = 0; attr < NVME_NR_HWMON_ATTRS; attr++) {
			ksnprintf(tmp, sizeof(tmp), "temp%u_%s", channel + 1,
				  nvme_hwmon_attr_names[attr]);
			if (kstrcmp(tmp, name))
				continue;
			mode = nvme_hwmon_is_visible(
				ctrl, (enum nvme_hwmon_attr)attr, (int)channel);
			if (!mode)
				return NULL;
			return nvme_new_file(dir, name,
					     nvme_sysfs_key(inst, 0,
							    NK_HWMON_ATTR,
							    channel * 8 + attr),
					     mode);
		}
	}
	return NULL;
}

/* ---- dispatch ------------------------------------------------------------- */

/* A controller directory adopted under a PCI function (static, `arg' is
 * that function) answers only for a controller on that function. */
static bool nvme_sysfs_dir_owner_ok(const struct pfs_node *dir,
				    unsigned int kind,
				    const struct nvme_ctrl *ctrl)
{
	if (kind != NK_CTRL_DIR || dir->transient || !dir->arg)
		return true;
	return (uintptr_t)dir->arg == nvme_sysfs_pci_id(ctrl->pdev);
}

static int nvme_dir_list(struct pfs_node *dir, unsigned *cursor, char *name,
			 long cap, int *type)
{
	unsigned int kind = nk_kind(dir);
	struct nvme_ctrl *ctrl;
	struct nvme_subsystem *subsys;
	int ret = 0;

	switch (kind) {
	case NK_CTRL_DIR:
	case NK_HWMON_DIR:
		ctrl = nvme_sysfs_get_ctrl(nk_instance(dir));
		if (!ctrl)
			return 0;
		if (!nvme_sysfs_dir_owner_ok(dir, kind, ctrl)) {
			nvme_put_ctrl(ctrl);
			return 0;
		}
		if (kind == NK_CTRL_DIR)
			ret = nvme_ctrl_dir_list(dir, ctrl, cursor, name, cap,
						 type);
		else
			ret = nvme_hwmon_dir_list(ctrl, cursor, name, cap,
						  type);
		nvme_put_ctrl(ctrl);
		return ret;
	case NK_POWER_DIR:
		if (*cursor)
			return 0;
		ksnprintf(name, (size_t)cap, "pm_qos_latency_tolerance_us");
		*type = DT_REG;
		*cursor = 1;
		return 1;
	case NK_NS_DIR:
	case NK_HEAD_DIR:
		return nvme_ns_dir_list(dir, cursor, name, cap, type);
	case NK_MPATH_DIR:
		return nvme_mpath_dir_list(dir, cursor, name, cap, type);
	case NK_SUBSYS_DIR:
		subsys = nvme_sysfs_get_subsys(nk_instance(dir));
		if (!subsys)
			return 0;
		ret = nvme_subsys_dir_list(subsys, cursor, name, cap, type);
		nvme_put_subsystem(subsys);
		return ret;
	default:
		return 0;
	}
}

static struct pfs_node *nvme_dir_lookup(struct pfs_node *dir,
					const char *name)
{
	unsigned int kind = nk_kind(dir);
	struct nvme_ctrl *ctrl;
	struct nvme_subsystem *subsys;
	struct pfs_node *r = NULL;

	switch (kind) {
	case NK_CTRL_DIR:
	case NK_HWMON_DIR:
		ctrl = nvme_sysfs_get_ctrl(nk_instance(dir));
		if (!ctrl)
			return NULL;
		if (!nvme_sysfs_dir_owner_ok(dir, kind, ctrl)) {
			nvme_put_ctrl(ctrl);
			return NULL;
		}
		if (kind == NK_CTRL_DIR)
			r = nvme_ctrl_dir_lookup(dir, ctrl, name);
		else
			r = nvme_hwmon_dir_lookup(dir, ctrl, name);
		nvme_put_ctrl(ctrl);
		return r;
	case NK_POWER_DIR:
		if (kstrcmp(name, "pm_qos_latency_tolerance_us"))
			return NULL;
		return nvme_new_file(dir, name,
				     nvme_sysfs_key(nk_instance(dir), 0,
						    NK_POWER_ATTR, 0),
				     0644);
	case NK_NS_DIR:
	case NK_HEAD_DIR:
		return nvme_ns_dir_lookup(dir, name);
	case NK_MPATH_DIR:
		return nvme_mpath_dir_lookup(dir, name);
	case NK_SUBSYS_DIR:
		subsys = nvme_sysfs_get_subsys(nk_instance(dir));
		if (!subsys)
			return NULL;
		r = nvme_subsys_dir_lookup(dir, subsys, name);
		nvme_put_subsystem(subsys);
		return r;
	default:
		return NULL;
	}
}

/* ---- the class directories ---------------------------------------------------- */

/* /sys/class/nvme: one directory per visible controller, by instance. */
static int nvme_class_list(struct pfs_node *dir, unsigned *cursor, char *name,
			   long cap, int *type)
{
	unsigned int i;
	uint64_t flags;
	int found = 0;

	spin_lock_irqsave(&nvme_sysfs_lock, &flags);
	for (i = *cursor; i < NVME_MAX_INSTANCES; i++) {
		if (!nvme_sysfs_ctrls[i])
			continue;
		ksnprintf(name, (size_t)cap, "%s", nvme_sysfs_ctrls[i]->name);
		/* A static link (the character device's) lists it already. */
		if (nvme_pfs_static_child(dir, name))
			continue;
		*type = DT_DIR;
		*cursor = i + 1;
		found = 1;
		break;
	}
	spin_unlock_irqrestore(&nvme_sysfs_lock, flags);
	return found;
}

static int nvme_parse_instance(const char *name, const char *prefix,
			       unsigned int *out)
{
	size_t plen = kstrlen(prefix);
	unsigned int v = 0;
	const char *p;

	if (kstrncmp(name, prefix, plen))
		return -EINVAL;
	p = name + plen;
	if (!*p)
		return -EINVAL;
	for (; *p; p++) {
		if (*p < '0' || *p > '9')
			return -EINVAL;
		v = v * 10 + (unsigned int)(*p - '0');
		if (v >= NVME_MAX_INSTANCES)
			return -EINVAL;
	}
	*out = v;
	return 0;
}

static struct pfs_node *nvme_class_lookup(struct pfs_node *dir,
					  const char *name)
{
	struct nvme_ctrl *ctrl;
	unsigned int inst;
	struct pfs_node *r;

	if (nvme_parse_instance(name, "nvme", &inst))
		return NULL;
	ctrl = nvme_sysfs_get_ctrl(inst);
	if (!ctrl)
		return NULL;
	r = nvme_new_dir(dir, name, nvme_sysfs_key(inst, 0, NK_CTRL_DIR, 0));
	nvme_put_ctrl(ctrl);
	return r;
}

/* /sys/class/nvme-subsystem */
static int nvme_subsys_class_list(struct pfs_node *dir, unsigned *cursor,
				  char *name, long cap, int *type)
{
	struct nvme_subsystem *subsys;
	int best = -1;

	(void)dir;
	mutex_lock(&nvme_subsystems_lock);
	list_for_each_entry(subsys, &nvme_subsystems, entry) {
		if (subsys->instance < 0 ||
		    (unsigned int)subsys->instance < *cursor)
			continue;
		if (best < 0 || subsys->instance < best)
			best = subsys->instance;
	}
	mutex_unlock(&nvme_subsystems_lock);
	if (best < 0)
		return 0;
	ksnprintf(name, (size_t)cap, "nvme-subsys%d", best);
	*type = DT_DIR;
	*cursor = (unsigned int)best + 1;
	return 1;
}

static struct pfs_node *nvme_subsys_class_lookup(struct pfs_node *dir,
						 const char *name)
{
	struct nvme_subsystem *subsys;
	unsigned int inst;
	struct pfs_node *r;

	if (nvme_parse_instance(name, "nvme-subsys", &inst))
		return NULL;
	subsys = nvme_sysfs_get_subsys(inst);
	if (!subsys)
		return NULL;
	r = nvme_new_dir(dir, name, nvme_sysfs_key(inst, 0, NK_SUBSYS_DIR, 0));
	nvme_put_subsystem(subsys);
	return r;
}

/* ==========================================================================
 * Registration
 * ========================================================================== */

void nvme_sysfs_init(void)
{
	struct pfs_node *d, t;

	/* Dynamic from the moment they are visible (pfs_add_node). */
	d = sysfs_mkdir("class");
	if (d) {
		kmemset(&t, 0, sizeof(t));
		t.list = nvme_class_list;
		t.lookup = nvme_class_lookup;
		d = pfs_add_node(d->fs, "class/nvme", PFS_DIR, &t);
	}
	if (d) {
		nvme_sysfs_fs = d->fs;
		kmemset(&t, 0, sizeof(t));
		t.list = nvme_subsys_class_list;
		t.lookup = nvme_subsys_class_lookup;
		pfs_add_node(d->fs, "class/nvme-subsystem", PFS_DIR, &t);
	}
	if (!nvme_sysfs_fs)
		kprintf(NVME_LOG_PREFIX "sysfs class directories missing\n");
}

/* Does the static path exist? */
static bool nvme_pfs_exists(const char *path)
{
	struct pfs_node *n = pfs_lookup(nvme_sysfs_fs, path);

	if (!n)
		return false;
	pfs_node_put(n);
	return true;
}

/* The controller's directory and the never-removed static links of its
 * instance number.  When the character device registration (P6,
 * sysfs_add_char_device) made <device>/nvme/nvmeX with the
 * /sys/class/nvme/nvmeX and /sys/dev/char/MAJ:MIN links already, that
 * static directory is adopted: it becomes dynamic (our attributes and
 * namespace directories appear next to its static uevent/dev/device/
 * subsystem).  Otherwise the dynamic /sys/class/nvme answers for the
 * controller and the links are made here, once per instance number. */
static void nvme_sysfs_static_links(struct nvme_ctrl *ctrl)
{
	unsigned int inst = (unsigned int)ctrl->instance;
	char path[160], target[64], devdir[128];
	struct pfs_node *d;

	if (!nvme_sysfs_fs)
		return;
	if (ctrl->pdev) {
		char pci[96];

		sysfs_pci_path(ctrl->pdev, pci, sizeof(pci));
		/* "/sys/bus/..." -> "bus/..." relative to the mount */
		ksnprintf(devdir, sizeof(devdir), "%s/nvme/%s",
			  !kstrncmp(pci, "/sys/", 5) ? pci + 5 : pci,
			  ctrl->name);
	} else {
		ksnprintf(devdir, sizeof(devdir), "devices/virtual/nvme/%s",
			  ctrl->name);
	}
	d = pfs_lookup(nvme_sysfs_fs, devdir);
	if (d) {
		if (d->type == PFS_DIR && !d->transient) {
			struct pfs_node t;

			/* The directory belongs to this PCI function: after a
			 * re-plug elsewhere the same instance must not show
			 * up under the old slot's directory too.  Its owner
			 * and key go in before the callbacks that read them
			 * (pfs_add_node on the existing static node). */
			kmemset(&t, 0, sizeof(t));
			t.arg = (void *)(uintptr_t)nvme_sysfs_pci_id(
				ctrl->pdev);
			t.arg2 = nvme_sysfs_key(inst, 0, NK_CTRL_DIR, 0);
			t.list = nvme_dir_list;
			t.lookup = nvme_dir_lookup;
			pfs_add_node(nvme_sysfs_fs, devdir, PFS_DIR, &t);
		}
		pfs_node_put(d);
	}

	/* Probes may run in parallel: claim the instance bit atomically. */
	if (__atomic_fetch_or(&nvme_sysfs_linked[inst / 8],
			      (uint8_t)(1u << (inst % 8)), __ATOMIC_ACQ_REL) &
	    (1u << (inst % 8)))
		return;
	ksnprintf(target, sizeof(target), "/sys/class/nvme/nvme%u", inst);
	if (nvme_char_major) {
		ksnprintf(path, sizeof(path), "dev/char/%u:%u", nvme_char_major,
			  inst);
		if (!nvme_pfs_exists(path))
			pfs_add_link(nvme_sysfs_fs, path, target);
	}
	if (ctrl->pdev && !nvme_pfs_exists(devdir)) {
		char parent[128];
		size_t n = kstrlen(devdir);

		/* <device>/nvme/nvmeX -> the class directory */
		while (n && devdir[n - 1] != '/')
			n--;
		if (n > 1 && n < sizeof(parent)) {
			kmemcpy(parent, devdir, n - 1);
			parent[n - 1] = '\0';
			pfs_mkdir(nvme_sysfs_fs, parent);
			pfs_add_link(nvme_sysfs_fs, devdir, target);
		}
	}
}

int nvme_sysfs_add_ctrl(struct nvme_ctrl *ctrl)
{
	uint64_t flags;
	int ret = 0;

	if (ctrl->instance < 0 || ctrl->instance >= NVME_MAX_INSTANCES)
		return -EINVAL;
	ksnprintf(ctrl->sysfs_base, sizeof(ctrl->sysfs_base),
		  "class/nvme/%s", ctrl->name);
	spin_lock_irqsave(&nvme_sysfs_lock, &flags);
	if (nvme_sysfs_ctrls[ctrl->instance])
		ret = -EEXIST;
	else
		nvme_sysfs_ctrls[ctrl->instance] = ctrl;
	spin_unlock_irqrestore(&nvme_sysfs_lock, flags);
	if (ret) {
		WARN(1, "nvme: instance %d registered twice", ctrl->instance);
		return ret;
	}
	nvme_sysfs_static_links(ctrl);
	return 0;
}

void nvme_sysfs_del_ctrl(struct nvme_ctrl *ctrl)
{
	uint64_t flags;

	if (ctrl->instance < 0 || ctrl->instance >= NVME_MAX_INSTANCES)
		return;
	spin_lock_irqsave(&nvme_sysfs_lock, &flags);
	if (nvme_sysfs_ctrls[ctrl->instance] == ctrl)
		nvme_sysfs_ctrls[ctrl->instance] = NULL;
	spin_unlock_irqrestore(&nvme_sysfs_lock, flags);
	ctrl->sysfs_base[0] = '\0';
}

int nvme_sysfs_add_subsys(struct nvme_subsystem *subsys)
{
	if (subsys->instance < 0)
		return -EINVAL;
	/* Listed from nvme_subsystems from now on (the caller links it). */
	ksnprintf(subsys->sysfs_base, sizeof(subsys->sysfs_base),
		  "class/nvme-subsystem/nvme-subsys%d", subsys->instance);
	return 0;
}

void nvme_sysfs_del_subsys(struct nvme_subsystem *subsys)
{
	subsys->sysfs_base[0] = '\0';
}

int nvme_sysfs_add_ns(struct nvme_ns *ns)
{
	if (!ns->ctrl || !ns->disk)
		return -EINVAL;
	ksnprintf(ns->sysfs_base, sizeof(ns->sysfs_base), "class/nvme/%s/%s",
		  ns->ctrl->name, ns->disk->disk_name);
	/* /sys/block/<disk>/device leads to the controller. */
	if (!ns->disk->device_link[0])
		ksnprintf(ns->disk->device_link, sizeof(ns->disk->device_link),
			  "%s", ns->ctrl->sysfs_base);
	return 0;
}

void nvme_sysfs_del_ns(struct nvme_ns *ns)
{
	ns->sysfs_base[0] = '\0';
}
