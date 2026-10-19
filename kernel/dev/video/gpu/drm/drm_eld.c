// LikeOS -- display-manager core: EDID-Like Data for audio.
//
// The ELD is what an HDMI or DisplayPort audio codec is told about the
// sink: its name, the audio formats and rates it takes (short audio
// descriptors from the CTA audio data blocks), its speaker allocation,
// whether it takes ACP/ISRC/ISRC2 packets, and whether it sits behind
// DisplayPort or HDMI.  Built from the EDID into the connector's eld[]
// together with the audio and video latencies of the HDMI vendor block,
// which give the audio-to-video sync delay.  The short audio descriptors
// and the speaker allocation block can also be read out on their own.
//
// Pure: fixed-width types only, so the host tests compile it.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Luc Verhaegen's, Intel's, Red Hat's and Dennis Munsie's code: MIT
// Portions Copyright (C) 2006 Luc Verhaegen (quirks list)
// Portions Copyright (C) 2007-2008 Intel Corporation
// Portions Copyright (C) 2010 Red Hat, Inc.
// Portions Copyright (C) 2006 Dennis Munsie <dmunsie@cecropia.com>
// Portions Copyright (C) 2023 Intel Corporation

#include "drm_edid_internal.h"

void clear_eld(struct drm_edid_conn *conn)
{
	if (conn->eld)
		edid_memset(conn->eld, 0, DRM_ELD_BUF_SIZE);

	conn->latency_present[0] = false;
	conn->latency_present[1] = false;
	conn->video_latency[0] = 0;
	conn->audio_latency[0] = 0;
	conn->video_latency[1] = 0;
	conn->audio_latency[1] = 0;
}

/* HDMI Vendor-Specific Data Block (HDMI VSDB, H14b-VSDB) */
void drm_parse_hdmi_vsdb_audio(struct drm_edid_conn *conn, const uint8_t *db)
{
	uint8_t len = (uint8_t)cea_db_payload_len(db);

	if (len >= 6 && (db[6] & (1 << 7)) && conn->eld)
		conn->eld[DRM_ELD_SAD_COUNT_CONN_TYPE] |= DRM_ELD_SUPPORTS_AI;

	if (len >= 10 && hdmi_vsdb_latency_present(db)) {
		conn->latency_present[0] = true;
		conn->video_latency[0] = db[9];
		conn->audio_latency[0] = db[10];
	}

	if (len >= 12 && hdmi_vsdb_i_latency_present(db)) {
		conn->latency_present[1] = true;
		conn->video_latency[1] = db[11];
		conn->audio_latency[1] = db[12];
	}
}

/*
 * Get 3-byte SAD buffer from struct cea_sad.
 */
void drm_edid_cta_sad_get(const struct cea_sad *cta_sad, uint8_t *sad)
{
	sad[0] = (uint8_t)(cta_sad->format << 3 | cta_sad->channels);
	sad[1] = cta_sad->freq;
	sad[2] = cta_sad->byte2;
}

/*
 * Set struct cea_sad from 3-byte SAD buffer.
 */
void drm_edid_cta_sad_set(struct cea_sad *cta_sad, const uint8_t *sad)
{
	cta_sad->format = (sad[0] & 0x78) >> 3;
	cta_sad->channels = sad[0] & 0x07;
	cta_sad->freq = sad[1] & 0x7f;
	cta_sad->byte2 = sad[2];
}

/*
 * drm_edid_to_eld - build ELD from EDID
 *
 * Fill the ELD (EDID-Like Data) buffer for passing to the audio driver. The
 * HDCP and Port_ID ELD fields are left for the graphics driver to fill in.
 * The latency data of the HDMI vendor block is collected on the way.
 */
void drm_edid_to_eld(struct drm_edid_conn *conn, const struct drm_edid *drm_edid)
{
	const struct drm_display_info *info = conn->info;
	const struct cea_db *db;
	struct cea_db_iter iter;
	uint8_t *eld = conn->eld;
	uint8_t scratch[DRM_ELD_BUF_SIZE];
	int total_sad_count = 0;
	int mnl;

	if (!drm_edid)
		return;
	/* Without an ELD buffer the latency data is still wanted. */
	if (!eld) {
		edid_memset(scratch, 0, sizeof(scratch));
		eld = scratch;
	}

	mnl = get_monitor_name(drm_edid, (char *)&eld[DRM_ELD_MONITOR_NAME_STRING]);

	eld[DRM_ELD_CEA_EDID_VER_MNL] = (uint8_t)(info->cea_rev << DRM_ELD_CEA_EDID_VER_SHIFT);
	eld[DRM_ELD_CEA_EDID_VER_MNL] |= (uint8_t)mnl;

	eld[DRM_ELD_VER] = DRM_ELD_VER_CEA861D;

	eld[DRM_ELD_MANUFACTURER_NAME0] = drm_edid->edid->mfg_id[0];
	eld[DRM_ELD_MANUFACTURER_NAME1] = drm_edid->edid->mfg_id[1];
	eld[DRM_ELD_PRODUCT_CODE0] = drm_edid->edid->prod_code[0];
	eld[DRM_ELD_PRODUCT_CODE1] = drm_edid->edid->prod_code[1];

	cea_db_iter_edid_begin(drm_edid, &iter);
	cea_db_iter_for_each(db, &iter) {
		const uint8_t *data = cea_db_data(db);
		int len = cea_db_payload_len(db);
		int sad_count;

		switch (cea_db_tag(db)) {
		case CTA_DB_AUDIO:
			/* Audio Data Block, contains SADs */
			sad_count = min(len / 3, 15 - total_sad_count);
			if (sad_count >= 1)
				edid_memcpy(&eld[DRM_ELD_CEA_SAD(mnl, total_sad_count)],
					    data, (size_t)(sad_count * 3));
			total_sad_count += sad_count;
			break;
		case CTA_DB_SPEAKER:
			/* Speaker Allocation Data Block */
			if (len >= 1)
				eld[DRM_ELD_SPEAKER] = data[0];
			break;
		case CTA_DB_VENDOR:
			/* HDMI Vendor-Specific Data Block */
			if (cea_db_is_hdmi_vsdb(db)) {
				uint8_t *saved = conn->eld;

				conn->eld = eld;
				drm_parse_hdmi_vsdb_audio(conn, (const uint8_t *)db);
				conn->eld = saved;
			}
			break;
		default:
			break;
		}
	}
	cea_db_iter_end(&iter);

	eld[DRM_ELD_SAD_COUNT_CONN_TYPE] |= (uint8_t)(total_sad_count << DRM_ELD_SAD_COUNT_SHIFT);

	if (conn->connector_type == DRM_MODE_CONNECTOR_DisplayPort ||
	    conn->connector_type == DRM_MODE_CONNECTOR_eDP)
		eld[DRM_ELD_SAD_COUNT_CONN_TYPE] |= DRM_ELD_CONN_TYPE_DP;
	else
		eld[DRM_ELD_SAD_COUNT_CONN_TYPE] |= DRM_ELD_CONN_TYPE_HDMI;

	eld[DRM_ELD_BASELINE_ELD_LEN] =
		(uint8_t)DIV_ROUND_UP(drm_eld_calc_baseline_block_size(eld), 4);
}

