// LikeOS - NVMe PCIe quirks: the PCI ID table, nvme.quirks=, platform quirks
//
// Three sources decide a controller's transport quirks (enum nvme_quirks
// in nvme.h), applied in this order:
//
//   1. the PCI ID table below (driver_data of the matching entry) -- the
//      established list of controllers with known defects, ending with
//      the class match that binds every other NVMe controller;
//   2. the platform: a few drives misbehave only on particular boards or
//      laptops (identified through the SMBIOS system and baseboard
//      strings), and the firmware may ask for the storage device to be
//      put in D3 for suspend (ACPI _DSD "StorageD3Enable") -- then the
//      suspend is a simple full shutdown (unless nvme.noacpi);
//   3. the boot command line, nvme.quirks=VID:DID:name[,name...] with
//      several devices separated by '-' and a '^' before a name clearing
//      that quirk, the names those of nvme_quirk_name().
//
// The core adds its own quirks by model and firmware revision once the
// controller is identified (nvme_core.c).
//
// Copyright (C) 2026 The LikeOS Project

/* The ACPICA headers carry unused parameters in their stubs. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <acpi.h>
#pragma GCC diagnostic pop
#include <kernel/dev/nvme/nvme_pci.h>
#include <kernel/io/console.h>

#define NVME_PCI_QUIRK_LOG NVME_LOG_PREFIX

/* ==========================================================================
 * The PCI ID table
 * ========================================================================== */

