// LikeOS Intel audio DSP host driver - firmware loading.
//
// An IPC4 firmware file (.ri) starts with an extended manifest ("$AE1")
// that the host strips; what follows is streamed into the DSP.  At a fixed
// distance behind the extended manifest sits the firmware header ("$AM1")
// with the version and the module table: one entry per processing module
// (copier, gain, mixer, ...) with its UUID, which is how a topology names
// a module.  The module's position in the table is its IPC4 module id;
// modules of a loadable library carry the library number in bits 12+.
//
// The boot sequence:
//   1. power the DSP (subsystem, core power domain) and keep it ungated,
//   2. claim an HDA output DMA engine as the code loader, put the image
//      into DMA memory behind it, and tell the ROM through the doorbell to
//      purge and fetch from that engine's stream tag,
//   3. start core 0, wait for the ROM's handshake and its INIT_DONE state,
//   4. run the DMA until the ROM reports that the firmware was entered,
//   5. wait for the FW_READY notification,
//   6. query the firmware configuration and load optional split-release
//      libraries.
// Libraries later go the same way through LOAD_LIBRARY(_PREPARE).
//
// Copyright (C) 2026 The LikeOS Project

#include "sof_regs.h"
#include "sof_tplg_internal.h"

#include <kernel/hal/lapic.h>
#include <kernel/ke/firmware.h>
#include <kernel/mm/memory.h>

/* ===================================================================== */
/* helpers                                                                */
/* ===================================================================== */

void sof_uuid_str(const uint8_t u[16], char *out, size_t len)
{
	/* little-endian GUID notation, the way the library files are named */
	ksnprintf(out, len,
		  "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
		  u[3], u[2], u[1], u[0], u[5], u[4], u[7], u[6], u[8], u[9],
		  u[10], u[11], u[12], u[13], u[14], u[15]);
}

static int sof_uuid_eq(const uint8_t *a, const uint8_t *b)
{
	return kmemcmp(a, b, 16) == 0;
}

static int sof_uuid_null(const uint8_t *a)
{
	int i;

	for (i = 0; i < 16; i++)
		if (a[i])
			return 0;
	return 1;
}

static uint32_t sof_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

/* ===================================================================== */
/* manifest                                                               */
/* ===================================================================== */

static void sof_lib_clear(struct sof_fw_lib *lib)
{
	int i;

	if (lib->mod) {
		for (i = 0; i < lib->nmod; i++)
			if (lib->mod[i].cfg)
				kfree(lib->mod[i].cfg);
		kfree(lib->mod);
	}
	kmemset(lib, 0, sizeof(*lib));
}

/* Validate the image and fill `lib' from its manifest.  Every offset and
 * count read from the file is checked against the file size. */
