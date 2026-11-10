// LikeOS AHCI driver - diagnostics: register dumps, trace ring, fault
// injection, sysfs
//
// What a person debugging a storage problem needs to see, without a
// debugger attached:
//
//   * REGISTER DUMPS (kprintf, also readable as sysfs files): every generic
//     host register and every port register, each with its bits decoded
//     by name -- "PxCMD 0x0000c017 { ST SUD POD FRE FR CR ICC=active }",
//     "SError 0x04050002 { RecovComm PHYRdyChg CommWake DevExch }" -- plus
//     the driver's own view of the port (slot bitmaps, deferred commands,
//     pending error-handling actions) and of one command slot (header,
//     command FIS, PRDT).
//
//   * The COMMAND TRACE RING: per port, the last AHCI_TRACE_ENTRIES events
//     (issue, completion with latency and result, error, timeout, abort,
//     reset, interrupt, EH run), recorded from any context under a leaf
//     lock by the command engine and the interrupt path, and formatted on
//     demand, oldest first.
//
//   * ERROR COUNTERS: the per-port and per-controller statistics, the
//     per-device error accounting of the ATA layer.
//
//   * FAULT INJECTION: a port can be told to fail its next N commands (or
//     every Nth) with a timeout, a device error, a host bus error or a
//     link loss, restricted to NCQ commands or writes if wanted, so the
//     error-handling ladder can be exercised on healthy hardware.  Internal
//     (EH) commands are never failed -- recovery itself must work.
//
//   * SYSFS: the established ATA class directories
//       /sys/class/ata_port/ataN/   port_no idle_irq nr_pmp_links stats
//                                   regs trace fault_inject eh state
//                                   link_power_management_policy
//       /sys/class/ata_link/linkN[.M]/  hw_sata_spd_limit sata_spd_limit
//                                   sata_spd sstatus serror scontrol
//       /sys/class/ata_device/devN.M[.D]/  class id model serial firmware
//                                   ncq_depth errors power_state security
//                                   trim
//     and the debug tree /sys/kernel/debug/ahci/ (controllers, dma,
//     dma_noncoherent, <hba>/{regs,info,stats,reset,power},
//     <hba>/ports/<n>/{regs,trace,fault}).
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/ata/ahci.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/fs/sysfs.h>

/* ==========================================================================
 * Output sink: the console, or a sysfs buffer
 * ========================================================================== */

struct ahci_sbuf {
	char *buf; /* NULL: print to the console */
	long cap;
	long pos; /* may run past cap: the length that would be needed */
};