const struct pci_device_id nvme_id_table[] = {
	{ PCI_VDEVICE(INTEL, 0x0953), /* Intel 750/P3500/P3600/P3700 */
	  .driver_data = NVME_QUIRK_STRIPE_SIZE |
			 NVME_QUIRK_DEALLOCATE_ZEROES },
	{ PCI_VDEVICE(INTEL, 0x0a53), /* Intel P3520 */
	  .driver_data = NVME_QUIRK_STRIPE_SIZE |
			 NVME_QUIRK_DEALLOCATE_ZEROES },
	{ PCI_VDEVICE(INTEL, 0x0a54), /* Intel P4500/P4600 */
	  .driver_data = NVME_QUIRK_STRIPE_SIZE |
			 NVME_QUIRK_IGNORE_DEV_SUBNQN | NVME_QUIRK_BOGUS_NID },
	{ PCI_VDEVICE(INTEL, 0x0a55), /* Dell Express Flash P4600 */
	  .driver_data = NVME_QUIRK_STRIPE_SIZE },
	{ PCI_VDEVICE(INTEL, 0xf1a5), /* Intel 600P/P3100 */
	  .driver_data = NVME_QUIRK_NO_DEEPEST_PS | NVME_QUIRK_MEDIUM_PRIO_SQ |
			 NVME_QUIRK_NO_TEMP_THRESH_CHANGE |
			 NVME_QUIRK_DISABLE_WRITE_ZEROES },
	{ PCI_VDEVICE(INTEL, 0xf1a6), /* Intel 760p/Pro 7600p */
	  .driver_data = NVME_QUIRK_IGNORE_DEV_SUBNQN },
	{ PCI_VDEVICE(INTEL, 0x5845), /* QEMU emulated controller (old ID) */
	  .driver_data = NVME_QUIRK_IDENTIFY_CNS |
			 NVME_QUIRK_DISABLE_WRITE_ZEROES | NVME_QUIRK_BOGUS_NID },
	{ PCI_VDEVICE(REDHAT, 0x0010), /* QEMU emulated controller */
	  .driver_data = NVME_QUIRK_BOGUS_NID },
	{ PCI_DEVICE(0x1217, 0x8760), /* O2 Micro 64GB Steam Deck */
	  .driver_data = NVME_QUIRK_DMAPOOL_ALIGN_512 },
	{ PCI_DEVICE(0x126f, 0x1001), /* Silicon Motion generic */
	  .driver_data = NVME_QUIRK_NO_DEEPEST_PS |
			 NVME_QUIRK_IGNORE_DEV_SUBNQN },
	{ PCI_DEVICE(0x126f, 0x2262), /* Silicon Motion generic */
	  .driver_data = NVME_QUIRK_NO_DEEPEST_PS | NVME_QUIRK_BOGUS_NID },
	{ PCI_DEVICE(0x126f, 0x2263), /* Silicon Motion unidentified */
	  .driver_data = NVME_QUIRK_NO_NS_DESC_LIST | NVME_QUIRK_BOGUS_NID },
	{ PCI_DEVICE(0x1bb1, 0x0100), /* Seagate Nytro Flash Storage */
	  .driver_data = NVME_QUIRK_DELAY_BEFORE_CHK_RDY |
			 NVME_QUIRK_NO_NS_DESC_LIST },
	{ PCI_DEVICE(0x1c58, 0x0003), /* HGST adapter */
	  .driver_data = NVME_QUIRK_DELAY_BEFORE_CHK_RDY },
	{ PCI_DEVICE(0x1c58, 0x0023), /* WDC SN200 adapter */
	  .driver_data = NVME_QUIRK_DELAY_BEFORE_CHK_RDY },
	{ PCI_DEVICE(0x1c5f, 0x0540), /* Memblaze Pblaze4 adapter */
	  .driver_data = NVME_QUIRK_DELAY_BEFORE_CHK_RDY },
	{ PCI_DEVICE(0x144d, 0xa821), /* Samsung PM1725 */
	  .driver_data = NVME_QUIRK_DELAY_BEFORE_CHK_RDY },
	{ PCI_DEVICE(0x144d, 0xa822), /* Samsung PM1725a */
	  .driver_data = NVME_QUIRK_DELAY_BEFORE_CHK_RDY |
			 NVME_QUIRK_DISABLE_WRITE_ZEROES |
			 NVME_QUIRK_IGNORE_DEV_SUBNQN },
	{ PCI_DEVICE(0x15b7, 0x5008), /* Sandisk SN530 */
	  .driver_data = NVME_QUIRK_BROKEN_MSI },
	{ PCI_DEVICE(0x15b7, 0x5009), /* Sandisk SN550 */
	  .driver_data = NVME_QUIRK_BROKEN_MSI | NVME_QUIRK_NO_DEEPEST_PS },
	{ PCI_DEVICE(0x1987, 0x5012), /* Phison E12 */
	  .driver_data = NVME_QUIRK_BOGUS_NID },
	{ PCI_DEVICE(0x1987, 0x5016), /* Phison E16 */
	  .driver_data = NVME_QUIRK_IGNORE_DEV_SUBNQN | NVME_QUIRK_BOGUS_NID },
	{ PCI_DEVICE(0x1987, 0x5019), /* Phison E19 */
	  .driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES },
	{ PCI_DEVICE(0x1987, 0x5021), /* Phison E21 */
	  .driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES },
	{ PCI_DEVICE(0x1b4b, 0x1092), /* Lexar 256 GB SSD */
	  .driver_data = NVME_QUIRK_NO_NS_DESC_LIST |
			 NVME_QUIRK_IGNORE_DEV_SUBNQN },
	{ PCI_DEVICE(0x1cc1, 0x33f8), /* ADATA IM2P33F8ABR1 1 TB */
	  .driver_data = NVME_QUIRK_BOGUS_NID },
	{ PCI_DEVICE(0x10ec, 0x5762), /* ADATA SX6000LNP */
	  .driver_data = NVME_QUIRK_IGNORE_DEV_SUBNQN | NVME_QUIRK_BOGUS_NID },
	{ PCI_DEVICE(0x10ec, 0x5763), /* ADATA SX6000PNP */
	  .driver_data = NVME_QUIRK_BOGUS_NID },
	{ PCI_DEVICE(0x1cc1, 0x8201), /* ADATA SX8200PNP 512GB */
	  .driver_data = NVME_QUIRK_NO_DEEPEST_PS |
			 NVME_QUIRK_IGNORE_DEV_SUBNQN },
	{ PCI_DEVICE(0x1344, 0x5407), /* Micron Technology Inc NVMe SSD */
	  .driver_data = NVME_QUIRK_IGNORE_DEV_SUBNQN },
	{ PCI_DEVICE(0x1344, 0x6001), /* Micron Nitro NVMe */
	  .driver_data = NVME_QUIRK_BOGUS_NID },
	{ PCI_DEVICE(0x1c5c, 0x1504), /* SK Hynix PC400 */
	  .driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES },
	{ PCI_DEVICE(0x1c5c, 0x174a), /* SK Hynix P31 SSD */
	  .driver_data = NVME_QUIRK_BOGUS_NID },
	{ PCI_DEVICE(0x1c5c, 0x1D59), /* SK Hynix BC901 */
	  .driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES },
	{ PCI_DEVICE(0x15b7, 0x2001), /* Sandisk Skyhawk */
	  .driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES },
	{ PCI_DEVICE(0x1d97, 0x2263), /* SPCC */
	  .driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES },
	{ PCI_DEVICE(0x144d, 0xa80b), /* Samsung PM9B1 256G and 512G */
	  .driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES |
			 NVME_QUIRK_BOGUS_NID },
	{ PCI_DEVICE(0x144d, 0xa809), /* Samsung MZALQ256HBJD 256G */
	  .driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES },
	{ PCI_DEVICE(0x144d, 0xa802), /* Samsung SM953 */
	  .driver_data = NVME_QUIRK_BOGUS_NID },
	{ PCI_DEVICE(0x1cc4, 0x6303), /* UMIS RPJTJ512MGE1QDY 512G */
	  .driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES },
	{ PCI_DEVICE(0x1cc4, 0x6302), /* UMIS RPJTJ256MGE1QDY 256G */
	  .driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES },
	{ PCI_DEVICE(0x2646, 0x2262), /* KINGSTON SKC2000 NVMe SSD */
	  .driver_data = NVME_QUIRK_NO_DEEPEST_PS },
	{ PCI_DEVICE(0x2646, 0x2263), /* KINGSTON A2000 NVMe SSD */
	  .driver_data = NVME_QUIRK_NO_DEEPEST_PS },
	{ PCI_DEVICE(0x2646, 0x5013), /* Kingston KC3000, FURY Renegade */
	  .driver_data = NVME_QUIRK_NO_SECONDARY_TEMP_THRESH },
	{ PCI_DEVICE(0x2646, 0x5018), /* KINGSTON OM8SFP4xxxxP OS21012 */
	  .driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES },
	{ PCI_DEVICE(0x2646, 0x5016), /* KINGSTON OM3PGP4xxxxP OS21011 */
	  .driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES },
	{ PCI_DEVICE(0x2646, 0x501A), /* KINGSTON OM8PGP4xxxxP OS21005 */
	  .driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES },
	{ PCI_DEVICE(0x2646, 0x501B), /* KINGSTON OM8PGP4xxxxQ OS21005 */
	  .driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES },
	{ PCI_DEVICE(0x2646, 0x501E), /* KINGSTON OM3PGP4xxxxQ OS21011 */
	  .driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES },
	{ PCI_DEVICE(0x1f40, 0x1202), /* Netac NV3000 NVMe SSD */
	  .driver_data = NVME_QUIRK_BOGUS_NID },
	{ PCI_DEVICE(0x1f40, 0x5236), /* Netac NV7000 NVMe SSD */
	  .driver_data = NVME_QUIRK_BOGUS_NID },
	{ PCI_DEVICE(0x1e4B, 0x1001), /* MAXIO MAP1001 */
	  .driver_data = NVME_QUIRK_BOGUS_NID },
	{ PCI_DEVICE(0x1e4B, 0x1002), /* MAXIO MAP1002 */
	  .driver_data = NVME_QUIRK_BOGUS_NID },
	{ PCI_DEVICE(0x1e4B, 0x1202), /* MAXIO MAP1202 */
	  .driver_data = NVME_QUIRK_BOGUS_NID },
	{ PCI_DEVICE(0x1e4B, 0x1602), /* MAXIO MAP1602 */
	  .driver_data = NVME_QUIRK_BOGUS_NID },
	{ PCI_DEVICE(0x1cc1, 0x5350), /* ADATA XPG GAMMIX S50 */
	  .driver_data = NVME_QUIRK_BOGUS_NID },
	{ PCI_DEVICE(0x1dbe, 0x5216), /* Acer/INNOGRIT FA100/5216 */
	  .driver_data = NVME_QUIRK_BOGUS_NID },
	{ PCI_DEVICE(0x1dbe, 0x5236), /* ADATA XPG GAMMIX S70 */
	  .driver_data = NVME_QUIRK_BOGUS_NID },
	{ PCI_DEVICE(0x1e49, 0x0021), /* ZHITAI TiPro5000 NVMe SSD */
	  .driver_data = NVME_QUIRK_NO_DEEPEST_PS },
	{ PCI_DEVICE(0x1e49, 0x0041), /* ZHITAI TiPro7000 NVMe SSD */
	  .driver_data = NVME_QUIRK_NO_DEEPEST_PS },
	{ PCI_DEVICE(0x1fa0, 0x2283), /* Wodposit WPBSNM8-256GTP */
	  .driver_data = NVME_QUIRK_NO_SECONDARY_TEMP_THRESH },
	{ PCI_DEVICE(0x025e, 0xf1ac), /* SOLIDIGM P44 pro SSDPFKKW020X7 */
	  .driver_data = NVME_QUIRK_NO_DEEPEST_PS },
	{ PCI_DEVICE(0xc0a9, 0x540a), /* Crucial P2 */
	  .driver_data = NVME_QUIRK_BOGUS_NID },
	{ PCI_DEVICE(0x1d97, 0x2263), /* Lexar NM610 */
	  .driver_data = NVME_QUIRK_BOGUS_NID },
	{ PCI_DEVICE(0x1d97, 0x1d97), /* Lexar NM620 */
	  .driver_data = NVME_QUIRK_BOGUS_NID },
	{ PCI_DEVICE(0x1d97, 0x2269), /* Lexar NM760 */
	  .driver_data = NVME_QUIRK_BOGUS_NID | NVME_QUIRK_IGNORE_DEV_SUBNQN },
	{ PCI_DEVICE(0x10ec, 0x5763), /* TEAMGROUP T-FORCE CARDEA ZERO Z330 */
	  .driver_data = NVME_QUIRK_BOGUS_NID },
	{ PCI_DEVICE(0x1e4b, 0x1602), /* HS-SSD-FUTURE 2048G */
	  .driver_data = NVME_QUIRK_BOGUS_NID },
	{ PCI_DEVICE(0x10ec, 0x5765), /* TEAMGROUP MP33 2TB SSD */
	  .driver_data = NVME_QUIRK_BOGUS_NID },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMAZON, 0x0061),
	  .driver_data = NVME_QUIRK_DMA_ADDRESS_BITS_48 },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMAZON, 0x0065),
	  .driver_data = NVME_QUIRK_DMA_ADDRESS_BITS_48 },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMAZON, 0x8061),
	  .driver_data = NVME_QUIRK_DMA_ADDRESS_BITS_48 },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMAZON, 0xcd00),
	  .driver_data = NVME_QUIRK_DMA_ADDRESS_BITS_48 },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMAZON, 0xcd01),
	  .driver_data = NVME_QUIRK_DMA_ADDRESS_BITS_48 },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMAZON, 0xcd02),
	  .driver_data = NVME_QUIRK_DMA_ADDRESS_BITS_48 },
	/* Apple controller of the MacBook8,1 and some MacBook7,1: avoids
	 * controller resets and data loss. */
	{ PCI_DEVICE(PCI_VENDOR_ID_APPLE, 0x2001),
	  .driver_data = NVME_QUIRK_SINGLE_VECTOR | NVME_QUIRK_QDEPTH_ONE },
	{ PCI_DEVICE(PCI_VENDOR_ID_APPLE, 0x2003) },
	{ PCI_DEVICE(PCI_VENDOR_ID_APPLE, 0x2005),
	  .driver_data = NVME_QUIRK_SINGLE_VECTOR | NVME_QUIRK_128_BYTES_SQES |
			 NVME_QUIRK_SHARED_TAGS | NVME_QUIRK_SKIP_CID_GEN |
			 NVME_QUIRK_IDENTIFY_CNS },
	/* Virtual controllers of the hypervisors LikeOS runs on: no
	 * transport quirk; listed so they bind by ID and show up in the
	 * table even with a non-standard class code. */
	{ PCI_DEVICE(PCI_VENDOR_ID_VMWARE, 0x07f0) }, /* VMware NVMe */
	{ PCI_DEVICE(PCI_VENDOR_ID_VIRTUALBOX, 0x4e56) }, /* VirtualBox */
	{ PCI_DEVICE_CLASS(PCI_CLASS_STORAGE_EXPRESS, 0xffffff) },
	{ 0 }
};

