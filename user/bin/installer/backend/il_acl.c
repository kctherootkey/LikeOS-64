/* libinstaller -- POSIX ACLs from their text form to the attribute value.
 *
 * Archives carry access control lists as text (pax records
 * SCHILY.acl.access and SCHILY.acl.default); the kernel takes them as the
 * binary value of the attributes system.posix_acl_access and
 * system.posix_acl_default:
 *
 *   u32 version (2), then per entry: u16 tag, u16 permissions, u32 id
 *
 * all little-endian, entries sorted by tag and, within the named user and
 * group entries, by id.  The owner, owning group, mask and other entries
 * have no id (0xffffffff).
 *
 * Accepted text: entries separated by ',' or line ends, '#' comments to the
 * end of a line, tags in long or short spelling (user/u, group/g, mask/m,
 * other/o), a qualifier that is a numeric id or a name followed by a fourth
 * field with the numeric id (the "user:name:rwx:1000" form), permissions as
 * any combination of r, w, x and '-'.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "il_tar.h"

#define IL_ACL_VERSION     2u
#define IL_ACL_USER_OBJ    0x01u
#define IL_ACL_USER        0x02u
#define IL_ACL_GROUP_OBJ   0x04u
#define IL_ACL_GROUP       0x08u
#define IL_ACL_MASK        0x10u
#define IL_ACL_OTHER       0x20u
#define IL_ACL_UNDEFINED   0xffffffffu
#define IL_ACL_MAX_ENTRIES 1024

struct il_acl_ent {
	uint16_t tag;
	uint16_t perm;
	uint32_t id;
};

static int il_acl_cmp(const void *a, const void *b)
{
	const struct il_acl_ent *x = a, *y = b;

	if (x->tag != y->tag)
		return x->tag < y->tag ? -1 : 1;
	if (x->id != y->id)
		return x->id < y->id ? -1 : 1;
	return 0;
}

static int il_acl_tag(const char *s, size_t n, int named)
{
	if ((n == 4 && memcmp(s, "user", 4) == 0) || (n == 1 && s[0] == 'u'))
		return named ? IL_ACL_USER : IL_ACL_USER_OBJ;
	if ((n == 5 && memcmp(s, "group", 5) == 0) || (n == 1 && s[0] == 'g'))
		return named ? IL_ACL_GROUP : IL_ACL_GROUP_OBJ;
	if ((n == 4 && memcmp(s, "mask", 4) == 0) || (n == 1 && s[0] == 'm'))
		return IL_ACL_MASK;
	if ((n == 5 && memcmp(s, "other", 5) == 0) || (n == 1 && s[0] == 'o'))
		return IL_ACL_OTHER;
	return -1;
}

static int il_acl_perm(const char *s, size_t n)
{
	int p = 0;

	if (n == 0)
		return -1;
	/* An octal digit is accepted too ("user::7"). */
	if (n == 1 && s[0] >= '0' && s[0] <= '7')
		return s[0] - '0';
	for (size_t i = 0; i < n; i++) {
		switch (s[i]) {
		case 'r':
			p |= 4;
			break;
		case 'w':
			p |= 2;
			break;
		case 'x':
			p |= 1;
			break;
		case '-':
			break;
		default:
			return -1;
		}
	}
	return p;
}

static int il_acl_id(const char *s, size_t n, uint32_t *out)
{
	uint64_t v = 0;

	if (n == 0)
		return -1;
	for (size_t i = 0; i < n; i++) {
		if (s[i] < '0' || s[i] > '9')
			return -1;
		v = v * 10 + (uint64_t)(s[i] - '0');
		if (v >= IL_ACL_UNDEFINED)
			return -1;
	}
	*out = (uint32_t)v;
	return 0;
}

static void il_put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void il_put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* Splits one entry (blanks trimmed) into at most 4 ':' fields. */
static int il_acl_fields(const char *s, size_t n, const char *f[4],
			 size_t fl[4])
{
	int nf = 0;
	size_t start = 0;

	for (size_t i = 0; i <= n; i++) {
		if (i == n || s[i] == ':') {
			if (nf == 4)
				return -1;
			f[nf] = s + start;
			fl[nf] = i - start;
			nf++;
			start = i + 1;
		}
	}
	return nf;
}