static int sof_parse_manifest(struct sof_host *h, const uint8_t *img,
			      size_t size, struct sof_fw_lib *lib,
			      uint32_t lib_id)
{
	const struct sof_man4_ext_hdr *ext;
	const struct sof_man4_fw_hdr *fw;
	const uint8_t *entries;
	const struct sof_man4_module_config *cfgs;
	size_t fw_off, remaining, ncfg_total;
	uint32_t i, nmod;

	if (size < sizeof(*ext) + SOF_MAN4_FW_HDR_OFFSET + sizeof(*fw)) {
		sof_log(h, "firmware image of %u bytes is too small\n",
			(unsigned)size);
		return -EINVAL;
	}
	ext = (const struct sof_man4_ext_hdr *)img;
	if (ext->id != SOF_MAN4_EXT_MAGIC) {
		sof_log(h, "not an IPC4 image (magic %#x)\n", ext->id);
		return -EINVAL;
	}
	if (ext->len < sizeof(*ext) || ext->len >= size ||
	    (ext->len & 3)) {
		sof_log(h, "bad extended manifest length %u\n", ext->len);
		return -EINVAL;
	}
	fw_off = (size_t)ext->len + SOF_MAN4_FW_HDR_OFFSET;
	if (fw_off + sizeof(*fw) >= size) {
		sof_log(h, "image too short for its firmware header\n");
		return -EINVAL;
	}
	fw = (const struct sof_man4_fw_hdr *)(img + fw_off);
	if (fw->id != SOF_MAN4_FW_MAGIC)
		sof_log(h, "unexpected firmware header magic %#x (continuing)\n",
			fw->id);
	remaining = size - fw_off;
	if (fw->len < sizeof(*fw) || fw->len >= remaining) {
		sof_log(h, "bad firmware header length %u\n", fw->len);
		return -EINVAL;
	}
	nmod = fw->num_module_entries;
	remaining -= fw->len;
	if (nmod > SOF_MAX_FW_MODULES ||
	    (size_t)nmod * sizeof(struct sof_man4_module) > remaining) {
		sof_log(h, "bad module count %u\n", nmod);
		return -EINVAL;
	}
	entries = (const uint8_t *)fw + fw->len;
	remaining -= (size_t)nmod * sizeof(struct sof_man4_module);
	cfgs = (const struct sof_man4_module_config *)
		(entries + (size_t)nmod * sizeof(struct sof_man4_module));
	ncfg_total = remaining / sizeof(struct sof_man4_module_config);

	sof_lib_clear(lib);
	lib->id = lib_id;
	lib->payload_offset = ext->len;
	lib->payload_bytes = (uint32_t)(size - ext->len);
	kmemcpy(lib->name, fw->name, 8);
	lib->name[8] = 0;
	for (i = 0; i < 8; i++)
		if (lib->name[i] && (lib->name[i] < 32 || lib->name[i] > 126))
			lib->name[i] = '?';
	lib->version[0] = fw->major_version;
	lib->version[1] = fw->minor_version;
	lib->version[2] = fw->hotfix_version;
	lib->version[3] = fw->build_version;
	if (nmod) {
		lib->mod = kcalloc(nmod, sizeof(struct sof_fw_module));
		if (!lib->mod)
			return -ENOMEM;
	}
	lib->nmod = (int)nmod;

	for (i = 0; i < nmod; i++) {
		const struct sof_man4_module *e =
			(const struct sof_man4_module *)(entries +
							 (size_t)i * sizeof(*e));
		struct sof_fw_module *m = &lib->mod[i];
		int k;

		m->id = (lib_id << SOF_MAN4_LIB_ID_SHIFT) | i;
		kmemcpy(m->name, e->name, SOF_MAN4_MOD_NAME_LEN);
		m->name[SOF_MAN4_MOD_NAME_LEN] = 0;
		for (k = 0; k < SOF_MAN4_MOD_NAME_LEN; k++)
			if (m->name[k] && (m->name[k] < 32 || m->name[k] > 126))
				m->name[k] = '?';
		kmemcpy(m->uuid, e->uuid, 16);
		m->type = e->type;
		m->affinity_mask = e->affinity_mask;
		m->instance_max = e->instance_max_count;
		m->stack_size = e->instance_stack_size;
		if (e->cfg_count &&
		    (size_t)e->cfg_offset + e->cfg_count <= ncfg_total) {
			uint16_t n = e->cfg_count;

			if (n > SOF_MAX_MOD_CFGS)
				n = SOF_MAX_MOD_CFGS;
			m->cfg = kcalloc(n, sizeof(struct sof_man4_module_config));
			if (m->cfg) {
				kmemcpy(m->cfg, &cfgs[e->cfg_offset],
					(size_t)n * sizeof(*m->cfg));
				m->ncfg = n;
			}
		} else if (e->cfg_count) {
			sof_log(h, "module %s: config table out of bounds (ignored)\n",
				m->name);
		}
	}
	lib->used = 1;
	return 0;
}

/* ===================================================================== */
/* code loader DMA memory                                                 */
/* ===================================================================== */

static void sof_cl_free(struct sof_host *h)
{
	int i;

	for (i = 0; i < h->cl_nchunks; i++) {
		if (h->cl_chunk[i].virt)
			snd_dma_free(h->cl_chunk[i].virt, h->cl_chunk[i].alloc);
		h->cl_chunk[i].virt = NULL;
		h->cl_chunk[i].alloc = 0;
		h->cl_chunk[i].bytes = 0;
	}
	h->cl_nchunks = 0;
}

/* Copy into uncached DMA memory a dword at a time. */
static void sof_copy_to_dma(uint8_t *dst, const uint8_t *src, uint32_t n)
{
	uint32_t i = 0;

	for (; i + 4 <= n; i += 4)
		*(volatile uint32_t *)(dst + i) = sof_le32(src + i);
	for (; i < n; i++)
		*(volatile uint8_t *)(dst + i) = src[i];
}

/* Put `bytes' of `data' into page-aligned DMA pieces.  Large pieces are
 * tried first; when memory is fragmented smaller ones are used, as long
 * as the pieces still fit one buffer descriptor list. */
