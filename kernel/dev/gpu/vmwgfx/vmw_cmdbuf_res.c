// LikeOS -- vmwgfx: objects a command stream creates and destroys.
//
// See vmw_cmdbuf_res.h.  One manager per guest-backed context: a hash of
// (type, client id) -> object for lookups, and the list of committed
// entries for teardown.  A staged entry is in the hash (so the rest of the
// same stream finds it) but on the caller's staging list rather than the
// manager's list; a staged removal is out of the hash but still owns its
// reference until the stream is committed or reverted.
//
// Locking: every entry point runs under vmw_device.execbuf_lock (or, for
// destroy, with the context unreachable by any submitter).  Commit, revert
// and destroy can drop the last reference to an object, whose release takes
// vmw_device.binding_lock -- call them without that lock held.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from VMware's code: GPL-2.0 OR MIT
// Portions Copyright 2014-2022 VMware, Inc., Palo Alto, CA., USA

#include <kernel/dev/gpu/vmwgfx/vmw_gb.h>
#include <kernel/mm/memory.h>

/* 256 buckets.  A context rarely holds more than a few hundred views and
 * shaders, and there can be 256 contexts: a bucket array of a page per
 * context keeps the chains short without making every context cost tens of
 * kilobytes. */
#define VMW_CMDBUF_RES_MAN_HT_ORDER 8
#define VMW_CMDBUF_RES_MAN_HT_SIZE (1u << VMW_CMDBUF_RES_MAN_HT_ORDER)

/**
 * struct vmw_cmdbuf_res - Command buffer managed resource entry.
 *
 * @res: Refcounted pointer to a struct vmw_resource.
 * @key: (type << 24) | user key.
 * @hash: Link in the manager's bucket for @key; empty (self-linked) while
 * the entry is out of the hash (a staged removal).
 * @head: List head used either by the staging list or the manager list
 * of committed resources.
 * @state: Staging state of this resource entry.
 * @man: Pointer to a resource manager for this entry.
 */
struct vmw_cmdbuf_res {
	struct vmw_resource *res;
	u32 key;
	struct list_head hash;
	struct list_head head;
	enum vmw_cmdbuf_res_state state;
	struct vmw_cmdbuf_res_manager *man;
};

/**
 * struct vmw_cmdbuf_res_manager - Command buffer resource manager.
 *
 * @resources: Hash buckets of staged and committed command buffer
 * resources.
 * @list: List of committed command buffer resources.
 * @dev: The device.
 *
 * @resources and @list are protected by vmw_device.execbuf_lock.
 */
struct vmw_cmdbuf_res_manager {
	struct list_head resources[VMW_CMDBUF_RES_MAN_HT_SIZE];
	struct list_head list;
	struct vmw_device *dev;
};

static u32 vmw_cmdbuf_res_key(enum vmw_cmdbuf_res_type res_type, u32 user_key)
{
	return user_key | ((u32)res_type << 24);
}

static struct list_head *vmw_cmdbuf_res_bucket(struct vmw_cmdbuf_res_manager *man,
					       u32 key)
{
	/* Multiplicative hashing: client ids are small and dense, and the
	 * type sits in the top byte, so the high bits of the product are
	 * what spreads them. */
	u32 h = (key * 0x61C88647u) >> (32 - VMW_CMDBUF_RES_MAN_HT_ORDER);

	return &man->resources[h];
}

static struct vmw_cmdbuf_res *
vmw_cmdbuf_res_find(struct vmw_cmdbuf_res_manager *man, u32 key)
{
	struct vmw_cmdbuf_res *entry;
	struct list_head *bucket = vmw_cmdbuf_res_bucket(man, key);

	list_for_each_entry(entry, bucket, hash) {
		if (entry->key == key)
			return entry;
	}
	return NULL;
}

/**
 * vmw_cmdbuf_res_lookup - Look up a command buffer resource
 *
 * @man: Pointer to the command buffer resource manager
 * @res_type: The resource type, that combined with the user key
 * identifies the resource.
 * @user_key: The user key.
 *
 * Returns a valid struct vmw_resource pointer (not referenced: valid while
 * the caller holds execbuf_lock) on success, an error pointer on failure.
 */
