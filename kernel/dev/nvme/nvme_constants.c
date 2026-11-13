// LikeOS - NVMe constants: names of opcodes, status codes, status code
// types, features, log pages and quirks; error logging of failed commands
//
// Every table is indexed by the on-wire value (nvme_spec.h).  A status is
// looked up by its status code type and status code (the CQE status
// shifted right by one, with CRD/More/DNR masked off), exactly the
// representation nvme_req(rq)->status stores.
//
// Error logging is rate-limited per call site class like the established
// driver (a burst of 10 lines per 5 seconds), so a dying device cannot
// flood the console from the completion path.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/nvme/nvme.h>
#include <kernel/io/console.h>
#include <kernel/ke/hrtimer.h>

#define NVME_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* ==========================================================================
 * Opcodes
 * ========================================================================== */

static const char *const nvme_ops[] = {
	[nvme_cmd_flush] = "Flush",
	[nvme_cmd_write] = "Write",
	[nvme_cmd_read] = "Read",
	[nvme_cmd_write_uncor] = "Write Uncorrectable",
	[nvme_cmd_compare] = "Compare",
	[nvme_cmd_write_zeroes] = "Write Zeroes",
	[nvme_cmd_dsm] = "Dataset Management",
	[nvme_cmd_verify] = "Verify",
	[nvme_cmd_resv_register] = "Reservation Register",
	[nvme_cmd_resv_report] = "Reservation Report",
	[nvme_cmd_resv_acquire] = "Reservation Acquire",
	[nvme_cmd_io_mgmt_recv] = "I/O Management Receive",
	[nvme_cmd_resv_release] = "Reservation Release",
	[nvme_cmd_cancel] = "Cancel",
	[nvme_cmd_copy] = "Copy",
	[nvme_cmd_io_mgmt_send] = "I/O Management Send",
	[nvme_cmd_zone_mgmt_send] = "Zone Management Send",
	[nvme_cmd_zone_mgmt_recv] = "Zone Management Receive",
	[nvme_cmd_zone_append] = "Zone Append",
};

static const char *const nvme_admin_ops[] = {
	[nvme_admin_delete_sq] = "Delete SQ",
	[nvme_admin_create_sq] = "Create SQ",
	[nvme_admin_get_log_page] = "Get Log Page",
	[nvme_admin_delete_cq] = "Delete CQ",
	[nvme_admin_create_cq] = "Create CQ",
	[nvme_admin_identify] = "Identify",
	[nvme_admin_abort_cmd] = "Abort Command",
	[nvme_admin_set_features] = "Set Features",
	[nvme_admin_get_features] = "Get Features",
	[nvme_admin_async_event] = "Async Event",
	[nvme_admin_ns_mgmt] = "Namespace Management",
	[nvme_admin_activate_fw] = "Activate Firmware",
	[nvme_admin_download_fw] = "Download Firmware",
	[nvme_admin_dev_self_test] = "Device Self Test",
	[nvme_admin_ns_attach] = "Namespace Attach",
	[nvme_admin_keep_alive] = "Keep Alive",
	[nvme_admin_directive_send] = "Directive Send",
	[nvme_admin_directive_recv] = "Directive Receive",
	[nvme_admin_virtual_mgmt] = "Virtual Management",
	[nvme_admin_nvme_mi_send] = "NVMe Send MI",
	[nvme_admin_nvme_mi_recv] = "NVMe Receive MI",
	[nvme_admin_capacity_mgmt] = "Capacity Management",
	[nvme_admin_lockdown] = "Lockdown",
	[nvme_admin_dbbuf] = "Doorbell Buffer Config",
	[nvme_admin_fabrics] = "Fabrics Command",
	[nvme_admin_format_nvm] = "Format NVM",
	[nvme_admin_security_send] = "Security Send",
	[nvme_admin_security_recv] = "Security Receive",
	[nvme_admin_sanitize_nvm] = "Sanitize NVM",
	[nvme_admin_get_lba_status] = "Get LBA Status",
};

/* Fabrics command types (fctype); no fabrics in LikeOS, the names are
 * kept for decoding. */
