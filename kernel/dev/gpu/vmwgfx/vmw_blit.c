// LikeOS -- vmwgfx: processor copies between buffer objects.
//
// See vmw_blit.h.  Two line copiers plug into the blit: vmw_memcpy() copies,
// vmw_diff_memcpy() compares first and copies only the span between the
// first and the last byte that differ, growing a bounding rectangle as it
// goes.  vmw_bo_cpu_blit() walks a w x h byte rectangle of two page-backed
// objects row by row, splitting each row wherever either side crosses a page,
// and hands every piece to the copier.  Both objects are reached through the
// direct map, so nothing is mapped or unmapped on the way.
//
// What the diff buys: the screen-target path copies a client's pixels into
// the driver's display surface and then asks the host to re-read the box it
// copied.  The host's re-read is the expensive half -- it is a copy of guest
// memory on the other side -- and a damage rectangle routinely covers far
// more than what actually changed (a blinking cursor reports its line, a
// redraw reports the window).  Comparing while copying costs one pass the
// copy makes anyway, and shrinks the box the host has to read to the pixels
// that changed, or to nothing at all.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from VMware's code: GPL-2.0 OR MIT
// Portions Copyright 2017 VMware, Inc., Palo Alto, CA., USA

#include <kernel/dev/gpu/vmwgfx/vmw_gb.h>
#include <kernel/mm/memory.h>
#include <kernel/uapi/bug.h>

/* Signed byte counts: the "last difference" search answers a negative value
 * for "none". */
typedef long vmw_ssize_t;

/* Round a byte offset down to the copy granularity (bytes per pixel, which
 * need not be a power of two).  Only for offsets known to be >= 0. */
static size_t vmw_round_down(size_t x, size_t granularity)
{
	return x - x % granularity;
}

/* The same for the signed answer of a last-difference search: a negative
 * value means "no difference" and must stay negative. */
static vmw_ssize_t vmw_round_down_s(vmw_ssize_t x, size_t granularity)
{
	if (x < 0)
		return x;
	return (vmw_ssize_t)vmw_round_down((size_t)x, granularity);
}

/*
 * find_first_diff() for one unsigned integer width.  @size and the return
 * value are in bytes; a return >= @size means no difference.
 */
#define VMW_FIND_FIRST_DIFF(_type)					\
static size_t vmw_find_first_diff_ ## _type				\
	(const _type *dst, const _type *src, size_t size)		\
{									\
	size_t i;							\
									\
	for (i = 0; i < size; i += sizeof(_type)) {			\
		if (*dst++ != *src++)					\
			break;						\
	}								\
									\
	return i;							\
}

/*
 * find_last_diff() for one unsigned integer width.  The pointers point just
 * past the END of the area examined.  @size and the return value are in
 * bytes: the length of the prefix that still contains the last difference,
 * 0 when there is none.
 */
#define VMW_FIND_LAST_DIFF(_type)					\
static vmw_ssize_t vmw_find_last_diff_ ## _type(			\
	const _type *dst, const _type *src, size_t size)		\
{									\
	while (size) {							\
		if (*--dst != *--src)					\
			break;						\
									\
		size -= sizeof(_type);					\
	}								\
	return (vmw_ssize_t)size;					\
}

/* Wider compares are faster, as long as both sides share an alignment; the
 * narrower ones finish the job at the needed resolution. */
VMW_FIND_FIRST_DIFF(u8)
VMW_FIND_LAST_DIFF(u8)

VMW_FIND_FIRST_DIFF(u16)
VMW_FIND_LAST_DIFF(u16)

VMW_FIND_FIRST_DIFF(u32)
VMW_FIND_LAST_DIFF(u32)

VMW_FIND_FIRST_DIFF(u64)
VMW_FIND_LAST_DIFF(u64)

/* Size-aligned compares: (addr - align_down(addr, sizeof(_type))). */
#define SPILL(_var, _type) ((unsigned long)(_var) & (sizeof(_type) - 1))