static int sof_cl_stage(struct sof_host *h, const uint8_t *data, uint32_t bytes)
{
	static const uint32_t sizes[] = { 256u << 10, 64u << 10, 16u << 10,
					  4u << 10 };
	int dma32 = !h->addr64;
	size_t s;

	sof_cl_free(h);
	if (!bytes)
		return -EINVAL;
	for (s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
		uint32_t piece = sizes[s];
		uint32_t n = (bytes + piece - 1) / piece;
		uint32_t i, done = 0;
		int ok = 1;

		if (n > SOF_MAX_CHUNKS || n > SOF_BDL_MAX_ENTRIES - 1)
			continue;
		for (i = 0; i < n; i++) {
			uint32_t len = bytes - done < piece ? bytes - done : piece;
			uint32_t alloc = (len + 4095u) & ~4095u;
			uint64_t phys = 0;
			uint8_t *v = snd_dma_alloc(alloc, dma32, 0, &phys);

			if (!v) {
				ok = 0;
				break;
			}
			h->cl_chunk[i].virt = v;
			h->cl_chunk[i].phys = phys;
			h->cl_chunk[i].alloc = alloc;
			h->cl_chunk[i].bytes = len;
			h->cl_nchunks = (int)i + 1;
			sof_copy_to_dma(v, data + done, len);
			done += len;
		}
		if (ok)
			return 0;
		sof_cl_free(h);
	}
	sof_log(h, "no DMA memory for a %u byte image\n", bytes);
	return -ENOMEM;
}

/* ===================================================================== */
/* ROM handshake                                                          */
/* ===================================================================== */

static uint32_t sof_ipc_req_reg(struct sof_host *h)
{
	return sof_is_ace(h) ? SOF_ACE_IPC_IDR : SOF_CAVS_IPC_IDR;
}

static uint32_t sof_ipc_ack_reg(struct sof_host *h)
{
	return sof_is_ace(h) ? SOF_ACE_IPC_IDA : SOF_CAVS_IPC_IDA;
}

static int sof_wait_rom_state(struct sof_host *h, uint32_t state,
			      uint32_t timeout_us)
{
	uint32_t last = 0;
	int rc = sof_poll_dsp32(h, h->chip->rom_status, SOF_FSR_STATE_MASK,
				state, timeout_us, &last);

	h->boot_rom_status = last;
	return rc;
}

static void sof_purge_request(struct sof_host *h, int tag)
{
	uint32_t hdr = SOF_IPC_BUSY | SOF_ROM_IPC_CONTROL | SOF_ROM_IPC_PURGE_FW |
		       ((uint32_t)(tag - 1) << SOF_ROM_IPC_DMA_SHIFT);

	sof_dsp_w32(h, sof_ipc_req_reg(h), hdr);
}

static int sof_wait_rom_ack(struct sof_host *h)
{
	uint32_t ack = sof_ipc_ack_reg(h);
	uint32_t last = 0;

	if (sof_poll_dsp32(h, ack, SOF_IPC_DONE, SOF_IPC_DONE,
			   SOF_ROM_IPC_DONE_TIMEOUT_US, &last)) {
		sof_log(h, "ROM did not acknowledge the purge request (%#x)\n",
			last);
		return -ETIMEDOUT;
	}
	sof_dsp_w32(h, ack, sof_dsp_r32(h, ack) | SOF_IPC_DONE);
	return 0;
}

/* cAVS: power the host-managed cores, ask the ROM for a purge boot from
 * stream `tag', run core 0 and wait for the ROM to be ready. */
static int sof_cl_init_cavs(struct sof_host *h, int tag)
{
	const struct sof_chip *c = h->chip;
	int rc;

	rc = sof_cavs_core_power_up(h, c->host_managed_mask);
	if (rc)
		goto err;
	sof_cavs_ssp_set_consumer(h);
	sof_purge_request(h, tag);
	rc = sof_cavs_core_run(h, c->init_core_mask);
	if (rc)
		goto err;
	rc = sof_wait_rom_ack(h);
	if (rc)
		goto err;
	rc = sof_cavs_core_reset_power_down(h, c->host_managed_mask &
						       ~c->init_core_mask);
	if (rc)
		goto err;
	h->ipc_live = 1;
	sof_dsp_ipc_int_enable(h, 1);
	rc = sof_wait_rom_state(h, SOF_FSR_STATE_INIT_DONE,
				c->rom_init_timeout_us);
	if (rc) {
		sof_log(h, "ROM did not reach INIT_DONE (status %#x)\n",
			h->boot_rom_status);
		goto err;
	}
	h->core0_running = 1;
	return 0;
err:
	h->ipc_live = 0;
	if (h->boot_attempt >= SOF_BOOT_ATTEMPTS)
		sof_dsp_dump(h, "ROM start failed");
	sof_cavs_core_reset_power_down(h, c->host_managed_mask);
	return rc;
}

