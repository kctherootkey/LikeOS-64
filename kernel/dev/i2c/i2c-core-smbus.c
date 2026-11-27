// LikeOS I2C - SMBus protocols, natively or emulated over I2C messages
//
// An SMBus transaction is a fixed pattern of I2C messages: the command
// byte written, then data written or -- after a repeated START -- read.
// An adapter with native SMBus support (algo->smbus_xfer) gets the
// transaction as such; every other adapter gets it as one or two
// messages from i2c_smbus_xfer_emulated():
//
//   quick         S addr R/W P                       (the R/W bit is the data)
//   byte          S addr W cmd P  /  S addr R [byte] P
//   byte data     S addr W cmd byte P  /  S addr W cmd Sr addr R [byte] P
//   word data     ... two bytes, low first
//   proc call     S addr W cmd lo hi Sr addr R [lo hi] P
//   block data    S addr W cmd count data... P  /
//                 S addr W cmd Sr addr R [count data...] P
//                 (the read length comes from the device: I2C_M_RECV_LEN)
//   block proc    block write then block read in one transaction
//   I2C block     like block data without the count byte (length given)
//
// With I2C_CLIENT_PEC a packet error code -- CRC-8 over every byte on the
// wire, address bytes included -- is appended to writes and checked on
// reads (-EBADMSG on a mismatch), except for quick and I2C block
// transactions.  Block lengths are bounded by I2C_SMBUS_BLOCK_MAX (32).
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) Frodo Looijaard, Mark Studebaker, Jean Delvare

#include <kernel/dev/i2c/i2c-core.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

/* ==========================================================================
 * Packet error checking: CRC-8, x^8 + x^2 + x + 1, MSB first, initial 0
 * ========================================================================== */

#define SMBUS_PEC_POLY (0x1070u << 3)

static uint8_t crc8_step(uint16_t data)
{
	for (int i = 0; i < 8; i++) {
		if (data & 0x8000)
			data = (uint16_t)(data ^ SMBUS_PEC_POLY);
		data = (uint16_t)(data << 1);
	}
	return (uint8_t)(data >> 8);
}

uint8_t i2c_smbus_pec(uint8_t crc, const uint8_t *p, size_t count)
{
	for (size_t i = 0; i < count; i++)
		crc = crc8_step((uint16_t)((crc ^ p[i]) << 8));
	return crc;
}

/* The PEC of one message: the address byte (7-bit addressing assumed,
 * as SMBus has no other), then the data. */
static uint8_t i2c_smbus_msg_pec(uint8_t pec, const struct i2c_msg *msg)
{
	uint8_t addr = i2c_8bit_addr_from_msg(msg);

	pec = i2c_smbus_pec(pec, &addr, 1);
	return i2c_smbus_pec(pec, msg->buf, msg->len);
}

/* Append the PEC to a write message (its buffer has room). */
static void i2c_smbus_add_pec(struct i2c_msg *msg)
{
	msg->buf[msg->len] = i2c_smbus_msg_pec(0, msg);
	msg->len++;
}

/* The last byte of a read message is the device's PEC: compare it with
 * ours over everything before it (and `cpec', the PEC of the write that
 * came first).  The PEC byte is removed from msg->len. */
static int i2c_smbus_check_pec(uint8_t cpec, struct i2c_msg *msg)
{
	uint8_t rpec;

	if (WARN_ON(!msg->len))
		return -EBADMSG;
	rpec = msg->buf[--msg->len];
	cpec = i2c_smbus_msg_pec(cpec, msg);
	if (rpec != cpec)
		return -EBADMSG;
	return 0;
}

/* ==========================================================================
 * Emulation over I2C messages
 * ========================================================================== */