/* ==========================================================================
 * nvme.quirks=
 * ========================================================================== */

static struct nvme_pci_quirk_entry nvme_pci_quirk_list[NVME_PCI_MAX_DYN_QUIRKS];
static unsigned int nvme_pci_quirk_count;

/* NVME_PCI_PURE_BEGIN(quirks) ----------------------------------------------
 * The nvme.quirks= parser. */

static int nvme_quirk_hex16(const char *s, size_t len, uint16_t *out)
{
	uint32_t v = 0;

	if (!len || len > 4)
		return -EINVAL;
	for (size_t i = 0; i < len; i++) {
		char c = s[i];
		uint32_t d;

		if (c >= '0' && c <= '9')
			d = (uint32_t)(c - '0');
		else if (c >= 'a' && c <= 'f')
			d = (uint32_t)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			d = (uint32_t)(c - 'A' + 10);
		else
			return -EINVAL;
		v = (v << 4) | d;
	}
	*out = (uint16_t)v;
	return 0;
}

/* One name (len bytes at s) to its quirk bit, 0 when unknown. */
static uint32_t nvme_quirk_by_name(const char *s, size_t len)
{
	for (unsigned int i = 0; i < 32; i++) {
		const char *q = nvme_quirk_name((enum nvme_quirks)(1u << i));
		size_t qlen;

		if (!q || !kstrcmp(q, "unknown"))
			continue;
		qlen = kstrlen(q);
		if (qlen == len && !kstrncmp(q, s, len))
			return 1u << i;
	}
	return 0;
}