struct vmw_resource *vmw_cmdbuf_res_lookup(struct vmw_cmdbuf_res_manager *man,
					   enum vmw_cmdbuf_res_type res_type,
					   u32 user_key)
{
	struct vmw_cmdbuf_res *entry;

	if (!man)
		return ERR_PTR(-EINVAL);
	entry = vmw_cmdbuf_res_find(man, vmw_cmdbuf_res_key(res_type, user_key));
	return entry ? entry->res : ERR_PTR(-EINVAL);
}

/**
 * vmw_cmdbuf_res_free - Free a command buffer resource.
 *
 * @entry: Pointer to a struct vmw_cmdbuf_res.
 *
 * Unlinks a struct vmw_cmdbuf_res entry from whatever list and bucket it is
 * on, frees it and drops its reference to the struct vmw_resource.
 */
static void vmw_cmdbuf_res_free(struct vmw_cmdbuf_res *entry)
{
	list_del(&entry->head);
	list_del_init(&entry->hash);
	vmw_resource_unreference(&entry->res);
	kfree(entry);
}

/**
 * vmw_cmdbuf_res_commit - Commit a list of command buffer resource actions
 *
 * @list: Caller's list of command buffer resource actions.
 *
 * This function commits a list of command buffer resource
 * additions or removals.
 * It is called when the execbuf ioctl call triggering these actions has
 * handed the command stream to the device, after
 * vmw_cmdbuf_res_commit_notify() and without binding_lock: a removed
 * resource may be released here.
 */
void vmw_cmdbuf_res_commit_notify(struct list_head *list)
{
	struct vmw_cmdbuf_res *entry;

	list_for_each_entry(entry, list, head)
		if (entry->res->func && entry->res->func->commit_notify)
			entry->res->func->commit_notify(entry->res,
							entry->state);
}

void vmw_cmdbuf_res_commit(struct list_head *list)
{
	struct vmw_cmdbuf_res *entry, *next;

	list_for_each_entry_safe(entry, next, list, head) {
		list_del(&entry->head);
		switch (entry->state) {
		case VMW_CMDBUF_RES_ADD:
			entry->state = VMW_CMDBUF_RES_COMMITTED;
			list_add_tail(&entry->head, &entry->man->list);
			break;
		case VMW_CMDBUF_RES_DEL:
			vmw_resource_unreference(&entry->res);
			kfree(entry);
			break;
		default:
			/* A committed entry is never on a staging list. */
			WARN_ON_ONCE(1);
			list_add_tail(&entry->head, &entry->man->list);
			break;
		}
	}
}

/**
 * vmw_cmdbuf_res_revert - Revert a list of command buffer resource actions
 *
 * @list: Caller's list of command buffer resource action
 *
 * This function reverts a list of command buffer resource
 * additions or removals.
 * It is called when the execbuf ioctl call triggering these actions failed
 * for some reason, and the command stream was never submitted.
 */
void vmw_cmdbuf_res_revert(struct list_head *list)
{
	struct vmw_cmdbuf_res *entry, *next;

	list_for_each_entry_safe(entry, next, list, head) {
		switch (entry->state) {
		case VMW_CMDBUF_RES_ADD:
			vmw_cmdbuf_res_free(entry);
			break;
		case VMW_CMDBUF_RES_DEL:
			list_add_tail(&entry->hash,
				      vmw_cmdbuf_res_bucket(entry->man,
							    entry->key));
			list_move_tail(&entry->head, &entry->man->list);
			entry->state = VMW_CMDBUF_RES_COMMITTED;
			break;
		default:
			WARN_ON_ONCE(1);
			list_move_tail(&entry->head, &entry->man->list);
			break;
		}
	}
}

/**
 * vmw_cmdbuf_res_add - Stage a command buffer managed resource for addition.
 *
 * @man: Pointer to the command buffer resource manager.
 * @res_type: The resource type.
 * @user_key: The user-space id of the resource.
 * @res: Valid (refcount != 0) pointer to a struct vmw_resource.
 * @list: The staging list.
 *
 * This function allocates a struct vmw_cmdbuf_res entry and adds the
 * resource to the hash table of the manager identified by @man. The
 * entry is then put on the staging list identified by @list.
 */