static void sbp(struct ahci_sbuf *sb, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void sbp(struct ahci_sbuf *sb, const char *fmt, ...)
{
	va_list ap;

	__builtin_va_start(ap, fmt);
	if (!sb || !sb->buf) {
		kvprintf(fmt, ap);
	} else {
		long avail = sb->cap - sb->pos;
		int n;

		if (avail > 1) {
			n = kvsnprintf(sb->buf + sb->pos, (size_t)avail, fmt, ap);
		} else {
			char dummy[1];

			n = kvsnprintf(dummy, 1, fmt, ap);
		}
		if (n > 0)
			sb->pos += n;
	}
	__builtin_va_end(ap);
}

static long sb_result(const struct ahci_sbuf *sb)
{
	return sb->pos;
}

/* Append a word to a decoder buffer ("TFES IFS ..."). */
static int ahci_strcat_word(char *buf, int cap, int pos, const char *w)
{
	int n;

	if (!buf || cap <= 0)
		return pos;
	if (pos >= cap)
		return pos;
	n = ksnprintf(buf + pos, (size_t)(cap - pos), "%s%s", pos ? " " : "",
		      w);
	if (n < 0)
		return pos;
	pos += n;
	return pos < cap ? pos : cap - 1;
}

/* ==========================================================================
 * Text decoders (pure)
 * ========================================================================== */

struct ahci_bitname {
	uint32_t bit;
	const char *name;
};

static const struct ahci_bitname ahci_is_names[] = {
	{ AHCI_PxIS_CPDS, "CPDS" }, { AHCI_PxIS_TFES, "TFES" },
	{ AHCI_PxIS_HBFS, "HBFS" }, { AHCI_PxIS_HBDS, "HBDS" },
	{ AHCI_PxIS_IFS, "IFS" },   { AHCI_PxIS_INFS, "INFS" },
	{ AHCI_PxIS_OFS, "OFS" },   { AHCI_PxIS_IPMS, "IPMS" },
	{ AHCI_PxIS_PRCS, "PRCS" }, { AHCI_PxIS_DMPS, "DMPS" },
	{ AHCI_PxIS_PCS, "PCS" },   { AHCI_PxIS_DPS, "DPS" },
	{ AHCI_PxIS_UFS, "UFS" },   { AHCI_PxIS_SDBS, "SDBS" },
	{ AHCI_PxIS_DSS, "DSS" },   { AHCI_PxIS_PSS, "PSS" },
	{ AHCI_PxIS_DHRS, "DHRS" }, { 0, NULL },
};

int ahci_irq_stat_str(uint32_t is, char *buf, int cap)
{
	int pos = 0;
	uint32_t known = 0;

	if (!buf || cap <= 0)
		return 0;
	buf[0] = 0;
	for (const struct ahci_bitname *b = ahci_is_names; b->name; b++) {
		known |= b->bit;
		if (is & b->bit)
			pos = ahci_strcat_word(buf, cap, pos, b->name);
	}
	if (is & ~known) {
		char tmp[24];

		ksnprintf(tmp, sizeof(tmp), "rsvd=0x%x", is & ~known);
		pos = ahci_strcat_word(buf, cap, pos, tmp);
	}
	return pos;
}

/* SError, with the names the established error reports use */
static const struct ahci_bitname ahci_serr_names[] = {
	{ AHCI_SERR_ERR_I, "RecovData" },
	{ AHCI_SERR_ERR_M, "RecovComm" },
	{ AHCI_SERR_ERR_T, "UnrecovData" },
	{ AHCI_SERR_ERR_C, "Persist" },
	{ AHCI_SERR_ERR_P, "Proto" },
	{ AHCI_SERR_ERR_E, "HostInt" },
	{ AHCI_SERR_DIAG_N, "PHYRdyChg" },
	{ AHCI_SERR_DIAG_I, "PHYInt" },
	{ AHCI_SERR_DIAG_W, "CommWake" },
	{ AHCI_SERR_DIAG_B, "10B8B" },
	{ AHCI_SERR_DIAG_D, "Dispar" },
	{ AHCI_SERR_DIAG_C, "BadCRC" },
	{ AHCI_SERR_DIAG_H, "Handshk" },
	{ AHCI_SERR_DIAG_S, "LinkSeq" },
	{ AHCI_SERR_DIAG_T, "TrStaTrns" },
	{ AHCI_SERR_DIAG_F, "UnrecFIS" },
	{ AHCI_SERR_DIAG_X, "DevExch" },
	{ 0, NULL },
};

int ahci_serror_str(uint32_t serr, char *buf, int cap)
{
	int pos;

	if (!buf || cap <= 0)
		return 0;
	pos = ksnprintf(buf, (size_t)cap, "{");
	if (pos >= cap)
		pos = cap - 1;
	for (const struct ahci_bitname *b = ahci_serr_names; b->name; b++)
		if (serr & b->bit)
			pos = ahci_strcat_word(buf, cap, pos, b->name);
	if (pos < cap - 2) {
		buf[pos++] = ' ';
		buf[pos++] = '}';
		buf[pos] = 0;
	}
	return pos;
}

static const struct ahci_bitname ahci_cmd_names[] = {
	{ AHCI_PxCMD_ST, "ST" },	 { AHCI_PxCMD_SUD, "SUD" },
	{ AHCI_PxCMD_POD, "POD" },	 { AHCI_PxCMD_CLO, "CLO" },
	{ AHCI_PxCMD_FRE, "FRE" },	 { AHCI_PxCMD_MPSS, "MPSS" },
	{ AHCI_PxCMD_FR, "FR" },	 { AHCI_PxCMD_CR, "CR" },
	{ AHCI_PxCMD_CPS, "CPS" },	 { AHCI_PxCMD_PMA, "PMA" },
	{ AHCI_PxCMD_HPCP, "HPCP" },	 { AHCI_PxCMD_MPSP, "MPSP" },
	{ AHCI_PxCMD_CPD, "CPD" },	 { AHCI_PxCMD_ESP, "ESP" },
	{ AHCI_PxCMD_FBSCP, "FBSCP" }, { AHCI_PxCMD_APSTE, "APSTE" },
	{ AHCI_PxCMD_ATAPI, "ATAPI" }, { AHCI_PxCMD_DLAE, "DLAE" },
	{ AHCI_PxCMD_ALPE, "ALPE" },	 { AHCI_PxCMD_ASP, "ASP" },
	{ 0, NULL },
};

static const char *ahci_icc_name(uint32_t icc)
{
	switch (icc) {
	case 0x0:
		return "idle";
	case 0x1:
		return "active";
	case 0x2:
		return "partial";
	case 0x6:
		return "slumber";
	case 0x8:
		return "devsleep";
	default:
		return "rsvd";
	}
}

int ahci_pxcmd_str(uint32_t cmd, char *buf, int cap)
{
	int pos = 0;
	char tmp[24];

	if (!buf || cap <= 0)
		return 0;
	buf[0] = 0;
	for (const struct ahci_bitname *b = ahci_cmd_names; b->name; b++)
		if (cmd & b->bit)
			pos = ahci_strcat_word(buf, cap, pos, b->name);
	ksnprintf(tmp, sizeof(tmp), "CCS=%u",
		  (cmd & AHCI_PxCMD_CCS_MASK) >> AHCI_PxCMD_CCS_SHIFT);
	pos = ahci_strcat_word(buf, cap, pos, tmp);
	ksnprintf(tmp, sizeof(tmp), "ICC=%s",
		  ahci_icc_name((cmd & AHCI_PxCMD_ICC_MASK) >>
				AHCI_PxCMD_ICC_SHIFT));
	pos = ahci_strcat_word(buf, cap, pos, tmp);
	return pos;
}

/* ATA status / error register bits (local, so the dumps work from any
 * context without the ATA layer) */
static int ahci_tfd_status_str(uint8_t st, char *buf, int cap)
{
	static const char *const n[8] = { "ERR", "IDX", "CORR", "DRQ",
					  "DSC", "DF",	"DRDY", "BSY" };
	int pos = 0;

	buf[0] = 0;
	for (int b = 7; b >= 0; b--)
		if (st & (1u << b))
			pos = ahci_strcat_word(buf, cap, pos, n[b]);
	return pos;
}

static int ahci_tfd_error_str(uint8_t er, char *buf, int cap)
{
	static const char *const n[8] = { "AMNF", "NM",	 "ABRT", "MCR",
					  "IDNF", "MC",	 "UNC",	 "ICRC" };
	int pos = 0;

	buf[0] = 0;
	for (int b = 7; b >= 0; b--)
		if (er & (1u << b))
			pos = ahci_strcat_word(buf, cap, pos, n[b]);
	return pos;
}

static const char *ahci_det_name(uint32_t det)
{
	switch (det) {
	case AHCI_SSTS_DET_NONE:
		return "no device";
	case AHCI_SSTS_DET_PRESENT:
		return "device, no phy";
	case AHCI_SSTS_DET_PHY:
		return "phy up";
	case AHCI_SSTS_DET_OFFLINE:
		return "offline";
	default:
		return "rsvd";
	}
}

static const char *ahci_ipm_name(uint32_t ipm)
{
	switch (ipm) {
	case AHCI_SSTS_IPM_NONE:
		return "none";
	case AHCI_SSTS_IPM_ACTIVE:
		return "active";
	case AHCI_SSTS_IPM_PARTIAL:
		return "partial";
	case AHCI_SSTS_IPM_SLUMBER:
		return "slumber";
	case AHCI_SSTS_IPM_DEVSLEEP:
		return "devsleep";
	default:
		return "rsvd";
	}
}

/* The commands a trace or a dump shows most often */
static const char *ahci_opcode_name(uint8_t op)
{
	switch (op) {
	case 0x00: return "NOP";
	case 0x06: return "DATA SET MANAGEMENT";
	case 0x08: return "DEVICE RESET";
	case 0x0B: return "REQUEST SENSE DATA EXT";
	case 0x20: return "READ SECTOR(S)";
	case 0x24: return "READ SECTOR(S) EXT";
	case 0x25: return "READ DMA EXT";
	case 0x27: return "READ NATIVE MAX ADDRESS EXT";
	case 0x29: return "READ MULTIPLE EXT";
	case 0x2F: return "READ LOG EXT";
	case 0x30: return "WRITE SECTOR(S)";
	case 0x34: return "WRITE SECTOR(S) EXT";
	case 0x35: return "WRITE DMA EXT";
	case 0x37: return "SET MAX ADDRESS EXT";
	case 0x39: return "WRITE MULTIPLE EXT";
	case 0x3D: return "WRITE DMA FUA EXT";
	case 0x3F: return "WRITE LOG EXT";
	case 0x40: return "READ VERIFY SECTOR(S)";
	case 0x42: return "READ VERIFY SECTOR(S) EXT";
	case 0x44: return "ZERO EXT";
	case 0x47: return "READ LOG DMA EXT";
	case 0x57: return "WRITE LOG DMA EXT";
	case 0x60: return "READ FPDMA QUEUED";
	case 0x61: return "WRITE FPDMA QUEUED";
	case 0x63: return "NCQ NON-DATA";
	case 0x64: return "SEND FPDMA QUEUED";
	case 0x65: return "RECEIVE FPDMA QUEUED";
	case 0x90: return "EXECUTE DEVICE DIAGNOSTIC";
	case 0x92: return "DOWNLOAD MICROCODE";
	case 0xA0: return "PACKET";
	case 0xA1: return "IDENTIFY PACKET DEVICE";
	case 0xB0: return "SMART";
	case 0xB4: return "SANITIZE DEVICE";
	case 0xC4: return "READ MULTIPLE";
	case 0xC5: return "WRITE MULTIPLE";
	case 0xC6: return "SET MULTIPLE MODE";
	case 0xC8: return "READ DMA";
	case 0xCA: return "WRITE DMA";
	case 0xE0: return "STANDBY IMMEDIATE";
	case 0xE1: return "IDLE IMMEDIATE";
	case 0xE2: return "STANDBY";
	case 0xE3: return "IDLE";
	case 0xE4: return "READ BUFFER / READ PORT MULTIPLIER";
	case 0xE5: return "CHECK POWER MODE";
	case 0xE6: return "SLEEP";
	case 0xE7: return "FLUSH CACHE";
	case 0xE8: return "WRITE BUFFER / WRITE PORT MULTIPLIER";
	case 0xEA: return "FLUSH CACHE EXT";
	case 0xEC: return "IDENTIFY DEVICE";
	case 0xEF: return "SET FEATURES";
	case 0xF1: return "SECURITY SET PASSWORD";
	case 0xF2: return "SECURITY UNLOCK";
	case 0xF3: return "SECURITY ERASE PREPARE";
	case 0xF4: return "SECURITY ERASE UNIT";
	case 0xF5: return "SECURITY FREEZE LOCK";
	case 0xF6: return "SECURITY DISABLE PASSWORD";
	case 0xF8: return "READ NATIVE MAX ADDRESS";
	case 0xF9: return "SET MAX ADDRESS";
	default: return "?";
	}
}

static const char *ahci_port_state_name(enum ahci_port_state s)
{
	switch (s) {
	case AHCI_PORT_UNUSED:
		return "unused";
	case AHCI_PORT_IDLE:
		return "idle";
	case AHCI_PORT_RUNNING:
		return "running";
	case AHCI_PORT_FROZEN:
		return "frozen";
	case AHCI_PORT_EH:
		return "eh";
	case AHCI_PORT_DISABLED:
		return "disabled";
	case AHCI_PORT_SUSPENDED:
		return "suspended";
	default:
		return "?";
	}
}

static const char *ahci_cmd_state_name(enum ahci_cmd_state s)
{
	switch (s) {
	case AHCI_CMD_FREE:
		return "free";
	case AHCI_CMD_ALLOCATED:
		return "allocated";
	case AHCI_CMD_ISSUED:
		return "issued";
	case AHCI_CMD_COMPLETING:
		return "completing";
	case AHCI_CMD_TIMEDOUT:
		return "timed-out";
	case AHCI_CMD_EH:
		return "eh";
	default:
		return "?";
	}
}

static const char *ahci_irq_mode_name(enum ahci_irq_mode m)
{
	switch (m) {
	case AHCI_IRQ_POLL:
		return "polling";
	case AHCI_IRQ_INTX:
		return "INTx";
	case AHCI_IRQ_MSI:
		return "MSI";
	case AHCI_IRQ_MSIX:
		return "MSI-X";
	default:
		return "?";
	}
}

static const char *ahci_hba_state_str(enum ahci_hba_state s)
{
	switch (s) {
	case AHCI_HBA_PROBING:
		return "probing";
	case AHCI_HBA_POLLED:
		return "polled";
	case AHCI_HBA_RUNNING:
		return "running";
	case AHCI_HBA_RESETTING:
		return "resetting";
	case AHCI_HBA_SUSPENDED:
		return "suspended";
	case AHCI_HBA_DEAD:
		return "dead";
	default:
		return "?";
	}
}

static const struct ahci_bitname ahci_eh_names[] = {
	{ AHCI_EH_TIMEOUT, "timeout" },
	{ AHCI_EH_DEV_ERROR, "dev-error" },
	{ AHCI_EH_NCQ_ERROR, "ncq-error" },
	{ AHCI_EH_HOST_ERROR, "host-error" },
	{ AHCI_EH_IF_FATAL, "if-fatal" },
	{ AHCI_EH_IF_NONFATAL, "if-nonfatal" },
	{ AHCI_EH_OVERFLOW, "overflow" },
	{ AHCI_EH_UNKNOWN_FIS, "unknown-fis" },
	{ AHCI_EH_IPMS, "ipms" },
	{ AHCI_EH_HOTPLUG, "hotplug" },
	{ AHCI_EH_SNTF, "sntf" },
	{ AHCI_EH_RESET_SOFT, "softreset" },
	{ AHCI_EH_RESET_HARD, "hardreset" },
	{ AHCI_EH_REVALIDATE, "revalidate" },
	{ AHCI_EH_LPM, "lpm" },
	{ AHCI_EH_HBA_RESET, "hba-reset" },
	{ AHCI_EH_DETACH, "detach" },
	{ AHCI_EH_SUSPEND, "suspend" },
	{ AHCI_EH_RESUME, "resume" },
	{ AHCI_EH_FAULT, "fault" },
	{ AHCI_EH_REISSUE, "reissue" },
	{ AHCI_EH_PORT, "port" },
	{ 0, NULL },
};

static int ahci_eh_str(uint32_t a, char *buf, int cap)
{
	int pos = 0;

	buf[0] = 0;
	for (const struct ahci_bitname *b = ahci_eh_names; b->name; b++)
		if (a & b->bit)
			pos = ahci_strcat_word(buf, cap, pos, b->name);
	if (!pos)
		pos = ahci_strcat_word(buf, cap, pos, "none");
	return pos;
}

/* ==========================================================================
 * Register dumps
 * ========================================================================== */

static void ahci_fmt_hba(const struct ahci_hba *hba, struct ahci_sbuf *sb)
{
	uint32_t cap, ghc, is, pi, vs, ccc, ccp, eml, emc, cap2, bohc;
	char d[160];

	if (!hba->mmio) {
		sbp(sb, "[ahci] %s: not mapped\n", hba->name);
		return;
	}
	cap = ahci_hba_readl(hba, AHCI_CAP);
	ghc = ahci_hba_readl(hba, AHCI_GHC);
	is = ahci_hba_readl(hba, AHCI_IS);
	pi = ahci_hba_readl(hba, AHCI_PI);
	vs = ahci_hba_readl(hba, AHCI_VS);
	ccc = ahci_hba_readl(hba, AHCI_CCC_CTL);
	ccp = ahci_hba_readl(hba, AHCI_CCC_PORTS);
	eml = ahci_hba_readl(hba, AHCI_EM_LOC);
	emc = ahci_hba_readl(hba, AHCI_EM_CTL);
	cap2 = vs >= AHCI_VS_1_2 ? ahci_hba_readl(hba, AHCI_CAP2) : 0;
	bohc = vs >= AHCI_VS_1_2 ? ahci_hba_readl(hba, AHCI_BOHC) : 0;

	sbp(sb, "[ahci] %s: register dump (ABAR 0x%llx, %s, irq %s vec %d, gen %u)\n",
	    hba->name, (unsigned long long)hba->mmio_phys,
	    ahci_hba_state_str(hba->state), ahci_irq_mode_name(hba->irq_mode),
	    hba->irq_vector, hba->generation);
	sbp(sb, "  CAP      0x%08x  NP=%u NCS=%u ISS=%u%s%s%s%s%s%s%s%s%s%s%s\n",
	    cap, (cap & AHCI_CAP_NP_MASK) + 1,
	    ((cap & AHCI_CAP_NCS_MASK) >> AHCI_CAP_NCS_SHIFT) + 1,
	    (cap & AHCI_CAP_ISS_MASK) >> AHCI_CAP_ISS_SHIFT,
	    (cap & AHCI_CAP_S64A) ? " S64A" : "",
	    (cap & AHCI_CAP_SNCQ) ? " SNCQ" : "",
	    (cap & AHCI_CAP_SSNTF) ? " SSNTF" : "",
	    (cap & AHCI_CAP_SSS) ? " SSS" : "",
	    (cap & AHCI_CAP_SALP) ? " SALP" : "",
	    (cap & AHCI_CAP_SCLO) ? " SCLO" : "",
	    (cap & AHCI_CAP_SAM) ? " SAM" : "",
	    (cap & AHCI_CAP_SPM) ? " SPM" : "",
	    (cap & AHCI_CAP_FBSS) ? " FBSS" : "",
	    (cap & AHCI_CAP_CCCS) ? " CCCS" : "",
	    (cap & AHCI_CAP_EMS) ? " EMS" : "");
	sbp(sb, "  GHC      0x%08x %s%s%s%s\n", ghc,
	    (ghc & AHCI_GHC_AE) ? " AE" : "", (ghc & AHCI_GHC_IE) ? " IE" : "",
	    (ghc & AHCI_GHC_HR) ? " HR" : "",
	    (ghc & AHCI_GHC_MRSM) ? " MRSM" : "");
	sbp(sb, "  IS       0x%08x\n", is);
	sbp(sb, "  PI       0x%08x  (in use 0x%08x, firmware 0x%08x)\n", pi,
	    hba->port_map, hba->saved_pi);
	sbp(sb, "  VS       0x%08x\n", vs);
	sbp(sb, "  CCC_CTL  0x%08x  %s TV=%u CC=%u INT=%u; CCC_PORTS 0x%08x\n",
	    ccc, (ccc & AHCI_CCC_EN) ? "enabled" : "disabled",
	    (ccc & AHCI_CCC_TV_MASK) >> AHCI_CCC_TV_SHIFT,
	    (ccc & AHCI_CCC_CC_MASK) >> AHCI_CCC_CC_SHIFT,
	    (ccc & AHCI_CCC_INT_MASK) >> AHCI_CCC_INT_SHIFT, ccp);
	sbp(sb, "  EM_LOC   0x%08x  EM_CTL 0x%08x\n", eml, emc);
	sbp(sb, "  CAP2     0x%08x %s%s%s%s%s%s\n", cap2,
	    (cap2 & AHCI_CAP2_BOH) ? " BOH" : "",
	    (cap2 & AHCI_CAP2_NVMP) ? " NVMP" : "",
	    (cap2 & AHCI_CAP2_APST) ? " APST" : "",
	    (cap2 & AHCI_CAP2_SDS) ? " SDS" : "",
	    (cap2 & AHCI_CAP2_SADM) ? " SADM" : "",
	    (cap2 & AHCI_CAP2_DESO) ? " DESO" : "");
	sbp(sb, "  BOHC     0x%08x %s%s%s%s%s\n", bohc,
	    (bohc & AHCI_BOHC_BOS) ? " BOS" : "",
	    (bohc & AHCI_BOHC_OOS) ? " OOS" : "",
	    (bohc & AHCI_BOHC_SOOE) ? " SOOE" : "",
	    (bohc & AHCI_BOHC_OOC) ? " OOC" : "",
	    (bohc & AHCI_BOHC_BB) ? " BB" : "");
	sbp(sb, "  quirks   0x%08x  dma%s  stats: irqs %llu spurious %llu polls %llu hba-resets %u host-errors %u kahci-wakeups %u\n",
	    hba->quirks, hba->dma64 ? "64" : "32",
	    (unsigned long long)hba->stats.irqs,
	    (unsigned long long)hba->stats.spurious_irqs,
	    (unsigned long long)hba->stats.polls, hba->stats.hba_resets,
	    hba->stats.host_errors, hba->stats.kahci_wakeups);
	for (int i = 0; i < AHCI_MAX_PORTS; i++) {
		const struct ahci_port *p = hba->ports[i];
		uint32_t ssts;

		if (!p)
			continue;
		ssts = ahci_port_readl(p, AHCI_PxSSTS);
		ahci_irq_stat_str(ahci_port_readl(p, AHCI_PxIS), d, sizeof(d));
		sbp(sb, "  port %2d %-6s %-9s SSTS 0x%03x CI 0x%08x SACT 0x%08x IS { %s }\n",
		    i, p->name, ahci_port_state_name(p->state), ssts,
		    ahci_port_readl(p, AHCI_PxCI), ahci_port_readl(p, AHCI_PxSACT),
		    d);
	}
}

void ahci_dump_hba(const struct ahci_hba *hba)
{
	if (!hba)
		return;
	ahci_fmt_hba(hba, NULL);
}

static void ahci_fmt_port(const struct ahci_port *p, struct ahci_sbuf *sb)
{
	uint32_t clb, clbu, fb, fbu, is, ie, cmd, tfd, sig, ssts, sctl, serr;
	uint32_t sact, ci, sntf, fbs, devslp;
	char d[256];
	struct ahci_link_status ls;

	if (!p->mmio) {
		sbp(sb, "[ahci] %s: not mapped\n", p->name);
		return;
	}
	clb = ahci_port_readl(p, AHCI_PxCLB);
	clbu = ahci_port_readl(p, AHCI_PxCLBU);
	fb = ahci_port_readl(p, AHCI_PxFB);
	fbu = ahci_port_readl(p, AHCI_PxFBU);
	is = ahci_port_readl(p, AHCI_PxIS);
	ie = ahci_port_readl(p, AHCI_PxIE);
	cmd = ahci_port_readl(p, AHCI_PxCMD);
	tfd = ahci_port_readl(p, AHCI_PxTFD);
	sig = ahci_port_readl(p, AHCI_PxSIG);
	ssts = ahci_port_readl(p, AHCI_PxSSTS);
	sctl = ahci_port_readl(p, AHCI_PxSCTL);
	serr = ahci_port_readl(p, AHCI_PxSERR);
	sact = ahci_port_readl(p, AHCI_PxSACT);
	ci = ahci_port_readl(p, AHCI_PxCI);
	sntf = ahci_port_readl(p, AHCI_PxSNTF);
	fbs = ahci_port_readl(p, AHCI_PxFBS);
	devslp = ahci_port_readl(p, AHCI_PxDEVSLP);

	sbp(sb, "[ahci] %s: port %d of %s, state %s, flags 0x%x, %s\n", p->name,
	    p->index, p->hba ? p->hba->name : "?",
	    ahci_port_state_name(p->state), p->flags,
	    p->polled ? "polled" : "interrupts");
	sbp(sb, "  PxCLB    0x%08x%08x (driver 0x%llx)\n", clbu, clb,
	    (unsigned long long)p->cmd_list_area.bus);
	sbp(sb, "  PxFB     0x%08x%08x (driver 0x%llx, %lu bytes)\n", fbu, fb,
	    (unsigned long long)p->rx_fis_area.bus,
	    (unsigned long)p->rx_fis_area.size);
	ahci_irq_stat_str(is, d, sizeof(d));
	sbp(sb, "  PxIS     0x%08x { %s }\n", is, d);
	ahci_irq_stat_str(ie, d, sizeof(d));
	sbp(sb, "  PxIE     0x%08x { %s } (driver 0x%08x)\n", ie, d, p->irq_mask);
	ahci_pxcmd_str(cmd, d, sizeof(d));
	sbp(sb, "  PxCMD    0x%08x { %s }\n", cmd, d);
	{
		char s1[64], s2[64];

		ahci_tfd_status_str((uint8_t)(tfd & AHCI_PxTFD_STS_MASK), s1,
				    sizeof(s1));
		ahci_tfd_error_str((uint8_t)((tfd & AHCI_PxTFD_ERR_MASK) >>
					     AHCI_PxTFD_ERR_SHIFT),
				   s2, sizeof(s2));
		sbp(sb, "  PxTFD    0x%08x status { %s } error { %s }\n", tfd,
		    s1, s2);
	}
	sbp(sb, "  PxSIG    0x%08x (%s)\n", sig,
	    ata_class_name(ata_class_from_sig(sig)));
	ahci_decode_ssts(ssts, &ls);
	sbp(sb, "  PxSSTS   0x%08x DET=%u (%s) SPD=%u (%s) IPM=%u (%s)\n", ssts,
	    ls.det, ahci_det_name(ls.det), ls.spd, ahci_speed_str(ls.spd),
	    ls.ipm, ahci_ipm_name(ls.ipm));
	sbp(sb, "  PxSCTL   0x%08x DET=%u SPD-limit=%u IPM-off=0x%x\n", sctl,
	    sctl & AHCI_SCTL_DET_MASK,
	    (sctl & AHCI_SCTL_SPD_MASK) >> AHCI_SCTL_SPD_SHIFT,
	    (sctl & AHCI_SCTL_IPM_MASK) >> AHCI_SCTL_IPM_SHIFT);
	ahci_serror_str(serr, d, sizeof(d));
	sbp(sb, "  PxSERR   0x%08x %s\n", serr, d);
	sbp(sb, "  PxSACT   0x%08x  PxCI 0x%08x  PxSNTF 0x%08x\n", sact, ci, sntf);
	sbp(sb, "  PxFBS    0x%08x %s DEV=%u ADO=%u DWE=%u%s  PxDEVSLP 0x%08x\n",
	    fbs, (fbs & AHCI_PxFBS_EN) ? "EN" : "off",
	    (fbs & AHCI_PxFBS_DEV_MASK) >> AHCI_PxFBS_DEV_SHIFT,
	    (fbs & AHCI_PxFBS_ADO_MASK) >> AHCI_PxFBS_ADO_SHIFT,
	    (fbs & AHCI_PxFBS_DWE_MASK) >> AHCI_PxFBS_DWE_SHIFT,
	    (fbs & AHCI_PxFBS_SDE) ? " SDE" : "", devslp);
	sbp(sb, "  driver: slots %u (internal %u) map 0x%08x active 0x%08x ncq 0x%08x depth %u zombies 0x%08x\n",
	    p->nr_slots, p->internal_slot, p->slot_map, p->active,
	    p->ncq_active, p->ncq_depth, p->zombie_slots);
	ahci_eh_str(p->eh_pending, d, sizeof(d));
	sbp(sb, "  eh: pending { %s } level %d consecutive %u seq %u latched IS 0x%08x SERR 0x%08x TFD 0x%08x failed 0x%08x\n",
	    d, (int)p->eh_level, p->eh_consecutive, p->eh_seq,
	    p->eh_irq_stat, p->eh_serror, p->eh_tfd, p->eh_failed_slots);
	sbp(sb, "  links: host %s%s, pmp %s (%u links), fbs %s/%s\n",
	    p->host_link.dev ? p->host_link.dev->name : "-",
	    p->host_link.state == AHCI_LINK_ONLINE ? " online" : "",
	    p->pmp_attached ? "yes" : "no", p->nr_pmp_links,
	    p->fbs_capable ? "capable" : "no", p->fbs_enabled ? "on" : "off");
}

void ahci_dump_port(const struct ahci_port *port)
{
	if (!port)
		return;
	ahci_fmt_port(port, NULL);
}

static void ahci_fmt_cmd(const struct ahci_port *port, uint32_t slot,
			 struct ahci_sbuf *sb)
{
	const struct ahci_cmd *cmd;
	const struct ahci_cmd_header *h;
	const struct ata_queued_cmd *qc;
	uint32_t opts, n;

	if (slot >= AHCI_MAX_SLOTS) {
		sbp(sb, "[ahci] %s: no slot %u\n", port->name, slot);
		return;
	}
	cmd = &port->cmds[slot];
	h = cmd->hdr;
	qc = cmd->qc;
	sbp(sb, "[ahci] %s: slot %u %s%s%s gen %u, %u PRD, %u bytes, timeout %u ms\n",
	    port->name, slot, ahci_cmd_state_name(cmd->state),
	    cmd->is_ncq ? " ncq" : "", cmd->internal ? " internal" : "",
	    cmd->generation, cmd->n_prd, cmd->nbytes, cmd->timeout_ms);
	if (qc)
		sbp(sb, "  qc: %s (0x%02x) lba %llu count %u flags 0x%x prot %d tag %d retries %u\n",
		    ahci_opcode_name(qc->tf.command), qc->tf.command,
		    (unsigned long long)qc->lba, qc->n_sect, qc->flags,
		    (int)qc->protocol, qc->tag, qc->retries);
	if (!h) {
		sbp(sb, "  (no command header)\n");
		return;
	}
	opts = h->opts;
	sbp(sb, "  header: opts 0x%08x PRDTL=%u PMP=%u CFL=%u%s%s%s%s%s%s PRDBC=%u CTBA 0x%08x%08x\n",
	    opts, opts >> AHCI_CMDH_PRDTL_SHIFT,
	    (opts & AHCI_CMDH_PMP_MASK) >> AHCI_CMDH_PMP_SHIFT,
	    opts & AHCI_CMDH_CFL_MASK, (opts & AHCI_CMDH_C) ? " C" : "",
	    (opts & AHCI_CMDH_B) ? " B" : "", (opts & AHCI_CMDH_R) ? " R" : "",
	    (opts & AHCI_CMDH_P) ? " P" : "", (opts & AHCI_CMDH_W) ? " W" : "",
	    (opts & AHCI_CMDH_A) ? " A" : "", h->prdbc, h->ctbau, h->ctba);
	if (!cmd->table)
		return;
	sbp(sb, "  CFIS:");
	for (int i = 0; i < 20; i++)
		sbp(sb, " %02x", cmd->table->cfis[i]);
	sbp(sb, "\n");
	if (opts & AHCI_CMDH_A) {
		sbp(sb, "  ACMD:");
		for (int i = 0; i < AHCI_CT_ACMD_LEN; i++)
			sbp(sb, " %02x", cmd->table->acmd[i]);
		sbp(sb, "\n");
	}
	n = opts >> AHCI_CMDH_PRDTL_SHIFT;
	if (n > AHCI_MAX_PRD)
		n = AHCI_MAX_PRD;
	for (uint32_t i = 0; i < n && i < 16; i++) {
		const struct ahci_prdt_entry *e = &cmd->table->prdt[i];

		sbp(sb, "  PRD %3u: 0x%08x%08x len %u%s\n", i, e->dbau, e->dba,
		    (e->dbc & AHCI_PRD_DBC_MASK) + 1,
		    (e->dbc & AHCI_PRD_I) ? " I" : "");
	}
	if (n > 16)
		sbp(sb, "  ... %u more PRD entries\n", n - 16);
}

void ahci_dump_cmd(const struct ahci_port *port, uint32_t slot)
{
	if (!port)
		return;
	ahci_fmt_cmd(port, slot, NULL);
}

/* ==========================================================================
 * Command trace ring
 * ========================================================================== */

struct ahci_trace_ring *ahci_trace_alloc(void)
{
	struct ahci_trace_ring *r = kcalloc(1, sizeof(*r));

	if (!r)
		return NULL;
	spinlock_init(&r->lock, "ahci_trace");
	r->head = 0;
	r->total = 0;
	return r;
}

void ahci_trace(struct ahci_port *port, uint8_t event,
		const struct ahci_cmd *cmd, int result, uint32_t aux)
{
	struct ahci_trace_ring *r;
	struct ahci_trace_entry *e;
	const struct ata_queued_cmd *qc;
	uint64_t fl, now;

	if (unlikely(!port))
		return;
	r = port->trace;
	if (!r)
		return;
	now = hrtimer_now_ns();
	qc = cmd ? cmd->qc : NULL;

	spin_lock_irqsave(&r->lock, &fl);
	e = &r->e[r->head % AHCI_TRACE_ENTRIES];
	r->head = (r->head + 1) % AHCI_TRACE_ENTRIES;
	r->total++;
	e->ts_ns = now;
	e->event = event;
	e->aux = aux;
	e->result = (int16_t)(result < -32768 ? -32768 : result);
	e->slot = cmd ? (uint8_t)cmd->slot : 0xFF;
	e->pmp = cmd ? cmd->pmp : 0;
	if (qc) {
		e->lba = qc->lba;
		e->count = qc->n_sect;
		e->command = qc->tf.command;
		e->status = qc->result_tf.status;
		e->error = qc->result_tf.error;
	} else {
		e->lba = 0;
		e->count = 0;
		e->command = 0;
		e->status = 0;
		e->error = 0;
	}
	if (cmd && cmd->start_ns && now > cmd->start_ns &&
	    (event == AHCI_TRACE_COMPLETE || event == AHCI_TRACE_ERROR ||
	     event == AHCI_TRACE_TIMEOUT || event == AHCI_TRACE_ABORT)) {
		uint64_t us = (now - cmd->start_ns) / 1000ULL;

		e->latency_us = us > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)us;
	} else {
		e->latency_us = 0;
	}
	spin_unlock_irqrestore(&r->lock, fl);
}

