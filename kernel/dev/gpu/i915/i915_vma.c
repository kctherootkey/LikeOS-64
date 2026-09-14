// LikeOS-64 -- which ranges of an address space are spoken for.
//
// A client places its objects itself and hands the range back to its
// own allocator the moment it is done with an object -- before the
// object is closed, and long before the driver hears of it when the
// object is merely moved to another range.  The next object it places
// lands on the same addresses.  If the old object's binding is then
// torn down by address, it tears down the new object's translations
// with it, and the new object reads the scratch page from then on:
// geometry with pieces missing, text with glyphs missing, from the
// moment the client starts recycling its ranges, and for good.
//
// So every address space keeps the list of bindings that own its
// ranges.  Binding a range takes it from whoever held it (they are
// marked superseded and their translations are simply overwritten);
// unbinding clears translations only for a binding that still owns
// its range.  The list logic is pure so the build host can check it.
#include <kernel/dev/gpu/i915/i915_drv.h>

void i915_vma_list_attach(struct i915_vma **head, struct i915_vma *v)
{
	v->vm_next = *head;
	*head = v;
	v->attached = 1;
}

/* Take `v' off the list.  Returns 1 if it was on it -- if it still
 * owned its range -- and 0 if something else has taken the range. */
int i915_vma_list_detach(struct i915_vma **head, struct i915_vma *v)
{
	if (!v->attached)
		return 0;
	for (struct i915_vma **pp = head; *pp; pp = &(*pp)->vm_next) {
		if (*pp == v) {
			*pp = v->vm_next;
			break;
		}
	}
	v->vm_next = NULL;
	v->attached = 0;
	return 1;
}

/* Every binding other than `except' that overlaps [addr, addr + npages
 * pages) loses its range.  Returns how many did. */
unsigned i915_vma_list_supersede(struct i915_vma **head, uint64_t addr,
				 uint32_t npages, const struct i915_vma *except)
{
	uint64_t end = addr + (uint64_t)npages * 4096;
	unsigned n = 0;

	for (struct i915_vma **pp = head; *pp;) {
		struct i915_vma *o = *pp;
		uint64_t oend = o->addr + (uint64_t)o->npages * 4096;
		if (o != except && o->addr < end && addr < oend) {
			*pp = o->vm_next;
			o->vm_next = NULL;
			o->attached = 0;
			n++;
			continue;
		}
		pp = &o->vm_next;
	}
	return n;
}

/* ---- with the address space's lock ------------------------------------------ */

/* `v' takes [addr, addr + npages pages) in `vm': whatever held any of
 * it is superseded.  Returns how many bindings were. */
unsigned i915_vm_vma_claim(struct i915_vm *vm, struct i915_vma *v,
			   uint64_t addr, uint32_t npages)
{
	uint64_t fl;
	unsigned n;

	spin_lock_irqsave(&vm->lock, &fl);
	if (v->attached)
		i915_vma_list_detach(&vm->vmas, v);
	v->addr = addr;
	v->npages = npages;
	n = i915_vma_list_supersede(&vm->vmas, addr, npages, v);
	i915_vma_list_attach(&vm->vmas, v);
	spin_unlock_irqrestore(&vm->lock, fl);
	return n;
}

/* `v' gives its range up.  Returns 1 if it still owned it, in which
 * case the caller clears the translations; 0 if another binding has
 * taken the range and the translations are that binding's now. */
int i915_vm_vma_release(struct i915_vm *vm, struct i915_vma *v)
{
	uint64_t fl;
	int owned;

	spin_lock_irqsave(&vm->lock, &fl);
	owned = i915_vma_list_detach(&vm->vmas, v);
	spin_unlock_irqrestore(&vm->lock, fl);
	return owned;
}