static const char *const nvme_fabrics_ops[] = {
	[0x00] = "Property Set",
	[0x01] = "Connect",
	[0x04] = "Property Get",
	[0x05] = "Authentication Send",
	[0x06] = "Authentication Receive",
	[0x08] = "Disconnect",
};

const char *nvme_get_opcode_str(uint8_t opcode)
{
	if (opcode < NVME_ARRAY_SIZE(nvme_ops) && nvme_ops[opcode])
		return nvme_ops[opcode];
	return "Unknown";
}

const char *nvme_get_admin_opcode_str(uint8_t opcode)
{
	if (opcode < NVME_ARRAY_SIZE(nvme_admin_ops) && nvme_admin_ops[opcode])
		return nvme_admin_ops[opcode];
	return "Unknown";
}

const char *nvme_get_fabrics_opcode_str(uint8_t opcode)
{
	if (opcode < NVME_ARRAY_SIZE(nvme_fabrics_ops) &&
	    nvme_fabrics_ops[opcode])
		return nvme_fabrics_ops[opcode];
	return "Unknown";
}

/* ==========================================================================
 * Status codes
 * ========================================================================== */

static const char *const nvme_statuses[] = {
	/* Generic Command Status */
	[NVME_SC_SUCCESS] = "Success",
	[NVME_SC_INVALID_OPCODE] = "Invalid Command Opcode",
	[NVME_SC_INVALID_FIELD] = "Invalid Field in Command",
	[NVME_SC_CMDID_CONFLICT] = "Command ID Conflict",
	[NVME_SC_DATA_XFER_ERROR] = "Data Transfer Error",
	[NVME_SC_POWER_LOSS] =
		"Commands Aborted due to Power Loss Notification",
	[NVME_SC_INTERNAL] = "Internal Error",
	[NVME_SC_ABORT_REQ] = "Command Abort Requested",
	[NVME_SC_ABORT_QUEUE] = "Command Aborted due to SQ Deletion",
	[NVME_SC_FUSED_FAIL] = "Command Aborted due to Failed Fused Command",
	[NVME_SC_FUSED_MISSING] =
		"Command Aborted due to Missing Fused Command",
	[NVME_SC_INVALID_NS] = "Invalid Namespace or Format",
	[NVME_SC_CMD_SEQ_ERROR] = "Command Sequence Error",
	[NVME_SC_SGL_INVALID_LAST] = "Invalid SGL Segment Descriptor",
	[NVME_SC_SGL_INVALID_COUNT] = "Invalid Number of SGL Descriptors",
	[NVME_SC_SGL_INVALID_DATA] = "Data SGL Length Invalid",
	[NVME_SC_SGL_INVALID_METADATA] = "Metadata SGL Length Invalid",
	[NVME_SC_SGL_INVALID_TYPE] = "SGL Descriptor Type Invalid",
	[NVME_SC_CMB_INVALID_USE] = "Invalid Use of Controller Memory Buffer",
	[NVME_SC_PRP_INVALID_OFFSET] = "PRP Offset Invalid",
	[NVME_SC_ATOMIC_WU_EXCEEDED] = "Atomic Write Unit Exceeded",
	[NVME_SC_OP_DENIED] = "Operation Denied",
	[NVME_SC_SGL_INVALID_OFFSET] = "SGL Offset Invalid",
	[NVME_SC_RESERVED] = "Reserved",
	[NVME_SC_HOST_ID_INCONSIST] = "Host Identifier Inconsistent Format",
	[NVME_SC_KA_TIMEOUT_EXPIRED] = "Keep Alive Timeout Expired",
	[NVME_SC_KA_TIMEOUT_INVALID] = "Keep Alive Timeout Invalid",
	[NVME_SC_ABORTED_PREEMPT_ABORT] =
		"Command Aborted due to Preempt and Abort",
	[NVME_SC_SANITIZE_FAILED] = "Sanitize Failed",
	[NVME_SC_SANITIZE_IN_PROGRESS] = "Sanitize In Progress",
	[NVME_SC_SGL_INVALID_GRANULARITY] =
		"SGL Data Block Granularity Invalid",
	[NVME_SC_CMD_NOT_SUP_CMB_QUEUE] =
		"Command Not Supported for Queue in CMB",
	[NVME_SC_NS_WRITE_PROTECTED] = "Namespace is Write Protected",
	[NVME_SC_CMD_INTERRUPTED] = "Command Interrupted",
	[NVME_SC_TRANSIENT_TR_ERR] = "Transient Transport Error",
	[NVME_SC_PROHIBITED_BY_LOCKDOWN] =
		"Command Prohibited by Command and Feature Lockdown",
	[NVME_SC_ADMIN_COMMAND_MEDIA_NOT_READY] =
		"Admin Command Media Not Ready",
	[NVME_SC_INVALID_KEY_TAG] = "Invalid Key Tag",
	[NVME_SC_HDISNS_NOT_ENABLED] =
		"Host Dispersed Namespace Support Not Enabled",
	[NVME_SC_HOST_ID_NOT_INIT] = "Host Identifier Not Initialized",
	[NVME_SC_INCORRECT_KEY] = "Incorrect Key",
	[NVME_SC_FDP_DISABLED] = "FDP Disabled",
	[NVME_SC_INVALID_PLACEMENT_HANDLE] =
		"Invalid Placement Handle List",
	[NVME_SC_LBA_RANGE] = "LBA Out of Range",
	[NVME_SC_CAP_EXCEEDED] = "Capacity Exceeded",
	[NVME_SC_NS_NOT_READY] = "Namespace Not Ready",
	[NVME_SC_RESERVATION_CONFLICT] = "Reservation Conflict",
	[NVME_SC_FORMAT_IN_PROGRESS] = "Format In Progress",
	[NVME_SC_INVALID_VALUE_SIZE] = "Invalid Value Size",
	[NVME_SC_INVALID_KEY_SIZE] = "Invalid Key Size",
	[NVME_SC_KV_KEY_NOT_EXISTS] = "KV Key Does Not Exist",
	[NVME_SC_UNRECOVERED_ERROR] = "Unrecovered Error",
	[NVME_SC_KEY_EXISTS] = "Key Exists",
	/* Command Specific Status */
	[NVME_SC_CQ_INVALID] = "Completion Queue Invalid",
	[NVME_SC_QID_INVALID] = "Invalid Queue Identifier",
	[NVME_SC_QUEUE_SIZE] = "Invalid Queue Size",
	[NVME_SC_ABORT_LIMIT] = "Abort Command Limit Exceeded",
	[NVME_SC_ABORT_MISSING] = "Reserved", /* obsolete */
	[NVME_SC_ASYNC_LIMIT] = "Asynchronous Event Request Limit Exceeded",
	[NVME_SC_FIRMWARE_SLOT] = "Invalid Firmware Slot",
	[NVME_SC_FIRMWARE_IMAGE] = "Invalid Firmware Image",
	[NVME_SC_INVALID_VECTOR] = "Invalid Interrupt Vector",
	[NVME_SC_INVALID_LOG_PAGE] = "Invalid Log Page",
	[NVME_SC_INVALID_FORMAT] = "Invalid Format",
	[NVME_SC_FW_NEEDS_CONV_RESET] =
		"Firmware Activation Requires Conventional Reset",
	[NVME_SC_INVALID_QUEUE] = "Invalid Queue Deletion",
	[NVME_SC_FEATURE_NOT_SAVEABLE] = "Feature Identifier Not Saveable",
	[NVME_SC_FEATURE_NOT_CHANGEABLE] = "Feature Not Changeable",
	[NVME_SC_FEATURE_NOT_PER_NS] = "Feature Not Namespace Specific",
	[NVME_SC_FW_NEEDS_SUBSYS_RESET] =
		"Firmware Activation Requires NVM Subsystem Reset",
	[NVME_SC_FW_NEEDS_RESET] = "Firmware Activation Requires Reset",
	[NVME_SC_FW_NEEDS_MAX_TIME] =
		"Firmware Activation Requires Maximum Time Violation",
	[NVME_SC_FW_ACTIVATE_PROHIBITED] = "Firmware Activation Prohibited",
	[NVME_SC_OVERLAPPING_RANGE] = "Overlapping Range",
	[NVME_SC_NS_INSUFFICIENT_CAP] = "Namespace Insufficient Capacity",
	[NVME_SC_NS_ID_UNAVAILABLE] = "Namespace Identifier Unavailable",
	[NVME_SC_NS_ALREADY_ATTACHED] = "Namespace Already Attached",
	[NVME_SC_NS_IS_PRIVATE] = "Namespace Is Private",
	[NVME_SC_NS_NOT_ATTACHED] = "Namespace Not Attached",
	[NVME_SC_THIN_PROV_NOT_SUPP] = "Thin Provisioning Not Supported",
	[NVME_SC_CTRL_LIST_INVALID] = "Controller List Invalid",
	[NVME_SC_SELF_TEST_IN_PROGRESS] = "Device Self-test In Progress",
	[NVME_SC_BP_WRITE_PROHIBITED] = "Boot Partition Write Prohibited",
	[NVME_SC_CTRL_ID_INVALID] = "Invalid Controller Identifier",
	[NVME_SC_SEC_CTRL_STATE_INVALID] = "Invalid Secondary Controller State",
	[NVME_SC_CTRL_RES_NUM_INVALID] =
		"Invalid Number of Controller Resources",
	[NVME_SC_RES_ID_INVALID] = "Invalid Resource Identifier",
	[NVME_SC_PMR_SAN_PROHIBITED] = "Sanitize Prohibited",
	[NVME_SC_ANA_GROUP_ID_INVALID] = "ANA Group Identifier Invalid",
	[NVME_SC_ANA_ATTACH_FAILED] = "ANA Attach Failed",
	[NVME_SC_INSUFFICIENT_CAP] = "Insufficient Capacity",
	[NVME_SC_NS_ATTACH_LIMIT] = "Namespace Attachment Limit Exceeded",
	[NVME_SC_PROHIBIT_CMD_EXEC_NOT_SUPP] =
		"Prohibition of Command Execution Not Supported",
	[NVME_SC_IOCS_NOT_SUPPORTED] = "I/O Command Set Not Supported",
	[NVME_SC_IOCS_NOT_ENABLED] = "I/O Command Set Not Enabled",
	[NVME_SC_IOCS_COMBINATION_REJECTED] =
		"I/O Command Set Combination Rejected",
	[NVME_SC_INVALID_IO_CMD_SET] = "Invalid IO Command Set",
	[NVME_SC_ID_UNAVAILABLE] = "Identifier Unavailable",
	[NVME_SC_BAD_ATTRIBUTES] = "Conflicting Attributes",
	[NVME_SC_INVALID_PI] = "Invalid Protection Information",
	[NVME_SC_READ_ONLY] = "Attempted Write to Read Only Range",
	[NVME_SC_CMD_SIZE_LIM_EXCEEDED] = "Command Size Limits Exceeded",
	[NVME_SC_INCOMPATIBLE_NS] = "Incompatible Namespace or Format",
	[NVME_SC_FAST_COPY_NOT_POSSIBLE] = "Fast Copy Not Possible",
	[NVME_SC_OVERLAPPING_IO_RANGE] = "Overlapping I/O Range",
	[NVME_SC_ZONE_BOUNDARY_ERROR] = "Zoned Boundary Error",
	[NVME_SC_ZONE_FULL] = "Zone Is Full",
	[NVME_SC_ZONE_READ_ONLY] = "Zone Is Read Only",
	[NVME_SC_ZONE_OFFLINE] = "Zone Is Offline",
	[NVME_SC_ZONE_INVALID_WRITE] = "Zone Invalid Write",
	[NVME_SC_ZONE_TOO_MANY_ACTIVE] = "Too Many Active Zones",
	[NVME_SC_ZONE_TOO_MANY_OPEN] = "Too Many Open Zones",
	[NVME_SC_ZONE_INVALID_TRANSITION] = "Invalid Zone State Transition",
	/* Media and Data Integrity Errors */
	[NVME_SC_WRITE_FAULT] = "Write Fault",
	[NVME_SC_READ_ERROR] = "Unrecovered Read Error",
	[NVME_SC_GUARD_CHECK] = "End-to-end Guard Check Error",
	[NVME_SC_APPTAG_CHECK] = "End-to-end Application Tag Check Error",
	[NVME_SC_REFTAG_CHECK] = "End-to-end Reference Tag Check Error",
	[NVME_SC_COMPARE_FAILED] = "Compare Failure",
	[NVME_SC_ACCESS_DENIED] = "Access Denied",
	[NVME_SC_UNWRITTEN_BLOCK] = "Deallocated or Unwritten Logical Block",
	[NVME_SC_STORAGE_TAG_CHECK] = "End-to-end Storage Tag Check Error",
	/* Path Related Status */
	[NVME_SC_INTERNAL_PATH_ERROR] = "Internal Pathing Error",
	[NVME_SC_ANA_PERSISTENT_LOSS] = "Asymmetric Access Persistent Loss",
	[NVME_SC_ANA_INACCESSIBLE] = "Asymmetric Access Inaccessible",
	[NVME_SC_ANA_TRANSITION] = "Asymmetric Access Transition",
	[NVME_SC_CTRL_PATH_ERROR] = "Controller Pathing Error",
	[NVME_SC_HOST_PATH_ERROR] = "Host Pathing Error",
	[NVME_SC_HOST_ABORTED_CMD] = "Host Aborted Command",
};

