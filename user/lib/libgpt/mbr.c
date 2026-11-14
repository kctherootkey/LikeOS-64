/* libgpt -- the master boot record.
 *
 * LBA 0 of a disk: 440 bytes of boot code, the 32-bit disk signature at
 * 440, two reserved bytes, four 16-byte partition entries from 446 and the
 * 0x55 0xaa signature at 510.  An entry: status (0x80 active), CHS of the
 * first sector, type, CHS of the last sector, first LBA, sector count.
 *
 * A GPT disk carries a "protective" MBR: one entry of type 0xee from LBA 1
 * over the whole disk (or 0xffffffff sectors when the disk is larger), so
 * that tools which only know MBR see the disk as used.  An MBR with an
 * 0xee entry and other entries beside it is a "hybrid" one.
 *
 * Logical partitions live in an extended partition (type 0x05, 0x0f or
 * 0x85) as a chain of extended boot records (EBRs): each EBR has the
 * logical partition in its first entry (relative to the EBR itself) and
 * the link to the next EBR in its second (relative to the start of the
 * extended partition).  The chain is followed with loop and range checks.
 *
 * Only sector 0 is ever written; logical partitions are read, not written.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "gpt_internal.h"

#define MBR_SIG_OFF     510
#define MBR_PART_OFF    446
#define MBR_DISKSIG_OFF 440
#define MBR_ENTRY_SIZE  16
#define MBR_MAX_LOGICAL (MBR_MAX_PARTS - 4)

static int mbr_is_extended_type(uint8_t t)
{
	return t == MBR_TYPE_EXTENDED_CHS || t == MBR_TYPE_EXTENDED_LBA ||
	       t == MBR_TYPE_EXTENDED_ALT;
}

void mbr_lba_to_chs(uint64_t lba, uint8_t chs[3])
{
	const uint64_t heads = 255, spt = 63;
	uint64_t c, h, s;

	if (lba >= 1024ULL * heads * spt) {
		/* Past what CHS can say: the customary "maximum" value. */
		chs[0] = 0xfe;
		chs[1] = 0xff;
		chs[2] = 0xff;
		return;
	}
	c = lba / (heads * spt);
	h = (lba / spt) % heads;
	s = lba % spt + 1;
	chs[0] = (uint8_t)h;
	chs[1] = (uint8_t)((s & 0x3f) | ((c >> 2) & 0xc0));
	chs[2] = (uint8_t)(c & 0xff);
}

static void mbr_decode_entry(const uint8_t *e, struct mbr_part *p)
{
	memset(p, 0, sizeof(*p));
	p->status = e[0];
	memcpy(p->chs_first, e + 1, 3);
	p->type = e[4];
	memcpy(p->chs_last, e + 5, 3);
	p->first_lba = gpt_le32(e + 8);
	p->nsectors = gpt_le32(e + 12);
}

static void mbr_encode_entry(const struct mbr_part *p, uint8_t *e)
{
	uint8_t chs[3];
	uint64_t first = p->first_lba, count = p->nsectors;

	memset(e, 0, MBR_ENTRY_SIZE);
	if (p->type == MBR_TYPE_EMPTY && count == 0)
		return;
	if (first > 0xffffffffULL)
		first = 0xffffffffULL;
	if (count > 0xffffffffULL)
		count = 0xffffffffULL;
	e[0] = p->status;
	mbr_lba_to_chs(first, chs);
	memcpy(e + 1, chs, 3);
	e[4] = p->type;
	mbr_lba_to_chs(first + (count ? count - 1 : 0), chs);
	memcpy(e + 5, chs, 3);
	gpt_put32(e + 8, (uint32_t)first);
	gpt_put32(e + 12, (uint32_t)count);
}

/* Follows the EBR chain of the extended partition `ext' and appends the
 * logical partitions to t. */