int il_acl_from_text(const char *text, uint8_t **out, size_t *outlen,
		     int *base_only, char *err, size_t errlen)
{
	struct il_acl_ent *ents;
	int n = 0;
	int have_uo = 0, have_go = 0, have_o = 0, have_m = 0, named = 0;
	uint16_t group_class = 0;
	const char *p = text;
	uint8_t *buf;

	*out = NULL;
	*outlen = 0;
	*base_only = 0;
	/* One more than the text may hold: room for a mask added below. */
	ents = calloc(IL_ACL_MAX_ENTRIES + 1, sizeof(*ents));
	if (!ents)
		return -ENOMEM;
	while (*p) {
		const char *e = p, *f[4];
		size_t len, fl[4];
		int nf, tag, perm;
		uint32_t id = IL_ACL_UNDEFINED;
		int is_named;

		while (*e && *e != ',' && *e != '\n')
			e++;
		len = (size_t)(e - p);
		/* comments and blanks */
		{
			const char *h = memchr(p, '#', len);

			if (h)
				len = (size_t)(h - p);
			while (len && (*p == ' ' || *p == '\t')) {
				p++;
				len--;
			}
			while (len && (p[len - 1] == ' ' || p[len - 1] == '\t' ||
				       p[len - 1] == '\r'))
				len--;
		}
		if (len == 0)
			goto next;
		nf = il_acl_fields(p, len, f, fl);
		if (nf < 3) {
			snprintf(err, errlen, "ACL entry '%.*s' has too few "
				 "fields", (int)(len > 40 ? 40 : len), p);
			goto bad;
		}
		is_named = fl[1] > 0;
		tag = il_acl_tag(f[0], fl[0], is_named);
		if (tag < 0) {
			snprintf(err, errlen, "unknown ACL tag in '%.*s'",
				 (int)(len > 40 ? 40 : len), p);
			goto bad;
		}
		perm = il_acl_perm(f[2], fl[2]);
		if (perm < 0) {
			snprintf(err, errlen, "bad ACL permissions in '%.*s'",
				 (int)(len > 40 ? 40 : len), p);
			goto bad;
		}
		if (tag == IL_ACL_USER || tag == IL_ACL_GROUP) {
			/* A numeric qualifier, or a name with the id as the
			 * fourth field. */
			if (il_acl_id(f[1], fl[1], &id) != 0 &&
			    (nf < 4 || il_acl_id(f[3], fl[3], &id) != 0)) {
				snprintf(err, errlen, "ACL entry '%.*s' names "
					 "no numeric id",
					 (int)(len > 40 ? 40 : len), p);
				goto bad;
			}
			named++;
		} else if (is_named && tag != IL_ACL_MASK &&
			   tag != IL_ACL_OTHER) {
			snprintf(err, errlen, "unexpected qualifier in '%.*s'",
				 (int)(len > 40 ? 40 : len), p);
			goto bad;
		}
		if (n >= IL_ACL_MAX_ENTRIES) {
			snprintf(err, errlen, "ACL has too many entries");
			goto bad;
		}
		/* A repeated entry replaces the earlier one. */
		{
			int k;

			for (k = 0; k < n; k++)
				if (ents[k].tag == tag && ents[k].id == id)
					break;
			ents[k].tag = (uint16_t)tag;
			ents[k].perm = (uint16_t)perm;
			ents[k].id = id;
			if (k == n)
				n++;
		}
		switch (tag) {
		case IL_ACL_USER_OBJ:
			have_uo = 1;
			break;
		case IL_ACL_GROUP_OBJ:
			have_go = 1;
			group_class |= (uint16_t)perm;
			break;
		case IL_ACL_OTHER:
			have_o = 1;
			break;
		case IL_ACL_MASK:
			have_m = 1;
			break;
		default:
			group_class |= (uint16_t)perm;
			break;
		}
next:
		p = *e ? e + 1 : e;
	}
	if (n == 0) {
		free(ents);
		*base_only = 1;
		return 0;
	}
	if (!have_uo || !have_go || !have_o) {
		snprintf(err, errlen, "ACL lacks the owner, group or other "
			 "entry");
		goto bad;
	}
	if (named && !have_m) {
		/* Named entries need a mask; the union of the group class is
		 * what a tool computes for one left out. */
		ents[n].tag = IL_ACL_MASK;
		ents[n].perm = group_class;
		ents[n].id = IL_ACL_UNDEFINED;
		n++;
		have_m = 1;
	}
	*base_only = !named && !have_m;
	qsort(ents, (size_t)n, sizeof(*ents), il_acl_cmp);
	buf = malloc(4 + (size_t)n * 8);
	if (!buf) {
		free(ents);
		return -ENOMEM;
	}
	il_put32(buf, IL_ACL_VERSION);
	for (int i = 0; i < n; i++) {
		il_put16(buf + 4 + 8 * i, ents[i].tag);
		il_put16(buf + 6 + 8 * i, ents[i].perm);
		il_put32(buf + 8 + 8 * i, ents[i].id);
	}
	free(ents);
	*out = buf;
	*outlen = 4 + (size_t)n * 8;
	return 0;
bad:
	free(ents);
	return -EINVAL;
}