static int i2c_smbus_xfer_emulated(struct i2c_adapter *adap, uint16_t addr,
				   unsigned short flags, char read_write,
				   uint8_t command, int size,
				   union i2c_smbus_data *data)
{
	/* A write needs one message, a read two.  The buffers have room for
	 * the command byte, a maximal block with its count, and the PEC. */
	uint8_t msgbuf0[I2C_SMBUS_BLOCK_MAX + 3];
	uint8_t msgbuf1[I2C_SMBUS_BLOCK_MAX + 2];
	int nmsgs = read_write == I2C_SMBUS_READ ? 2 : 1;
	uint8_t partial_pec = 0;
	int status;
	struct i2c_msg msg[2];
	int wants_pec = (flags & I2C_CLIENT_PEC) && size != I2C_SMBUS_QUICK &&
			size != I2C_SMBUS_I2C_BLOCK_DATA;

	msg[0].addr = addr;
	msg[0].flags = flags;
	msg[0].len = 1;
	msg[0].buf = msgbuf0;
	msg[1].addr = addr;
	msg[1].flags = (uint16_t)(flags | I2C_M_RD);
	msg[1].len = 0;
	msg[1].buf = msgbuf1;
	msgbuf0[0] = command;
	msgbuf1[0] = 0;

	switch (size) {
	case I2C_SMBUS_QUICK:
		msg[0].len = 0;
		/* The direction bit is the data. */
		msg[0].flags = (uint16_t)(flags | (read_write == I2C_SMBUS_READ ?
							   I2C_M_RD :
							   0));
		nmsgs = 1;
		break;
	case I2C_SMBUS_BYTE:
		if (read_write == I2C_SMBUS_READ) {
			/* only a read */
			msg[0].flags = (uint16_t)(I2C_M_RD | flags);
			nmsgs = 1;
		}
		break;
	case I2C_SMBUS_BYTE_DATA:
		if (read_write == I2C_SMBUS_READ) {
			msg[1].len = 1;
		} else {
			msg[0].len = 2;
			msgbuf0[1] = data->byte;
		}
		break;
	case I2C_SMBUS_WORD_DATA:
		if (read_write == I2C_SMBUS_READ) {
			msg[1].len = 2;
		} else {
			msg[0].len = 3;
			msgbuf0[1] = (uint8_t)(data->word & 0xff);
			msgbuf0[2] = (uint8_t)(data->word >> 8);
		}
		break;
	case I2C_SMBUS_PROC_CALL:
		nmsgs = 2; /* a write and a read whatever read_write says */
		read_write = I2C_SMBUS_READ;
		msg[0].len = 3;
		msg[1].len = 2;
		msgbuf0[1] = (uint8_t)(data->word & 0xff);
		msgbuf0[2] = (uint8_t)(data->word >> 8);
		break;
	case I2C_SMBUS_BLOCK_DATA:
		if (read_write == I2C_SMBUS_READ) {
			msg[1].flags |= I2C_M_RECV_LEN;
			/* the controller adds the length it receives */
			msg[1].len = 1;
		} else {
			if (data->block[0] > I2C_SMBUS_BLOCK_MAX) {
				kprintf("i2c %s: Invalid block write size %d\n",
					adap->dev_name, data->block[0]);
				return -EINVAL;
			}
			msg[0].len = (uint16_t)(data->block[0] + 2);
			kmemcpy(msgbuf0 + 1, data->block, msg[0].len - 1u);
		}
		break;
	case I2C_SMBUS_BLOCK_PROC_CALL:
		nmsgs = 2;
		read_write = I2C_SMBUS_READ;
		if (data->block[0] > I2C_SMBUS_BLOCK_MAX) {
			kprintf("i2c %s: Invalid block write size %d\n",
				adap->dev_name, data->block[0]);
			return -EINVAL;
		}
		msg[0].len = (uint16_t)(data->block[0] + 2);
		kmemcpy(msgbuf0 + 1, data->block, msg[0].len - 1u);
		msg[1].flags |= I2C_M_RECV_LEN;
		msg[1].len = 1;
		break;
	case I2C_SMBUS_I2C_BLOCK_DATA:
		if (data->block[0] > I2C_SMBUS_BLOCK_MAX) {
			kprintf("i2c %s: Invalid block %s size %d\n",
				adap->dev_name,
				read_write == I2C_SMBUS_READ ? "read" : "write",
				data->block[0]);
			return -EINVAL;
		}
		if (read_write == I2C_SMBUS_READ) {
			msg[1].len = data->block[0];
		} else {
			msg[0].len = (uint16_t)(data->block[0] + 1);
			kmemcpy(msgbuf0 + 1, data->block + 1, data->block[0]);
		}
		break;
	default:
		kprintf("i2c %s: Unsupported transaction %d\n", adap->dev_name,
			size);
		return -EOPNOTSUPP;
	}