/* "name,^name,..." into the entry's masks. */
static int nvme_parse_quirk_names(const char *s, size_t len,
				  struct nvme_pci_quirk_entry *e)
{
	size_t pos = 0;

	if (!len)
		return -EINVAL;
	while (pos < len) {
		size_t start = pos, flen;
		bool disabled = false;
		uint32_t bit;

		while (pos < len && s[pos] != ',')
			pos++;
		flen = pos - start;
		if (pos < len)
			pos++; /* the ',' */
		if (!flen)
			return -EINVAL;
		if (s[start] == '^') {
			disabled = true;
			start++;
			flen--;
		}
		bit = nvme_quirk_by_name(s + start, flen);
		if (!bit)
			return -EINVAL;
		if (disabled)
			e->disabled_quirks |= bit;
		else
			e->enabled_quirks |= bit;
	}
	return 0;
}

/* "VID:DID:names" (len bytes). */
static int nvme_parse_quirk_entry(const char *s, size_t len,
				  struct nvme_pci_quirk_entry *e)
{
	size_t c1, c2;

	for (c1 = 0; c1 < len && s[c1] != ':'; c1++)
		;
	if (c1 == len || nvme_quirk_hex16(s, c1, &e->vendor_id))
		return -EINVAL;
	for (c2 = c1 + 1; c2 < len && s[c2] != ':'; c2++)
		;
	if (c2 == len || nvme_quirk_hex16(s + c1 + 1, c2 - c1 - 1, &e->dev_id))
		return -EINVAL;
	return nvme_parse_quirk_names(s + c2 + 1, len - c2 - 1, e);
}

