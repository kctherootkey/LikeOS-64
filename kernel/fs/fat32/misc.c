/* LikeOS - FAT filesystem driver: byte helpers, the long-name checksum, the
 * error policy (errors=), corruption reports and timestamps
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <kernel/fs/fat32/fat_int.h>

/* ===========================================================================
 * 2. Byte helpers and checksums
 * =========================================================================== */

static inline uint64_t fat_max64(uint64_t a, uint64_t b)
{
	return a > b ? a : b;
}

unsigned fat_ilog2(unsigned long v)
{
	unsigned s = 0;

	while (v > 1) {
		v >>= 1;
		s++;
	}
	return s;
}

unsigned long fat_strlen(const char *s)
{
	unsigned long n = 0;

	while (s[n])
		n++;
	return n;
}

int fat_streq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

/* Case-insensitive ASCII comparison of option words. */
int fat_streq_ci(const char *a, const char *b)
{
	while (*a && *b) {
		char x = *a, y = *b;

		if (x >= 'A' && x <= 'Z')
			x = (char)(x + 32);
		if (y >= 'A' && y <= 'Z')
			y = (char)(y + 32);
		if (x != y)
			return 0;
		a++;
		b++;
	}
	return *a == *b;
}

/* The checksum of an 11-byte short name that every long-name entry of the
 * set carries: an 8-bit rotate-right-and-add over the name bytes. */
uint8_t fat_lfn_checksum(const uint8_t name[11])
{
	uint8_t sum = 0;

	for (int i = 0; i < 11; i++)
		sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + name[i]);
	return sum;
}

/* ===========================================================================
 * 5. Error policy
 *
 * Corruption found while mounted: the operation that met it fails with
 * ST_IO, and the file system reacts as errors= says -- remount-ro (the
 * default) latches the instance read-only, continue only logs, panic stops
 * the machine.  Nothing extra is written to record the error: a volume armed
 * for writing keeps its dirty marks (the clean marking refuses once errors
 * were seen), which is exactly the "check me" mark FAT has; one that was
 * only read stays untouched.
 * =========================================================================== */

const char *fat_name_of(fat_fs_t *fs)
{
	const char *src = fs->mnt ? fs->mnt->source : NULL;

	/* The mount's source is a pointer the mount table fills in; it may
	 * not be there yet (or any more). */
	if (src && src[0])
		return src;
	return (fs->bdev && fs->bdev->name) ? fs->bdev->name : "?";
}

void fat_fs_error(fat_fs_t *fs, const char *what, uint32_t where)
{
	if (!fs)
		return;
	fs->errors_seen++;
	kprintf("fat: %s: ERROR: %s (at %u)\n", fat_name_of(fs), what, where);
	switch (fs->opts.errors) {
	case FAT_ERRORS_PANIC:
		panic("fat: %s: errors=panic: %s\n", fat_name_of(fs), what);
		break;
	case FAT_ERRORS_CONTINUE:
		break;
	default:
		if (!fs->ro_latched) {
			fs->ro_latched = 1;
			if (!fs->ro)
				kprintf("fat: %s: remounting read-only\n",
					fat_name_of(fs));
		}
		break;
	}
}

/* ===========================================================================
 * 11. Timestamps
 *
 * FAT keeps local time: a date (Year-1980 in bits 9-15, Month 5-8, Day 0-4)
 * and a time (Hour 11-15, Minute 5-10, Seconds/2 0-4), 2-second resolution;
 * the creation time adds 10 ms units (0..199, carrying the odd second); the
 * access time is a date only.  What "local" means is the mount's: tz=UTC
 * (the default here) or time_offset= minutes east of UTC.
 * =========================================================================== */

/* Days since 1970-01-01 of a civil date (proleptic Gregorian). */
static int64_t fat_days_from_civil(int64_t y, unsigned m, unsigned d)
{
	int64_t era;
	unsigned yoe, doy, doe;

	y -= m <= 2;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = (unsigned)(y - era * 400);
	doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (int64_t)doe - 719468;
}