/* ACE: purge request first, then power core 0; the ROM answers with DONE
 * and proceeds to INIT_DONE. */
static int sof_cl_init_ace(struct sof_host *h, int tag)
{
	int rc;

	sof_purge_request(h, tag);
	rc = sof_ace_core0_power(h, 1);
	if (rc)
		goto err;
	rc = sof_wait_rom_ack(h);
	if (rc)
		goto err;
	rc = sof_dsp_irq_enable(h, 1);
	if (rc)
		goto err;
	h->ipc_live = 1;
	sof_dsp_ipc_int_enable(h, 1);
	rc = sof_wait_rom_state(h, SOF_FSR_STATE_INIT_DONE,
				h->chip->rom_init_timeout_us);
	if (rc) {
		sof_log(h, "ROM did not reach INIT_DONE (status %#x)\n",
			h->boot_rom_status);
		goto err;
	}
	return 0;
err:
	h->ipc_live = 0;
	if (h->boot_attempt >= SOF_BOOT_ATTEMPTS)
		sof_dsp_dump(h, "ROM start failed");
	sof_dsp_irq_enable(h, 0);
	sof_dsp_ipc_int_enable(h, 0);
	sof_ace_core0_power(h, 0);
	return rc;
}

/* ===================================================================== */
/* image transfer                                                         */
/* ===================================================================== */

/* Claim the loader engine and program it with the staged image. */
static struct sof_hstream *sof_cl_prepare(struct sof_host *h, uint32_t bytes)
{
	struct sof_hstream *st = sof_stream_get(h, AUDIO_DIR_PLAYBACK);
	int rc;

	if (!st) {
		sof_log(h, "no free output DMA engine for the code loader\n");
		return NULL;
	}
	rc = sof_stream_program(st, h->cl_chunk, h->cl_nchunks, bytes, 0,
				SOF_CL_FORMAT, 1);
	if (rc) {
		sof_log(h, "code loader setup failed: %d\n", rc);
		sof_stream_put(st);
		return NULL;
	}
	/* stop exactly at the end of the image */
	sof_stream_spib(st, 1, bytes);
	return st;
}

static void sof_cl_cleanup(struct sof_hstream *st)
{
	if (!st)
		return;
	sof_stream_release_hw(st);
	sof_stream_put(st);
}

/* The ICCMAX stream (cAVS 2.5): a short capture that keeps the power
 * delivery in the right state while the image streams in. */
struct sof_iccmax {
	struct sof_hstream *st;
	struct sof_dma_chunk buf;
	uint8_t saved_gb;
};

static void sof_iccmax_start(struct sof_host *h, struct sof_iccmax *ic)
{
	int rc;

	kmemset(ic, 0, sizeof(*ic));
	if (!(h->chip->flags & SOF_CHIP_ICCMAX))
		return;
	ic->buf.virt = snd_dma_alloc(4096, !h->addr64, 0, &ic->buf.phys);
	if (!ic->buf.virt)
		return;
	ic->buf.alloc = 4096;
	ic->buf.bytes = 4096;
	ic->st = sof_stream_get(h, AUDIO_DIR_CAPTURE);
	if (!ic->st)
		goto fail;
	rc = sof_stream_program(ic->st, &ic->buf, 1, 4096, 0, SOF_CL_FORMAT, 1);
	if (rc)
		goto fail;
	ic->saved_gb = sof_hda_r8(h, SOF_HDA_VS_LTRP) & SOF_HDA_VS_LTRP_GB_MASK;
	sof_hda_upd8(h, SOF_HDA_VS_LTRP, SOF_HDA_VS_LTRP_GB_MASK,
		     SOF_HDA_LTRP_GB_BOOT_US);
	sof_hda_upd32(h, ic->st->sd_off + SOF_SD_CTL, SOF_SD_CTL_RUN,
		      SOF_SD_CTL_RUN);
	ic->st->running = 1;
	return;
fail:
	if (ic->st) {
		sof_stream_put(ic->st);
		ic->st = NULL;
	}
	snd_dma_free(ic->buf.virt, 4096);
	ic->buf.virt = NULL;
}

