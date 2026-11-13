// LikeOS - ID allocator (ida.h)
//
// A bitmap under a spinlock.  Growing it allocates outside the lock (the
// allocator may not be called with a spinlock held); a grower that loses
// the race to another grower frees its copy and looks again.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/ke/ida.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/uapi/bug.h>

void ida_init(struct ida *ida, const char *name)
{
	BUG_ON(ida == NULL);
	spinlock_init(&ida->lock, name ? name : "ida");
	ida->bits = NULL;
	ida->nwords = 0;
	ida->nr_allocated = 0;
	ida->name = name ? name : "ida";
}

/* Make room for bit `id'.  0 or -ENOMEM.  Called without the lock. */
static int ida_grow(struct ida *ida, unsigned int id)
{
	uint32_t need = id / 64 + 1;
	uint64_t fl;

	for (;;) {
		uint32_t have, nw;
		uint64_t *nb, *old;

		spin_lock_irqsave(&ida->lock, &fl);
		have = ida->nwords;
		spin_unlock_irqrestore(&ida->lock, fl);
		if (have >= need)
			return 0;
		nw = have ? have : 1;
		while (nw < need)
			nw *= 2;
		nb = (uint64_t *)kcalloc(nw, sizeof(uint64_t));
		if (!nb)
			return -ENOMEM;
		spin_lock_irqsave(&ida->lock, &fl);
		if (ida->nwords != have) {
			/* Somebody else grew it meanwhile: look again. */
			spin_unlock_irqrestore(&ida->lock, fl);
			kfree(nb);
			continue;
		}
		for (uint32_t i = 0; i < have; i++)
			nb[i] = ida->bits[i];
		old = ida->bits;
		ida->bits = nb;
		ida->nwords = nw;
		spin_unlock_irqrestore(&ida->lock, fl);
		if (old)
			kfree(old);
		return 0;
	}
}

int ida_alloc_range(struct ida *ida, unsigned int min, unsigned int max)
{
	BUG_ON(ida == NULL);
	if (min > max || max > IDA_MAX)
		return -EINVAL;
	for (;;) {
		uint64_t fl;
		uint64_t limit;
		int r;

		spin_lock_irqsave(&ida->lock, &fl);
		limit = (uint64_t)ida->nwords * 64;
		for (uint64_t id = min; id <= max && id < limit; id++) {
			uint64_t w = ida->bits[id / 64];

			if (w == ~0ULL && (id % 64) == 0 && id + 63 <= max) {
				id += 63;
				continue;
			}
			if (!(w & (1ULL << (id % 64)))) {
				ida->bits[id / 64] = w | (1ULL << (id % 64));
				ida->nr_allocated++;
				spin_unlock_irqrestore(&ida->lock, fl);
				return (int)id;
			}
		}
		spin_unlock_irqrestore(&ida->lock, fl);
		/* Everything mapped in the range is taken. */
		if (limit > max)
			return -ENOSPC;
		r = ida_grow(ida, (unsigned int)(limit > min ? limit : min));
		if (r)
			return r;
	}
}

int ida_alloc_exact(struct ida *ida, unsigned int id)
{
	uint64_t fl;
	int r;

	BUG_ON(ida == NULL);
	if (id > IDA_MAX)
		return -EINVAL;
	r = ida_grow(ida, id);
	if (r)
		return r;
	spin_lock_irqsave(&ida->lock, &fl);
	if (ida->bits[id / 64] & (1ULL << (id % 64))) {
		r = -EBUSY;
	} else {
		ida->bits[id / 64] |= 1ULL << (id % 64);
		ida->nr_allocated++;
	}
	spin_unlock_irqrestore(&ida->lock, fl);
	return r;
}

void ida_free(struct ida *ida, unsigned int id)
{
	uint64_t fl;
	int ok = 0;

	BUG_ON(ida == NULL);
	spin_lock_irqsave(&ida->lock, &fl);
	if (id / 64 < ida->nwords &&
	    (ida->bits[id / 64] & (1ULL << (id % 64)))) {
		ida->bits[id / 64] &= ~(1ULL << (id % 64));
		ida->nr_allocated--;
		ok = 1;
	}
	spin_unlock_irqrestore(&ida->lock, fl);
	WARN(!ok, "ida %s: freeing id %u that is not allocated", ida->name,
	     id);
}

int ida_is_allocated(struct ida *ida, unsigned int id)
{
	uint64_t fl;
	int r = 0;

	spin_lock_irqsave(&ida->lock, &fl);
	if (id / 64 < ida->nwords)
		r = (ida->bits[id / 64] >> (id % 64)) & 1;
	spin_unlock_irqrestore(&ida->lock, fl);
	return r;
}

void ida_destroy(struct ida *ida)
{
	uint64_t fl;
	uint64_t *old;

	spin_lock_irqsave(&ida->lock, &fl);
	old = ida->bits;
	ida->bits = NULL;
	ida->nwords = 0;
	ida->nr_allocated = 0;
	spin_unlock_irqrestore(&ida->lock, fl);
	if (old)
		kfree(old);
}
