// LikeOS Intel audio DSP host driver - PCI side: the device table, the
// decision between this driver and the plain HDA driver, BAR mapping,
// interrupts, and the late boot that loads firmware and topology and
// publishes the sound card.
//
// Ownership: an Intel audio function with a DSP (class 0x0403 prog-if
// 0x80, or class 0x0401) can be driven either way.  The HDA driver probes
// first.  When it drives an analog codec, the device stays with it.  When
// the machine's speakers and microphones hang off SoundWire or PCH DMICs
// (the ACPI scan finds them), only the DSP can play them: this driver
// takes the controller over at late init - after making sure a firmware
// image is there - and boots the DSP.
//
// Copyright (C) 2026 The LikeOS Project

#include "sof_regs.h"
#include "hda_regs.h"
#include "sdw.h"

#include <kernel/hal/acpi.h>
#include <kernel/hal/lapic.h>
#include <kernel/ke/irq.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/userinit.h>

/* ===================================================================== */
/* platforms                                                              */
/* ===================================================================== */

#define CAVS_CHIP(nm, plat, ncores, hmask, flg, gen)                         \
	{                                                                    \
		.name = nm, .platform = plat, .cores = ncores,               \
		.init_core_mask = 1, .host_managed_mask = hmask,             \
		.ipc_layout = SOF_IPC_LAYOUT_CAVS,                           \
		.rom_status = SOF_CAVS_ROM_STATUS,                           \
		.rom_init_timeout_us = SOF_ROM_INIT_TIMEOUT_US,              \
		.ssp_count = 3, .ssp_base = SOF_CAVS_SSP_BASE,               \
		.d0i3c = SOF_HDA_VS_D0I3C, .flags = flg, .generation = gen, \
	}

#define ACE_CHIP(nm, plat, ncores, rom, flg, gen)                            \
	{                                                                    \
		.name = nm, .platform = plat, .cores = ncores,               \
		.init_core_mask = 1, .host_managed_mask = 1,                 \
		.ipc_layout = SOF_IPC_LAYOUT_ACE, .rom_status = rom,         \
		.rom_init_timeout_us = SOF_ROM_INIT_TIMEOUT_US,              \
		.ssp_count = 3, .ssp_base = SOF_ACE_SSP_BASE,                \
		.d0i3c = SOF_ACE_VS_D0I3C, .flags = SOF_CHIP_ACE | (flg),    \
		.generation = gen,                                           \
	}

static const struct sof_chip sof_chip_skl =
	CAVS_CHIP("skl", SOF_PLAT_SKL, 2, 0x3, SOF_CHIP_DECLINE, "cAVS 1.5");
static const struct sof_chip sof_chip_apl =
	CAVS_CHIP("apl", SOF_PLAT_APL, 2, 0x3, SOF_CHIP_DECLINE, "cAVS 1.5+");
static const struct sof_chip sof_chip_cnl =
	CAVS_CHIP("cnl", SOF_PLAT_CNL, 4, 0xf, SOF_CHIP_DECLINE, "cAVS 1.8");
static const struct sof_chip sof_chip_icl =
	CAVS_CHIP("icl", SOF_PLAT_ICL, 4, 0xf, SOF_CHIP_DECLINE, "cAVS 2.0");
static const struct sof_chip sof_chip_jsl =
	CAVS_CHIP("jsl", SOF_PLAT_ICL, 2, 0x3, SOF_CHIP_DECLINE, "cAVS 2.0");
static const struct sof_chip sof_chip_tgl =
	CAVS_CHIP("tgl", SOF_PLAT_TGL, 4, 0x1, SOF_CHIP_ICCMAX, "cAVS 2.5");
static const struct sof_chip sof_chip_tglh =
	CAVS_CHIP("tgl-h", SOF_PLAT_TGL, 2, 0x1, SOF_CHIP_ICCMAX, "cAVS 2.5");
static const struct sof_chip sof_chip_ehl =
	CAVS_CHIP("ehl", SOF_PLAT_TGL, 4, 0x1, SOF_CHIP_ICCMAX, "cAVS 2.5");
static const struct sof_chip sof_chip_adls =
	CAVS_CHIP("adl-s", SOF_PLAT_TGL, 2, 0x1, SOF_CHIP_ICCMAX, "cAVS 2.5");
static const struct sof_chip sof_chip_mtl =
	ACE_CHIP("mtl", SOF_PLAT_MTL, 3, SOF_ACE1_ROM_STATUS, 0, "ACE 1.0");
static const struct sof_chip sof_chip_arls =
	ACE_CHIP("arl-s", SOF_PLAT_MTL, 2, SOF_ACE1_ROM_STATUS, 0, "ACE 1.0");