const char *nvme_get_error_status_str(uint16_t status)
{
	status &= NVME_SCT_SC_MASK;
	if (status < NVME_ARRAY_SIZE(nvme_statuses) && nvme_statuses[status])
		return nvme_statuses[status];
	return "Unknown";
}

const char *nvme_get_sct_str(uint16_t status)
{
	switch (NVME_SCT(status)) {
	case NVME_SCT_TYPE_GENERIC:
		return "Generic Command Status";
	case NVME_SCT_TYPE_CMD_SPECIFIC:
		return "Command Specific Status";
	case NVME_SCT_TYPE_MEDIA:
		return "Media and Data Integrity Errors";
	case NVME_SCT_TYPE_PATH:
		return "Path Related Status";
	case NVME_SCT_TYPE_VENDOR:
		return "Vendor Specific";
	default:
		return "Reserved";
	}
}

/* ==========================================================================
 * Features, log pages, quirks
 * ========================================================================== */

static const char *const nvme_features_names[] = {
	[NVME_FEAT_ARBITRATION] = "Arbitration",
	[NVME_FEAT_POWER_MGMT] = "Power Management",
	[NVME_FEAT_LBA_RANGE] = "LBA Range Type",
	[NVME_FEAT_TEMP_THRESH] = "Temperature Threshold",
	[NVME_FEAT_ERR_RECOVERY] = "Error Recovery",
	[NVME_FEAT_VOLATILE_WC] = "Volatile Write Cache",
	[NVME_FEAT_NUM_QUEUES] = "Number of Queues",
	[NVME_FEAT_IRQ_COALESCE] = "Interrupt Coalescing",
	[NVME_FEAT_IRQ_CONFIG] = "Interrupt Vector Configuration",
	[NVME_FEAT_WRITE_ATOMIC] = "Write Atomicity Normal",
	[NVME_FEAT_ASYNC_EVENT] = "Asynchronous Event Configuration",
	[NVME_FEAT_AUTO_PST] = "Autonomous Power State Transition",
	[NVME_FEAT_HOST_MEM_BUF] = "Host Memory Buffer",
	[NVME_FEAT_TIMESTAMP] = "Timestamp",
	[NVME_FEAT_KATO] = "Keep Alive Timer",
	[NVME_FEAT_HCTM] = "Host Controlled Thermal Management",
	[NVME_FEAT_NOPSC] = "Non-Operational Power State Config",
	[NVME_FEAT_RRL] = "Read Recovery Level Config",
	[NVME_FEAT_PLM_CONFIG] = "Predictable Latency Mode Config",
	[NVME_FEAT_PLM_WINDOW] = "Predictable Latency Mode Window",
	[NVME_FEAT_LBA_STS_INTERVAL] = "LBA Status Information Attributes",
	[NVME_FEAT_HOST_BEHAVIOR] = "Host Behavior Support",
	[NVME_FEAT_SANITIZE] = "Sanitize Config",
	[NVME_FEAT_ENDURANCE_EVT_CFG] = "Endurance Group Event Configuration",
	[NVME_FEAT_IOCS_PROFILE] = "I/O Command Set Profile",
	[NVME_FEAT_SPINUP_CONTROL] = "Spinup Control",
	[NVME_FEAT_FDP] = "Flexible Data Placement",
	[NVME_FEAT_FDP_EVENTS] = "FDP Events",
	[NVME_FEAT_ENH_CTRL_METADATA] = "Enhanced Controller Metadata",
	[NVME_FEAT_CTRL_METADATA] = "Controller Metadata",
	[NVME_FEAT_NS_METADATA] = "Namespace Metadata",
	[NVME_FEAT_SW_PROGRESS] = "Software Progress Marker",
	[NVME_FEAT_HOST_ID] = "Host Identifier",
	[NVME_FEAT_RESV_MASK] = "Reservation Notification Mask",
	[NVME_FEAT_RESV_PERSIST] = "Reservation Persistence",
	[NVME_FEAT_WRITE_PROTECT] = "Namespace Write Protection Config",
};

