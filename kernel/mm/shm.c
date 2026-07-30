/* LikeOS-64 POSIX shared memory objects — see include/kernel/mm/shm.h.
 *
 * Locking: one spinlock covers the table and every object's fields.  Nothing is
 * allocated or freed while it is held — the page array is large enough to take
 * the slab's big-allocation path, which does a cross-CPU TLB shootdown, and
 * that cannot run with interrupts disabled.  So:
 *
 *   - resizes claim the object with its `resizing` flag, drop the lock, do all
 *     allocation, then re-take the lock only to install the result;
 *   - teardown detaches the storage under the lock and frees it afterwards.
 *
 * Reading obj->pages unlocked during a resize is safe because `resizing`
 * excludes the only other writer.
 */
#include <kernel/mm/shm.h>
#include <kernel/mm/memory.h>
#include <kernel/mm/slab.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/cred.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

static shm_object_t g_shm[SHM_MAX_OBJECTS];
static spinlock_t g_shm_lock;
static int g_shm_ready;
static unsigned long g_shm_next_ino = 1;

void shm_init(void)
{
	if (g_shm_ready)
		return;
	spinlock_init(&g_shm_lock, "shm");
	for (int i = 0; i < SHM_MAX_OBJECTS; i++)
		g_shm[i].in_use = 0;
	g_shm_ready = 1;
}

static int shm_name_eq(const shm_object_t *o, const char *name)
{
	int i = 0;

	for (; i < SHM_NAME_MAX && o->name[i] && name[i]; i++)
		if (o->name[i] != name[i])
			return 0;
	return (i < SHM_NAME_MAX) && o->name[i] == '\0' && name[i] == '\0';
}

/* Caller holds g_shm_lock. */
static shm_object_t *shm_find_locked(const char *name)
{
	for (int i = 0; i < SHM_MAX_OBJECTS; i++) {
		shm_object_t *o = &g_shm[i];
		if (o->in_use && !o->unlinked && shm_name_eq(o, name))
			return o;
	}
	return NULL;
}

/* Caller holds g_shm_lock.  Detaches the storage and hands it back so the
 * caller can release it after unlocking. */
static uint64_t *shm_detach_locked(shm_object_t *obj, unsigned long *n_out)
{
	uint64_t *p = obj->pages;

	*n_out = obj->npages;
	obj->pages = NULL;
	obj->npages = 0;
	obj->size = 0;
	obj->in_use = 0;
	obj->unlinked = 0;
	obj->resizing = 0;
	obj->name[0] = '\0';
	return p;
}

/* Called with the lock DROPPED. */
static void shm_free_detached(uint64_t *pages, unsigned long n)
{
	if (!pages)
		return;
	for (unsigned long i = 0; i < n; i++)
		if (pages[i])
			mm_free_physical_page(pages[i]);
	kfree(pages);
}

shm_object_t *shm_lookup_get(const char *name)
{
	shm_object_t *o;
	uint64_t f;

	if (!name || !*name)
		return NULL;
	shm_init();
	spin_lock_irqsave(&g_shm_lock, &f);
	o = shm_find_locked(name);
	if (o)
		o->refs++;
	spin_unlock_irqrestore(&g_shm_lock, f);
	return o;
}

shm_object_t *shm_create_get(const char *name, unsigned mode)
{
	shm_object_t *o = NULL;
	task_t *cur = sched_current();
	uint64_t f;
	int n;

	if (!name || !*name)
		return NULL;
	shm_init();
	spin_lock_irqsave(&g_shm_lock, &f);
	if (shm_find_locked(name)) {
		spin_unlock_irqrestore(&g_shm_lock, f);
		return NULL; /* caller reports EEXIST */
	}
	for (int i = 0; i < SHM_MAX_OBJECTS; i++) {
		if (!g_shm[i].in_use) {
			o = &g_shm[i];
			break;
		}
	}
	if (!o) {
		spin_unlock_irqrestore(&g_shm_lock, f);
		return NULL; /* table full */
	}

	n = 0;
	while (name[n] && n < SHM_NAME_MAX - 1) {
		o->name[n] = name[n];
		n++;
	}
	o->name[n] = '\0';
	o->in_use = 1;
	o->unlinked = 0;
	o->resizing = 0;
	o->refs = 1;
	o->size = 0;
	o->npages = 0;
	o->pages = NULL;
	o->mode = mode & 0777;
	o->uid = cur ? cur->cred.euid : 0;
	o->gid = cur ? cur->cred.egid : 0;
	o->ino = g_shm_next_ino++;
	spin_unlock_irqrestore(&g_shm_lock, f);
	return o;
}