int nvme_pci_cmdline_parse_quirks(const char *value,
				  struct nvme_pci_quirk_entry *out,
				  unsigned int max, unsigned int *count)
{
	struct nvme_pci_quirk_entry tmp[NVME_PCI_MAX_DYN_QUIRKS];
	size_t len, pos = 0;
	unsigned int n = 0;

	*count = 0;
	if (!value || !*value)
		return 0;
	if (max > NVME_PCI_MAX_DYN_QUIRKS)
		max = NVME_PCI_MAX_DYN_QUIRKS;
	len = kstrlen(value);
	while (pos <= len) {
		size_t start = pos;

		while (pos < len && value[pos] != '-')
			pos++;
		if (n == max)
			return -EINVAL;
		tmp[n].vendor_id = 0;
		tmp[n].dev_id = 0;
		tmp[n].enabled_quirks = 0;
		tmp[n].disabled_quirks = 0;
		if (nvme_parse_quirk_entry(value + start, pos - start, &tmp[n]))
			return -EINVAL;
		n++;
		pos++; /* the '-' (or past the end) */
	}
	for (unsigned int i = 0; i < n; i++)
		out[i] = tmp[i];
	*count = n;
	return 0;
}

/* NVME_PCI_PURE_END(quirks) ---------------------------------------------- */

void nvme_pci_quirks_init(void)
{
	unsigned int n = 0;

	nvme_pci_quirk_count = 0;
	if (!nvme_pci_param.quirks[0])
		return;
	if (nvme_pci_cmdline_parse_quirks(nvme_pci_param.quirks,
					  nvme_pci_quirk_list,
					  NVME_PCI_MAX_DYN_QUIRKS, &n)) {
		kprintf(NVME_PCI_QUIRK_LOG "failed to parse quirk string %s\n",
			nvme_pci_param.quirks);
		return;
	}
	nvme_pci_quirk_count = n;
}

void nvme_pci_cmdline_quirks(const pci_device_t *pdev, unsigned long *quirks)
{
	for (unsigned int i = 0; i < nvme_pci_quirk_count; i++) {
		const struct nvme_pci_quirk_entry *e = &nvme_pci_quirk_list[i];

		if (pdev->vendor_id == e->vendor_id &&
		    pdev->device_id == e->dev_id) {
			*quirks |= e->enabled_quirks;
			*quirks &= ~(unsigned long)e->disabled_quirks;
			kprintf(NVME_PCI_QUIRK_LOG
				"%04x:%04x: quirks +%#x -%#x from the command line\n",
				e->vendor_id, e->dev_id, e->enabled_quirks,
				e->disabled_quirks);
			return;
		}
	}
}

/* ==========================================================================
 * The platform's identity (SMBIOS system and baseboard strings)
 * ========================================================================== */

#define NVME_DMI_STR_LEN 64
#define NVME_SMBIOS_TABLE_MAX (1024u * 1024u)

struct nvme_dmi_ids {
	char sys_vendor[NVME_DMI_STR_LEN];
	char product_name[NVME_DMI_STR_LEN];
	char board_vendor[NVME_DMI_STR_LEN];
	char board_name[NVME_DMI_STR_LEN];
};

static struct nvme_dmi_ids nvme_dmi;
static volatile int nvme_dmi_state; /* 0 unread, 1 read, -1 none */

static int nvme_smbios_checksum_ok(const uint8_t *p, uint32_t len)
{
	uint8_t sum = 0;

	for (uint32_t i = 0; i < len; i++)
		sum = (uint8_t)(sum + p[i]);
	return sum == 0;
}

/* String `idx' (1-based) of the structure whose formatted area is [s,
 * s + flen) and whose strings end before `end'; trailing blanks dropped
 * as the established DMI layer does. */
static void nvme_smbios_string(const uint8_t *s, uint32_t flen,
			       const uint8_t *end, uint8_t idx, char *out,
			       size_t outlen)
{
	const uint8_t *p = s + flen;
	uint8_t n = 1;
	size_t k = 0;

	out[0] = 0;
	if (!idx || !outlen)
		return;
	while (p < end && *p) {
		if (n == idx) {
			while (p < end && *p && k + 1 < outlen)
				out[k++] = (char)*p++;
			while (k && out[k - 1] == ' ')
				k--;
			out[k] = 0;
			return;
		}
		while (p < end && *p)
			p++;
		p++;
		n++;
	}
}

static void nvme_smbios_walk(const uint8_t *t, uint32_t len,
			     uint32_t max_count, struct nvme_dmi_ids *ids)
{
	uint32_t pos = 0, count = 0;

	while (pos + 4 <= len && (!max_count || count < max_count)) {
		uint8_t type = t[pos], flen = t[pos + 1];
		const uint8_t *s = t + pos, *end;
		uint32_t next;

		if (flen < 4 || pos + flen > len)
			break;
		next = pos + flen;
		while (next + 1 < len && (t[next] || t[next + 1]))
			next++;
		end = next + 1 >= len ? t + len : t + next + 1;

		if (type == 1) { /* system information */
			if (flen > 0x04)
				nvme_smbios_string(s, flen, end, s[0x04],
						   ids->sys_vendor,
						   sizeof(ids->sys_vendor));
			if (flen > 0x05)
				nvme_smbios_string(s, flen, end, s[0x05],
						   ids->product_name,
						   sizeof(ids->product_name));
		} else if (type == 2) { /* baseboard information */
			if (flen > 0x04)
				nvme_smbios_string(s, flen, end, s[0x04],
						   ids->board_vendor,
						   sizeof(ids->board_vendor));
			if (flen > 0x05)
				nvme_smbios_string(s, flen, end, s[0x05],
						   ids->board_name,
						   sizeof(ids->board_name));
		} else if (type == 127) {
			break; /* end of table */
		}
		pos = next + 2;
		count++;
	}
}