static int mbr_read_logical(struct gpt_dev *d, struct mbr_table *t,
			    const struct mbr_part *ext, uint8_t *sector)
{
	uint64_t ext_start = ext->first_lba;
	uint64_t ext_end = ext->first_lba + ext->nsectors; /* exclusive */
	uint64_t ebr = ext_start;
	uint64_t visited[MBR_MAX_LOGICAL + 1];
	int nvisited = 0, number = 5;

	while (ebr != 0) {
		const uint8_t *e1, *e2;
		struct mbr_part lp, link;
		int rc;

		if (ebr < ext_start || ebr >= ext_end || ebr >= d->nsectors) {
			t->chain_broken = 1;
			break;
		}
		for (int i = 0; i < nvisited; i++) {
			if (visited[i] == ebr) {
				t->chain_broken = 1; /* a loop */
				return 0;
			}
		}
		if (nvisited >= MBR_MAX_LOGICAL || t->nparts >= MBR_MAX_PARTS) {
			t->chain_broken = 1;
			break;
		}
		visited[nvisited++] = ebr;
		rc = gpt_dev_read(d, ebr, 1, sector);
		if (rc)
			return rc;
		if (sector[MBR_SIG_OFF] != 0x55 ||
		    sector[MBR_SIG_OFF + 1] != 0xaa) {
			t->chain_broken = 1;
			break;
		}
		e1 = sector + MBR_PART_OFF;
		e2 = e1 + MBR_ENTRY_SIZE;
		mbr_decode_entry(e1, &lp);
		mbr_decode_entry(e2, &link);
		if (lp.type != MBR_TYPE_EMPTY && lp.nsectors) {
			/* The logical partition is relative to its EBR. */
			lp.first_lba += ebr;
			lp.is_logical = 1;
			lp.number = number++;
			if (lp.first_lba + lp.nsectors > ext_end)
				t->chain_broken = 1;
			t->parts[t->nparts++] = lp;
		}
		if (link.type == MBR_TYPE_EMPTY || link.nsectors == 0 ||
		    !mbr_is_extended_type(link.type))
			break;
		/* The next EBR is relative to the extended partition. */
		ebr = ext_start + link.first_lba;
	}
	return 0;
}