/*
 * One width of vmw_find_first_diff(): compare a byte-wise head up to the
 * alignment when both sides are equally misaligned, then whole words.  When
 * the answer is final at the resolution asked for, this returns from the
 * enclosing function; otherwise it narrows the window to start at the first
 * differing word and falls through to the next, narrower width.
 */
#define VMW_TRY_FIND_FIRST_DIFF(_type)					\
do {									\
	unsigned int spill = SPILL(dst, _type);				\
	size_t diff_offs;						\
									\
	if (spill && spill == SPILL(src, _type) &&			\
	    sizeof(_type) - spill <= size) {				\
		spill = sizeof(_type) - spill;				\
		diff_offs = vmw_find_first_diff_u8(dst, src, spill);	\
		if (diff_offs < spill)					\
			return vmw_round_down(offset + diff_offs,	\
					      granularity);		\
									\
		dst += spill;						\
		src += spill;						\
		size -= spill;						\
		offset += spill;					\
		spill = 0;						\
	}								\
	if (!spill && !SPILL(src, _type)) {				\
		size_t to_copy = size & ~(sizeof(_type) - 1);		\
									\
		diff_offs = vmw_find_first_diff_ ## _type		\
			((const _type *)dst, (const _type *)src, to_copy); \
		/* A word that differs is an answer at this width's	\
		 * resolution; all words equal with a tail of fewer	\
		 * than sizeof(_type) bytes left over is not one -- the	\
		 * tail goes on to the narrower widths. */		\
		if (diff_offs >= size ||				\
		    (granularity == sizeof(_type) && diff_offs < to_copy)) \
			return (offset + diff_offs);			\
									\
		dst += diff_offs;					\
		src += diff_offs;					\
		size -= diff_offs;					\
		offset += diff_offs;					\
	}								\
} while (0)

/**
 * vmw_find_first_diff - find the first difference between dst and src
 *
 * @dst: The destination address
 * @src: The source address
 * @size: Number of bytes to compare
 * @granularity: The granularity needed for the return value in bytes.
 * return: The offset from find start where the first difference was
 * encountered in bytes. If no difference was found, the function returns
 * a value >= @size.
 */
static size_t vmw_find_first_diff(const u8 *dst, const u8 *src, size_t size,
				  size_t granularity)
{
	size_t offset = 0;

	VMW_TRY_FIND_FIRST_DIFF(u64);
	VMW_TRY_FIND_FIRST_DIFF(u32);
	VMW_TRY_FIND_FIRST_DIFF(u16);

	return vmw_round_down(offset + vmw_find_first_diff_u8(dst, src, size),
			      granularity);
}

/*
 * One width of vmw_find_last_diff(), the mirror image of the above: a
 * byte-wise tail down to the alignment, then whole words backwards.
 */
#define VMW_TRY_FIND_LAST_DIFF(_type)					\
do {									\
	unsigned int spill = SPILL(dst, _type);				\
	vmw_ssize_t location;						\
	vmw_ssize_t diff_offs;						\
									\
	if (spill && spill <= size && spill == SPILL(src, _type)) {	\
		diff_offs = vmw_find_last_diff_u8(dst, src, spill);	\
		if (diff_offs) {					\
			location = (vmw_ssize_t)size - spill +		\
				   diff_offs - 1;			\
			return vmw_round_down_s(location, granularity);	\
		}							\
									\
		dst -= spill;						\
		src -= spill;						\
		size -= spill;						\
		spill = 0;						\
	}								\
	if (!spill && !SPILL(src, _type)) {				\
		size_t to_copy = vmw_round_down(size, sizeof(_type));	\
									\
		diff_offs = vmw_find_last_diff_ ## _type		\
			((const _type *)dst, (const _type *)src, to_copy); \
		if (!diff_offs) {					\
			/* Every whole word matches.  What is left is	\
			 * the head below the first aligned word, fewer	\
			 * than sizeof(_type) bytes, and it still has to	\
			 * be looked at: the narrower widths do. */	\
			if (size == to_copy)				\
				return -1;				\
			dst -= to_copy;					\
			src -= to_copy;					\
			size -= to_copy;				\
		} else {						\
			location = (vmw_ssize_t)(size - to_copy) +	\
				   diff_offs -				\
				   (vmw_ssize_t)sizeof(_type);		\
			if (granularity == sizeof(_type))		\
				return location;			\
									\
			dst -= to_copy - (size_t)diff_offs;		\
			src -= to_copy - (size_t)diff_offs;		\
			size -= to_copy - (size_t)diff_offs;		\
		}							\
	}								\
} while (0)