void shm_put(shm_object_t *obj)
{
	uint64_t *dead = NULL;
	unsigned long dead_n = 0;
	uint64_t f;

	if (!obj)
		return;
	spin_lock_irqsave(&g_shm_lock, &f);
	if (obj->refs > 0)
		obj->refs--;
	/* Name gone and last user departed: nothing can reach it again. */
	if (obj->refs == 0 && obj->unlinked)
		dead = shm_detach_locked(obj, &dead_n);
	spin_unlock_irqrestore(&g_shm_lock, f);
	shm_free_detached(dead, dead_n);
}

int shm_unlink_name(const char *name)
{
	shm_object_t *o;
	uint64_t *dead = NULL;
	unsigned long dead_n = 0;
	uint64_t f;

	if (!name || !*name)
		return -EINVAL;
	shm_init();
	spin_lock_irqsave(&g_shm_lock, &f);
	o = shm_find_locked(name);
	if (!o) {
		spin_unlock_irqrestore(&g_shm_lock, f);
		return -ENOENT;
	}
	/* The name goes now; the memory stays until the last holder drops it.
	 * Creating an object and unlinking it immediately is the normal idiom —
	 * it is what stops anything being left behind if the process dies — so
	 * an unlinked object has to keep working for everyone still holding it. */
	o->unlinked = 1;
	o->name[0] = '\0';
	if (o->refs == 0)
		dead = shm_detach_locked(o, &dead_n);
	spin_unlock_irqrestore(&g_shm_lock, f);
	shm_free_detached(dead, dead_n);
	return 0;
}

int shm_set_size(shm_object_t *obj, unsigned long size)
{
	unsigned long want, have, i;
	uint64_t *np = NULL, *old_pages = NULL;
	uint64_t f;
	int rc = 0;

	if (!obj)
		return -EINVAL;
	want = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	if (want > SHM_MAX_PAGES)
		return -ENOMEM;

	spin_lock_irqsave(&g_shm_lock, &f);
	if (!obj->in_use) {
		spin_unlock_irqrestore(&g_shm_lock, f);
		return -EINVAL;
	}
	if (obj->resizing) {
		spin_unlock_irqrestore(&g_shm_lock, f);
		return -EBUSY;
	}
	/* Shrinking only changes the reported length; the pages stay put until
	 * the object is destroyed.
	 *
	 * Freeing them here would be a use-after-free waiting to happen: the
	 * object's reference count tracks open HANDLES, not mappings (a mapping
	 * pins the same vfs_file the descriptor does), so there is no reliable
	 * way to tell from here whether some address space is still pointing at
	 * the tail.  Holding the memory until the object goes costs a little
	 * space in a case nothing does on purpose, and is always safe. */
	if (want <= obj->npages) {
		obj->size = size;
		spin_unlock_irqrestore(&g_shm_lock, f);
		return 0;
	}
	obj->resizing = 1;
	have = obj->npages;
	spin_unlock_irqrestore(&g_shm_lock, f);

	/* ---- unlocked from here; `resizing` keeps other resizers out ---- */
	np = (uint64_t *)kalloc(want * sizeof(uint64_t));
	if (!np) {
		rc = -ENOMEM;
		goto out;
	}
	for (i = 0; i < want; i++)
		np[i] = 0;
	for (i = 0; i < have; i++)
		np[i] = obj->pages ? obj->pages[i] : 0;

	/* New pages must read as zero: their contents become visible to anyone
	 * who opens the object by name. */
	for (i = have; i < want; i++) {
		uint64_t p = mm_allocate_physical_page();
		if (!p) {
			for (unsigned long j = have; j < i; j++)
				mm_free_physical_page(np[j]);
			kfree(np);
			np = NULL;
			rc = -ENOMEM;
			goto out;
		}
		mm_memset(phys_to_virt(p), 0, PAGE_SIZE);
		np[i] = p;
	}

	spin_lock_irqsave(&g_shm_lock, &f);
	old_pages = obj->pages;
	obj->pages = np;
	obj->npages = want;
	obj->size = size;
	obj->resizing = 0;
	spin_unlock_irqrestore(&g_shm_lock, f);
	if (old_pages)
		kfree(old_pages); /* the array only; its pages moved to np */
	return 0;

out:
	spin_lock_irqsave(&g_shm_lock, &f);
	obj->resizing = 0;
	spin_unlock_irqrestore(&g_shm_lock, f);
	return rc;
}

uint64_t shm_page_phys(shm_object_t *obj, unsigned long index)
{
	uint64_t p = 0;
	uint64_t f;

	if (!obj)
		return 0;
	spin_lock_irqsave(&g_shm_lock, &f);
	if (obj->pages && index < obj->npages)
		p = obj->pages[index];
	spin_unlock_irqrestore(&g_shm_lock, f);
	return p;
}