static const struct sof_chip sof_chip_lnl =
	ACE_CHIP("lnl", SOF_PLAT_LNL, 5, SOF_ACE2_ROM_STATUS, SOF_CHIP_ACE2,
		 "ACE 2.0");
static const struct sof_chip sof_chip_ptl =
	ACE_CHIP("ptl", SOF_PLAT_PTL, 5, SOF_ACE2_ROM_STATUS,
		 SOF_CHIP_ACE2 | SOF_CHIP_ACE3, "ACE 3.0");
static const struct sof_chip sof_chip_wcl =
	ACE_CHIP("wcl", SOF_PLAT_PTL, 3, SOF_ACE2_ROM_STATUS,
		 SOF_CHIP_ACE2 | SOF_CHIP_ACE3, "ACE 3.0");
static const struct sof_chip sof_chip_nvl =
	ACE_CHIP("nvl", SOF_PLAT_PTL, 5, SOF_ACE2_ROM_STATUS,
		 SOF_CHIP_ACE2 | SOF_CHIP_ACE3 | SOF_CHIP_DECLINE |
			 SOF_CHIP_EXPERIMENTAL,
		 "ACE 4.0");

#define SOF_IPC3_ONLY "only IPC3/closed firmware exists for this generation"
#define SOF_NO_ACE4 "ACE 4.x is not supported yet"

#define ID(dev, chip, nm, tg, decline)                                         \
	{ 0x8086, dev, &chip, nm, tg, "intel/sof-ipc4/" tg, "sof-" tg ".ri",  \
	  "intel/sof-ipc4-lib/" tg, decline }

static const struct sof_pci_id sof_pci_ids[] = {
	/* cAVS 1.5: Skylake, Kaby Lake (declined) */
	ID(0x9d70, sof_chip_skl, "Sunrise Point-LP", "skl", SOF_IPC3_ONLY),
	ID(0xa170, sof_chip_skl, "Sunrise Point-H", "skl", SOF_IPC3_ONLY),
	ID(0x9d71, sof_chip_skl, "Kaby Lake-LP", "kbl", SOF_IPC3_ONLY),
	ID(0xa171, sof_chip_skl, "Kaby Lake", "kbl", SOF_IPC3_ONLY),
	ID(0xa2f0, sof_chip_skl, "Union Point-H", "kbl", SOF_IPC3_ONLY),
	/* cAVS 1.5+: Apollo Lake, Gemini Lake (declined) */
	ID(0x5a98, sof_chip_apl, "Apollo Lake", "apl", SOF_IPC3_ONLY),
	ID(0x1a98, sof_chip_apl, "Broxton-T", "apl", SOF_IPC3_ONLY),
	ID(0x3198, sof_chip_apl, "Gemini Lake", "glk", SOF_IPC3_ONLY),
	/* cAVS 1.8: Cannon, Coffee, Whiskey, Comet Lake (declined) */
	ID(0x9dc8, sof_chip_cnl, "Cannon Point-LP", "cnl", SOF_IPC3_ONLY),
	ID(0xa348, sof_chip_cnl, "Cannon Point-H", "cfl", SOF_IPC3_ONLY),
	ID(0x02c8, sof_chip_cnl, "Comet Lake-LP", "cml", SOF_IPC3_ONLY),
	ID(0x06c8, sof_chip_cnl, "Comet Lake-H", "cml", SOF_IPC3_ONLY),
	ID(0xa3f0, sof_chip_cnl, "Comet Lake-S", "cml", SOF_IPC3_ONLY),
	ID(0xf0c8, sof_chip_cnl, "Comet Lake-R", "cml", SOF_IPC3_ONLY),
	/* cAVS 2.0: Ice Lake, Jasper Lake, Lakefield (declined) */
	ID(0x34c8, sof_chip_icl, "Ice Lake-LP", "icl", SOF_IPC3_ONLY),
	ID(0x3dc8, sof_chip_icl, "Ice Lake-H", "icl", SOF_IPC3_ONLY),
	ID(0x98c8, sof_chip_icl, "Lakefield", "lkf", SOF_IPC3_ONLY),
	ID(0x38c8, sof_chip_jsl, "Ice Lake-N", "jsl", SOF_IPC3_ONLY),
	ID(0x4dc8, sof_chip_jsl, "Jasper Lake", "jsl", SOF_IPC3_ONLY),
	/* cAVS 2.5: Tiger, Elkhart, Rocket, Alder, Raptor Lake */
	ID(0xa0c8, sof_chip_tgl, "Tiger Lake-LP", "tgl", NULL),
	ID(0x43c8, sof_chip_tglh, "Tiger Lake-H", "tgl-h", NULL),
	ID(0xf1c8, sof_chip_tglh, "Rocket Lake-S", "tgl-h", NULL),
	ID(0x4b55, sof_chip_ehl, "Elkhart Lake", "ehl", NULL),
	ID(0x4b58, sof_chip_ehl, "Elkhart Lake", "ehl", NULL),
	ID(0x7ad0, sof_chip_adls, "Alder Lake-S", "adl-s", NULL),
	ID(0x7a50, sof_chip_adls, "Raptor Lake-S", "rpl-s", NULL),
	ID(0x51c8, sof_chip_tgl, "Alder Lake-P", "adl", NULL),
	ID(0x51c9, sof_chip_tgl, "Alder Lake-PS", "adl", NULL),
	ID(0x51cc, sof_chip_tgl, "Alder Lake-M", "adl", NULL),
	ID(0x51cd, sof_chip_tgl, "Alder Lake-PX", "adl", NULL),
	ID(0x54c8, sof_chip_tgl, "Alder Lake-N", "adl-n", NULL),
	ID(0x51ca, sof_chip_tgl, "Raptor Lake-P", "rpl", NULL),
	ID(0x51cb, sof_chip_tgl, "Raptor Lake-P", "rpl", NULL),
	ID(0x51ce, sof_chip_tgl, "Raptor Lake-M", "rpl", NULL),
	ID(0x51cf, sof_chip_tgl, "Raptor Lake-PX", "rpl", NULL),
	/* ACE 1.x: Meteor Lake, Arrow Lake */
	ID(0x7e28, sof_chip_mtl, "Meteor Lake", "mtl", NULL),
	ID(0x7728, sof_chip_mtl, "Arrow Lake-H", "arl", NULL),
	ID(0x7f50, sof_chip_arls, "Arrow Lake-S", "arl-s", NULL),
	/* ACE 2.x: Lunar Lake */
	ID(0xa828, sof_chip_lnl, "Lunar Lake", "lnl", NULL),
	/* ACE 3.x: Panther Lake, Wildcat Lake */
	ID(0xe428, sof_chip_ptl, "Panther Lake", "ptl", NULL),
	ID(0xe328, sof_chip_ptl, "Panther Lake-H", "ptl", NULL),
	ID(0x4d28, sof_chip_wcl, "Wildcat Lake", "wcl", NULL),
	/* ACE 4.x: Nova Lake (declined) */
	ID(0xd328, sof_chip_nvl, "Nova Lake", "nvl", SOF_NO_ACE4),
	ID(0x6e50, sof_chip_nvl, "Nova Lake-S", "nvl-s", SOF_NO_ACE4),
};