static const char *ahci_trace_event_name(uint8_t ev)
{
	switch (ev) {
	case AHCI_TRACE_ISSUE:
		return "issue";
	case AHCI_TRACE_COMPLETE:
		return "done";
	case AHCI_TRACE_ERROR:
		return "ERROR";
	case AHCI_TRACE_TIMEOUT:
		return "TIMEOUT";
	case AHCI_TRACE_ABORT:
		return "abort";
	case AHCI_TRACE_RESET:
		return "reset";
	case AHCI_TRACE_IRQ:
		return "irq";
	case AHCI_TRACE_EH:
		return "eh";
	default:
		return "?";
	}
}

static void ahci_trace_fmt_entry(const struct ahci_trace_entry *e,
				 struct ahci_sbuf *sb)
{
	uint64_t s = e->ts_ns / 1000000000ULL;
	uint32_t us = (uint32_t)((e->ts_ns / 1000ULL) % 1000000ULL);

	switch (e->event) {
	case AHCI_TRACE_IRQ: {
		char d[160];

		ahci_irq_stat_str(e->aux, d, sizeof(d));
		sbp(sb, "%5llu.%06u irq     PxIS 0x%08x { %s }\n",
		    (unsigned long long)s, us, e->aux, d);
		break;
	}
	case AHCI_TRACE_EH: {
		char d[200];

		ahci_eh_str(e->aux, d, sizeof(d));
		sbp(sb, "%5llu.%06u eh      { %s } result %d\n",
		    (unsigned long long)s, us, d, e->result);
		break;
	}
	case AHCI_TRACE_RESET:
		sbp(sb, "%5llu.%06u reset   level %u result %d\n",
		    (unsigned long long)s, us, e->aux, e->result);
		break;
	default:
		if (e->slot == 0xFF)
			sbp(sb, "%5llu.%06u %-7s aux 0x%08x result %d\n",
			    (unsigned long long)s, us,
			    ahci_trace_event_name(e->event), e->aux, e->result);
		else
			sbp(sb, "%5llu.%06u %-7s slot %2u pmp %u %-22s lba %llu n %u lat %u us st 0x%02x err 0x%02x res %d aux 0x%x\n",
			    (unsigned long long)s, us,
			    ahci_trace_event_name(e->event), e->slot, e->pmp,
			    ahci_opcode_name(e->command),
			    (unsigned long long)e->lba, e->count, e->latency_us,
			    e->status, e->error, e->result, e->aux);
		break;
	}
}

