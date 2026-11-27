// LikeOS I2C - /dev/i2c-N, the I2C bus character devices
//
// One node per adapter: /dev/i2c-N, character major 89, minor N, mode
// 0600 root.  Every open file carries a private, never registered client
// -- the target address and flags of that file -- so programs can talk to
// any address of the bus with plain read()/write() and the ioctls of
// <linux/i2c-dev.h>:
//
//   I2C_SLAVE / I2C_SLAVE_FORCE   select the address (7-bit unless
//                                 I2C_TENBIT); I2C_SLAVE refuses an
//                                 address a kernel driver owns (-EBUSY)
//   I2C_TENBIT, I2C_PEC           per-file flags
//   I2C_FUNCS                     the adapter's I2C_FUNC_* mask
//   I2C_RDWR                      a combined transfer of up to 42
//                                 messages of up to 8192 bytes each
//   I2C_SMBUS                     one SMBus transaction
//   I2C_RETRIES, I2C_TIMEOUT      adapter retries / timeout (10 ms units)
//
// Lifetime.  The node lives in a struct i2c_dev registered when the
// adapter registers.  An open takes an adapter reference (i2c_get_adapter)
// that the file keeps until it is closed, so i2c_del_adapter() waits for
// open files; the node is unregistered first (no new opens: an open that
// finds a dead entry fails with -ENODEV before touching the adapter) and
// its memory is freed only after the adapter's references are gone and no
// devfs open is in flight on it.
//
// Every user pointer is reached through copy_from_user / copy_to_user
// after a range check (SMAP).
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 1995-97 Simon G. Vogl, 1998-99 Frodo Looijaard,
// 2003 Greg Kroah-Hartman

#include <kernel/dev/i2c/i2c-core.h>
#include <kernel/fs/device.h>
#include <kernel/fs/chrdev.h>
#include <kernel/ke/uaccess.h>
#include <kernel/ke/delay.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

/* ioctl requests (the established numbers). */
#define I2C_RETRIES 0x0701
#define I2C_TIMEOUT 0x0702
#define I2C_SLAVE 0x0703
#define I2C_TENBIT 0x0704
#define I2C_FUNCS 0x0705
#define I2C_SLAVE_FORCE 0x0706
#define I2C_RDWR 0x0707
#define I2C_PEC 0x0708
#define I2C_SMBUS 0x0720

#define I2C_RDWR_IOCTL_MAX_MSGS 42
/* Bytes per message (I2C_RDWR) and per read()/write(). */
#define I2C_DEV_MAX_XFER 8192
#define I2C_DEV_INT_MAX 0x7fffffffl

/* The user's argument structures (x86-64 layout). */
struct i2c_rdwr_ioctl_data {
	uint64_t msgs; /* struct i2c_msg * */
	uint32_t nmsgs;
	uint32_t pad;
};

struct i2c_smbus_ioctl_data {
	uint8_t read_write;
	uint8_t command;
	uint8_t pad[2];
	uint32_t size;
	uint64_t data; /* union i2c_smbus_data * */
};

/* The kernel's message is the user's message. */
_Static_assert(sizeof(struct i2c_msg) == 16, "struct i2c_msg layout");
_Static_assert(sizeof(struct i2c_rdwr_ioctl_data) == 16, "rdwr layout");
_Static_assert(sizeof(struct i2c_smbus_ioctl_data) == 16, "smbus layout");

struct i2c_dev {
	struct devfs_node node;
	struct list_head link; /* g_i2c_devs */
	int nr; /* the adapter's bus number */
	int dead; /* detached: no new opens */
	int opens; /* opens past the dead check */
};

/* Per open file. */
struct i2c_dev_file {
	struct i2c_client client; /* private address/flags, adapter ref */
	struct i2c_dev *idev;
};

static LIST_HEAD(g_i2c_devs);
static spinlock_t g_i2c_dev_lock = SPINLOCK_INIT("i2c_dev");
static int g_i2c_dev_major;

/* ==========================================================================
 * User memory
 * ========================================================================== */

static int i2cdev_copy_in(void *dst, uint64_t usrc, size_t n)
{
	if (!n)
		return 0;
	if (!usrc || !validate_user_ptr(usrc, n))
		return -EFAULT;
	return copy_from_user(dst, (const void *)usrc, n) ? -EFAULT : 0;
}