const char *nvme_feature_name(uint8_t fid)
{
	if (fid < NVME_ARRAY_SIZE(nvme_features_names) &&
	    nvme_features_names[fid])
		return nvme_features_names[fid];
	if (fid >= NVME_FEAT_VENDOR_START)
		return "Vendor Specific";
	return "Unknown";
}

static const char *const nvme_log_names[] = {
	[NVME_LOG_SUPPORTED] = "Supported Log Pages",
	[NVME_LOG_ERROR] = "Error Information",
	[NVME_LOG_SMART] = "SMART / Health Information",
	[NVME_LOG_FW_SLOT] = "Firmware Slot Information",
	[NVME_LOG_CHANGED_NS] = "Changed Namespace List",
	[NVME_LOG_CMD_EFFECTS] = "Commands Supported and Effects",
	[NVME_LOG_DEVICE_SELF_TEST] = "Device Self-test",
	[NVME_LOG_TELEMETRY_HOST] = "Telemetry Host-Initiated",
	[NVME_LOG_TELEMETRY_CTRL] = "Telemetry Controller-Initiated",
	[NVME_LOG_ENDURANCE_GROUP] = "Endurance Group Information",
	[NVME_LOG_PLM] = "Predictable Latency Per NVM Set",
	[NVME_LOG_PLM_AGGREGATE] = "Predictable Latency Event Aggregate",
	[NVME_LOG_ANA] = "Asymmetric Namespace Access",
	[NVME_LOG_PERSISTENT_EVENT] = "Persistent Event Log",
	[NVME_LOG_LBA_STATUS] = "LBA Status Information",
	[NVME_LOG_ENDGRP_EVENT_AGG] = "Endurance Group Event Aggregate",
	[NVME_LOG_MEDIA_UNIT_STATUS] = "Media Unit Status",
	[NVME_LOG_SUPPORTED_CAP_CONFIG] = "Supported Capacity Configuration",
	[NVME_LOG_FEATURES] = "Feature Identifiers Supported and Effects",
	[NVME_LOG_MI_CMD_EFFECTS] = "NVMe-MI Commands Supported and Effects",
	[NVME_LOG_CMD_FEAT_LOCKDOWN] = "Command and Feature Lockdown",
	[NVME_LOG_BOOT_PARTITION] = "Boot Partition",
	[NVME_LOG_RMI] = "Rotational Media Information",
	[NVME_LOG_DISPERSED_NS] =
		"Dispersed Namespace Participating NVM Subsystems",
	[NVME_LOG_MGMT_ADDR_LIST] = "Management Address List",
	[NVME_LOG_PHY_RX_EOM] = "Physical Interface Receiver Eye Opening",
	[NVME_LOG_REACH_GROUPS] = "Reachability Groups",
	[NVME_LOG_REACH_ASSOC] = "Reachability Associations",
	[NVME_LOG_CHANGED_ALLOC_NS] = "Changed Allocated Namespace List",
	[NVME_LOG_FDP_CONFIGS] = "FDP Configurations",
	[NVME_LOG_RUHU] = "Reclaim Unit Handle Usage",
	[NVME_LOG_FDP_STATS] = "FDP Statistics",
	[NVME_LOG_FDP_EVENTS] = "FDP Events",
	[NVME_LOG_DISC] = "Discovery",
	[NVME_LOG_RESERVATION] = "Reservation Notification",
	[NVME_LOG_SANITIZE] = "Sanitize Status",
	[NVME_LOG_ZNS_CHANGED_ZONES] = "Changed Zone List",
};