/**
 * vmw_find_last_diff - find the last difference between dst and src
 *
 * @dst: The destination address
 * @src: The source address
 * @size: Number of bytes to compare
 * @granularity: The granularity needed for the return value in bytes.
 * return: The offset from find start where the last difference was
 * encountered in bytes, or a negative value if no difference was found.
 */
static vmw_ssize_t vmw_find_last_diff(const u8 *dst, const u8 *src,
				      size_t size, size_t granularity)
{
	dst += size;
	src += size;

	VMW_TRY_FIND_LAST_DIFF(u64);
	VMW_TRY_FIND_LAST_DIFF(u32);
	VMW_TRY_FIND_LAST_DIFF(u16);

	return vmw_round_down_s(vmw_find_last_diff_u8(dst, src, size) - 1,
				granularity);
}

/**
 * vmw_memcpy - plain copy with the signature of a struct vmw_diff_cpy
 * copier.
 *
 * @diff: The struct vmw_diff_cpy closure argument (unused).
 * @dest: The copy destination.
 * @src: The copy source.
 * @n: Number of bytes to copy.
 */
void vmw_memcpy(struct vmw_diff_cpy *diff, u8 *dest, const u8 *src, size_t n)
{
	(void)diff;
	mm_memcpy(dest, src, n);
}

/**
 * vmw_adjust_rect - grow the bounding box over a newly found difference
 *
 * @diff: The struct vmw_diff_cpy used to track the modified bounding box.
 * @diff_offs: The offset from @diff->line_offset where the difference was
 * found.
 */
static void vmw_adjust_rect(struct vmw_diff_cpy *diff, size_t diff_offs)
{
	size_t offs = (diff_offs + diff->line_offset) / (size_t)diff->cpp;
	struct drm_mode_rect_k *rect = &diff->rect;

	rect->x1 = min_t(int, rect->x1, (int)offs);
	rect->x2 = max_t(int, rect->x2, (int)offs + 1);
	rect->y1 = min_t(int, rect->y1, (int)diff->line);
	rect->y2 = max_t(int, rect->y2, (int)diff->line + 1);
}

/**
 * vmw_diff_memcpy - copy that records a bounding box of what it changed.
 *
 * @diff: The struct vmw_diff_cpy used to track the modified bounding box.
 * @dest: The copy destination.
 * @src: The copy source.
 * @n: Number of bytes to copy.
 *
 * The caller pre-loads @diff->line with the current line number and
 * @diff->line_offset with the byte offset within the line where this copy
 * starts (vmw_bo_cpu_blit() does both), and @diff->cpp with the bytes per
 * unit in the horizontal direction -- bytes per pixel, typically.  That is
 * the resolution of the box; a larger cpp also compares faster.
 *
 * Only the span from the first to the last differing unit is written:
 * everything outside it already holds what the source holds.
 *
 * A piece whose length is not a whole number of units, or that does not start
 * on a unit on both sides, is compared byte by byte instead.  A source at an
 * odd offset or pitch produces both kinds, and so does a row split where a
 * page boundary of one side falls inside a pixel.  Comparing such a piece in
 * units would let the copy of the last unit run past the end of the piece,
 * and dropping it would leave stale pixels on the screen; at byte resolution
 * the box still lands on the right pixels, because bytes are converted to
 * pixels through the line offset.
 */