	if (wants_pec) {
		/* PEC of a write that comes first */
		if (!(msg[0].flags & I2C_M_RD)) {
			if (nmsgs == 1) /* write only */
				i2c_smbus_add_pec(&msg[0]);
			else /* write then read */
				partial_pec = i2c_smbus_msg_pec(0, &msg[0]);
		}
		/* a PEC byte more to read at the end */
		if (msg[nmsgs - 1].flags & I2C_M_RD)
			msg[nmsgs - 1].len++;
	}

	status = __i2c_transfer(adap, msg, nmsgs);
	if (status < 0)
		return status;
	if (status != nmsgs)
		return -EIO;
	status = 0;

	/* A block read's length came from the device: the controller may
	 * never have stored more than the buffer holds. */
	if (nmsgs == 2 && msg[1].len > sizeof(msgbuf1)) {
		WARN(1, "i2c %s: read of %u bytes overran its buffer",
		     adap->dev_name, msg[1].len);
		return -EPROTO;
	}

	if (wants_pec && (msg[nmsgs - 1].flags & I2C_M_RD)) {
		status = i2c_smbus_check_pec(partial_pec, &msg[nmsgs - 1]);
		if (status < 0)
			return status;
	}

	if (read_write == I2C_SMBUS_READ) {
		switch (size) {
		case I2C_SMBUS_BYTE:
			data->byte = msgbuf0[0];
			break;
		case I2C_SMBUS_BYTE_DATA:
			data->byte = msgbuf1[0];
			break;
		case I2C_SMBUS_WORD_DATA:
		case I2C_SMBUS_PROC_CALL:
			data->word = (uint16_t)(msgbuf1[0] | (msgbuf1[1] << 8));
			break;
		case I2C_SMBUS_I2C_BLOCK_DATA:
			kmemcpy(data->block + 1, msg[1].buf, data->block[0]);
			break;
		case I2C_SMBUS_BLOCK_DATA:
		case I2C_SMBUS_BLOCK_PROC_CALL:
			if (msg[1].buf[0] > I2C_SMBUS_BLOCK_MAX) {
				kprintf("i2c %s: Invalid block size returned: %d\n",
					adap->dev_name, msg[1].buf[0]);
				return -EPROTO;
			}
			kmemcpy(data->block, msg[1].buf,
				(size_t)msg[1].buf[0] + 1);
			break;
		}
	}
	return status;
}

/* ==========================================================================
 * Entry points
 * ========================================================================== */