static void sof_iccmax_stop(struct sof_host *h, struct sof_iccmax *ic)
{
	if (!ic->st)
		return;
	sof_cl_cleanup(ic->st);
	sof_hda_upd8(h, SOF_HDA_VS_LTRP, SOF_HDA_VS_LTRP_GB_MASK, ic->saved_gb);
	snd_dma_free(ic->buf.virt, 4096);
	ic->buf.virt = NULL;
	ic->st = NULL;
}

/* Run the loader DMA until the ROM jumped into the firmware. */
static int sof_cl_copy(struct sof_host *h, struct sof_hstream *st)
{
	int rc, rc2;

	rc = sof_stream_start(st);
	if (rc)
		return rc;
	rc = sof_wait_rom_state(h, SOF_FSR_STATE_FW_ENTERED,
				SOF_BASEFW_TIMEOUT_US);
	if (rc)
		sof_log(h, "firmware was not entered (status %#x: %s)\n",
			h->boot_rom_status,
			sof_fsr_state_name(h, h->boot_rom_status));
	rc2 = sof_stream_stop(st);
	return rc ? rc : rc2;
}

/* Wait for the FW_READY notification. */
static int sof_wait_fw_ready(struct sof_host *h)
{
	uint64_t start = sof_now_us();
	uint64_t spent = 0;

	for (;;) {
		uint64_t now;

		sof_ipc4_service(h);
		if (h->fw_ready)
			return 0;
		if (h->crashed)
			return -EIO;
		if (sof_can_sleep()) {
			sof_wait_us(5000);
		} else {
			lapic_delay_us(100);
			spent += 100;
		}
		now = sof_now_us() - start;
		if (now > spent)
			spent = now;
		if (spent >= SOF_FW_READY_TIMEOUT_US) {
			sof_ipc4_service(h);
			return h->fw_ready ? 0 : -ETIMEDOUT;
		}
	}
}

/* ===================================================================== */
/* firmware file lookup                                                   */
/* ===================================================================== */

static int sof_request(struct sof_host *h, const char *path, void **data,
		       size_t *size)
{
	int rc = firmware_request(path, data, size);

	if (rc == 0) {
		if (*size == 0) {
			firmware_release(*data);
			return -EINVAL;
		}
		sof_strlcpy(h->fw_path, path, sizeof(h->fw_path));
	}
	return rc;
}

/* The base firmware.  The machine scan names the image for the machine
 * (sof-ipc4/<plat>/sof-<plat>.ri, or the community/ one on a machine
 * fused with the community key); then the PCI table's directory, with the
 * image for the same key first and the other one as the last resort (the
 * plain name is the Intel-signed image; Elkhart Lake only has a
 * community build).  A name already tried is not asked for again. */
static int sof_request_basefw(struct sof_host *h, void **data, size_t *size)
{
	char tried[4][160];
	char path[160];
	const char *dir = h->id->fw_dir;
	const char *file = h->id->fw_file;
	const char *mfw = h->s.machine.fw_name;
	int community = 0, ntried = 0, k, i;
	int rc = -ENOENT, last = -ENOENT;

	if (mfw[0]) {
		const char *p;

		for (p = mfw; *p; p++)
			if (!kstrncmp(p, "/community/", 11))
				community = 1;
	}
	for (k = 0; k < 3; k++) {
		int dup = 0;

		if (k == 0) {
			if (!mfw[0])
				continue;
			if (kstrncmp(mfw, "intel/", 6) == 0)
				sof_strlcpy(path, mfw, sizeof(path));
			else
				ksnprintf(path, sizeof(path), "%s/%s", dir, mfw);
		} else if ((k == 1) == (community != 0)) {
			ksnprintf(path, sizeof(path), "%s/community/%s", dir,
				  file);
		} else {
			ksnprintf(path, sizeof(path), "%s/%s", dir, file);
		}
		for (i = 0; i < ntried; i++)
			if (!kstrcmp(tried[i], path))
				dup = 1;
		if (dup)
			continue;
		sof_strlcpy(tried[ntried++], path, sizeof(tried[0]));
		rc = sof_request(h, path, data, size);
		if (rc == 0)
			return 0;
		if (rc != -ENOENT) {
			sof_log(h, "firmware %s: error %d\n", path, rc);
			last = rc;
		}
	}
	if (last != -ENOENT)
		rc = last;
	sof_log(h, "firmware %s/%s not loadable (%d)\n", dir, file, rc);
	return rc;
}

int sof_fw_probe_file(struct sof_host *h)
{
	void *img = NULL;
	size_t size = 0;
	int rc = sof_request_basefw(h, &img, &size);

	if (rc == 0)
		firmware_release(img);
	return rc;
}