static const struct sof_pci_id *sof_pci_lookup(const pci_device_t *d)
{
	size_t i;

	for (i = 0; i < sizeof(sof_pci_ids) / sizeof(sof_pci_ids[0]); i++)
		if (sof_pci_ids[i].vendor == d->vendor_id &&
		    sof_pci_ids[i].device == d->device_id)
			return &sof_pci_ids[i];
	return NULL;
}

static struct sof_host *g_sof_hosts;

/* ===================================================================== */
/* interrupts                                                             */
/* ===================================================================== */

static int sof_irq(void *arg)
{
	struct sof_host *h = arg;
	uint32_t intsts;
	int ipc = 0, sdw = 0;

	if (!h || !h->s.hda || !h->taken)
		return 0;
	intsts = sof_hda_r32(h, SOF_HDA_INTSTS);
	if (intsts == 0xffffffffu)
		return 0; /* the device is gone */
	if (h->s.dsp && h->dsp_powered) {
		ipc = sof_dsp_ipc_irq_pending(h);
		if (h->sdw_up)
			sdw = sof_dsp_sdw_irq_pending(h);
	}
	if (!(intsts & SOF_HDA_INTSTS_GIS) && !ipc && !sdw) {
		h->irq_spurious++;
		return 0;
	}
	h->irq_count++;

	if (h->dsp_powered && sof_ipc4_service(h))
		h->irq_ipc++;
	if (intsts & SOF_HDA_INTCTL_SIE_MASK) {
		h->irq_stream++;
		sof_streams_irq(h, intsts & SOF_HDA_INTCTL_SIE_MASK);
	}
	if (intsts & SOF_HDA_INTSTS_CIS) {
		/* no command rings here: just quiet the controller bits */
		uint8_t rsts = sof_hda_r8(h, SOF_HDA_RIRBSTS);
		uint16_t ssts = sof_hda_r16(h, SOF_HDA_STATESTS);

		if (rsts && rsts != 0xff)
			sof_hda_w8(h, SOF_HDA_RIRBSTS, rsts & SOF_HDA_RIRBSTS_MASK);
		if (ssts && ssts != 0xffff)
			sof_hda_w16(h, SOF_HDA_STATESTS,
				    ssts & SOF_HDA_STATESTS_MASK);
	}
	if (sdw) {
		h->irq_sdw++;
		sdw_intel_irq(&h->s);
	}
	return 1;
}