int __i2c_smbus_xfer(struct i2c_adapter *adap, uint16_t addr,
		     unsigned short flags, char read_write, uint8_t command,
		     int protocol, union i2c_smbus_data *data)
{
	int (*xfer_func)(struct i2c_adapter *, uint16_t, unsigned short, char,
			 uint8_t, int, union i2c_smbus_data *);
	uint64_t orig_ns;
	int res, try;

	lockdep_assert_held(&adap->bus_lock);
	res = __i2c_check_suspended(adap);
	if (res)
		return res;
	/* Every protocol but quick and byte write moves data. */
	if (!data && protocol != I2C_SMBUS_QUICK &&
	    !(protocol == I2C_SMBUS_BYTE && read_write == I2C_SMBUS_WRITE))
		return -EINVAL;
	i2c_stat_add(&adap->stats.smbus, 1);

	flags &= I2C_M_TEN | I2C_CLIENT_PEC | I2C_CLIENT_SCCB;

	xfer_func = adap->algo->smbus_xfer;
	if (i2c_in_atomic_xfer_mode()) {
		if (adap->algo->smbus_xfer_atomic)
			xfer_func = adap->algo->smbus_xfer_atomic;
		else if (adap->algo->master_xfer_atomic)
			xfer_func = NULL; /* emulate over the atomic I2C */
	}

	if (xfer_func) {
		/* Retry automatically on arbitration loss. */
		orig_ns = hrtimer_now_ns();
		for (res = 0, try = 0; try <= adap->retries; try++) {
			res = xfer_func(adap, addr, flags, read_write, command,
					protocol, data);
			if (res != -EAGAIN)
				break;
			if (hrtimer_now_ns() - orig_ns >
			    (uint64_t)adap->timeout * 1000000ull)
				break;
		}
		if (res != -EOPNOTSUPP || !adap->algo->master_xfer)
			return res;
		/* The protocol is not native: emulate it. */
	}
	return i2c_smbus_xfer_emulated(adap, addr, flags, read_write, command,
				       protocol, data);
}

int i2c_smbus_xfer(struct i2c_adapter *adap, uint16_t addr,
		   unsigned short flags, char read_write, uint8_t command,
		   int protocol, union i2c_smbus_data *data)
{
	int res;

	if (WARN_ON(!adap))
		return -EINVAL;
	res = __i2c_lock_bus_helper(adap);
	if (res)
		return res;
	res = __i2c_smbus_xfer(adap, addr, flags, read_write, command,
			       protocol, data);
	i2c_unlock_bus(adap, I2C_LOCK_SEGMENT);
	return res;
}

/* ==========================================================================
 * Helpers for client drivers
 * ========================================================================== */

int i2c_smbus_read_byte(const struct i2c_client *client)
{
	union i2c_smbus_data data;
	int status;

	status = i2c_smbus_xfer(client->adapter, client->addr, client->flags,
				I2C_SMBUS_READ, 0, I2C_SMBUS_BYTE, &data);
	return (status < 0) ? status : data.byte;
}

int i2c_smbus_write_byte(const struct i2c_client *client, uint8_t value)
{
	return i2c_smbus_xfer(client->adapter, client->addr, client->flags,
			      I2C_SMBUS_WRITE, value, I2C_SMBUS_BYTE, NULL);
}

int i2c_smbus_read_byte_data(const struct i2c_client *client,
			     uint8_t command)
{
	union i2c_smbus_data data;
	int status;

	status = i2c_smbus_xfer(client->adapter, client->addr, client->flags,
				I2C_SMBUS_READ, command, I2C_SMBUS_BYTE_DATA,
				&data);
	return (status < 0) ? status : data.byte;
}

int i2c_smbus_write_byte_data(const struct i2c_client *client,
			      uint8_t command, uint8_t value)
{
	union i2c_smbus_data data;

	data.byte = value;
	return i2c_smbus_xfer(client->adapter, client->addr, client->flags,
			      I2C_SMBUS_WRITE, command, I2C_SMBUS_BYTE_DATA,
			      &data);
}

int i2c_smbus_read_word_data(const struct i2c_client *client,
			     uint8_t command)
{
	union i2c_smbus_data data;
	int status;

	status = i2c_smbus_xfer(client->adapter, client->addr, client->flags,
				I2C_SMBUS_READ, command, I2C_SMBUS_WORD_DATA,
				&data);
	return (status < 0) ? status : data.word;
}

int i2c_smbus_write_word_data(const struct i2c_client *client,
			      uint8_t command, uint16_t value)
{
	union i2c_smbus_data data;

	data.word = value;
	return i2c_smbus_xfer(client->adapter, client->addr, client->flags,
			      I2C_SMBUS_WRITE, command, I2C_SMBUS_WORD_DATA,
			      &data);
}

int i2c_smbus_read_word_swapped(const struct i2c_client *client,
				uint8_t command)
{
	int value = i2c_smbus_read_word_data(client, command);

	return (value < 0) ? value : (int)(((value & 0xff) << 8) |
					    ((value >> 8) & 0xff));
}