/* ===================================================================== */
/* boot                                                                   */
/* ===================================================================== */

static int sof_boot_once(struct sof_host *h, struct sof_hstream *st)
{
	int rc;

	rc = sof_is_ace(h) ? sof_cl_init_ace(h, st->tag) :
			     sof_cl_init_cavs(h, st->tag);
	if (rc)
		return rc;
	return 0;
}

int sof_fw_boot(struct sof_host *h)
{
	void *img = NULL;
	size_t size = 0;
	struct sof_hstream *st = NULL;
	struct sof_iccmax ic;
	uint64_t t0 = sof_now_us();
	int rc, i;

	h->boot_state = SOF_BOOT_LOADING;
	sof_ipc4_reset(h);

	rc = sof_request_basefw(h, &img, &size);
	if (rc)
		goto fail;
	if (size > FIRMWARE_MAX_BYTES) {
		rc = -EFBIG;
		goto fail;
	}
	rc = sof_parse_manifest(h, img, size, &h->lib[0], 0);
	if (rc)
		goto fail;
	sof_log(h, "firmware %s: %s %u.%u.%u.%u, %d modules, %u bytes\n",
		h->fw_path, h->lib[0].name, h->lib[0].version[0],
		h->lib[0].version[1], h->lib[0].version[2],
		h->lib[0].version[3], h->lib[0].nmod, h->lib[0].payload_bytes);
	sof_strlcpy(h->lib[0].file, h->fw_path, sizeof(h->lib[0].file));

	rc = sof_cl_stage(h, (const uint8_t *)img + h->lib[0].payload_offset,
			  h->lib[0].payload_bytes);
	firmware_release(img);
	img = NULL;
	if (rc)
		goto fail;

	rc = sof_dsp_power_up(h);
	if (rc)
		goto fail;

	st = sof_cl_prepare(h, h->lib[0].payload_bytes);
	if (!st) {
		rc = -EBUSY;
		goto fail_power;
	}

	sof_iccmax_start(h, &ic);
	for (i = 0; i < SOF_BOOT_ATTEMPTS; i++) {
		h->boot_attempt = i + 1;
		rc = sof_boot_once(h, st);
		if (rc == 0)
			break;
		sof_log(h, "ROM start attempt %d/%d failed: %d\n", i + 1,
			SOF_BOOT_ATTEMPTS, rc);
	}
	if (rc == 0) {
		rc = sof_cl_copy(h, st);
		if (rc)
			sof_dsp_dump(h, "firmware download failed");
	}
	sof_iccmax_stop(h, &ic);
	sof_cl_cleanup(st);
	st = NULL;
	sof_cl_free(h);
	if (rc)
		goto fail_power;

	rc = sof_wait_fw_ready(h);
	if (rc) {
		sof_dsp_dump(h, rc == -EIO ? "firmware crashed while starting" :
					     "no FW_READY from the firmware");
		goto fail_power;
	}
	h->s.fw_booted = 1;
	h->boot_state = SOF_BOOT_FW_READY;
	h->boot_ms = (uint32_t)((sof_now_us() - t0) / 1000);

	/* cAVS: gating back on once the firmware runs */
	sof_dsp_gating(h, 1);

	rc = sof_ipc4_query_fw_config(h);
	if (rc)
		sof_log(h, "continuing without the firmware configuration\n");
	/* errors the firmware reports for an IPC come with its reason */
	(void)sof_ipc4_enable_fw_logs(h);
	if (h->s.fw_version[0] || h->s.fw_version[1])
		sof_log(h, "firmware %u.%u.%u.%u running after %u ms\n",
			h->s.fw_version[0], h->s.fw_version[1],
			h->s.fw_version[2], h->s.fw_version[3], h->boot_ms);
	else
		sof_log(h, "firmware running after %u ms\n", h->boot_ms);

	/* From ACE 3.x on the release is split: the open (non-Intel)
	 * modules come as a second image next to the base one, signed with
	 * the same key: ".../sof-ptl.ri" -> ".../sof-ptl-openmodules.ri".
	 * Earlier generations carry everything in the base image. */
	if (h->chip && (h->chip->flags & SOF_CHIP_ACE3)) {
		char path[160];
		char base[128];
		size_t l = kstrlen(h->lib[0].file);

		if (l > 3 && l < sizeof(base) &&
		    !kstrcmp(h->lib[0].file + l - 3, ".ri")) {
			sof_strlcpy(base, h->lib[0].file, l - 3 + 1);
			ksnprintf(path, sizeof(path), "%s-openmodules.ri", base);
			sof_fw_load_library(h, path, 1);
		}
	}
	return 0;

fail_power:
	h->ipc_live = 0;
	sof_dsp_irq_enable(h, 0);
	sof_dsp_ipc_int_enable(h, 0);
	sof_dsp_power_down(h);
fail:
	if (img)
		firmware_release(img);
	if (st)
		sof_cl_cleanup(st);
	sof_cl_free(h);
	h->boot_state = SOF_BOOT_FAILED;
	h->boot_error = rc;
	return rc;
}