/* Decode an entry point ("_SM3_" 64-bit or "_SM_"/"_DMI_" 32-bit). */
static int nvme_smbios_entry(uint64_t phys, int want3, uint64_t *table,
			     uint32_t *tlen, uint32_t *tcount)
{
	const uint8_t *e;
	int ok = 0;

	if (!phys)
		return 0;
	e = AcpiOsMapMemory((ACPI_PHYSICAL_ADDRESS)phys, 0x20);
	if (!e)
		return 0;
	if (want3) {
		uint8_t elen = e[6];

		if (!kmemcmp(e, "_SM3_", 5) && elen >= 0x18 && elen <= 0x20 &&
		    nvme_smbios_checksum_ok(e, elen)) {
			*tlen = (uint32_t)e[0x0c] | ((uint32_t)e[0x0d] << 8) |
				((uint32_t)e[0x0e] << 16) |
				((uint32_t)e[0x0f] << 24);
			*table = 0;
			for (int b = 7; b >= 0; b--)
				*table = (*table << 8) | e[0x10 + b];
			*tcount = 0;
			ok = 1;
		}
	} else {
		uint8_t elen = e[5];

		if (!kmemcmp(e, "_SM_", 4) && elen >= 0x1f && elen <= 0x20 &&
		    nvme_smbios_checksum_ok(e, elen) &&
		    !kmemcmp(e + 0x10, "_DMI_", 5) &&
		    nvme_smbios_checksum_ok(e + 0x10, 0x0f)) {
			*tlen = (uint32_t)e[0x16] | ((uint32_t)e[0x17] << 8);
			*table = (uint64_t)e[0x18] | ((uint64_t)e[0x19] << 8) |
				 ((uint64_t)e[0x1a] << 16) |
				 ((uint64_t)e[0x1b] << 24);
			*tcount = (uint32_t)e[0x1c] | ((uint32_t)e[0x1d] << 8);
			ok = 1;
		}
	}
	AcpiOsUnmapMemory((void *)e, 0x20);
	return ok;
}

extern uint64_t g_boot_smbios_entry;
extern uint64_t g_boot_smbios3_entry;

static const struct nvme_dmi_ids *nvme_dmi_get(void)
{
	uint64_t table = 0;
	uint32_t tlen = 0, tcount = 0;
	const uint8_t *t;

	if (nvme_dmi_state > 0)
		return &nvme_dmi;
	if (nvme_dmi_state < 0)
		return NULL;
	if (!nvme_smbios_entry(g_boot_smbios3_entry, 1, &table, &tlen,
			       &tcount) &&
	    !nvme_smbios_entry(g_boot_smbios_entry, 0, &table, &tlen,
			       &tcount)) {
		nvme_dmi_state = -1;
		return NULL;
	}
	if (!table || tlen < 4) {
		nvme_dmi_state = -1;
		return NULL;
	}
	if (tlen > NVME_SMBIOS_TABLE_MAX)
		tlen = NVME_SMBIOS_TABLE_MAX;
	t = AcpiOsMapMemory((ACPI_PHYSICAL_ADDRESS)table, tlen);
	if (!t) {
		nvme_dmi_state = -1;
		return NULL;
	}
	kmemset(&nvme_dmi, 0, sizeof(nvme_dmi));
	nvme_smbios_walk(t, tlen, tcount, &nvme_dmi);
	AcpiOsUnmapMemory((void *)t, tlen);
	nvme_dmi_state = 1;
	return &nvme_dmi;
}

enum nvme_dmi_field {
	NVME_DMI_SYS_VENDOR,
	NVME_DMI_PRODUCT_NAME,
	NVME_DMI_BOARD_VENDOR,
	NVME_DMI_BOARD_NAME,
};

/* Exact match of a platform string, as the established DMI match. */
static bool nvme_dmi_match(enum nvme_dmi_field f, const char *str)
{
	const struct nvme_dmi_ids *ids = nvme_dmi_get();
	const char *v;

	if (!ids)
		return false;
	switch (f) {
	case NVME_DMI_SYS_VENDOR:
		v = ids->sys_vendor;
		break;
	case NVME_DMI_PRODUCT_NAME:
		v = ids->product_name;
		break;
	case NVME_DMI_BOARD_VENDOR:
		v = ids->board_vendor;
		break;
	case NVME_DMI_BOARD_NAME:
	default:
		v = ids->board_name;
		break;
	}
	return v[0] && !kstrcmp(v, str);
}