long ahci_trace_format(const struct ahci_port *port, char *buf, long cap)
{
	struct ahci_sbuf sb = { buf, cap, 0 };
	struct ahci_trace_ring *r;
	struct ahci_trace_entry *snap;
	uint64_t fl, total;
	uint32_t head, n;

	if (!port)
		return 0;
	r = port->trace;
	if (!r) {
		sbp(&sb, "(no trace ring)\n");
		return sb_result(&sb);
	}
	/* Snapshot under the lock, format outside it. */
	snap = kalloc(sizeof(r->e));
	spin_lock_irqsave(&r->lock, &fl);
	head = r->head;
	total = r->total;
	if (snap)
		kmemcpy(snap, r->e, sizeof(r->e));
	spin_unlock_irqrestore(&r->lock, fl);
	if (!snap) {
		sbp(&sb, "(out of memory)\n");
		return sb_result(&sb);
	}
	n = total < AHCI_TRACE_ENTRIES ? (uint32_t)total : AHCI_TRACE_ENTRIES;
	sbp(&sb, "# %s: %llu events, last %u (time event slot pmp command lba count latency status error result aux)\n",
	    port->name, (unsigned long long)total, n);
	for (uint32_t i = 0; i < n; i++) {
		uint32_t idx = (head + AHCI_TRACE_ENTRIES - n + i) %
			       AHCI_TRACE_ENTRIES;

		ahci_trace_fmt_entry(&snap[idx], &sb);
	}
	kfree(snap);
	return sb_result(&sb);
}

/* ==========================================================================
 * Fault injection
 * ========================================================================== */

int ahci_fault_check_locked(struct ahci_port *port, const struct ahci_cmd *cmd)
{
	struct ahci_fault_inject *f;
	const struct ata_queued_cmd *qc;
	int fire = 0;

	BUG_ON(port == NULL);
	lockdep_assert_held(&port->lock);
	f = &port->fault;
	if (likely(f->kind == AHCI_FAULT_NONE ||
		   (f->remaining == 0 && f->every_n == 0)))
		return AHCI_FAULT_NONE;
	if (!cmd)
		return AHCI_FAULT_NONE;
	qc = cmd->qc;
	/* Error handling must be able to recover: its commands never fail. */
	if (cmd->internal || (qc && (qc->flags & ATA_QCFLAG_EH)))
		return AHCI_FAULT_NONE;
	if (f->only_ncq && !cmd->is_ncq)
		return AHCI_FAULT_NONE;
	if (f->only_write && !(qc && (qc->flags & ATA_QCFLAG_WRITE)))
		return AHCI_FAULT_NONE;

	f->counter++;
	if (f->remaining) {
		f->remaining--;
		fire = 1;
	} else if (f->every_n && (f->counter % f->every_n) == 0) {
		fire = 1;
	}
	if (!fire)
		return AHCI_FAULT_NONE;
	f->injected++;
	kprintf("[ahci] %s: fault injection #%u: kind %u on slot %u (%s)\n",
		port->name, f->injected, f->kind, cmd->slot,
		qc ? ahci_opcode_name(qc->tf.command) : "-");
	if (port->trace)
		ahci_trace(port, AHCI_TRACE_ERROR, cmd, -EIO,
			   0xFA000000u | f->kind);
	return (int)f->kind;
}