const char *nvme_log_page_name(uint8_t lid)
{
	if (lid < NVME_ARRAY_SIZE(nvme_log_names) && nvme_log_names[lid])
		return nvme_log_names[lid];
	if (lid >= NVME_LOG_VENDOR_START)
		return "Vendor Specific";
	return "Unknown";
}

const char *nvme_quirk_name(enum nvme_quirks q)
{
	switch (q) {
	case NVME_QUIRK_STRIPE_SIZE:
		return "stripe_size";
	case NVME_QUIRK_IDENTIFY_CNS:
		return "identify_cns";
	case NVME_QUIRK_DEALLOCATE_ZEROES:
		return "deallocate_zeroes";
	case NVME_QUIRK_DELAY_BEFORE_CHK_RDY:
		return "delay_before_chk_rdy";
	case NVME_QUIRK_NO_APST:
		return "no_apst";
	case NVME_QUIRK_NO_DEEPEST_PS:
		return "no_deepest_ps";
	case NVME_QUIRK_QDEPTH_ONE:
		return "qdepth_one";
	case NVME_QUIRK_MEDIUM_PRIO_SQ:
		return "medium_prio_sq";
	case NVME_QUIRK_IGNORE_DEV_SUBNQN:
		return "ignore_dev_subnqn";
	case NVME_QUIRK_DISABLE_WRITE_ZEROES:
		return "disable_write_zeroes";
	case NVME_QUIRK_SIMPLE_SUSPEND:
		return "simple_suspend";
	case NVME_QUIRK_SINGLE_VECTOR:
		return "single_vector";
	case NVME_QUIRK_128_BYTES_SQES:
		return "128_bytes_sqes";
	case NVME_QUIRK_SHARED_TAGS:
		return "shared_tags";
	case NVME_QUIRK_NO_TEMP_THRESH_CHANGE:
		return "no_temp_thresh_change";
	case NVME_QUIRK_NO_NS_DESC_LIST:
		return "no_ns_desc_list";
	case NVME_QUIRK_DMA_ADDRESS_BITS_48:
		return "dma_address_bits_48";
	case NVME_QUIRK_SKIP_CID_GEN:
		return "skip_cid_gen";
	case NVME_QUIRK_BOGUS_NID:
		return "bogus_nid";
	case NVME_QUIRK_NO_SECONDARY_TEMP_THRESH:
		return "no_secondary_temp_thresh";
	case NVME_QUIRK_FORCE_NO_SIMPLE_SUSPEND:
		return "force_no_simple_suspend";
	case NVME_QUIRK_BROKEN_MSI:
		return "broken_msi";
	case NVME_QUIRK_DMAPOOL_ALIGN_512:
		return "dmapool_align_512";
	}
	return "unknown";
}