static int sof_gsi_lookup(struct sof_host *h, uint32_t *gsi)
{
	static const char *const roots[] = { "\\_SB_.PC00", "\\_SB_.PCI0",
					     "\\\\_SB_.PC00", "\\\\_SB_.PCI0" };
	const pci_device_t *d = h->s.pci;
	uint8_t pin, dev;
	size_t i;

	if (d->interrupt_pin < 1 || d->interrupt_pin > 4)
		return -ENOENT;
	pin = (uint8_t)(d->interrupt_pin - 1);
	dev = d->device;
	if (d->bus != 0) {
		const pci_device_t *br = pci_find_bridge_for_bus(d->bus);

		if (!br)
			return -ENOENT;
		pin = (uint8_t)((pin + d->device) % 4);
		dev = br->device;
	}
	for (i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
		if (acpi_pci_lookup_irq(roots[i], dev, pin, gsi) == 0)
			return 0;
	return -ENOENT;
}

static void sof_irq_setup(struct sof_host *h)
{
	int vec = -1, rc;
	uint32_t gsi = 0;

	if (h->irq_registered)
		return;
	rc = irq_request_msi(h->s.pci, 0, sof_irq, h, "sof", &vec);
	if (rc == 0) {
		h->msi = 1;
		h->s.irq_vector = vec;
		h->irq_registered = 1;
		return;
	}
	if (sof_gsi_lookup(h, &gsi) != 0) {
		uint8_t line = h->s.pci->interrupt_line;

		if (line == 0xff || line == 0) {
			sof_log(h, "no interrupt route; running on polling (PCM periods will not be reported)\n");
			return;
		}
		gsi = line;
	}
	/* INTx must not be masked in the command register */
	{
		uint16_t cmd = pci_cfg_read16(h->s.pci, 0x04);

		if (cmd & PCI_CMD_INTX_DISABLE)
			pci_cfg_write16(h->s.pci, 0x04,
					(uint16_t)(cmd & ~PCI_CMD_INTX_DISABLE));
	}
	rc = irq_request_gsi((int)gsi, sof_irq, h, "sof", 1, 1);
	if (rc) {
		sof_log(h, "cannot claim GSI %u: %d\n", gsi, rc);
		return;
	}
	h->gsi = (int)gsi;
	h->s.irq_vector = 32 + (int)gsi;
	h->irq_registered = 1;
}

/* ===================================================================== */
/* probe                                                                  */
/* ===================================================================== */

static int sof_map_bar(struct sof_host *h, int index, volatile uint8_t **virt,
		       uint64_t *phys, uint64_t *size, uint32_t *mapped)
{
	struct pci_bar bar;
	uint64_t len, v;

	if (pci_bar_decode(h->s.pci, index, &bar) != 0 ||
	    (bar.flags & PCI_BAR_IO) || !bar.base || !bar.size)
		return -ENODEV;
	len = bar.size;
	if (len > (4u << 20))
		len = 4u << 20; /* nothing of interest beyond 4 MiB */
	if (len < 0x1000)
		len = 0x1000;
	v = mm_map_mmio_flags(bar.base, (size_t)(len / 4096), MM_MMIO_UC);
	if (!v)
		return -ENOMEM;
	*virt = (volatile uint8_t *)v;
	*phys = bar.base;
	*size = bar.size;
	*mapped = (uint32_t)len;
	return 0;
}

static void sof_unmap(struct sof_host *h)
{
	if (h->s.hda)
		mm_unmap_mmio((uint64_t)(uintptr_t)h->s.hda,
			      h->bar0_map_size / 4096);
	if (h->s.dsp)
		mm_unmap_mmio((uint64_t)(uintptr_t)h->s.dsp,
			      h->bar4_map_size / 4096);
	h->s.hda = NULL;
	h->s.dsp = NULL;
}

/* The device is ours: put the controller into the state the DSP needs. */
static int sof_take_controller(struct sof_host *h)
{
	uint8_t tcsel;
	int rc;

	pci_enable_busmaster_mem(h->s.pci);
	/* traffic class 0 for the DMA engines */
	tcsel = pci_cfg_read8(h->s.pci, SOF_PCI_TCSEL);
	pci_cfg_write8(h->s.pci, SOF_PCI_TCSEL, (uint8_t)(tcsel & ~0x07));

	rc = sof_hda_enum_caps(h);
	if (rc) {
		sof_log(h, "no processing-pipe capability: no usable DSP\n");
		return rc;
	}
	h->addr64 = (sof_hda_r16(h, SOF_HDA_GCAP) & SOF_HDA_GCAP_64OK) ? 1 : 0;
	if (!h->nstreams) {
		h->gcap = sof_hda_r16(h, SOF_HDA_GCAP);
		h->num_iss = (h->gcap >> 8) & 0xf;
		h->num_oss = (h->gcap >> 12) & 0xf;
		h->nstreams = h->num_iss + h->num_oss;
	}
	rc = sof_hda_init_chip(h);
	if (rc)
		return rc;
	h->taken = 1;
	return 0;
}

/* Everything the boot needs beyond the reset controller. */
static int sof_finish_takeover(struct sof_host *h)
{
	int rc = sof_streams_init(h);

	if (rc)
		return rc;
	sof_ipc4_init_host(h);
	sof_irq_setup(h);
	sof_pp_enable(h, 1);
	sof_pp_int_enable(h, 1);
	if (sof_is_ace2(h))
		sof_ml_set_offload(h, 1);
	return 0;
}

static int sof_machine_has_dsp_audio(const struct sof_machine *m)
{
	/* SoundWire peripherals or PCH microphones: only the DSP reaches
	 * them.  An SSP (Bluetooth) link alone is no reason to take over. */
	return m->sdw_link_mask || m->nperiph > 0 || m->dmic_count > 0;
}

static void sof_host_free(struct sof_host *h)
{
	sof_unmap(h);
	kfree(h);
}

static int sof_probe_one(const pci_device_t *d, const struct sof_pci_id *id)
{
	struct sof_host *h;
	int owned, rc;
	char fw_lib_save[96];

	owned = hda_controller_dsp_owned(d);
	if (owned == 0) {
		kprintf("[snd] sof: %02x:%02x.%x %s: the HDA driver drives its codecs; DSP left unused\n",
			d->bus, d->device, d->function, id->name);
		return 0;
	}

	h = kcalloc(1, sizeof(*h));
	if (!h)
		return -ENOMEM;
	h->s.pci = d;
	h->s.platform = id->chip->platform;
	h->s.irq_vector = -1;
	h->id = id;
	h->chip = id->chip;
	h->prep_pcm = -1;
	h->prep_dir = -1;
	h->boot_state = SOF_BOOT_NONE;
	spinlock_init(&h->reg_lock, "sof-regs");
	spinlock_init(&h->s.ipc_lock, "sof-ipc");
	sof_mutex_init(&h->ctl_mutex, "sof-ctl");
	sof_mutex_init(&h->pcm_mutex, "sof-pcm");
	ksnprintf(h->bus, sizeof(h->bus), "pci:0000:%02x:%02x.%x", d->bus,
		  d->device, d->function);
	ksnprintf(h->name, sizeof(h->name), "%s %02x:%02x.%x", id->name, d->bus,
		  d->device, d->function);

	rc = sof_map_bar(h, 0, &h->s.hda, &h->s.hda_phys, &h->s.hda_size,
			 &h->bar0_map_size);
	if (rc) {
		sof_log(h, "BAR0 unusable (%d)\n", rc);
		goto fail;
	}
	rc = sof_map_bar(h, 4, &h->s.dsp, &h->s.dsp_phys, &h->s.dsp_size,
			 &h->bar4_map_size);
	if (rc) {
		sof_log(h, "no DSP register BAR (%d): DSP disabled by firmware?\n",
			rc);
		goto fail;
	}
	if (sof_hda_r32(h, SOF_HDA_GCAP) == 0xffffffffu) {
		sof_log(h, "controller does not respond\n");
		rc = -ENODEV;
		goto fail;
	}

	if (owned < 0) {
		/* nobody drives it yet: reset it so the codec mask is real */
		rc = sof_take_controller(h);
		if (rc)
			goto fail;
	} else {
		/* the HDA driver keeps it until the firmware is there; only
		 * read-only accesses now */
		h->hda_release_pending = 1;
		sof_hda_enum_caps(h);
	}

	rc = sof_acpi_scan(&h->s);
	if (rc < 0 || !sof_machine_has_dsp_audio(&h->s.machine)) {
		sof_log(h, "no SoundWire or DMIC audio described (%d); leaving the device to the HDA driver\n",
			rc);
		if (h->taken)
			sof_hda_stop_chip(h);
		rc = 0;
		goto fail;
	}
	if (!h->s.machine.fw_lib_path[0]) {
		sof_strlcpy(fw_lib_save, id->lib_dir, sizeof(fw_lib_save));
		sof_strlcpy(h->s.machine.fw_lib_path, fw_lib_save,
			    sizeof(h->s.machine.fw_lib_path));
	}

	sof_log(h, "%s (%s) at %s: SoundWire links %#x, %d peripherals, %u DMICs, SSP %#x, HDA codecs %#x%s\n",
		id->name, h->chip->generation, h->bus, h->s.machine.sdw_link_mask,
		h->s.machine.nperiph, h->s.machine.dmic_count,
		h->s.machine.ssp_mask, h->s.machine.hda_codec_mask,
		h->hda_release_pending ? " (taken over at boot)" : "");

	if (h->taken) {
		rc = sof_finish_takeover(h);
		if (rc) {
			sof_log(h, "controller setup failed: %d\n", rc);
			sof_hda_stop_chip(h);
			goto fail;
		}
	}
	h->boot_state = SOF_BOOT_PROBED;
	h->next = g_sof_hosts;
	g_sof_hosts = h;
	return 1;

fail:
	sof_host_free(h);
	return rc;
}

int sof_probe_all(void)
{
	const pci_device_t *devs;
	int n = 0, i, found = 0;

	devs = pci_get_devices(&n);
	if (!devs)
		return 0;
	for (i = 0; i < n; i++) {
		const pci_device_t *d = &devs[i];
		const struct sof_pci_id *id;
		int dsp_class;

		if (d->vendor_id != 0x8086 || d->class_code != 0x04)
			continue;
		dsp_class = (d->subclass == 0x03 && d->prog_if == 0x80) ||
			    d->subclass == 0x01;
		if (!dsp_class)
			continue;
		id = sof_pci_lookup(d);
		if (!id) {
			kprintf("[snd] sof: %02x:%02x.%x: Intel audio DSP %04x not in the table\n",
				d->bus, d->device, d->function, d->device_id);
			continue;
		}
		if (id->decline_reason) {
			kprintf("[snd] sof: %02x:%02x.%x %s (%s): declined, %s\n",
				d->bus, d->device, d->function, id->name,
				id->chip->generation, id->decline_reason);
			continue;
		}
		if (sof_probe_one(d, id) > 0)
			found++;
	}
	/* the cards appear when sof_late_init() booted the DSPs */
	(void)found;
	return 0;
}

/* ===================================================================== */
/* boot                                                                   */
/* ===================================================================== */

#define SOF_FT_JACK (1u << 0)
#define SOF_FT_AMP (1u << 1)
#define SOF_FT_MIC (1u << 2)

/* SDCA function types */
#define SDCA_FN_SMART_AMP 0x01
#define SDCA_FN_SIMPLE_AMP 0x02
#define SDCA_FN_SMART_MIC 0x03
#define SDCA_FN_SIMPLE_MIC 0x04
#define SDCA_FN_SPEAKER_MIC 0x05
#define SDCA_FN_UAJ 0x06
#define SDCA_FN_RJ 0x07
#define SDCA_FN_SIMPLE_JACK 0x08

/* What a peripheral contributes: SDCA function bits when the scan
 * reported them, the part number otherwise. */
static uint32_t sof_periph_features(const struct sof_sdw_peripheral *p)
{
	uint32_t f = 0, fn = p->functions;

	if (fn) {
		if (fn & ((1u << SDCA_FN_UAJ) | (1u << SDCA_FN_RJ) |
			  (1u << SDCA_FN_SIMPLE_JACK)))
			f |= SOF_FT_JACK;
		if (fn & ((1u << SDCA_FN_SMART_AMP) | (1u << SDCA_FN_SIMPLE_AMP)))
			f |= SOF_FT_AMP;
		if (fn & ((1u << SDCA_FN_SMART_MIC) | (1u << SDCA_FN_SIMPLE_MIC) |
			  (1u << SDCA_FN_SPEAKER_MIC)))
			f |= SOF_FT_MIC;
		return f;
	}
	switch (p->part_id) {
	case 0x4243: /* cs42l43: headset, speaker amp, microphones */
	case 0x4245:
	case 0x0722:
	case 0x0721:
		return SOF_FT_JACK | SOF_FT_AMP | SOF_FT_MIC;
	case 0x0711:
	case 0x4242:
		return SOF_FT_JACK;
	case 0x0712:
	case 0x0713:
		return SOF_FT_JACK | SOF_FT_MIC;
	case 0x0714:
	case 0x0715:
		return SOF_FT_MIC;
	case 0x1308:
	case 0x1316:
	case 0x1318:
	case 0x1320:
	case 0x1712:
	case 0x1713:
	case 0x3556:
	case 0x3557:
	case 0x3563:
	case 0x8373:
	case 0x8363:
		return SOF_FT_AMP;
	default:
		return 0;
	}
}

/* sof_acpi.c: the machine table asks for its topology before the
 * function topologies */

/* Every IPC4 platform keeps its topologies in one directory. */
#define SOF_TPLG_DIR "intel/sof-ipc4-tplg"

static int sof_try_tplg(struct sof_host *h, const char *file)
{
	char path[160];
	int rc;

	if (kstrncmp(file, "intel/", 6) == 0)
		sof_strlcpy(path, file, sizeof(path));
	else
		ksnprintf(path, sizeof(path), "%s/%s", SOF_TPLG_DIR, file);
	rc = sof_tplg_load(&h->s, path);
	if (rc == 0 && h->ntplg < 6)
		sof_strlcpy(h->tplg_files[h->ntplg++], path,
			    sizeof(h->tplg_files[0]));
	return rc;
}

/* Function topologies: one file per audio function of a SoundWire
 * machine, combined at load time.  They exist for the ACE platforms only
 * and are named:
 *   sof-sdca-jack-id0.tplg          SDCA (or SDCA-like) headset jack
 *   sof-sdca-<N>amp-id2.tplg        speaker amplifiers on N links (1..4)
 *   sof-sdca-mic-id4.tplg           SoundWire microphones
 *   sof-<plat>-dmic-<2|4>ch-id4.tplg  PCH DMICs when no SoundWire mic
 *   sof-hdmi-pcm5-id5.tplg          HDMI/DP outputs
 * Returns the number of names written to `files', 0 when the machine is
 * not served this way (cAVS, no SoundWire codec, a board the machine
 * table insists on, or an amplifier layout no topology covers). */
static int sof_function_tplg_names(struct sof_host *h, char files[][64],
				   int max)
{
	const struct sof_machine *m = &h->s.machine;
	uint32_t feat = 0, amp_links = 0, dedicated_links = 0;
	int nfiles = 0, i, amps = 0;
	char plat[4];

	if (!sof_is_ace(h) || m->nperiph <= 0 || max < 5)
		return 0;
	if (sof_acpi_machine_tplg_first(&h->s)) {
		sof_log(h, "this machine uses its machine topology\n");
		return 0;
	}
	for (i = 0; i < m->nperiph && i < SOF_MAX_SDW_PERIPHERALS; i++) {
		const struct sof_sdw_peripheral *p = &m->periph[i];
		uint32_t f = sof_periph_features(p);

		feat |= f;
		if ((f & SOF_FT_AMP) && p->link < 32) {
			amp_links |= 1u << p->link;
			if (!(f & (SOF_FT_JACK | SOF_FT_MIC)))
				dedicated_links |= 1u << p->link;
		}
	}
	if (!feat)
		return 0;
	/* The speaker topology aggregates one DAI per amplifier link; a
	 * headset codec's own amplifier only counts when nothing dedicated
	 * drives the speakers. */
	if (dedicated_links)
		amp_links = dedicated_links;
	for (i = 0; i < 32; i++)
		if (amp_links & (1u << i))
			amps++;
	if (amps > 4) {
		sof_log(h, "amplifiers on %d links: no function topology for that\n",
			amps);
		return 0;
	}

	/* "arl-s" -> "arl", "tgl-h" -> "tgl" */
	sof_strlcpy(plat, h->id->tag, sizeof(plat));
	if (feat & SOF_FT_JACK)
		ksnprintf(files[nfiles++], 64, "sof-sdca-jack-id0.tplg");
	if ((feat & SOF_FT_AMP) && amps > 0)
		ksnprintf(files[nfiles++], 64, "sof-sdca-%damp-id2.tplg", amps);
	if (feat & SOF_FT_MIC)
		ksnprintf(files[nfiles++], 64, "sof-sdca-mic-id4.tplg");
	else if (m->dmic_count == 2 || m->dmic_count == 4)
		ksnprintf(files[nfiles++], 64, "sof-%s-dmic-%uch-id4.tplg",
			  plat, m->dmic_count);
	else if (m->dmic_count)
		sof_log(h, "%u DMICs: no DMIC function topology for that count\n",
			m->dmic_count);
	if (m->hda_codec_mask & (1u << 2))
		ksnprintf(files[nfiles++], 64, "sof-hdmi-pcm5-id5.tplg");
	return nfiles;
}

/* Function topologies (one per audio function the machine has) first;
 * when one of them is missing, the machine's single topology. */
static int sof_load_topologies(struct sof_host *h)
{
	const struct sof_machine *m = &h->s.machine;
	char files[5][64];
	int nfiles, i, rc;

	nfiles = sof_function_tplg_names(h, files, 5);
	if (nfiles) {
		for (i = 0; i < nfiles; i++) {
			rc = sof_try_tplg(h, files[i]);
			if (rc) {
				sof_log(h, "function topology %s: %d; using the machine topology\n",
					files[i], rc);
				break;
			}
		}
		if (i == nfiles) {
			sof_log(h, "%d function topologies loaded\n", nfiles);
			return 0;
		}
		sof_tplg_free(&h->s);
		h->ntplg = 0;
	}

	if (m->tplg_name[0]) {
		rc = sof_try_tplg(h, m->tplg_name);
		if (rc == 0) {
			sof_log(h, "topology %s loaded\n", h->tplg_files[0]);
			return 0;
		}
		sof_log(h, "topology %s: %d\n", m->tplg_name, rc);
		sof_tplg_free(&h->s);
		h->ntplg = 0;
	}
	/* the machine scan's further suggestions, first one that loads */
	rc = -ENOENT;
	for (i = 0; i < 16; i++) {
		const char *c = sof_acpi_tplg_candidate(&h->s, i);

		if (!c)
			break;
		if (!c[0] || kstrcmp(c, m->tplg_name) == 0)
			continue;
		rc = sof_try_tplg(h, c);
		if (rc == 0) {
			sof_log(h, "topology %s loaded\n", h->tplg_files[0]);
			return 0;
		}
		sof_tplg_free(&h->s);
		h->ntplg = 0;
	}
	sof_log(h, "no topology could be loaded (%d)\n", rc);
	return rc;
}

static void sof_boot_run(void *arg)
{
	struct sof_host *h = arg;
	int rc;

	if (h->hda_release_pending) {
		/* do not take the device from the HDA driver for nothing */
		rc = sof_fw_probe_file(h);
		if (rc) {
			sof_log(h, "no firmware (%d); the HDA driver keeps the device\n",
				rc);
			h->boot_state = SOF_BOOT_FAILED;
			h->boot_error = rc;
			return;
		}
		rc = hda_controller_release(h->s.pci);
		if (rc) {
			sof_log(h, "the HDA driver did not release the device (%d)\n",
				rc);
			h->boot_state = SOF_BOOT_FAILED;
			h->boot_error = rc;
			return;
		}
		h->hda_release_pending = 0;
		rc = sof_take_controller(h);
		if (rc == 0)
			rc = sof_finish_takeover(h);
		if (rc) {
			sof_log(h, "taking the controller over failed: %d\n", rc);
			h->boot_state = SOF_BOOT_FAILED;
			h->boot_error = rc;
			return;
		}
	}
	if (!h->taken) {
		h->boot_state = SOF_BOOT_FAILED;
		return;
	}

	rc = sof_fw_boot(h);
	if (rc) {
		sof_log(h, "DSP boot failed: %d\n", rc);
		return;
	}

	rc = sof_load_topologies(h);
	if (rc) {
		sof_log(h, "no usable topology: no sound card\n");
		h->boot_state = SOF_BOOT_FAILED;
		h->boot_error = rc;
		return;
	}

	if (h->s.machine.sdw_link_mask) {
		rc = sdw_intel_init(&h->s);
		if (rc) {
			sof_log(h, "SoundWire bring-up failed: %d\n", rc);
		} else {
			h->sdw_up = 1;
			sof_dsp_sdw_int_enable(h, 1);
		}
	}

	rc = sof_card_create(h);
	if (rc) {
		sof_log(h, "no sound card: %d\n", rc);
		h->boot_state = SOF_BOOT_FAILED;
		h->boot_error = rc;
		return;
	}
	h->boot_state = SOF_BOOT_RUNNING;
}

/* The boot is late boot work the login prompt waits for (its firmware,
 * topology, SoundWire and card lines all print after the root mount). */
static void sof_boot_work(void *arg)
{
	sof_boot_run(arg);
	boot_work_end();
}

void sof_late_init(void)
{
	struct sof_host *h;

	g_sof_can_block = 1;
	for (h = g_sof_hosts; h; h = h->next) {
		if (h->boot_state != SOF_BOOT_PROBED)
			continue;
		boot_work_begin();
		if (snd_schedule_work(sof_boot_work, h, 0) < 0)
			sof_boot_work(h);
	}
}