/* ==========================================================================
 * Small parsers for sysfs stores
 * ========================================================================== */

/* Copy a store buffer into a NUL-terminated string without trailing
 * whitespace. */
static void ahci_store_str(const char *buf, long len, char *out, int cap)
{
	int n = 0;

	while (len > 0 && (*buf == ' ' || *buf == '\t')) {
		buf++;
		len--;
	}
	while (n < len && n < cap - 1) {
		out[n] = buf[n];
		n++;
	}
	out[n] = 0;
	while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == ' ' ||
			 out[n - 1] == '\t' || out[n - 1] == '\r'))
		out[--n] = 0;
}

/* Number in decimal or 0x hex; returns 0 and *v, or -EINVAL. */
static int ahci_parse_u32(const char *s, uint32_t *v)
{
	uint64_t r = 0;
	int base = 10, digits = 0;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		base = 16;
		s += 2;
	}
	for (; *s; s++) {
		int d;

		if (*s >= '0' && *s <= '9')
			d = *s - '0';
		else if (base == 16 && *s >= 'a' && *s <= 'f')
			d = *s - 'a' + 10;
		else if (base == 16 && *s >= 'A' && *s <= 'F')
			d = *s - 'A' + 10;
		else
			return -EINVAL;
		r = r * (uint64_t)base + (uint64_t)d;
		if (r > 0xFFFFFFFFULL)
			return -EINVAL;
		digits++;
	}
	if (!digits)
		return -EINVAL;
	*v = (uint32_t)r;
	return 0;
}

/* Split off the next whitespace-separated token. */
static char *ahci_next_tok(char **p)
{
	char *s = *p, *t;

	while (*s == ' ' || *s == '\t')
		s++;
	if (!*s) {
		*p = s;
		return NULL;
	}
	t = s;
	while (*s && *s != ' ' && *s != '\t')
		s++;
	if (*s)
		*s++ = 0;
	*p = s;
	return t;
}

static int ahci_streq(const char *a, const char *b)
{
	return kstrcmp(a, b) == 0;
}

static int ahci_starts(const char *s, const char *prefix)
{
	return kstrncmp(s, prefix, kstrlen(prefix)) == 0;
}

/* ==========================================================================
 * sysfs: ports
 * ========================================================================== */

static long ahci_show_port_no(struct pfs_node *n, char *buf, long cap)
{
	const struct ahci_port *p = n->arg;
	struct ahci_sbuf sb = { buf, cap, 0 };

	sbp(&sb, "%d\n", p->port_no);
	return sb_result(&sb);
}

static long ahci_show_idle_irq(struct pfs_node *n, char *buf, long cap)
{
	const struct ahci_port *p = n->arg;
	struct ahci_sbuf sb = { buf, cap, 0 };

	sbp(&sb, "%u\n", p->stats.spurious);
	return sb_result(&sb);
}

static long ahci_show_nr_pmp_links(struct pfs_node *n, char *buf, long cap)
{
	const struct ahci_port *p = n->arg;
	struct ahci_sbuf sb = { buf, cap, 0 };

	sbp(&sb, "%u\n", p->pmp_attached ? p->nr_pmp_links : 0);
	return sb_result(&sb);
}

static long ahci_show_port_stats(struct pfs_node *n, char *buf, long cap)
{
	const struct ahci_port *p = n->arg;
	const struct ahci_port_stats *s = &p->stats;
	struct ahci_sbuf sb = { buf, cap, 0 };

	sbp(&sb, "irqs %llu\n", (unsigned long long)s->irqs);
	sbp(&sb, "cmds_issued %llu\n", (unsigned long long)s->cmds_issued);
	sbp(&sb, "cmds_completed %llu\n",
	    (unsigned long long)s->cmds_completed);
	sbp(&sb, "ncq_issued %llu\n", (unsigned long long)s->ncq_issued);
	sbp(&sb, "bytes_read %llu\n", (unsigned long long)s->bytes_read);
	sbp(&sb, "bytes_written %llu\n", (unsigned long long)s->bytes_written);
	sbp(&sb, "max_ncq_depth_seen %u\n", s->max_ncq_depth_seen);
	sbp(&sb, "errors %u\n", s->errors);
	sbp(&sb, "tfes %u\nhbfs %u\nhbds %u\nifs %u\ninfs %u\nofs %u\nipms %u\nufs %u\n",
	    s->tfes, s->hbfs, s->hbds, s->ifs, s->infs, s->ofs, s->ipms,
	    s->ufs);
	sbp(&sb, "timeouts %u\nncq_errors %u\nretries %u\naborts %u\nclo %u\n",
	    s->timeouts, s->ncq_errors, s->retries, s->aborts, s->clo);
	sbp(&sb, "srst %u\ncomreset %u\nhba_resets %u\neh_runs %u\n", s->srst,
	    s->comreset, s->hba_resets, s->eh_runs);
	sbp(&sb, "hotplug_add %u\nhotplug_remove %u\n", s->hotplug_add,
	    s->hotplug_remove);
	sbp(&sb, "serr_crc %u\nserr_disparity %u\nserr_decode %u\nserr_handshake %u\n",
	    s->serr_crc, s->serr_disparity, s->serr_decode, s->serr_handshake);
	sbp(&sb, "serr_phyrdy %u\nserr_comwake %u\nserr_internal %u\nserr_protocol %u\n",
	    s->serr_phyrdy, s->serr_comwake, s->serr_internal,
	    s->serr_protocol);
	sbp(&sb, "bounces %u\ndeferred %u\nspurious %u\n", s->bounces,
	    s->deferred, s->spurious);
	return sb_result(&sb);
}

static long ahci_show_port_regs(struct pfs_node *n, char *buf, long cap)
{
	const struct ahci_port *p = n->arg;
	struct ahci_sbuf sb = { buf, cap, 0 };

	ahci_fmt_port(p, &sb);
	for (uint32_t s = 0; s < p->nr_slots && s < AHCI_MAX_SLOTS; s++)
		if (p->cmds[s].state != AHCI_CMD_FREE)
			ahci_fmt_cmd(p, s, &sb);
	return sb_result(&sb);
}

static long ahci_show_port_trace(struct pfs_node *n, char *buf, long cap)
{
	return ahci_trace_format(n->arg, buf, cap);
}

static long ahci_show_port_state(struct pfs_node *n, char *buf, long cap)
{
	const struct ahci_port *p = n->arg;
	struct ahci_sbuf sb = { buf, cap, 0 };

	sbp(&sb, "%s\n", ahci_port_state_name(p->state));
	return sb_result(&sb);
}

static const char *ahci_fault_kind_name(uint32_t k)
{
	switch (k) {
	case AHCI_FAULT_NONE:
		return "off";
	case AHCI_FAULT_TIMEOUT:
		return "timeout";
	case AHCI_FAULT_DEV_ERROR:
		return "dev_error";
	case AHCI_FAULT_HOST_ERROR:
		return "host_error";
	case AHCI_FAULT_LINK_DOWN:
		return "link_down";
	default:
		return "?";
	}
}

static long ahci_show_fault(struct pfs_node *n, char *buf, long cap)
{
	const struct ahci_port *p = n->arg;
	const struct ahci_fault_inject *f = &p->fault;
	struct ahci_sbuf sb = { buf, cap, 0 };

	sbp(&sb, "%s remaining=%u every=%u err=0x%02x%s%s seen=%u injected=%u\n",
	    ahci_fault_kind_name(f->kind), f->remaining, f->every_n, f->error,
	    f->only_ncq ? " ncq" : "", f->only_write ? " write" : "",
	    f->counter, f->injected);
	sbp(&sb, "# write: off | <timeout|dev_error|host_error|link_down> [count] [every=N] [err=0xNN] [ncq] [write]\n");
	return sb_result(&sb);
}

static long ahci_store_fault(struct pfs_node *n, const char *buf, long len)
{
	struct ahci_port *p = n->arg;
	struct ahci_fault_inject nf;
	char s[128], *cur = s, *tok;
	uint32_t v;
	uint64_t fl;

	ahci_store_str(buf, len, s, sizeof(s));
	kmemset(&nf, 0, sizeof(nf));
	nf.error = ATA_ABORTED;

	tok = ahci_next_tok(&cur);
	if (!tok)
		return -EINVAL;
	if (ahci_streq(tok, "off") || ahci_streq(tok, "none") ||
	    ahci_streq(tok, "0"))
		nf.kind = AHCI_FAULT_NONE;
	else if (ahci_streq(tok, "timeout"))
		nf.kind = AHCI_FAULT_TIMEOUT;
	else if (ahci_streq(tok, "dev_error"))
		nf.kind = AHCI_FAULT_DEV_ERROR;
	else if (ahci_streq(tok, "host_error"))
		nf.kind = AHCI_FAULT_HOST_ERROR;
	else if (ahci_streq(tok, "link_down"))
		nf.kind = AHCI_FAULT_LINK_DOWN;
	else
		return -EINVAL;
	if (nf.kind != AHCI_FAULT_NONE)
		nf.remaining = 1;

	while ((tok = ahci_next_tok(&cur)) != NULL) {
		if (ahci_starts(tok, "every=")) {
			if (ahci_parse_u32(tok + 6, &v))
				return -EINVAL;
			nf.every_n = v;
			nf.remaining = 0;
		} else if (ahci_starts(tok, "err=")) {
			if (ahci_parse_u32(tok + 4, &v) || v > 0xFF)
				return -EINVAL;
			nf.error = (uint8_t)v;
		} else if (ahci_streq(tok, "ncq")) {
			nf.only_ncq = 1;
		} else if (ahci_streq(tok, "write")) {
			nf.only_write = 1;
		} else if (ahci_parse_u32(tok, &v) == 0) {
			nf.remaining = v;
		} else {
			return -EINVAL;
		}
	}

	spin_lock_irqsave(&p->lock, &fl);
	p->fault.kind = nf.kind;
	p->fault.error = nf.error;
	p->fault.only_ncq = nf.only_ncq;
	p->fault.only_write = nf.only_write;
	p->fault.every_n = nf.every_n;
	p->fault.remaining = nf.remaining;
	p->fault.counter = 0;
	spin_unlock_irqrestore(&p->lock, fl);
	kprintf("[ahci] %s: fault injection: %s remaining %u every %u err 0x%02x%s%s\n",
		p->name, ahci_fault_kind_name(nf.kind), nf.remaining,
		nf.every_n, nf.error, nf.only_ncq ? " ncq" : "",
		nf.only_write ? " write" : "");
	return len;
}