/* ==========================================================================
 * Error logging
 * ========================================================================== */

/* A burst of NVME_LOG_BURST lines per NVME_LOG_INTERVAL_NS, the rest
 * counted and reported when the next interval opens.  Any context; the
 * counters are approximate under contention, which only costs a line. */
#define NVME_LOG_BURST 10
#define NVME_LOG_INTERVAL_NS (5ULL * 1000000000ULL)

struct nvme_ratelimit {
	volatile uint64_t begin_ns;
	volatile uint32_t printed;
	volatile uint32_t missed;
};

static bool nvme_ratelimit(struct nvme_ratelimit *rl)
{
	uint64_t now = hrtimer_now_ns();
	uint64_t begin = __atomic_load_n(&rl->begin_ns, __ATOMIC_RELAXED);

	if (!begin || now - begin > NVME_LOG_INTERVAL_NS) {
		if (__atomic_compare_exchange_n(&rl->begin_ns, &begin, now, 0,
						__ATOMIC_ACQ_REL,
						__ATOMIC_RELAXED)) {
			uint32_t missed = __atomic_exchange_n(
				&rl->missed, 0, __ATOMIC_ACQ_REL);

			__atomic_store_n(&rl->printed, 0, __ATOMIC_RELEASE);
			if (missed)
				kprintf(NVME_LOG_PREFIX
					"%u error messages suppressed\n",
					missed);
		}
	}
	if (__atomic_add_fetch(&rl->printed, 1, __ATOMIC_ACQ_REL) <=
	    NVME_LOG_BURST)
		return true;
	__atomic_add_fetch(&rl->missed, 1, __ATOMIC_RELAXED);
	return false;
}