void vmw_diff_memcpy(struct vmw_diff_cpy *diff, u8 *dest, const u8 *src,
		     size_t n)
{
	vmw_ssize_t csize, byte_len;
	size_t granularity;

	if (WARN_ON_ONCE(diff->cpp <= 0)) {
		mm_memcpy(dest, src, n);
		return;
	}
	/* Unit-wide compares only where the piece is whole units and starts
	 * on a unit, in memory on both sides and within the line: the
	 * word-wise searches round their answers to the granularity counting
	 * from the start of the piece. */
	granularity = (size_t)diff->cpp;
	if ((granularity != 2 && granularity != 4 && granularity != 8) ||
	    vmw_round_down(n, granularity) != n ||
	    diff->line_offset % granularity != 0 ||
	    ((uintptr_t)dest % granularity) != 0 ||
	    ((uintptr_t)src % granularity) != 0)
		granularity = 1;

	csize = (vmw_ssize_t)vmw_find_first_diff(dest, src, n, granularity);
	if ((size_t)csize < n) {
		vmw_adjust_rect(diff, (size_t)csize);
		byte_len = (vmw_ssize_t)granularity;

		/*
		 * Starting from where the first difference was found, find
		 * the last one, and copy everything in between.
		 */
		diff->line_offset += (size_t)csize;
		dest += csize;
		src += csize;
		n -= (size_t)csize;
		csize = vmw_find_last_diff(dest, src, n, granularity);
		if (csize >= 0) {
			byte_len += csize;
			vmw_adjust_rect(diff, (size_t)csize);
		}
		mm_memcpy(dest, src, (size_t)byte_len);
	}
	diff->line_offset += n;
}

/**
 * struct vmw_bo_blit_line_data - state carried along one blit
 *
 * @mapped_dst: Page index of @dst_addr in the destination object.
 * @dst_addr: Kernel virtual address of that destination page, or NULL.
 * @dst: The destination object.
 * @dst_num_pages: Number of pages of the destination object.
 * @mapped_src: Page index of @src_addr in the source object.
 * @src_addr: Kernel virtual address of that source page, or NULL.
 * @src: The source object.
 * @src_num_pages: Number of pages of the source object.
 * @diff: Struct vmw_diff_cpy, in the end forwarded to the copier.
 *
 * Pages are reached through the direct map, so "mapping" one is a lookup;
 * the last one of each side is kept because consecutive pieces of a row
 * almost always fall in the same page.
 */
struct vmw_bo_blit_line_data {
	u32 mapped_dst;
	u8 *dst_addr;
	struct drm_gem_object *dst;
	u32 dst_num_pages;
	u32 mapped_src;
	u8 *src_addr;
	struct drm_gem_object *src;
	u32 src_num_pages;
	struct vmw_diff_cpy *diff;
};

/**
 * vmw_bo_cpu_blit_line - blit part of a line from one object to another
 *
 * @d: Blit data as described above.
 * @dst_offset: Destination copy start offset from start of the object.
 * @src_offset: Source copy start offset from start of the object.
 * @bytes_to_copy: Number of bytes to copy in this line.
 */
static int vmw_bo_cpu_blit_line(struct vmw_bo_blit_line_data *d,
				u32 dst_offset,
				u32 src_offset,
				u32 bytes_to_copy)
{
	struct vmw_diff_cpy *diff = d->diff;

	while (bytes_to_copy) {
		u32 copy_size = bytes_to_copy;
		u32 dst_page = dst_offset / PAGE_SIZE;
		u32 src_page = src_offset / PAGE_SIZE;
		u32 dst_page_offset = dst_offset % PAGE_SIZE;
		u32 src_page_offset = src_offset % PAGE_SIZE;

		copy_size = min_t(u32, copy_size, PAGE_SIZE - dst_page_offset);
		copy_size = min_t(u32, copy_size, PAGE_SIZE - src_page_offset);

		if (d->dst_addr && dst_page != d->mapped_dst)
			d->dst_addr = NULL;
		if (d->src_addr && src_page != d->mapped_src)
			d->src_addr = NULL;

		if (!d->dst_addr) {
			if (WARN_ON_ONCE(dst_page >= d->dst_num_pages))
				return -EINVAL;

			d->dst_addr = drm_gem_page_virt(d->dst, dst_page);
			if (!d->dst_addr)
				return -EIO;

			d->mapped_dst = dst_page;
		}

		if (!d->src_addr) {
			if (WARN_ON_ONCE(src_page >= d->src_num_pages))
				return -EINVAL;

			d->src_addr = drm_gem_page_virt(d->src, src_page);
			if (!d->src_addr)
				return -EIO;

			d->mapped_src = src_page;
		}
		diff->do_cpy(diff, d->dst_addr + dst_page_offset,
			     d->src_addr + src_page_offset, copy_size);

		bytes_to_copy -= copy_size;
		dst_offset += copy_size;
		src_offset += copy_size;
	}

	return 0;
}