static long ahci_show_lpm(struct pfs_node *n, char *buf, long cap)
{
	const struct ahci_port *p = n->arg;
	struct ahci_sbuf sb = { buf, cap, 0 };

	sbp(&sb, "%s\n", ahci_lpm_policy_name(p->lpm_policy));
	return sb_result(&sb);
}

static long ahci_store_lpm(struct pfs_node *n, const char *buf, long len)
{
	struct ahci_port *p = n->arg;
	char s[48];
	int pol;
	uint64_t fl;

	ahci_store_str(buf, len, s, sizeof(s));
	pol = ahci_lpm_policy_parse(s);
	if (pol < 0)
		return -EINVAL;
	if ((p->hba->quirks & AHCI_Q_NO_LPM) &&
	    pol != AHCI_LPM_KEEP_FIRMWARE && pol != AHCI_LPM_MAX_PERFORMANCE)
		return -EOPNOTSUPP;
	spin_lock_irqsave(&p->lock, &fl);
	p->lpm_policy = (enum ahci_lpm_policy)pol;
	p->host_link.lpm_policy = (enum ahci_lpm_policy)pol;
	for (uint32_t l = 0; l < AHCI_PMP_MAX_LINKS; l++)
		p->pmp_links[l].lpm_policy = (enum ahci_lpm_policy)pol;
	spin_unlock_irqrestore(&p->lock, fl);
	ahci_eh_schedule(p, AHCI_EH_LPM);
	return len;
}

static long ahci_show_eh(struct pfs_node *n, char *buf, long cap)
{
	const struct ahci_port *p = n->arg;
	struct ahci_sbuf sb = { buf, cap, 0 };
	char d[256];

	ahci_eh_str(p->eh_pending, d, sizeof(d));
	sbp(&sb, "state %s\npending %s\nlevel %d\nconsecutive %u\nseq %u\nruns %u\n",
	    ahci_port_state_name(p->state), d, (int)p->eh_level,
	    p->eh_consecutive, p->eh_seq, p->stats.eh_runs);
	sbp(&sb, "# write: softreset | hardreset | revalidate | hotplug | lpm | media | detach | hba_reset | dump\n");
	return sb_result(&sb);
}

static long ahci_store_eh(struct pfs_node *n, const char *buf, long len)
{
	struct ahci_port *p = n->arg;
	char s[32];
	uint32_t a;

	ahci_store_str(buf, len, s, sizeof(s));
	if (ahci_streq(s, "softreset") || ahci_streq(s, "reset"))
		a = AHCI_EH_RESET_SOFT;
	else if (ahci_streq(s, "hardreset"))
		a = AHCI_EH_RESET_HARD;
	else if (ahci_streq(s, "revalidate"))
		a = AHCI_EH_REVALIDATE;
	else if (ahci_streq(s, "hotplug"))
		a = AHCI_EH_HOTPLUG;
	else if (ahci_streq(s, "lpm"))
		a = AHCI_EH_LPM;
	else if (ahci_streq(s, "media")) {
		/* the media events of the port's optical drives, now */
		atapi_media_event(p->host_link.dev);
		for (uint32_t l = 0;
		     l < p->nr_pmp_links && l < AHCI_PMP_MAX_LINKS; l++)
			atapi_media_event(p->pmp_links[l].dev);
		return len;
	} else if (ahci_streq(s, "detach"))
		a = AHCI_EH_DETACH;
	else if (ahci_streq(s, "hba_reset"))
		a = AHCI_EH_HBA_RESET;
	else if (ahci_streq(s, "dump")) {
		ahci_dump_port(p);
		return len;
	} else
		return -EINVAL;
	kprintf("[ahci] %s: error handling requested from sysfs: %s\n",
		p->name, s);
	/* Nothing is frozen: nothing new is admitted, what is in flight
	 * completes normally, and the run starts once it has drained (a
	 * controller reset holds what is still at the device until the
	 * reset is done). */
	ahci_eh_request(p, a);
	if (a == AHCI_EH_HBA_RESET)
		ahci_kahci_kick(p->hba, AHCI_WORK_HBA_RESET);
	return len;
}

int ahci_sysfs_add_port(struct ahci_port *port)
{
	char dbg[96];
	int rc;

	if (!port || !port->hba)
		return -EINVAL;
	if (port->sysfs_base[0])
		return 0; /* already there */
	rc = sysfs_add_class_device("ata_port", port->name, port->hba->pci,
				    port->sysfs_base, sizeof(port->sysfs_base));
	if (rc) {
		port->sysfs_base[0] = 0;
		return rc;
	}
	sysfs_add_attr(port->sysfs_base, "port_no", ahci_show_port_no, NULL,
		       port, 0);
	sysfs_add_attr(port->sysfs_base, "idle_irq", ahci_show_idle_irq, NULL,
		       port, 0);
	sysfs_add_attr(port->sysfs_base, "nr_pmp_links", ahci_show_nr_pmp_links,
		       NULL, port, 0);
	sysfs_add_attr(port->sysfs_base, "stats", ahci_show_port_stats, NULL,
		       port, 0);
	sysfs_add_attr(port->sysfs_base, "regs", ahci_show_port_regs, NULL,
		       port, 0);
	sysfs_add_attr(port->sysfs_base, "trace", ahci_show_port_trace, NULL,
		       port, 0);
	sysfs_add_attr(port->sysfs_base, "fault_inject", ahci_show_fault,
		       ahci_store_fault, port, 0);
	sysfs_add_attr(port->sysfs_base, "link_power_management_policy",
		       ahci_show_lpm, ahci_store_lpm, port, 0);
	sysfs_add_attr(port->sysfs_base, "eh", ahci_show_eh, ahci_store_eh,
		       port, 0);
	sysfs_add_attr(port->sysfs_base, "state", ahci_show_port_state, NULL,
		       port, 0);

	/* the debug tree */
	if (port->hba->sysfs_base[0]) {
		ksnprintf(dbg, sizeof(dbg), "%s/ports/%d",
			  port->hba->sysfs_base, port->index);
		sysfs_add_attr(dbg, "regs", ahci_show_port_regs, NULL, port, 0);
		sysfs_add_attr(dbg, "trace", ahci_show_port_trace, NULL, port,
			       0);
		sysfs_add_attr(dbg, "fault", ahci_show_fault, ahci_store_fault,
			       port, 0);
		sysfs_add_attr(dbg, "stats", ahci_show_port_stats, NULL, port,
			       0);
	}
	return 0;
}

/* ==========================================================================
 * sysfs: links
 * ========================================================================== */

/* Links get a directory once; the table remembers which. */
#define AHCI_SYSFS_MAX_LINKS (AHCI_MAX_HBAS * AHCI_MAX_PORTS)
static const struct ahci_link *g_link_sysfs[AHCI_SYSFS_MAX_LINKS];
static spinlock_t g_ahci_sysfs_lock = SPINLOCK_INIT("ahci_sysfs");

static uint8_t ahci_iss_to_spd(uint32_t iss)
{
	return (uint8_t)(iss > AHCI_SSTS_SPD_GEN3 ? AHCI_SSTS_SPD_GEN3 : iss);
}

/* Read a link's SCR for display: host links directly; fan-out links
 * through the port multiplier, only when its port is not in the middle of
 * error handling (otherwise the cached value). */
static uint32_t ahci_link_scr_show(struct ahci_link *link, int scr,
				   uint32_t cached)
{
	struct ahci_port *port = link->port;
	uint32_t v = cached;

	if (!port)
		return cached;
	if (link->is_host) {
		if (ahci_link_scr_read(link, scr, &v) != 0)
			v = cached;
		return v;
	}
	if (ata_mutex_trylock(&port->eh_mutex)) {
		if (ahci_link_scr_read(link, scr, &v) != 0)
			v = cached;
		ata_mutex_unlock(&port->eh_mutex);
	}
	return v;
}

static long ahci_show_hw_spd_limit(struct pfs_node *n, char *buf, long cap)
{
	const struct ahci_link *l = n->arg;
	struct ahci_sbuf sb = { buf, cap, 0 };

	sbp(&sb, "%s\n",
	    ahci_speed_str(l->port && l->port->hba ?
				   ahci_iss_to_spd(l->port->hba->caps.iss) :
				   0));
	return sb_result(&sb);
}

static long ahci_show_spd_limit(struct pfs_node *n, char *buf, long cap)
{
	const struct ahci_link *l = n->arg;
	struct ahci_sbuf sb = { buf, cap, 0 };

	sbp(&sb, "%s\n", ahci_speed_str(l->speed_limit));
	return sb_result(&sb);
}

static long ahci_store_spd_limit(struct pfs_node *n, const char *buf, long len)
{
	struct ahci_link *l = n->arg;
	struct ahci_port *port = l->port;
	char s[32];
	uint8_t spd;
	int rc;

	ahci_store_str(buf, len, s, sizeof(s));
	if (ahci_starts(s, "1.5"))
		spd = AHCI_SSTS_SPD_GEN1;
	else if (ahci_starts(s, "3"))
		spd = AHCI_SSTS_SPD_GEN2;
	else if (ahci_starts(s, "6"))
		spd = AHCI_SSTS_SPD_GEN3;
	else if (ahci_streq(s, "0") || ahci_streq(s, "none") ||
		 ahci_starts(s, "<unknown>"))
		spd = 0;
	else
		return -EINVAL;
	if (!port)
		return -ENODEV;
	ata_mutex_lock(&port->eh_mutex);
	rc = ahci_link_set_speed_limit(l, spd);
	ata_mutex_unlock(&port->eh_mutex);
	return rc ? rc : len;
}

static long ahci_show_spd(struct pfs_node *n, char *buf, long cap)
{
	struct ahci_link *l = n->arg;
	struct ahci_sbuf sb = { buf, cap, 0 };
	uint32_t ssts = ahci_link_scr_show(l, AHCI_SCR_STATUS, l->sstatus);

	sbp(&sb, "%s\n", AHCI_SSTS_DET(ssts) == AHCI_SSTS_DET_PHY ?
				  ahci_speed_str(AHCI_SSTS_SPD(ssts)) :
				  "<unknown>");
	return sb_result(&sb);
}

static long ahci_show_sstatus(struct pfs_node *n, char *buf, long cap)
{
	struct ahci_link *l = n->arg;
	struct ahci_sbuf sb = { buf, cap, 0 };

	sbp(&sb, "0x%08x\n", ahci_link_scr_show(l, AHCI_SCR_STATUS, l->sstatus));
	return sb_result(&sb);
}