static struct nvme_ratelimit nvme_err_rl;
static struct nvme_ratelimit nvme_passthru_rl;

void nvme_log_error(struct blk_request *rq)
{
	struct nvme_request *nr = nvme_req(rq);
	struct nvme_ns *ns = rq->queue ? blk_queue_get_queuedata(rq->queue) :
					 NULL;
	uint8_t opcode = nr->cmd ? nr->cmd->common.opcode : 0xff;

	if (!nvme_ratelimit(&nvme_err_rl))
		return;
	if (ns && ns->head) {
		kprintf(NVME_LOG_PREFIX
			"%s: %s(0x%x) @ LBA %llu, %u blocks, %s (sct 0x%x / sc "
			"0x%x) %s%s\n",
			ns->disk ? ns->disk->disk_name : "?",
			nvme_get_opcode_str(opcode), opcode,
			(unsigned long long)nvme_sect_to_lba(ns->head,
							     blk_rq_pos(rq)),
			(unsigned int)(blk_rq_bytes(rq) >> ns->head->lba_shift),
			nvme_get_error_status_str(nr->status),
			NVME_SCT(nr->status), nr->status & NVME_SC_MASK,
			nr->status & NVME_STATUS_MORE ? "MORE " : "",
			nr->status & NVME_STATUS_DNR ? "DNR " : "");
		return;
	}
	kprintf(NVME_LOG_PREFIX "%s: %s(0x%x), %s (sct 0x%x / sc 0x%x) %s%s\n",
		nr->ctrl ? nr->ctrl->name : "?",
		nvme_get_admin_opcode_str(opcode), opcode,
		nvme_get_error_status_str(nr->status), NVME_SCT(nr->status),
		nr->status & NVME_SC_MASK,
		nr->status & NVME_STATUS_MORE ? "MORE " : "",
		nr->status & NVME_STATUS_DNR ? "DNR " : "");
}