int vmw_cmdbuf_res_add(struct vmw_cmdbuf_res_manager *man,
		       enum vmw_cmdbuf_res_type res_type, u32 user_key,
		       struct vmw_resource *res, struct list_head *list)
{
	struct vmw_cmdbuf_res *cres;

	if (!man)
		return -EINVAL;
	cres = kcalloc(1, sizeof(*cres));
	if (unlikely(!cres))
		return -ENOMEM;

	cres->key = vmw_cmdbuf_res_key(res_type, user_key);
	list_add_tail(&cres->hash, vmw_cmdbuf_res_bucket(man, cres->key));

	cres->state = VMW_CMDBUF_RES_ADD;
	cres->res = vmw_resource_reference(res);
	cres->man = man;
	list_add_tail(&cres->head, list);

	return 0;
}

/**
 * vmw_cmdbuf_res_remove - Stage a command buffer managed resource for removal.
 *
 * @man: Pointer to the command buffer resource manager.
 * @res_type: The resource type.
 * @user_key: The user-space id of the resource.
 * @list: The staging list.
 * @res_p: If the resource is in an already committed state, points to the
 * struct vmw_resource on successful return. The pointer will be
 * non ref-counted.
 *
 * This function looks up the struct vmw_cmdbuf_res entry from the manager
 * hash table and, if it exists, removes it. Depending on its current staging
 * state it then either removes the entry from the staging list or adds it
 * to it with a staging state of removal.
 */
int vmw_cmdbuf_res_remove(struct vmw_cmdbuf_res_manager *man,
			  enum vmw_cmdbuf_res_type res_type, u32 user_key,
			  struct list_head *list, struct vmw_resource **res_p)
{
	struct vmw_cmdbuf_res *entry;

	*res_p = NULL;
	if (!man)
		return -EINVAL;
	entry = vmw_cmdbuf_res_find(man, vmw_cmdbuf_res_key(res_type, user_key));
	if (unlikely(!entry))
		return -EINVAL;

	switch (entry->state) {
	case VMW_CMDBUF_RES_ADD:
		/* Defined and destroyed in the same stream: the object never
		 * reached the device as far as the manager is concerned. */
		vmw_cmdbuf_res_free(entry);
		*res_p = NULL;
		break;
	case VMW_CMDBUF_RES_COMMITTED:
		list_del_init(&entry->hash);
		list_del(&entry->head);
		entry->state = VMW_CMDBUF_RES_DEL;
		list_add_tail(&entry->head, list);
		*res_p = entry->res;
		break;
	default:
		/* A staged removal is out of the hash, so it cannot be
		 * found again. */
		WARN_ON_ONCE(1);
		return -EINVAL;
	}

	return 0;
}

/**
 * vmw_cmdbuf_res_man_create - Allocate a command buffer managed resource
 * manager.
 *
 * @v: The device.
 *
 * Allocates and initializes a command buffer managed resource manager. Returns
 * an error pointer on failure.
 */
struct vmw_cmdbuf_res_manager *vmw_cmdbuf_res_man_create(struct vmw_device *v)
{
	struct vmw_cmdbuf_res_manager *man;

	man = kcalloc(1, sizeof(*man));
	if (!man)
		return ERR_PTR(-ENOMEM);

	man->dev = v;
	INIT_LIST_HEAD(&man->list);
	for (u32 i = 0; i < VMW_CMDBUF_RES_MAN_HT_SIZE; i++)
		INIT_LIST_HEAD(&man->resources[i]);
	return man;
}

/**
 * vmw_cmdbuf_res_man_destroy - Destroy a command buffer managed resource
 * manager.
 *
 * @man: Pointer to the  manager to destroy.
 *
 * This function destroys a command buffer managed resource manager and
 * unreferences / frees all command buffer managed resources and -entries
 * associated with it.  Nothing may be staged against it: a submission
 * reverts or commits its staging list before it lets go of execbuf_lock.
 */
void vmw_cmdbuf_res_man_destroy(struct vmw_cmdbuf_res_manager *man)
{
	struct vmw_cmdbuf_res *entry, *next;

	if (!man)
		return;
	list_for_each_entry_safe(entry, next, &man->list, head)
		vmw_cmdbuf_res_free(entry);

	kfree(man);
}