static long ahci_show_serror(struct pfs_node *n, char *buf, long cap)
{
	struct ahci_link *l = n->arg;
	struct ahci_sbuf sb = { buf, cap, 0 };
	char d[200];
	uint32_t v = ahci_link_scr_show(l, AHCI_SCR_ERROR, l->serror);

	ahci_serror_str(v, d, sizeof(d));
	sbp(&sb, "0x%08x %s\n", v, d);
	return sb_result(&sb);
}

static long ahci_show_scontrol(struct pfs_node *n, char *buf, long cap)
{
	struct ahci_link *l = n->arg;
	struct ahci_sbuf sb = { buf, cap, 0 };

	sbp(&sb, "0x%08x\n",
	    ahci_link_scr_show(l, AHCI_SCR_CONTROL, l->scontrol));
	return sb_result(&sb);
}

static long ahci_show_link_counters(struct pfs_node *n, char *buf, long cap)
{
	const struct ahci_link *l = n->arg;
	struct ahci_sbuf sb = { buf, cap, 0 };

	sbp(&sb, "class %s\nsig 0x%08x\nresets %u\nlink_errors %u\nspeed_downs %u\n",
	    ata_class_name(l->class), l->sig, l->n_resets, l->n_link_errors,
	    l->n_speed_downs);
	return sb_result(&sb);
}

int ahci_sysfs_add_link(struct ahci_link *link)
{
	char base[128], name[16];
	struct ahci_port *port;
	uint64_t fl;
	int slot = -1, rc;

	if (!link || !link->port || !link->port->hba)
		return -EINVAL;
	port = link->port;

	spin_lock_irqsave(&g_ahci_sysfs_lock, &fl);
	for (int i = 0; i < AHCI_SYSFS_MAX_LINKS; i++) {
		if (g_link_sysfs[i] == link) {
			spin_unlock_irqrestore(&g_ahci_sysfs_lock, fl);
			return 0;
		}
		if (slot < 0 && !g_link_sysfs[i])
			slot = i;
	}
	if (slot >= 0)
		g_link_sysfs[slot] = link;
	spin_unlock_irqrestore(&g_ahci_sysfs_lock, fl);
	if (slot < 0)
		return -ENOSPC;

	if (link->name[0])
		ksnprintf(name, sizeof(name), "%s", link->name);
	else if (link->is_host)
		ksnprintf(name, sizeof(name), "link%d", port->port_no);
	else
		ksnprintf(name, sizeof(name), "link%d.%d", port->port_no,
			  link->pmp);
	rc = sysfs_add_class_device("ata_link", name, port->hba->pci, base,
				    sizeof(base));
	if (rc)
		return rc;
	sysfs_add_attr(base, "hw_sata_spd_limit", ahci_show_hw_spd_limit, NULL,
		       link, 0);
	sysfs_add_attr(base, "sata_spd_limit", ahci_show_spd_limit,
		       ahci_store_spd_limit, link, 0);
	sysfs_add_attr(base, "sata_spd", ahci_show_spd, NULL, link, 0);
	sysfs_add_attr(base, "sstatus", ahci_show_sstatus, NULL, link, 0);
	sysfs_add_attr(base, "serror", ahci_show_serror, NULL, link, 0);
	sysfs_add_attr(base, "scontrol", ahci_show_scontrol, NULL, link, 0);
	sysfs_add_attr(base, "counters", ahci_show_link_counters, NULL, link,
		       0);
	return 0;
}

/* ==========================================================================
 * sysfs: devices
 *
 * A device can go away (hot-unplug, EH giving up) while its files stay --
 * the attribute tree has no removal.  Each device directory therefore
 * reads through a slot that the removal clears; a later device of the same
 * name reuses the slot and its files.
 * ========================================================================== */

#define AHCI_SYSFS_MAX_DEVS 128
struct ahci_dev_slot {
	struct ata_device *dev; /* NULL once removed */
	char name[24];
};
static struct ahci_dev_slot g_dev_sysfs[AHCI_SYSFS_MAX_DEVS];

/* Take a reference to the slot's device for one show call. */
static struct ata_device *ahci_dev_slot_get(struct ahci_dev_slot *s)
{
	struct ata_device *dev;
	uint64_t fl;

	spin_lock_irqsave(&g_ahci_sysfs_lock, &fl);
	dev = s->dev;
	if (dev && (dev->magic != ATA_DEVICE_MAGIC ||
		    !refcount_inc_not_zero(&dev->refcount)))
		dev = NULL;
	spin_unlock_irqrestore(&g_ahci_sysfs_lock, fl);
	return dev;
}

static void ahci_dev_slot_put(struct ata_device *dev)
{
	if (dev)
		ata_dev_put(dev);
}

#define AHCI_DEV_SHOW_BEGIN                                    \
	struct ahci_dev_slot *slot = n->arg;                   \
	struct ahci_sbuf sb = { buf, cap, 0 };                 \
	struct ata_device *dev = ahci_dev_slot_get(slot);      \
	if (!dev)                                              \
		return -ENODEV

#define AHCI_DEV_SHOW_END        \
	ahci_dev_slot_put(dev);  \
	return sb_result(&sb)

static long ahci_show_dev_class(struct pfs_node *n, char *buf, long cap)
{
	AHCI_DEV_SHOW_BEGIN;
	sbp(&sb, "%s\n", dev->class == ATA_DEV_ATAPI ? "atapi" :
			 dev->class == ATA_DEV_PMP   ? "pmp" :
			 dev->class == ATA_DEV_SEMB  ? "semb" :
			 dev->class == ATA_DEV_ZAC   ? "zac" :
			 dev->class == ATA_DEV_ATA   ? "ata" :
						       "none");
	AHCI_DEV_SHOW_END;
}

static long ahci_show_dev_id(struct pfs_node *n, char *buf, long cap)
{
	AHCI_DEV_SHOW_BEGIN;
	for (int w = 0; w < ATA_ID_WORDS; w++)
		sbp(&sb, "%04x%c", dev->id[w], (w % 8) == 7 ? '\n' : ' ');
	AHCI_DEV_SHOW_END;
}

static long ahci_show_dev_model(struct pfs_node *n, char *buf, long cap)
{
	AHCI_DEV_SHOW_BEGIN;
	sbp(&sb, "%s\n", dev->model);
	AHCI_DEV_SHOW_END;
}

static long ahci_show_dev_serial(struct pfs_node *n, char *buf, long cap)
{
	AHCI_DEV_SHOW_BEGIN;
	sbp(&sb, "%s\n", dev->serial);
	AHCI_DEV_SHOW_END;
}

static long ahci_show_dev_fw(struct pfs_node *n, char *buf, long cap)
{
	AHCI_DEV_SHOW_BEGIN;
	sbp(&sb, "%s\n", dev->firmware);
	AHCI_DEV_SHOW_END;
}

static long ahci_show_dev_ncq(struct pfs_node *n, char *buf, long cap)
{
	AHCI_DEV_SHOW_BEGIN;
	sbp(&sb, "%u\n", (dev->flags & ATA_DFLAG_NCQ_ON) ? dev->ncq_depth : 1);
	AHCI_DEV_SHOW_END;
}

static long ahci_show_dev_errors(struct pfs_node *n, char *buf, long cap)
{
	AHCI_DEV_SHOW_BEGIN;
	sbp(&sb, "errors %u\ntimeouts %u\nretries %u\nmedia_errors %u\nbus_errors %u\nresets %u\nrevalidations %u\n",
	    dev->n_errors, dev->n_timeouts, dev->n_retries,
	    dev->n_media_errors, dev->n_bus_errors, dev->n_resets,
	    dev->n_revalidations);
	if (dev->n_errors)
		sbp(&sb, "last_command 0x%02x (%s) status 0x%02x error 0x%02x\n",
		    dev->last_error_cmd, ahci_opcode_name(dev->last_error_cmd),
		    dev->last_error_tf.status, dev->last_error_tf.error);
	AHCI_DEV_SHOW_END;
}

static long ahci_show_dev_power(struct pfs_node *n, char *buf, long cap)
{
	AHCI_DEV_SHOW_BEGIN;
	sbp(&sb, "%s\n", dev->power_state == ATA_PWR_ACTIVE  ? "active" :
			 dev->power_state == ATA_PWR_IDLE    ? "idle" :
			 dev->power_state == ATA_PWR_STANDBY ? "standby" :
			 dev->power_state == ATA_PWR_SLEEP   ? "sleep" :
							       "unknown");
	AHCI_DEV_SHOW_END;
}

static long ahci_show_dev_security(struct pfs_node *n, char *buf, long cap)
{
	const struct ata_security *s;

	AHCI_DEV_SHOW_BEGIN;
	s = &dev->security;
	if (!s->supported) {
		sbp(&sb, "not supported\n");
	} else {
		sbp(&sb, "supported%s%s%s%s%s level %s",
		    s->enabled ? " enabled" : " disabled",
		    s->locked ? " locked" : " unlocked",
		    s->frozen ? " frozen" : " not-frozen",
		    s->count_expired ? " count-expired" : "",
		    s->enhanced_erase ? " enhanced-erase" : "",
		    s->level_max ? "maximum" : "high");
		if (s->erase_time_min)
			sbp(&sb, " erase %u min", s->erase_time_min);
		if (s->enh_erase_time_min)
			sbp(&sb, " enhanced-erase %u min",
			    s->enh_erase_time_min);
		sbp(&sb, "\n");
	}
	AHCI_DEV_SHOW_END;
}

static long ahci_show_dev_trim(struct pfs_node *n, char *buf, long cap)
{
	AHCI_DEV_SHOW_BEGIN;
	if (!(dev->flags & ATA_DFLAG_TRIM))
		sbp(&sb, "unsupported\n");
	else
		sbp(&sb, "%s%s%s\n",
		    (dev->flags & ATA_DFLAG_NCQ_TRIM) ? "queued" : "unqueued",
		    (dev->flags & ATA_DFLAG_TRIM_DRAT) ? " deterministic" : "",
		    (dev->flags & ATA_DFLAG_TRIM_RZAT) ? " zeroing" : "");
	AHCI_DEV_SHOW_END;
}