int shm_enumerate(unsigned idx, char *name, size_t cap)
{
	unsigned seen = 0;
	int found = 0;
	uint64_t f;

	if (!name || cap == 0)
		return 0;
	shm_init();
	spin_lock_irqsave(&g_shm_lock, &f);
	for (int i = 0; i < SHM_MAX_OBJECTS; i++) {
		shm_object_t *o = &g_shm[i];
		if (!o->in_use || o->unlinked)
			continue;
		if (seen == idx) {
			size_t n = 0;
			while (o->name[n] && n < cap - 1) {
				name[n] = o->name[n];
				n++;
			}
			name[n] = '\0';
			found = 1;
			break;
		}
		seen++;
	}
	spin_unlock_irqrestore(&g_shm_lock, f);
	return found;
}

/* ================= System V IPC view ================================= */

/* SysV segments live in the same table under a synthesised name, so a segment
 * created either way is the same object and `ls /dev/shm` shows both. */
static void shm_sysv_name(int key, char *out, size_t cap)
{
	static const char pfx[] = "sysv.";
	unsigned long v = (unsigned long)(unsigned int)key;
	char digits[16];
	size_t n = 0, i = 0;

	while (pfx[i] && n < cap - 1)
		out[n++] = pfx[i++];
	i = 0;
	do {
		digits[i++] = (char)('0' + (v % 10));
		v /= 10;
	} while (v && i < sizeof(digits));
	while (i > 0 && n < cap - 1)
		out[n++] = digits[--i];
	out[n] = '\0';
}

int shm_id_of(const shm_object_t *obj)
{
	if (!obj || obj < &g_shm[0] || obj >= &g_shm[SHM_MAX_OBJECTS])
		return -1;
	/* Identifiers start at 1 so 0 is never a valid segment. */
	return (int)(obj - &g_shm[0]) + 1;
}

shm_object_t *shm_by_id_get(int id)
{
	shm_object_t *o;
	uint64_t f;

	if (id < 1 || id > SHM_MAX_OBJECTS)
		return NULL;
	shm_init();
	spin_lock_irqsave(&g_shm_lock, &f);
	o = &g_shm[id - 1];
	if (!o->in_use) {
		spin_unlock_irqrestore(&g_shm_lock, f);
		return NULL;
	}
	o->refs++;
	spin_unlock_irqrestore(&g_shm_lock, f);
	return o;
}

int shm_name_of(const shm_object_t *obj, char *name, size_t cap)
{
	uint64_t f;
	size_t n = 0;

	if (!obj || !name || cap == 0)
		return -EINVAL;
	spin_lock_irqsave(&g_shm_lock, &f);
	while (obj->name[n] && n < cap - 1) {
		name[n] = obj->name[n];
		n++;
	}
	name[n] = '\0';
	spin_unlock_irqrestore(&g_shm_lock, f);
	return n ? 0 : -ENOENT;
}

shm_object_t *shm_sysv_get(int key, unsigned long size, int create, int excl,
			   unsigned mode, int *out_id)
{
	char nm[SHM_NAME_MAX];
	shm_object_t *o = NULL;

	shm_init();

	/* IPC_PRIVATE always makes a fresh segment: the key is not a name, so
	 * it must never collide with another caller's. */
	if (key == SHM_SYSV_KEY_PRIVATE) {
		static unsigned long priv_seq;
		uint64_t f;
		unsigned long seq;

		spin_lock_irqsave(&g_shm_lock, &f);
		seq = ++priv_seq;
		spin_unlock_irqrestore(&g_shm_lock, f);
		shm_sysv_name((int)(0x40000000u | (unsigned)seq), nm,
			      sizeof(nm));
		o = shm_create_get(nm, mode);
	} else {
		shm_sysv_name(key, nm, sizeof(nm));
		o = shm_lookup_get(nm);
		if (o && excl) {
			shm_put(o);
			return NULL; /* caller reports EEXIST */
		}
		if (!o) {
			if (!create)
				return NULL; /* caller reports ENOENT */
			o = shm_create_get(nm, mode);
		}
	}
	if (!o)
		return NULL;

	/* shmget() fixes the size at creation; an existing segment keeps the
	 * size it already has, and asking for more than it holds is an error
	 * the caller reports as EINVAL. */
	if (size && o->size < size) {
		if (shm_set_size(o, size) != 0) {
			shm_put(o);
			return NULL;
		}
	}
	if (out_id)
		*out_id = shm_id_of(o);
	return o;
}