void sof_fw_release(struct sof_host *h)
{
	int i;

	for (i = 0; i < SOF_MAX_LIBS; i++)
		sof_lib_clear(&h->lib[i]);
	sof_cl_free(h);
}

/* ===================================================================== */
/* libraries                                                              */
/* ===================================================================== */

static int sof_lib_slot(struct sof_host *h)
{
	int max = SOF_MAX_LIBS;
	int i;

	if (h->cfg_max_libs && (int)h->cfg_max_libs < max)
		max = (int)h->cfg_max_libs;
	for (i = 1; i < max; i++)
		if (!h->lib[i].used)
			return i;
	return -1;
}

int sof_fw_load_library(struct sof_host *h, const char *path, int optional)
{
	struct sof_fw_lib *lib;
	struct sof_hstream *st;
	void *img = NULL;
	size_t size = 0;
	uint32_t primary;
	int slot, rc, i;

	if (!h->fw_ready || h->crashed)
		return -ENODEV;
	for (i = 1; i < SOF_MAX_LIBS; i++)
		if (h->lib[i].used && kstrcmp(h->lib[i].file, path) == 0)
			return 0; /* already there */
	slot = sof_lib_slot(h);
	if (slot < 0) {
		if (!optional)
			sof_log(h, "no free library slot for %s\n", path);
		return -ENOSPC;
	}
	rc = firmware_request(path, &img, &size);
	if (rc) {
		if (!optional || rc != -ENOENT)
			sof_log(h, "library %s: error %d\n", path, rc);
		return optional && rc == -ENOENT ? 0 : rc;
	}
	lib = &h->lib[slot];
	rc = sof_parse_manifest(h, img, size, lib, (uint32_t)slot);
	if (rc)
		goto out;
	sof_strlcpy(lib->file, path, sizeof(lib->file));
	rc = sof_cl_stage(h, (const uint8_t *)img + lib->payload_offset,
			  lib->payload_bytes);
	firmware_release(img);
	img = NULL;
	if (rc)
		goto out;

	st = sof_cl_prepare(h, lib->payload_bytes);
	if (!st) {
		rc = -EBUSY;
		goto out;
	}
	h->ipc_timeout_us = SOF_BASEFW_TIMEOUT_US;

	/* stage 1: tell the firmware which DMA to expect the library on */
	primary = (uint32_t)(st->tag - 1) |
		  SOF_IPC4_TYPE(SOF_IPC4_GLB_LOAD_LIBRARY_PREPARE);
	rc = sof_ipc4_glb(h, SOF_IPC4_GLB_LOAD_LIBRARY_PREPARE, primary, 0,
			  NULL, 0);
	if (rc == 0) {
		/* the firmware sets up the gateway: FIFOS becomes non-zero */
		for (i = 0; i < 3000; i++) {
			if (sof_hda_r16(h, st->sd_off + SOF_SD_FIFOS) &
			    SOF_SD_FIFOS_MASK)
				break;
			sof_wait_us(1000);
		}
		if (i == 3000)
			sof_log(h, "library DMA FIFO never came up\n");
	} else if (rc != -EOPNOTSUPP) {
		goto out_stream;
	}

	rc = sof_stream_start(st);
	if (rc)
		goto out_stream;
	/* stage 2: load */
	primary = (uint32_t)(st->tag - 1) | (((uint32_t)slot & 0xf) << 16);
	rc = sof_ipc4_glb(h, SOF_IPC4_GLB_LOAD_LIBRARY, primary, 0, NULL, 0);
	sof_stream_stop(st);
out_stream:
	h->ipc_timeout_us = 0;
	sof_cl_cleanup(st);
	sof_cl_free(h);
	if (rc == 0) {
		lib->used = 1;
		sof_log(h, "library %s (%s %u.%u.%u.%u, %d modules) loaded as #%d\n",
			path, lib->name, lib->version[0], lib->version[1],
			lib->version[2], lib->version[3], lib->nmod, slot);
		return 0;
	}
	sof_log(h, "loading library %s failed: %d\n", path, rc);
out:
	if (img)
		firmware_release(img);
	sof_lib_clear(lib);
	return rc;
}

