// LikeOS - persistent reservations: the IOC_PR_* ioctls of a block device
//
// A disk that supports persistent reservations (SCSI PR, NVMe
// reservations) offers them through blk_ops->pr_ops; the block layer turns
// the ioctls into those calls the way the established interface does:
//   IOC_PR_REGISTER          pr_register(old_key, new_key, flags)
//   IOC_PR_RESERVE           pr_reserve(key, type, flags)
//   IOC_PR_RELEASE           pr_release(key, type)
//   IOC_PR_PREEMPT(_ABORT)   pr_preempt(old_key, new_key, type, abort)
//   IOC_PR_CLEAR             pr_clear(key)
//   IOC_PR_READ_KEYS         pr_read_keys(): generation + registered keys,
//                            copied to the caller's array
//   IOC_PR_READ_RESERVATION  pr_read_reservation(): holder's key + type
// Only flags the interface defines (PR_FL_IGNORE_KEY for register) pass.
// A caller needs CAP_SYS_ADMIN or a node opened for writing.  A positive
// return is the device's PR_STS_* status (a reservation conflict...),
// passed to the caller as the ioctl's result, as established.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/block/blkdev.h>
#include <kernel/dev/block/blk_internal.h>
#include <kernel/ke/uaccess.h>
#include <kernel/ke/cred.h>
#include <kernel/mm/memory.h>
#include <kernel/uapi/pr.h>
#include <kernel/uapi/bug.h>

static int blkdev_pr_allowed(int open_flags)
{
	int acc = open_flags & 3;

	if (capable())
		return 1;
	return acc == 1 /* O_WRONLY */ || acc == 2 /* O_RDWR */;
}

static long blkdev_pr_register(struct gendisk *disk, const struct pr_ops *ops,
			       void *argp)
{
	struct pr_registration reg;

	if (!ops->pr_register)
		return -EOPNOTSUPP;
	if (copy_from_user(&reg, argp, sizeof(reg)))
		return -EFAULT;
	if (reg.flags & ~PR_FL_IGNORE_KEY)
		return -EOPNOTSUPP;
	return ops->pr_register(disk, reg.old_key, reg.new_key, reg.flags);
}

static int blkdev_pr_type_ok(uint32_t type)
{
	return type >= PR_WRITE_EXCLUSIVE && type <= PR_EXCLUSIVE_ACCESS_ALL_REGS;
}

static long blkdev_pr_reserve(struct gendisk *disk, const struct pr_ops *ops,
			      void *argp)
{
	struct pr_reservation rsv;

	if (!ops->pr_reserve)
		return -EOPNOTSUPP;
	if (copy_from_user(&rsv, argp, sizeof(rsv)))
		return -EFAULT;
	if (rsv.flags & ~PR_FL_IGNORE_KEY)
		return -EOPNOTSUPP;
	if (!blkdev_pr_type_ok(rsv.type))
		return -EINVAL;
	return ops->pr_reserve(disk, rsv.key, rsv.type, rsv.flags);
}

static long blkdev_pr_release(struct gendisk *disk, const struct pr_ops *ops,
			      void *argp)
{
	struct pr_reservation rsv;

	if (!ops->pr_release)
		return -EOPNOTSUPP;
	if (copy_from_user(&rsv, argp, sizeof(rsv)))
		return -EFAULT;
	if (rsv.flags)
		return -EOPNOTSUPP;
	if (!blkdev_pr_type_ok(rsv.type))
		return -EINVAL;
	return ops->pr_release(disk, rsv.key, rsv.type);
}

static long blkdev_pr_preempt(struct gendisk *disk, const struct pr_ops *ops,
			      void *argp, int abort)
{
	struct pr_preempt p;

	if (!ops->pr_preempt)
		return -EOPNOTSUPP;
	if (copy_from_user(&p, argp, sizeof(p)))
		return -EFAULT;
	if (p.flags)
		return -EOPNOTSUPP;
	if (!blkdev_pr_type_ok(p.type))
		return -EINVAL;
	return ops->pr_preempt(disk, p.old_key, p.new_key, p.type, abort);
}