int i2c_smbus_write_word_swapped(const struct i2c_client *client,
				 uint8_t command, uint16_t value)
{
	return i2c_smbus_write_word_data(client, command,
					 (uint16_t)((value << 8) |
						    (value >> 8)));
}

int i2c_smbus_read_block_data(const struct i2c_client *client,
			      uint8_t command, uint8_t *values)
{
	union i2c_smbus_data data;
	int status;

	status = i2c_smbus_xfer(client->adapter, client->addr, client->flags,
				I2C_SMBUS_READ, command, I2C_SMBUS_BLOCK_DATA,
				&data);
	if (status)
		return status;
	kmemcpy(values, &data.block[1], data.block[0]);
	return data.block[0];
}

int i2c_smbus_write_block_data(const struct i2c_client *client,
			       uint8_t command, uint8_t length,
			       const uint8_t *values)
{
	union i2c_smbus_data data;

	if (length > I2C_SMBUS_BLOCK_MAX)
		length = I2C_SMBUS_BLOCK_MAX;
	data.block[0] = length;
	kmemcpy(&data.block[1], values, length);
	return i2c_smbus_xfer(client->adapter, client->addr, client->flags,
			      I2C_SMBUS_WRITE, command, I2C_SMBUS_BLOCK_DATA,
			      &data);
}

int i2c_smbus_read_i2c_block_data(const struct i2c_client *client,
				  uint8_t command, uint8_t length,
				  uint8_t *values)
{
	union i2c_smbus_data data;
	int status;

	if (length > I2C_SMBUS_BLOCK_MAX)
		length = I2C_SMBUS_BLOCK_MAX;
	data.block[0] = length;
	status = i2c_smbus_xfer(client->adapter, client->addr, client->flags,
				I2C_SMBUS_READ, command,
				I2C_SMBUS_I2C_BLOCK_DATA, &data);
	if (status < 0)
		return status;
	kmemcpy(values, &data.block[1], data.block[0]);
	return data.block[0];
}

int i2c_smbus_write_i2c_block_data(const struct i2c_client *client,
				   uint8_t command, uint8_t length,
				   const uint8_t *values)
{
	union i2c_smbus_data data;

	if (length > I2C_SMBUS_BLOCK_MAX)
		length = I2C_SMBUS_BLOCK_MAX;
	data.block[0] = length;
	kmemcpy(data.block + 1, values, length);
	return i2c_smbus_xfer(client->adapter, client->addr, client->flags,
			      I2C_SMBUS_WRITE, command,
			      I2C_SMBUS_I2C_BLOCK_DATA, &data);
}

int i2c_smbus_read_i2c_block_data_or_emulated(const struct i2c_client *client,
					      uint8_t command, uint8_t length,
					      uint8_t *values)
{
	uint8_t i = 0;
	int status;

	if (length > I2C_SMBUS_BLOCK_MAX)
		length = I2C_SMBUS_BLOCK_MAX;
	if (i2c_check_functionality(client->adapter,
				    I2C_FUNC_SMBUS_READ_I2C_BLOCK))
		return i2c_smbus_read_i2c_block_data(client, command, length,
						     values);
	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_READ_BYTE_DATA))
		return -EOPNOTSUPP;

	if (i2c_check_functionality(client->adapter,
				    I2C_FUNC_SMBUS_READ_WORD_DATA)) {
		while ((i + 2) <= length) {
			status = i2c_smbus_read_word_data(client,
							  (uint8_t)(command +
								    i));
			if (status < 0)
				return status;
			values[i] = (uint8_t)(status & 0xff);
			values[i + 1] = (uint8_t)(status >> 8);
			i = (uint8_t)(i + 2);
		}
	}
	while (i < length) {
		status = i2c_smbus_read_byte_data(client,
						  (uint8_t)(command + i));
		if (status < 0)
			return status;
		values[i] = (uint8_t)status;
		i++;
	}
	return i;
}