/* Drives that misbehave only on particular platforms. */
static unsigned long nvme_check_vendor_combination_bug(const pci_device_t *pdev)
{
	uint16_t vid = pdev->vendor_id, did = pdev->device_id;

	if (vid == 0x144d && did == 0xa802) {
		/* Samsung SM951/PM951/950 PRO drop off the bus with APST in
		 * the deepest state on two Dell laptops. */
		if (nvme_dmi_match(NVME_DMI_SYS_VENDOR, "Dell Inc.") &&
		    (nvme_dmi_match(NVME_DMI_PRODUCT_NAME, "XPS 15 9550") ||
		     nvme_dmi_match(NVME_DMI_PRODUCT_NAME, "Precision 5510")))
			return NVME_QUIRK_NO_DEEPEST_PS;
	} else if (vid == 0x144d && did == 0xa804) {
		/* Samsung SSD 960 EVO drops off after suspend on a Ryzen
		 * board, and within minutes on a Coffee Lake one. */
		if (nvme_dmi_match(NVME_DMI_BOARD_VENDOR,
				   "ASUSTeK COMPUTER INC.") &&
		    (nvme_dmi_match(NVME_DMI_BOARD_NAME, "PRIME B350M-A") ||
		     nvme_dmi_match(NVME_DMI_BOARD_NAME, "PRIME Z370-A")))
			return NVME_QUIRK_NO_APST;
	} else if ((vid == 0x144d &&
		    (did == 0xa801 || did == 0xa808 || did == 0xa809)) ||
		   (vid == 0x1e0f && did == 0x0001)) {
		/* Host-managed power settings for the lowest idle power on
		 * Samsung and Toshiba drives in a Lenovo C640. */
		if (nvme_dmi_match(NVME_DMI_BOARD_VENDOR, "LENOVO") &&
		    nvme_dmi_match(NVME_DMI_BOARD_NAME, "LNVNB161216"))
			return NVME_QUIRK_SIMPLE_SUSPEND;
	} else if (vid == 0x2646 && (did == 0x2263 || did == 0x500f)) {
		/* Kingston NV1 / A2000: a full suspend saves a lot of
		 * energy in s2idle on some TUXEDO platforms. */
		if (nvme_dmi_match(NVME_DMI_BOARD_NAME, "NS5X_NS7XAU") ||
		    nvme_dmi_match(NVME_DMI_BOARD_NAME, "NS5x_7xAU") ||
		    nvme_dmi_match(NVME_DMI_BOARD_NAME, "NS5x_7xPU") ||
		    nvme_dmi_match(NVME_DMI_BOARD_NAME, "PH4PRX1_PH6PRX1"))
			return NVME_QUIRK_FORCE_NO_SIMPLE_SUSPEND;
	} else if (vid == 0x144d && did == 0xa80d) {
		/* Samsung 990 Evo: more than 2 W in s2idle with the simple
		 * suspend on some boards with Intel CPUs. */
		if (nvme_dmi_match(NVME_DMI_BOARD_NAME, "DN50Z-140HC-YD") ||
		    nvme_dmi_match(NVME_DMI_BOARD_NAME, "GMxPXxx") ||
		    nvme_dmi_match(NVME_DMI_BOARD_NAME, "GXxMRXx") ||
		    nvme_dmi_match(NVME_DMI_BOARD_NAME, "NS5X_NS7XAU") ||
		    nvme_dmi_match(NVME_DMI_BOARD_NAME, "PH4PG31") ||
		    nvme_dmi_match(NVME_DMI_BOARD_NAME, "PH4PRX1_PH6PRX1") ||
		    nvme_dmi_match(NVME_DMI_BOARD_NAME, "PH6PG01_PH6PG71"))
			return NVME_QUIRK_FORCE_NO_SIMPLE_SUSPEND;
	}

	/* An NVMe SSD drops off the bus after 10 hours idle on a Lenovo
	 * N60z board. */
	if (nvme_dmi_match(NVME_DMI_BOARD_NAME, "LXKT-ZXEG-N6"))
		return NVME_QUIRK_NO_APST;
	return 0;
}

/* ==========================================================================
 * ACPI: StorageD3Enable
 * ========================================================================== */

/* The device-properties _DSD UUID daffd814-6eba-4d8c-8a91-bc9bbf4aa301 in
 * its buffer byte order. */
static const uint8_t nvme_acpi_dsd_uuid[16] = {
	0x14, 0xd8, 0xff, 0xda, 0xba, 0x6e, 0x8c, 0x4d,
	0x8a, 0x91, 0xbc, 0x9b, 0xbf, 0x4a, 0xa3, 0x01,
};

static int nvme_acpi_adr(ACPI_HANDLE h, uint64_t *out)
{
	ACPI_BUFFER buf = { ACPI_ALLOCATE_BUFFER, NULL };
	ACPI_HANDLE tmp;
	int rc = -ENOENT;

	if (ACPI_FAILURE(AcpiGetHandle(h, (char *)"_ADR", &tmp)))
		return -ENOENT;
	if (ACPI_FAILURE(AcpiEvaluateObject(h, (char *)"_ADR", NULL, &buf)))
		return -EIO;
	if (buf.Pointer) {
		ACPI_OBJECT *o = buf.Pointer;

		if (o->Type == ACPI_TYPE_INTEGER) {
			*out = o->Integer.Value;
			rc = 0;
		}
		AcpiOsFree(buf.Pointer);
	}
	return rc;
}