int mbr_read(struct gpt_dev *d, struct mbr_table *out)
{
	uint8_t *sector;
	int rc, nprimary = 0, has_ee = 0, has_other = 0;
	struct mbr_part ext;
	int have_ext = 0;

	if (!d || !out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	sector = malloc(d->sector_size);
	if (!sector)
		return -ENOMEM;
	rc = gpt_dev_read(d, 0, 1, sector);
	if (rc) {
		free(sector);
		return rc;
	}
	memcpy(out->boot_code, sector, sizeof(out->boot_code));
	out->disk_signature = gpt_le32(sector + MBR_DISKSIG_OFF);
	out->reserved = gpt_le16(sector + MBR_DISKSIG_OFF + 4);
	if (sector[MBR_SIG_OFF] != 0x55 || sector[MBR_SIG_OFF + 1] != 0xaa) {
		out->kind = MBR_KIND_NONE;
		free(sector);
		return 0;
	}
	memset(&ext, 0, sizeof(ext));
	for (int i = 0; i < 4; i++) {
		struct mbr_part p;

		mbr_decode_entry(sector + MBR_PART_OFF + i * MBR_ENTRY_SIZE, &p);
		if (p.type == MBR_TYPE_EMPTY)
			continue;
		p.number = i + 1;
		if (p.type == MBR_TYPE_GPT_PROTECTIVE)
			has_ee = 1;
		else
			has_other = 1;
		if (mbr_is_extended_type(p.type)) {
			p.is_extended = 1;
			if (!have_ext) {
				ext = p;
				have_ext = 1;
			}
		}
		out->parts[out->nparts++] = p;
		nprimary++;
	}
	if (nprimary == 0)
		out->kind = MBR_KIND_EMPTY;
	else if (has_ee && !has_other)
		out->kind = MBR_KIND_PROTECTIVE;
	else if (has_ee)
		out->kind = MBR_KIND_HYBRID;
	else
		out->kind = MBR_KIND_LEGACY;
	if (out->kind == MBR_KIND_LEGACY && have_ext && ext.nsectors)
		rc = mbr_read_logical(d, out, &ext, sector);
	free(sector);
	return rc;
}

int mbr_write(struct gpt_dev *d, const struct mbr_table *t)
{
	uint8_t *sector;
	int rc, slot = 0;

	if (!d || !t)
		return -EINVAL;
	sector = calloc(1, d->sector_size);
	if (!sector)
		return -ENOMEM;
	/* Keep anything past byte 512 of a large sector as it is. */
	rc = gpt_dev_read(d, 0, 1, sector);
	if (rc) {
		free(sector);
		return rc;
	}
	memcpy(sector, t->boot_code, sizeof(t->boot_code));
	gpt_put32(sector + MBR_DISKSIG_OFF, t->disk_signature);
	gpt_put16(sector + MBR_DISKSIG_OFF + 4, t->reserved);
	memset(sector + MBR_PART_OFF, 0, 4 * MBR_ENTRY_SIZE);
	for (int i = 0; i < t->nparts; i++) {
		const struct mbr_part *p = &t->parts[i];
		int idx;

		if (p->is_logical)
			continue;
		idx = (p->number >= 1 && p->number <= 4) ? p->number - 1 : slot;
		if (idx > 3) {
			free(sector);
			return -ENOSPC;
		}
		mbr_encode_entry(p, sector + MBR_PART_OFF + idx * MBR_ENTRY_SIZE);
		if (idx >= slot)
			slot = idx + 1;
	}
	sector[MBR_SIG_OFF] = 0x55;
	sector[MBR_SIG_OFF + 1] = 0xaa;
	rc = gpt_dev_write(d, 0, 1, sector);
	free(sector);
	return rc;
}

int mbr_write_protective(struct gpt_dev *d, int keep_boot_code)
{
	struct mbr_table t;
	uint8_t *sector;
	int rc;

	if (!d)
		return -EINVAL;
	memset(&t, 0, sizeof(t));
	if (keep_boot_code) {
		sector = malloc(d->sector_size);
		if (!sector)
			return -ENOMEM;
		rc = gpt_dev_read(d, 0, 1, sector);
		if (rc) {
			free(sector);
			return rc;
		}
		if (sector[MBR_SIG_OFF] == 0x55 &&
		    sector[MBR_SIG_OFF + 1] == 0xaa) {
			memcpy(t.boot_code, sector, sizeof(t.boot_code));
			t.disk_signature = gpt_le32(sector + MBR_DISKSIG_OFF);
		}
		free(sector);
	}
	t.kind = MBR_KIND_PROTECTIVE;
	t.nparts = 1;
	t.parts[0].number = 1;
	t.parts[0].status = 0;
	t.parts[0].type = MBR_TYPE_GPT_PROTECTIVE;
	t.parts[0].first_lba = 1;
	t.parts[0].nsectors = d->nsectors - 1;
	if (t.parts[0].nsectors > 0xffffffffULL)
		t.parts[0].nsectors = 0xffffffffULL;
	/* The protective entry starts at CHS 0/0/2, as the UEFI
	 * specification shows it. */
	rc = mbr_write(d, &t);
	return rc;
}

const char *mbr_type_name(uint8_t type)
{
	switch (type) {
	case 0x00: return "Empty";
	case 0x01: return "FAT12";
	case 0x02: return "XENIX root";
	case 0x03: return "XENIX usr";
	case 0x04: return "FAT16 <32M";
	case 0x05: return "Extended";
	case 0x06: return "FAT16";
	case 0x07: return "NTFS/exFAT/HPFS";
	case 0x08: return "AIX";
	case 0x0a: return "OS/2 Boot Manager";
	case 0x0b: return "W95 FAT32";
	case 0x0c: return "W95 FAT32 (LBA)";
	case 0x0e: return "W95 FAT16 (LBA)";
	case 0x0f: return "W95 extended (LBA)";
	case 0x11: return "Hidden FAT12";
	case 0x12: return "Diagnostics";
	case 0x14: return "Hidden FAT16 <32M";
	case 0x16: return "Hidden FAT16";
	case 0x17: return "Hidden NTFS/HPFS";
	case 0x1b: return "Hidden W95 FAT32";
	case 0x1c: return "Hidden W95 FAT32 (LBA)";
	case 0x1e: return "Hidden W95 FAT16 (LBA)";
	case 0x27: return "Hidden NTFS recovery";
	case 0x39: return "Plan 9";
	case 0x42: return "Dynamic disk (LDM)";
	case 0x4d: return "QNX4.x";
	case 0x63: return "GNU HURD / SysV";
	case 0x80: return "Old Minix";
	case 0x81: return "Minix";
	case 0x82: return "Swap";
	case 0x83: return "Generic data";
	case 0x84: return "Hibernation";
	case 0x85: return "Extended (alt.)";
	case 0x86: return "NTFS volume set";
	case 0x87: return "NTFS volume set";
	case 0x88: return "Plaintext";
	case 0x8e: return "LVM";
	case 0x9f: return "BSD/OS";
	case 0xa0: return "Hibernation";
	case 0xa5: return "FreeBSD";
	case 0xa6: return "OpenBSD";
	case 0xa8: return "Darwin UFS";
	case 0xa9: return "NetBSD";
	case 0xab: return "Darwin boot";
	case 0xaf: return "HFS / HFS+";
	case 0xb7: return "BSDI fs";
	case 0xb8: return "BSDI swap";
	case 0xbe: return "Solaris boot";
	case 0xbf: return "Solaris";
	case 0xda: return "Non-FS data";
	case 0xde: return "Dell utility";
	case 0xe8: return "LUKS";
	case 0xeb: return "BeOS BFS";
	case 0xee: return "GPT protective";
	case 0xef: return "EFI system";
	case 0xf0: return "PA-RISC boot";
	case 0xfb: return "VMware VMFS";
	case 0xfc: return "VMware VMKCORE";
	case 0xfd: return "RAID autodetect";
	default: return "Unknown";
	}
}

const char *mbr_kind_name(int kind)
{
	switch (kind) {
	case MBR_KIND_NONE: return "none";
	case MBR_KIND_EMPTY: return "empty";
	case MBR_KIND_PROTECTIVE: return "protective";
	case MBR_KIND_HYBRID: return "hybrid";
	case MBR_KIND_LEGACY: return "legacy";
	default: return "unknown";
	}
}