void nvme_log_err_passthru(struct blk_request *rq)
{
	struct nvme_request *nr = nvme_req(rq);
	struct nvme_ns *ns = rq->queue ? blk_queue_get_queuedata(rq->queue) :
					 NULL;
	const struct nvme_command *c = nr->cmd;
	uint8_t opcode;

	if (!c || !nvme_ratelimit(&nvme_passthru_rl))
		return;
	opcode = c->common.opcode;
	kprintf(NVME_LOG_PREFIX
		"%s: %s(0x%x), %s (sct 0x%x / sc 0x%x) %s%scdw10=0x%x "
		"cdw11=0x%x cdw12=0x%x cdw13=0x%x cdw14=0x%x cdw15=0x%x\n",
		ns ? (ns->disk ? ns->disk->disk_name : "?") :
		     (nr->ctrl ? nr->ctrl->name : "?"),
		ns ? nvme_get_opcode_str(opcode) :
		     nvme_get_admin_opcode_str(opcode),
		opcode, nvme_get_error_status_str(nr->status),
		NVME_SCT(nr->status), nr->status & NVME_SC_MASK,
		nr->status & NVME_STATUS_MORE ? "MORE " : "",
		nr->status & NVME_STATUS_DNR ? "DNR " : "",
		le32_to_cpu(c->common.cdw10), le32_to_cpu(c->common.cdw11),
		le32_to_cpu(c->common.cdw12), le32_to_cpu(c->common.cdw13),
		le32_to_cpu(c->common.cdw14), le32_to_cpu(c->common.cdw15));
}