/* The civil date of a day count since 1970-01-01. */
static void fat_civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d)
{
	int64_t era;
	unsigned doe, yoe, doy, mp;

	z += 719468;
	era = (z >= 0 ? z : z - 146096) / 146097;
	doe = (unsigned)(z - era * 146097);
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	*y = (int64_t)yoe + era * 400;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	*d = doy - (153 * mp + 2) / 5 + 1;
	*m = mp < 10 ? mp + 3 : mp - 9;
	*y += *m <= 2;
}

/* Minutes east of UTC that "local time" on this volume means. */
static int fat_tz_offset(fat_fs_t *fs)
{
	if (fs->opts.tz_utc)
		return 0;
	return fs->opts.time_offset_given ? fs->opts.time_offset : 0;
}

/* Encode UTC `sec'.`nsec' as a FAT date, time and 10 ms count (any of the
 * outputs may be NULL).  Clamped to the range FAT can hold (1980-01-01 ..
 * 2107-12-31 23:59:59). */
void fat_time_encode(fat_fs_t *fs, int64_t sec, long nsec,
			    uint16_t *time, uint16_t *date, uint8_t *cs10)
{
	int64_t local = sec + (int64_t)fat_tz_offset(fs) * 60, days, y;
	unsigned m, d, rem, hh, mm, ss;

	if (nsec < 0 || nsec >= 1000000000L)
		nsec = 0;
	days = local >= 0 ? local / 86400 : -((-local + 86399) / 86400);
	rem = (unsigned)(local - days * 86400);
	fat_civil_from_days(days, &y, &m, &d);
	if (y < 1980) {
		y = 1980;
		m = 1;
		d = 1;
		rem = 0;
		nsec = 0;
	} else if (y > 2107) {
		y = 2107;
		m = 12;
		d = 31;
		rem = 86399;
		nsec = 990000000L;
	}
	hh = rem / 3600;
	mm = (rem / 60) % 60;
	ss = rem % 60;
	if (date)
		*date = (uint16_t)(((uint32_t)(y - 1980) << 9) |
				   ((uint32_t)m << 5) | d);
	if (time)
		*time = (uint16_t)((hh << 11) | (mm << 5) | (ss / 2));
	if (cs10)
		*cs10 = (uint8_t)((ss & 1) * 100 + nsec / 10000000L);
}

/* Decode a FAT date / time / 10 ms count to UTC seconds and nanoseconds.
 * Out-of-range fields are clamped (a date of 0, never set, reads as
 * 1980-01-01). */
void fat_time_decode(fat_fs_t *fs, uint16_t time, uint16_t date,
			    uint8_t cs10, uint64_t *sec, uint64_t *nsec)
{
	unsigned y = (date >> 9) + 1980, m = (date >> 5) & 0xF, d = date & 0x1F,
		 hh = time >> 11, mm = (time >> 5) & 0x3F,
		 ss = (time & 0x1F) * 2;
	int64_t t;

	if (m < 1 || m > 12)
		m = 1;
	if (d < 1 || d > 31)
		d = 1;
	if (hh > 23)
		hh = 23;
	if (mm > 59)
		mm = 59;
	if (ss > 59)
		ss = 58;
	if (cs10 > 199)
		cs10 = 0;
	ss += cs10 / 100;
	t = fat_days_from_civil(y, m, d) * 86400 + hh * 3600 + mm * 60 + ss;
	t -= (int64_t)fat_tz_offset(fs) * 60;
	if (t < 0)
		t = 0;
	*sec = (uint64_t)t;
	if (nsec)
		*nsec = (uint64_t)(cs10 % 100) * 10000000ULL;
}

/* The current time, encoded. */
void fat_now(fat_fs_t *fs, uint16_t *time, uint16_t *date, uint8_t *cs10)
{
	uint64_t ns = timer_get_realtime_ns();

	fat_time_encode(fs, (int64_t)(ns / 1000000000ULL),
			(long)(ns % 1000000000ULL), time, date, cs10);
}