int sof_fw_load_library_uuid(struct sof_host *h, const uint8_t uuid[16])
{
	char path[160], u[40];

	sof_uuid_str(uuid, u, sizeof(u));
	ksnprintf(path, sizeof(path), "%s/%s.bin",
		  h->s.machine.fw_lib_path[0] ? h->s.machine.fw_lib_path :
						h->id->lib_dir,
		  u);
	return sof_fw_load_library(h, path, 0);
}

static const struct sof_fw_module *sof_find_loaded(struct sof_host *h,
						   const uint8_t uuid[16])
{
	int l, m;

	for (l = 0; l < SOF_MAX_LIBS; l++) {
		if (!h->lib[l].used)
			continue;
		for (m = 0; m < h->lib[l].nmod; m++)
			if (sof_uuid_eq(h->lib[l].mod[m].uuid, uuid))
				return &h->lib[l].mod[m];
	}
	return NULL;
}

int sof_fw_find_module(struct sof_dev *sdev, const uint8_t uuid[16],
		       const struct sof_fw_module **out)
{
	struct sof_host *h = sof_host_of(sdev);
	const struct sof_fw_module *m;

	if (!sdev || !uuid || !out)
		return -EINVAL;
	*out = NULL;
	if (sof_uuid_null(uuid))
		return -ENOENT;
	m = sof_find_loaded(h, uuid);
	if (!m && h->fw_ready && sof_fw_load_library_uuid(h, uuid) == 0)
		m = sof_find_loaded(h, uuid);
	if (!m)
		return -ENOENT;
	*out = m;
	return 0;
}

long sof_fw_diag(struct sof_host *h, char *buf, size_t len)
{
	size_t pos = 0;
	int l, m;

	pos = sof_append(buf, len, pos, "firmware: %s, boot %s",
			 h->fw_path[0] ? h->fw_path : "(none)",
			 h->boot_state == SOF_BOOT_RUNNING   ? "complete" :
			 h->boot_state == SOF_BOOT_FW_READY  ? "firmware ready" :
			 h->boot_state == SOF_BOOT_LOADING   ? "in progress" :
			 h->boot_state == SOF_BOOT_FAILED    ? "FAILED" :
			 h->boot_state == SOF_BOOT_CRASHED   ? "CRASHED" :
			 h->boot_state == SOF_BOOT_PROBED    ? "pending" :
							       "not started");
	if (h->boot_state == SOF_BOOT_FAILED)
		pos = sof_append(buf, len, pos, " (error %d, ROM status %#x)",
				 h->boot_error, h->boot_rom_status);
	pos = sof_append(buf, len, pos, ", %u ms\n", h->boot_ms);
	for (l = 0; l < SOF_MAX_LIBS; l++) {
		struct sof_fw_lib *lib = &h->lib[l];

		if (!lib->used)
			continue;
		pos = sof_append(buf, len, pos,
				 "  image #%d %s %u.%u.%u.%u (%s), %d modules:",
				 l, lib->name, lib->version[0], lib->version[1],
				 lib->version[2], lib->version[3], lib->file,
				 lib->nmod);
		for (m = 0; m < lib->nmod; m++)
			pos = sof_append(buf, len, pos, " %s", lib->mod[m].name);
		pos = sof_append(buf, len, pos, "\n");
	}
	return (long)pos;
}

/* The lookup the topology side uses (weak reference there): a module's
 * IPC4 id and the manifest's sizing hints, by UUID. */
int sof_fw_module_find(struct sof_dev *sdev, const uint8_t uuid[16],
		       struct sof_fw_module_info *out)
{
	const struct sof_fw_module *m = NULL;
	int rc;
	uint16_t i;

	if (!out)
		return -EINVAL;
	rc = sof_fw_find_module(sdev, uuid, &m);
	if (rc)
		return rc;
	kmemset(out, 0, sizeof(*out));
	out->module_id = m->id;
	out->type = m->type;
	out->instance_max = m->instance_max;
	for (i = 0; i < m->ncfg; i++) {
		if (m->cfg[i].cpc > out->cpc)
			out->cpc = m->cfg[i].cpc;
	}
	if (m->ncfg)
		out->is_pages = (m->cfg[0].is_bytes + 4095u) / 4096u;
	return 0;
}