static int _drm_edid_to_sad(const struct drm_edid *drm_edid,
			    struct cea_sad **psads)
{
	const struct cea_db *db;
	struct cea_db_iter iter;
	int count = 0;

	cea_db_iter_edid_begin(drm_edid, &iter);
	cea_db_iter_for_each(db, &iter) {
		if (cea_db_tag(db) == CTA_DB_AUDIO) {
			struct cea_sad *sads;
			int i;

			count = cea_db_payload_len(db) / 3; /* SAD is 3B */
			sads = kalloc(sizeof(*sads) * (size_t)(count ? count : 1));
			*psads = sads;
			if (!sads) {
				cea_db_iter_end(&iter);
				return -ENOMEM;
			}
			edid_memset(sads, 0, sizeof(*sads) * (size_t)(count ? count : 1));
			for (i = 0; i < count; i++)
				drm_edid_cta_sad_set(&sads[i], &db->data[i * 3]);
			break;
		}
	}
	cea_db_iter_end(&iter);

	return count;
}

int drm_edid_to_sad(const struct edid *edid, struct cea_sad **sads)
{
	struct drm_edid drm_edid;

	return _drm_edid_to_sad(drm_edid_legacy_init(&drm_edid, edid), sads);
}

static int _drm_edid_to_speaker_allocation(const struct drm_edid *drm_edid,
					   uint8_t **sadb)
{
	const struct cea_db *db;
	struct cea_db_iter iter;
	int count = 0;

	cea_db_iter_edid_begin(drm_edid, &iter);
	cea_db_iter_for_each(db, &iter) {
		if (cea_db_tag(db) == CTA_DB_SPEAKER &&
		    cea_db_payload_len(db) == 3) {
			*sadb = kalloc((size_t)cea_db_payload_len(db));
			if (!*sadb) {
				cea_db_iter_end(&iter);
				return -ENOMEM;
			}
			edid_memcpy(*sadb, db->data, (size_t)cea_db_payload_len(db));
			count = cea_db_payload_len(db);
			break;
		}
	}
	cea_db_iter_end(&iter);

	return count;
}

int drm_edid_to_speaker_allocation(const struct edid *edid, uint8_t **sadb)
{
	struct drm_edid drm_edid;

	return _drm_edid_to_speaker_allocation(drm_edid_legacy_init(&drm_edid, edid),
					       sadb);
}

/*
 * The HDMI/DP sink's audio-video sync delay in milliseconds, or 0 if the
 * sink doesn't support audio or video.
 */
int drm_av_sync_delay(const struct drm_edid_conn *conn,
		      const struct drm_display_mode *mode)
{
	int i = !!(mode->flags & DRM_MODE_FLAG_INTERLACE);
	int a, v;

	if (!conn->latency_present[0])
		return 0;
	if (!conn->latency_present[1])
		i = 0;

	a = conn->audio_latency[i];
	v = conn->video_latency[i];

	/*
	 * HDMI/DP sink doesn't support audio or video?
	 */
	if (a == 255 || v == 255)
		return 0;

	/*
	 * Convert raw EDID values to millisecond.
	 * Treat unknown latency as 0ms.
	 */
	if (a)
		a = min(2 * (a - 1), 500);
	if (v)
		v = min(2 * (v - 1), 500);

	return max(v - a, 0);
}

int drm_eld_sad_get(const uint8_t *eld, int sad_index, struct cea_sad *cta_sad)
{
	const uint8_t *sad;

	if (sad_index >= drm_eld_sad_count(eld))
		return -EINVAL;

	sad = eld + DRM_ELD_CEA_SAD(drm_eld_mnl(eld), sad_index);

	drm_edid_cta_sad_set(cta_sad, sad);

	return 0;
}

int drm_eld_sad_set(uint8_t *eld, int sad_index, const struct cea_sad *cta_sad)
{
	uint8_t *sad;

	if (sad_index >= drm_eld_sad_count(eld))
		return -EINVAL;

	sad = eld + DRM_ELD_CEA_SAD(drm_eld_mnl(eld), sad_index);

	drm_edid_cta_sad_get(cta_sad, sad);

	return 0;
}