static int i2cdev_copy_out(uint64_t udst, const void *src, size_t n)
{
	if (!n)
		return 0;
	if (!udst || !validate_user_ptr(udst, n))
		return -EFAULT;
	return copy_to_user((void *)udst, src, n) ? -EFAULT : 0;
}

/* ==========================================================================
 * Open / release
 * ========================================================================== */

static struct i2c_dev *i2cdev_find_locked(struct devfs_node *node)
{
	struct i2c_dev *d;

	list_for_each_entry(d, &g_i2c_devs, link)
		if (&d->node == node && !d->dead)
			return d;
	return NULL;
}

static int i2cdev_open(struct devfs_node *node, vfs_file_t *file, int flags,
		       struct task *cur)
{
	struct i2c_dev_file *f;
	struct i2c_adapter *adap;
	struct i2c_dev *idev;
	uint64_t fl;
	int nr;

	(void)flags;
	(void)cur;
	spin_lock_irqsave(&g_i2c_dev_lock, &fl);
	idev = i2cdev_find_locked(node);
	if (idev)
		idev->opens++;
	spin_unlock_irqrestore(&g_i2c_dev_lock, fl);
	if (!idev)
		return -ENODEV;
	nr = idev->nr;

	adap = i2c_get_adapter(nr);
	f = adap ? kcalloc(1, sizeof(*f)) : NULL;
	if (!f) {
		if (adap)
			i2c_put_adapter(adap);
		spin_lock_irqsave(&g_i2c_dev_lock, &fl);
		idev->opens--;
		spin_unlock_irqrestore(&g_i2c_dev_lock, fl);
		return adap ? -ENOMEM : -ENODEV;
	}
	/* This client is never registered: it only carries the address
	 * and flags of this file. */
	ksnprintf(f->client.name, sizeof(f->client.name), "i2c-dev %d", nr);
	ksnprintf(f->client.dev_name, sizeof(f->client.dev_name), "i2c-dev %d",
		  nr);
	f->client.adapter = adap;
	f->idev = idev;
	device_file_set_priv(file, f);
	return 0;
}

static void i2cdev_release(vfs_file_t *file)
{
	struct i2c_dev_file *f = device_file_priv(file);
	uint64_t fl;

	if (WARN_ON_ONCE(!f))
		return;
	device_file_set_priv(file, NULL);
	spin_lock_irqsave(&g_i2c_dev_lock, &fl);
	BUG_ON(f->idev->opens <= 0);
	f->idev->opens--;
	spin_unlock_irqrestore(&g_i2c_dev_lock, fl);
	/* Last: the adapter may be deleted once this reference is gone. */
	i2c_put_adapter(f->client.adapter);
	kfree(f);
}

/* ==========================================================================
 * read / write
 * ========================================================================== */