struct nvme_acpi_find {
	uint32_t adr; /* device << 16 | function */
	uint32_t parent_adr; /* the bridge's, when there is one */
	int check_parent;
	ACPI_HANDLE found;
};

static ACPI_STATUS nvme_acpi_find_cb(ACPI_HANDLE obj, UINT32 level, void *ctx,
				     void **ret)
{
	struct nvme_acpi_find *f = ctx;
	ACPI_HANDLE parent;
	uint64_t a;

	(void)level;
	(void)ret;
	if (nvme_acpi_adr(obj, &a) || (uint32_t)a != f->adr || (a >> 32))
		return AE_OK;
	if (f->check_parent) {
		if (ACPI_FAILURE(AcpiGetParent(obj, &parent)) ||
		    nvme_acpi_adr(parent, &a) || (uint32_t)a != f->parent_adr)
			return AE_OK;
	}
	f->found = obj;
	return AE_CTRL_TERMINATE;
}

/* The _DSD of `h': a package of (UUID buffer, property package) pairs;
 * the device-properties one holds ("StorageD3Enable", 1). */
static bool nvme_acpi_dsd_storage_d3(ACPI_HANDLE h)
{
	ACPI_BUFFER buf = { ACPI_ALLOCATE_BUFFER, NULL };
	ACPI_HANDLE tmp;
	ACPI_OBJECT *pkg;
	bool on = false;

	if (ACPI_FAILURE(AcpiGetHandle(h, (char *)"_DSD", &tmp)))
		return false;
	if (ACPI_FAILURE(AcpiEvaluateObject(h, (char *)"_DSD", NULL, &buf)) ||
	    !buf.Pointer)
		return false;
	pkg = buf.Pointer;
	if (pkg->Type != ACPI_TYPE_PACKAGE)
		goto out;
	for (UINT32 i = 0; i + 1 < pkg->Package.Count; i += 2) {
		ACPI_OBJECT *uuid = &pkg->Package.Elements[i];
		ACPI_OBJECT *props = &pkg->Package.Elements[i + 1];

		if (uuid->Type != ACPI_TYPE_BUFFER ||
		    uuid->Buffer.Length != 16 ||
		    kmemcmp(uuid->Buffer.Pointer, nvme_acpi_dsd_uuid, 16) ||
		    props->Type != ACPI_TYPE_PACKAGE)
			continue;
		for (UINT32 j = 0; j < props->Package.Count; j++) {
			ACPI_OBJECT *p = &props->Package.Elements[j];
			ACPI_OBJECT *name, *val;

			if (p->Type != ACPI_TYPE_PACKAGE ||
			    p->Package.Count != 2)
				continue;
			name = &p->Package.Elements[0];
			val = &p->Package.Elements[1];
			if (name->Type != ACPI_TYPE_STRING ||
			    kstrcmp(name->String.Pointer, "StorageD3Enable"))
				continue;
			if (val->Type == ACPI_TYPE_INTEGER)
				on = val->Integer.Value == 1;
			goto out;
		}
	}
out:
	AcpiOsFree(buf.Pointer);
	return on;
}

bool nvme_pci_acpi_storage_d3(const pci_device_t *pdev)
{
	struct nvme_acpi_find f;

	kmemset(&f, 0, sizeof(f));
	f.adr = ((uint32_t)pdev->device << 16) | pdev->function;
	if (pdev->bus_self) {
		f.check_parent = 1;
		f.parent_adr = ((uint32_t)pdev->bus_self->device << 16) |
			       pdev->bus_self->function;
	} else if (pdev->bus != 0) {
		return false; /* no way to tell which bus' device it is */
	}
	if (ACPI_FAILURE(AcpiWalkNamespace(ACPI_TYPE_DEVICE, ACPI_ROOT_OBJECT,
					   16, nvme_acpi_find_cb, NULL, &f,
					   NULL)) ||
	    !f.found)
		return false;
	return nvme_acpi_dsd_storage_d3(f.found);
}

unsigned long nvme_pci_platform_quirks(const pci_device_t *pdev)
{
	unsigned long quirks = nvme_check_vendor_combination_bug(pdev);

	/* The firmware asks for D3 of the storage device during suspend on
	 * platforms that support kernel-managed suspend. */
	if (!nvme_pci_param.noacpi &&
	    !(quirks & NVME_QUIRK_FORCE_NO_SIMPLE_SUSPEND) &&
	    nvme_pci_acpi_storage_d3(pdev)) {
		kprintf(NVME_PCI_QUIRK_LOG
			"%02x:%02x.%x: platform quirk: setting simple suspend\n",
			pdev->bus, pdev->device, pdev->function);
		quirks |= NVME_QUIRK_SIMPLE_SUSPEND;
	}
	return quirks;
}