int ahci_sysfs_add_device(struct ata_device *dev)
{
	struct ahci_link *link;
	struct ahci_port *port;
	struct ahci_dev_slot *s = NULL;
	char name[24], base[128];
	uint64_t fl;
	int existing = 0, rc;

	if (!dev || dev->magic != ATA_DEVICE_MAGIC || !dev->xport_priv)
		return -EINVAL;
	link = ahci_dev_link(dev);
	port = link->port;
	if (!port || !port->hba)
		return -EINVAL;

	if (link->is_host)
		ksnprintf(name, sizeof(name), "dev%d.0", port->port_no);
	else
		ksnprintf(name, sizeof(name), "dev%d.%d.0", port->port_no,
			  link->pmp);

	spin_lock_irqsave(&g_ahci_sysfs_lock, &fl);
	for (int i = 0; i < AHCI_SYSFS_MAX_DEVS; i++) {
		if (g_dev_sysfs[i].name[0] &&
		    kstrcmp(g_dev_sysfs[i].name, name) == 0) {
			s = &g_dev_sysfs[i];
			existing = 1;
			break;
		}
	}
	if (!s) {
		for (int i = 0; i < AHCI_SYSFS_MAX_DEVS; i++) {
			if (!g_dev_sysfs[i].name[0]) {
				s = &g_dev_sysfs[i];
				ksnprintf(s->name, sizeof(s->name), "%s", name);
				break;
			}
		}
	}
	if (s)
		s->dev = dev;
	spin_unlock_irqrestore(&g_ahci_sysfs_lock, fl);
	if (!s)
		return -ENOSPC;
	if (existing)
		return 0; /* the files are there and read through the slot */

	rc = sysfs_add_class_device("ata_device", name, port->hba->pci, base,
				    sizeof(base));
	if (rc)
		return rc;
	sysfs_add_attr(base, "class", ahci_show_dev_class, NULL, s, 0);
	sysfs_add_attr(base, "id", ahci_show_dev_id, NULL, s, 0);
	sysfs_add_attr(base, "model", ahci_show_dev_model, NULL, s, 0);
	sysfs_add_attr(base, "serial", ahci_show_dev_serial, NULL, s, 0);
	sysfs_add_attr(base, "firmware", ahci_show_dev_fw, NULL, s, 0);
	sysfs_add_attr(base, "ncq_depth", ahci_show_dev_ncq, NULL, s, 0);
	sysfs_add_attr(base, "errors", ahci_show_dev_errors, NULL, s, 0);
	sysfs_add_attr(base, "power_state", ahci_show_dev_power, NULL, s, 0);
	sysfs_add_attr(base, "security", ahci_show_dev_security, NULL, s, 0);
	sysfs_add_attr(base, "trim", ahci_show_dev_trim, NULL, s, 0);
	return 0;
}

void ahci_sysfs_remove_device(struct ata_device *dev)
{
	uint64_t fl;

	if (!dev)
		return;
	spin_lock_irqsave(&g_ahci_sysfs_lock, &fl);
	for (int i = 0; i < AHCI_SYSFS_MAX_DEVS; i++)
		if (g_dev_sysfs[i].dev == dev)
			g_dev_sysfs[i].dev = NULL;
	spin_unlock_irqrestore(&g_ahci_sysfs_lock, fl);
}

/* ==========================================================================
 * sysfs: controllers and the debug tree
 * ========================================================================== */

static long ahci_show_hba_regs(struct pfs_node *n, char *buf, long cap)
{
	struct ahci_sbuf sb = { buf, cap, 0 };

	ahci_fmt_hba(n->arg, &sb);
	return sb_result(&sb);
}

static long ahci_show_hba_info(struct pfs_node *n, char *buf, long cap)
{
	const struct ahci_hba *h = n->arg;
	const struct ahci_caps *c = &h->caps;
	struct ahci_sbuf sb = { buf, cap, 0 };

	sbp(&sb, "name %s\ncontroller %s\npci %04x:%04x at %02x:%02x.%u\n",
	    h->name, h->id && h->id->name ? h->id->name : "AHCI",
	    h->pci->vendor_id, h->pci->device_id, h->pci->bus, h->pci->device,
	    h->pci->function);
	sbp(&sb, "state %s\nirq_mode %s\nirq_vector %d\nirq_gsi %d\n",
	    ahci_hba_state_str(h->state), ahci_irq_mode_name(h->irq_mode),
	    h->irq_vector, h->irq_gsi);
	sbp(&sb, "version 0x%08x\ncap 0x%08x\ncap2 0x%08x\npi 0x%08x\nport_map 0x%08x\n",
	    h->vs, h->cap, h->cap2, h->pi, h->port_map);
	sbp(&sb, "ports %u\nslots %u\nspeed %u\n", c->n_ports, c->n_slots,
	    c->iss);
	sbp(&sb, "ncq %u\npmp %u\nfbs %u\nsntf %u\nalpm %u\nsss %u\nclo %u\ndevslp %u\napst %u\n",
	    c->sncq, c->spm, c->fbss, c->ssntf, c->salp, c->sss, c->sclo,
	    c->sds, c->apst);
	sbp(&sb, "dma64 %d\nquirks 0x%08x\ngeneration %u\nfirst_port_no %d\nkahci %s\n",
	    h->dma64, h->quirks, h->generation, h->first_port_no,
	    h->kahci_running ? "running" : "stopped");
	return sb_result(&sb);
}

static long ahci_show_hba_stats(struct pfs_node *n, char *buf, long cap)
{
	const struct ahci_hba *h = n->arg;
	struct ahci_sbuf sb = { buf, cap, 0 };

	sbp(&sb, "irqs %llu\nspurious_irqs %llu\npolls %llu\nhba_resets %u\nhost_errors %u\nkahci_wakeups %u\n",
	    (unsigned long long)h->stats.irqs,
	    (unsigned long long)h->stats.spurious_irqs,
	    (unsigned long long)h->stats.polls, h->stats.hba_resets,
	    h->stats.host_errors, h->stats.kahci_wakeups);
	return sb_result(&sb);
}

static long ahci_show_hba_reset(struct pfs_node *n, char *buf, long cap)
{
	const struct ahci_hba *h = n->arg;
	struct ahci_sbuf sb = { buf, cap, 0 };

	sbp(&sb, "%u\n# write 1 to reset and re-initialise the controller\n",
	    h->stats.hba_resets);
	return sb_result(&sb);
}

static long ahci_store_hba_reset(struct pfs_node *n, const char *buf, long len)
{
	struct ahci_hba *h = n->arg;
	char s[16];

	ahci_store_str(buf, len, s, sizeof(s));
	if (!ahci_streq(s, "1"))
		return -EINVAL;
	if (!h->kahci)
		return -EAGAIN;
	kprintf("[ahci] %s: controller reset requested from sysfs\n", h->name);
	ahci_kahci_kick(h, AHCI_WORK_HBA_RESET);
	return len;
}

static long ahci_show_hba_power(struct pfs_node *n, char *buf, long cap)
{
	const struct ahci_hba *h = n->arg;
	struct ahci_sbuf sb = { buf, cap, 0 };

	sbp(&sb, "%s\n# write suspend | resume\n",
	    h->state == AHCI_HBA_SUSPENDED ? "suspended" : "active");
	return sb_result(&sb);
}

static long ahci_store_hba_power(struct pfs_node *n, const char *buf, long len)
{
	struct ahci_hba *h = n->arg;
	char s[16];
	int rc;

	ahci_store_str(buf, len, s, sizeof(s));
	if (ahci_streq(s, "suspend"))
		rc = ahci_hba_suspend(h);
	else if (ahci_streq(s, "resume"))
		rc = ahci_hba_resume(h);
	else
		return -EINVAL;
	return rc ? rc : len;
}

static long ahci_show_controllers(struct pfs_node *n, char *buf, long cap)
{
	struct ahci_sbuf sb = { buf, cap, 0 };

	(void)n;
	for (int i = 0; i < ahci_hba_count(); i++) {
		const struct ahci_hba *h = ahci_hba_get(i);

		if (!h)
			continue;
		sbp(&sb, "%s %04x:%04x %02x:%02x.%u %s %s %s ports 0x%08x\n",
		    h->name, h->pci->vendor_id, h->pci->device_id,
		    h->pci->bus, h->pci->device, h->pci->function,
		    h->id && h->id->name ? h->id->name : "AHCI",
		    ahci_hba_state_str(h->state),
		    ahci_irq_mode_name(h->irq_mode), h->port_map);
	}
	return sb_result(&sb);
}

static long ahci_show_dma(struct pfs_node *n, char *buf, long cap)
{
	struct ahci_sbuf sb = { buf, cap, 0 };
	uint64_t live = 0, failed = 0, total;

	(void)n;
	total = ahci_dma_stat_bounces(&live, &failed);
	sbp(&sb, "structure_pages %llu\nbounces %llu\nbounces_live %llu\nbounce_failures %llu\nnoncoherent %d\n",
	    (unsigned long long)ahci_dma_stat_pages(),
	    (unsigned long long)total, (unsigned long long)live,
	    (unsigned long long)failed, ahci_dma_noncoherent);
	return sb_result(&sb);
}

static long ahci_show_noncoherent(struct pfs_node *n, char *buf, long cap)
{
	struct ahci_sbuf sb = { buf, cap, 0 };

	(void)n;
	sbp(&sb, "%d\n", ahci_dma_noncoherent);
	return sb_result(&sb);
}

static long ahci_store_noncoherent(struct pfs_node *n, const char *buf,
				   long len)
{
	char s[8];

	(void)n;
	ahci_store_str(buf, len, s, sizeof(s));
	if (ahci_streq(s, "1"))
		ahci_dma_noncoherent = 1;
	else if (ahci_streq(s, "0"))
		ahci_dma_noncoherent = 0;
	else
		return -EINVAL;
	kprintf("[ahci] DMA cache maintenance %s\n",
		ahci_dma_noncoherent ? "on (non-coherent mode)" : "off");
	return len;
}

int ahci_sysfs_add_hba(struct ahci_hba *hba)
{
	static int global_done;

	if (!hba)
		return -EINVAL;
	if (!global_done) {
		global_done = 1;
		sysfs_add_attr("kernel/debug/ahci", "controllers",
			       ahci_show_controllers, NULL, NULL, 0);
		sysfs_add_attr("kernel/debug/ahci", "dma", ahci_show_dma, NULL,
			       NULL, 0);
		sysfs_add_attr("kernel/debug/ahci", "dma_noncoherent",
			       ahci_show_noncoherent, ahci_store_noncoherent,
			       NULL, 0);
	}
	if (hba->sysfs_base[0])
		return 0;
	ksnprintf(hba->sysfs_base, sizeof(hba->sysfs_base),
		  "kernel/debug/ahci/%s", hba->name);
	if (!sysfs_add_attr(hba->sysfs_base, "regs", ahci_show_hba_regs, NULL,
			    hba, 0)) {
		hba->sysfs_base[0] = 0;
		return -ENOMEM;
	}
	sysfs_add_attr(hba->sysfs_base, "info", ahci_show_hba_info, NULL, hba,
		       0);
	sysfs_add_attr(hba->sysfs_base, "stats", ahci_show_hba_stats, NULL,
		       hba, 0);
	sysfs_add_attr(hba->sysfs_base, "reset", ahci_show_hba_reset,
		       ahci_store_hba_reset, hba, 0);
	sysfs_add_attr(hba->sysfs_base, "power", ahci_show_hba_power,
		       ahci_store_hba_power, hba, 0);
	return 0;
}