static long i2cdev_read(vfs_file_t *file, void *buf, long bytes, int nonblock)
{
	struct i2c_dev_file *f = device_file_priv(file);
	char *tmp;
	long count = bytes;
	int ret;

	(void)nonblock;
	if (!f)
		return -ENXIO;
	/* The adapter must do plain I2C. */
	if (!i2c_check_functionality(f->client.adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;
	if (count < 0)
		return -EINVAL;
	if (count > I2C_DEV_MAX_XFER)
		count = I2C_DEV_MAX_XFER;
	tmp = kcalloc(1, (size_t)(count ? count : 1));
	if (!tmp)
		return -ENOMEM;
	ret = i2c_master_recv(&f->client, tmp, (int)count);
	if (ret >= 0 && i2cdev_copy_out((uint64_t)buf, tmp, (size_t)ret))
		ret = -EFAULT;
	kfree(tmp);
	return ret;
}

static long i2cdev_write(vfs_file_t *file, const void *buf, long bytes,
			 int nonblock)
{
	struct i2c_dev_file *f = device_file_priv(file);
	char *tmp;
	long count = bytes;
	int ret;

	(void)nonblock;
	if (!f)
		return -ENXIO;
	if (!i2c_check_functionality(f->client.adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;
	if (count < 0)
		return -EINVAL;
	if (count > I2C_DEV_MAX_XFER)
		count = I2C_DEV_MAX_XFER;
	tmp = kcalloc(1, (size_t)(count ? count : 1));
	if (!tmp)
		return -ENOMEM;
	ret = i2cdev_copy_in(tmp, (uint64_t)buf, (size_t)count);
	if (!ret)
		ret = i2c_master_send(&f->client, tmp, (int)count);
	kfree(tmp);
	return ret;
}

/* ==========================================================================
 * ioctl
 * ========================================================================== */

static long i2cdev_ioctl_rdwr(struct i2c_client *client, unsigned int nmsgs,
			      struct i2c_msg *msgs)
{
	uint64_t *data_ptrs;
	uint16_t *user_len;
	unsigned int i;
	long res = 0;

	/* The adapter must do plain I2C. */
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;

	data_ptrs = kcalloc(nmsgs, sizeof(*data_ptrs));
	user_len = kcalloc(nmsgs, sizeof(*user_len));
	if (!data_ptrs || !user_len) {
		kfree(data_ptrs);
		kfree(user_len);
		return -ENOMEM;
	}

	for (i = 0; i < nmsgs; i++) {
		size_t alloc;

		/* A sane size per message. */
		if (msgs[i].len > I2C_DEV_MAX_XFER) {
			res = -EINVAL;
			break;
		}
		data_ptrs[i] = (uint64_t)msgs[i].buf;
		user_len[i] = msgs[i].len;
		/* A received-length read may grow to the length byte, a
		 * maximal block and a PEC byte whatever buf[0] says: the
		 * kernel buffer always has room for that. */
		alloc = msgs[i].len;
		if ((msgs[i].flags & I2C_M_RECV_LEN) &&
		    alloc < I2C_SMBUS_BLOCK_MAX + 2u)
			alloc = I2C_SMBUS_BLOCK_MAX + 2u;
		msgs[i].buf = kcalloc(1, alloc ? alloc : 1);
		if (!msgs[i].buf) {
			res = -ENOMEM;
			break;
		}
		res = i2cdev_copy_in(msgs[i].buf, data_ptrs[i], msgs[i].len);
		if (res) {
			i++; /* its buffer is freed below */
			break;
		}
		msgs[i].flags |= I2C_M_DMA_SAFE;

		/* A length received from the device (like an SMBus block
		 * read): the user buffer must hold I2C_SMBUS_BLOCK_MAX
		 * more bytes than the count of extra bytes in buf[0] (at
		 * least 1, the length byte itself; more for a checksum
		 * byte), and the read starts with that many. */
		if (msgs[i].flags & I2C_M_RECV_LEN) {
			if (!(msgs[i].flags & I2C_M_RD) || msgs[i].len < 1 ||
			    msgs[i].buf[0] < 1 ||
			    msgs[i].len < msgs[i].buf[0] + I2C_SMBUS_BLOCK_MAX) {
				i++;
				res = -EINVAL;
				break;
			}
			msgs[i].len = msgs[i].buf[0];
		}
	}
	if (res < 0) {
		for (unsigned int j = 0; j < i && j < nmsgs; j++)
			kfree(msgs[j].buf);
		kfree(data_ptrs);
		kfree(user_len);
		return res;
	}

	res = i2c_transfer(client->adapter, msgs, (int)nmsgs);
	while (i-- > 0) {
		if (res >= 0 && (msgs[i].flags & I2C_M_RD)) {
			/* Never more than the user's buffer holds. */
			size_t n = msgs[i].len < user_len[i] ? msgs[i].len :
							       user_len[i];

			if (i2cdev_copy_out(data_ptrs[i], msgs[i].buf, n))
				res = -EFAULT;
		}
		kfree(msgs[i].buf);
	}
	kfree(data_ptrs);
	kfree(user_len);
	return res;
}

static long i2cdev_ioctl_smbus(struct i2c_client *client, uint8_t read_write,
			       uint8_t command, uint32_t size, uint64_t udata)
{
	union i2c_smbus_data temp;
	size_t datasize;
	int res;

	if (size != I2C_SMBUS_BYTE && size != I2C_SMBUS_QUICK &&
	    size != I2C_SMBUS_BYTE_DATA && size != I2C_SMBUS_WORD_DATA &&
	    size != I2C_SMBUS_PROC_CALL && size != I2C_SMBUS_BLOCK_DATA &&
	    size != I2C_SMBUS_I2C_BLOCK_BROKEN &&
	    size != I2C_SMBUS_I2C_BLOCK_DATA &&
	    size != I2C_SMBUS_BLOCK_PROC_CALL)
		return -EINVAL;
	/* READ and WRITE are 1 and 0, so this also holds for QUICK. */
	if (read_write != I2C_SMBUS_READ && read_write != I2C_SMBUS_WRITE)
		return -EINVAL;

	/* Every command value is valid. */
	if (size == I2C_SMBUS_QUICK ||
	    (size == I2C_SMBUS_BYTE && read_write == I2C_SMBUS_WRITE))
		/* No data. */
		return i2c_smbus_xfer(client->adapter, client->addr,
				      client->flags, (char)read_write, command,
				      (int)size, NULL);

	if (!udata)
		return -EINVAL;

	if (size == I2C_SMBUS_BYTE_DATA || size == I2C_SMBUS_BYTE)
		datasize = sizeof(temp.byte);
	else if (size == I2C_SMBUS_WORD_DATA || size == I2C_SMBUS_PROC_CALL)
		datasize = sizeof(temp.word);
	else /* SMBus block, I2C block, block process call */
		datasize = sizeof(temp.block);

	kmemset(&temp, 0, sizeof(temp));
	if (size == I2C_SMBUS_PROC_CALL || size == I2C_SMBUS_BLOCK_PROC_CALL ||
	    size == I2C_SMBUS_I2C_BLOCK_DATA ||
	    read_write == I2C_SMBUS_WRITE) {
		if (i2cdev_copy_in(&temp, udata, datasize))
			return -EFAULT;
	}
	if (size == I2C_SMBUS_I2C_BLOCK_BROKEN) {
		/* The old I2C block convention, kept binary compatible. */
		size = I2C_SMBUS_I2C_BLOCK_DATA;
		if (read_write == I2C_SMBUS_READ)
			temp.block[0] = I2C_SMBUS_BLOCK_MAX;
	}
	res = i2c_smbus_xfer(client->adapter, client->addr, client->flags,
			     (char)read_write, command, (int)size, &temp);
	if (!res && (size == I2C_SMBUS_PROC_CALL ||
		     size == I2C_SMBUS_BLOCK_PROC_CALL ||
		     read_write == I2C_SMBUS_READ)) {
		if (i2cdev_copy_out(udata, &temp, datasize))
			return -EFAULT;
	}
	return res;
}

static long i2cdev_ioctl(vfs_file_t *file, unsigned long cmd, void *argp,
			 struct task *cur)
{
	struct i2c_dev_file *f = device_file_priv(file);
	struct i2c_client *client;
	unsigned long arg = (unsigned long)argp;

	(void)cur;
	if (!f)
		return -ENXIO;
	client = &f->client;

	switch (cmd) {
	case I2C_SLAVE:
	case I2C_SLAVE_FORCE:
		if (arg > 0x3ff ||
		    (!(client->flags & I2C_M_TEN) && arg > 0x7f))
			return -EINVAL;
		if (cmd == I2C_SLAVE &&
		    i2c_check_addr_busy(client->adapter, (unsigned int)arg, 0,
					1))
			return -EBUSY;
		/* The address may become busy later; that is accepted. */
		client->addr = (unsigned short)arg;
		return 0;
	case I2C_TENBIT:
		if (arg)
			client->flags |= I2C_M_TEN;
		else
			client->flags &= (unsigned short)~I2C_M_TEN;
		return 0;
	case I2C_PEC:
		/* Only this file's transactions; drivers' clients have
		 * their own flag. */
		if (arg)
			client->flags |= I2C_CLIENT_PEC;
		else
			client->flags &= (unsigned short)~I2C_CLIENT_PEC;
		return 0;
	case I2C_FUNCS: {
		unsigned long funcs = i2c_get_functionality(client->adapter);

		return i2cdev_copy_out(arg, &funcs, sizeof(funcs));
	}
	case I2C_RDWR: {
		struct i2c_rdwr_ioctl_data rdwr;
		struct i2c_msg *msgs;
		long res;

		if (i2cdev_copy_in(&rdwr, arg, sizeof(rdwr)))
			return -EFAULT;
		if (!rdwr.msgs || rdwr.nmsgs == 0)
			return -EINVAL;
		/* An arbitrary limit on the messages of one call. */
		if (rdwr.nmsgs > I2C_RDWR_IOCTL_MAX_MSGS)
			return -EINVAL;
		msgs = kcalloc(rdwr.nmsgs, sizeof(*msgs));
		if (!msgs)
			return -ENOMEM;
		if (i2cdev_copy_in(msgs, rdwr.msgs,
				   rdwr.nmsgs * sizeof(*msgs))) {
			kfree(msgs);
			return -EFAULT;
		}
		res = i2cdev_ioctl_rdwr(client, rdwr.nmsgs, msgs);
		kfree(msgs);
		return res;
	}
	case I2C_SMBUS: {
		struct i2c_smbus_ioctl_data d;

		if (i2cdev_copy_in(&d, arg, sizeof(d)))
			return -EFAULT;
		return i2cdev_ioctl_smbus(client, d.read_write, d.command,
					  d.size, d.data);
	}
	case I2C_RETRIES:
		if (arg > I2C_DEV_INT_MAX)
			return -EINVAL;
		client->adapter->retries = (int)arg;
		return 0;
	case I2C_TIMEOUT:
		if (arg > I2C_DEV_INT_MAX)
			return -EINVAL;
		/* In units of 10 ms, for historical reasons. */
		client->adapter->timeout =
			arg > I2C_DEV_INT_MAX / 10 ? (int)I2C_DEV_INT_MAX :
						     (int)(arg * 10);
		return 0;
	default:
		/* Not an error code a buggy program could depend on being
		 * 0: the conventional "no such ioctl". */
		return -ENOTTY;
	}
}

static const struct device_ops i2cdev_ops = {
	.open = i2cdev_open,
	.release = i2cdev_release,
	.read = i2cdev_read,
	.write = i2cdev_write,
	.ioctl = i2cdev_ioctl,
};

/* ==========================================================================
 * Nodes
 * ========================================================================== */

int i2c_dev_attach_adapter(struct i2c_adapter *adap)
{
	struct i2c_dev *idev;
	uint64_t fl;
	int rc;

	might_sleep();
	if (!g_i2c_dev_major)
		return -ENODEV;
	if (WARN_ON(adap->i2c_dev))
		return -EEXIST;
	idev = kcalloc(1, sizeof(*idev));
	if (!idev)
		return -ENOMEM;
	idev->nr = adap->nr;
	ksnprintf(idev->node.path, sizeof(idev->node.path), "/dev/i2c-%d",
		  adap->nr);
	idev->node.mode = 0600;
	idev->node.uid = 0;
	idev->node.gid = 0;
	idev->node.major = (uint32_t)g_i2c_dev_major;
	idev->node.minor = (uint32_t)adap->nr;
	idev->node.ops = &i2cdev_ops;
	idev->node.priv = idev;

	/* The registry first: an open that finds the node finds the
	 * entry. */
	spin_lock_irqsave(&g_i2c_dev_lock, &fl);
	list_add_tail(&idev->link, &g_i2c_devs);
	spin_unlock_irqrestore(&g_i2c_dev_lock, fl);

	rc = device_register(&idev->node);
	if (rc) {
		spin_lock_irqsave(&g_i2c_dev_lock, &fl);
		list_del(&idev->link);
		spin_unlock_irqrestore(&g_i2c_dev_lock, fl);
		kfree(idev);
		return rc;
	}
	adap->i2c_dev = idev;
	return 0;
}

void i2c_dev_detach_adapter(struct i2c_adapter *adap)
{
	struct i2c_dev *idev = adap->i2c_dev;
	uint64_t fl;

	if (!idev)
		return;
	spin_lock_irqsave(&g_i2c_dev_lock, &fl);
	idev->dead = 1;
	spin_unlock_irqrestore(&g_i2c_dev_lock, fl);
	device_unregister(&idev->node);
}

void i2c_dev_free_adapter(struct i2c_adapter *adap)
{
	struct i2c_dev *idev = adap->i2c_dev;
	uint64_t fl;

	might_sleep();
	if (!idev)
		return;
	WARN_ON(!idev->dead);
	/* Open files held adapter references, so none is left; an open of
	 * the dead node still in flight in devfs fails without touching
	 * the entry beyond the registry lookup -- let it finish. */
	while (devfs_node_in_use(&idev->node))
		msleep(1);
	spin_lock_irqsave(&g_i2c_dev_lock, &fl);
	WARN_ON(idev->opens);
	list_del(&idev->link);
	spin_unlock_irqrestore(&g_i2c_dev_lock, fl);
	adap->i2c_dev = NULL;
	kfree(idev);
}

int i2c_dev_init(void)
{
	int major;

	if (g_i2c_dev_major)
		return 0;
	major = chrdev_register(I2C_DEV_MAJOR, "i2c");
	if (major < 0)
		return major;
	g_i2c_dev_major = major;
	kprintf("i2c_dev: i2c /dev entries driver\n");
	return 0;
}