/* Does a stride-separated rectangle starting at `offset' stay inside an
 * object of `size' bytes?  Every product and sum is checked, because the
 * offsets and strides come from a client's framebuffer description. */
static int vmw_blit_in_bounds(u64 size, u32 offset, u32 stride,
			      u32 width_in_bytes, u32 height)
{
	u64 end;

	if (!stride || stride < width_in_bytes)
		return 0;
	if (__builtin_mul_overflow((u64)stride, (u64)height - 1, &end) ||
	    __builtin_add_overflow(end, (u64)width_in_bytes, &end) ||
	    __builtin_add_overflow(end, (u64)offset, &end))
		return 0;
	return end <= size;
}

/**
 * vmw_bo_cpu_blit - in-kernel processor blit between two objects.
 *
 * @dst: Destination object.
 * @dst_offset: Destination offset of blit start in bytes.
 * @dst_stride: Destination stride in bytes.
 * @src: Source object.
 * @src_offset: Source offset of blit start in bytes.
 * @src_stride: Source stride in bytes.
 * @w: Width of blit, in BYTES.
 * @h: Height of blit, in lines.
 * @diff: The struct vmw_diff_cpy used to track the modified bounding box
 * (VMW_CPU_BLIT_INITIALIZER for a plain copy).
 * return: Zero on success. Negative error value on failure.  Caller bugs
 * (an object blitted onto itself, a rectangle outside an object) are
 * refused, and the first of each kind is warned about.
 *
 * With the diff copier, @diff->rect comes back as the bounding box of the
 * pixels that changed, in pixels of the DESTINATION (its line number from
 * @dst_offset / @dst_stride, its column from the remainder over @diff->cpp);
 * x1 >= x2 means nothing changed.
 *
 * Neither object may be one whose pages live in device memory: both are
 * read and written through the direct map.
 */
int vmw_bo_cpu_blit(struct drm_gem_object *dst, u32 dst_offset, u32 dst_stride,
		    struct drm_gem_object *src, u32 src_offset, u32 src_stride,
		    u32 w, u32 h, struct vmw_diff_cpy *diff)
{
	struct vmw_bo_blit_line_data d;
	u32 j, initial_line;
	int ret = 0;

	if (WARN_ON_ONCE(!dst || !src || !diff || !diff->do_cpy))
		return -EINVAL;
	if (WARN_ON(dst == src))
		return -EINVAL;
	if (!w || !h)
		return 0;
	if (!dst->pages || !src->pages)
		return -EINVAL;
	if (WARN_ON_ONCE(!vmw_blit_in_bounds(dst->size, dst_offset, dst_stride,
					     w, h) ||
			 !vmw_blit_in_bounds(src->size, src_offset, src_stride,
					     w, h)))
		return -EINVAL;

	initial_line = dst_offset / dst_stride;

	mm_memset(&d, 0, sizeof(d));
	d.dst = dst;
	d.src = src;
	d.dst_num_pages = dst->npages;
	d.src_num_pages = src->npages;
	d.diff = diff;

	for (j = 0; j < h; ++j) {
		diff->line = j + initial_line;
		diff->line_offset = dst_offset % dst_stride;
		ret = vmw_bo_cpu_blit_line(&d, dst_offset, src_offset, w);
		if (ret)
			break;

		dst_offset += dst_stride;
		src_offset += src_stride;
	}

	return ret;
}