static long blkdev_pr_clear(struct gendisk *disk, const struct pr_ops *ops,
			    void *argp)
{
	struct pr_clear c;

	if (!ops->pr_clear)
		return -EOPNOTSUPP;
	if (copy_from_user(&c, argp, sizeof(c)))
		return -EFAULT;
	if (c.flags)
		return -EOPNOTSUPP;
	return ops->pr_clear(disk, c.key);
}

static long blkdev_pr_read_keys(struct gendisk *disk, const struct pr_ops *ops,
				void *argp)
{
	struct pr_read_keys rk;
	struct pr_keys *keys;
	uint32_t room;
	size_t bytes;
	long ret;

	if (!ops->pr_read_keys)
		return -EOPNOTSUPP;
	if (copy_from_user(&rk, argp, sizeof(rk)))
		return -EFAULT;
	if (rk.num_keys > PR_KEYS_MAX)
		return -EINVAL;
	room = rk.num_keys;
	bytes = sizeof(*keys) + (size_t)room * sizeof(uint64_t);
	keys = (struct pr_keys *)kcalloc(1, bytes);
	if (!keys)
		return -ENOMEM;
	keys->num_keys = room;
	ret = ops->pr_read_keys(disk, keys);
	if (ret)
		goto out;
	/* As many keys as fit; num_keys tells how many are registered. */
	if (room && rk.keys_ptr) {
		uint32_t n = keys->num_keys < room ? keys->num_keys : room;

		if (n && copy_to_user((void *)(uintptr_t)rk.keys_ptr, keys->keys,
				      (size_t)n * sizeof(uint64_t))) {
			ret = -EFAULT;
			goto out;
		}
	}
	rk.generation = keys->generation;
	rk.num_keys = keys->num_keys;
	if (copy_to_user(argp, &rk, sizeof(rk)))
		ret = -EFAULT;
out:
	kfree(keys);
	return ret;
}

static long blkdev_pr_read_reservation(struct gendisk *disk,
				       const struct pr_ops *ops, void *argp)
{
	struct pr_held_reservation rsv;
	struct pr_read_reservation out;
	long ret;

	if (!ops->pr_read_reservation)
		return -EOPNOTSUPP;
	mm_memset(&rsv, 0, sizeof(rsv));
	ret = ops->pr_read_reservation(disk, &rsv);
	if (ret)
		return ret;
	mm_memset(&out, 0, sizeof(out));
	out.key = rsv.key;
	out.generation = rsv.generation;
	out.type = rsv.type;
	return copy_to_user(argp, &out, sizeof(out)) ? -EFAULT : 0;
}

long blkdev_pr_ioctl(struct gendisk *disk, unsigned long cmd, void *argp,
		     int open_flags)
{
	const struct pr_ops *ops;

	switch (cmd) {
	case IOC_PR_REGISTER:
	case IOC_PR_RESERVE:
	case IOC_PR_RELEASE:
	case IOC_PR_PREEMPT:
	case IOC_PR_PREEMPT_ABORT:
	case IOC_PR_CLEAR:
	case IOC_PR_READ_KEYS:
	case IOC_PR_READ_RESERVATION:
		break;
	default:
		return -ENOTTY;
	}
	if (!disk || !disk->ops)
		return -ENODEV;
	ops = disk->ops->pr_ops;
	if (!ops)
		return -EOPNOTSUPP;
	if (!blkdev_pr_allowed(open_flags))
		return -EPERM;
	if (!argp)
		return -EINVAL;
	blk_might_sleep();
	switch (cmd) {
	case IOC_PR_REGISTER:
		return blkdev_pr_register(disk, ops, argp);
	case IOC_PR_RESERVE:
		return blkdev_pr_reserve(disk, ops, argp);
	case IOC_PR_RELEASE:
		return blkdev_pr_release(disk, ops, argp);
	case IOC_PR_PREEMPT:
		return blkdev_pr_preempt(disk, ops, argp, 0);
	case IOC_PR_PREEMPT_ABORT:
		return blkdev_pr_preempt(disk, ops, argp, 1);
	case IOC_PR_CLEAR:
		return blkdev_pr_clear(disk, ops, argp);
	case IOC_PR_READ_KEYS:
		return blkdev_pr_read_keys(disk, ops, argp);
	default:
		return blkdev_pr_read_reservation(disk, ops, argp);
	}
}
