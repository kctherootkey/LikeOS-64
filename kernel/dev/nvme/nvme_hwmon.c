// LikeOS - NVMe health monitoring: SMART / health log, critical warning
// watch, temperatures and thresholds, error log tracking
//
// Temperatures are exposed like the established hardware monitoring
// interface (temp<N>_input/max/min/crit/alarm/label under the
// controller's sysfs directory, nvme_sysfs.c): channel 0 is the
// composite temperature, channels 1..8 the controller's sensors.  Every
// reading of a temperature reads the SMART / health log anew (like the
// established driver; the log is small); max/min are the Temperature
// Threshold feature (over/under), crit is CCTEMP.
//
// A SMART / health asynchronous event, a temperature hysteresis notice or
// an error event lands here from async_event_work: the log page is read
// with RAE clear (which also ends the event), newly raised critical
// warnings and new error log entries are reported.
//
// Lifetime: struct nvme_hwmon_data hangs off ctrl->hwmon from the first
// nvme_init_ctrl_finish until the controller is freed.  nvme_hwmon_exit
// (controller removal) only marks it dead under its read lock, so a
// sysfs reader that still holds a controller reference finds -ENODEV
// instead of freed memory; nvme_hwmon_free (last reference) frees it.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/nvme/nvme.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/fs/sysfs.h>

/* Error log entries read per error event (64 bytes each).  Reading any
 * part of the log with RAE clear ends the event; the newest entries come
 * first. */
#define NVME_HWMON_ERRLOG_ENTRIES 16

struct nvme_hwmon_data {
	struct nvme_ctrl *ctrl;
	struct nvme_smart_log *log; /* the last SMART / health log read */
	struct nvme_error_slot *errlog; /* error log buffer */
	struct mutex read_lock; /* log buffers, dead */
	bool dead; /* the controller is going away */
	bool log_valid;
	uint8_t last_crit; /* critical warning byte last reported */
	uint64_t errors_seen; /* highest error count reported */
	/* /sys/class/hwmon/hwmon<N> number, -1 when not registered. */
	int hwmon_id;
};

static const char *const nvme_crit_warning_names[8] = {
	"available spare below threshold",
	"temperature outside a threshold",
	"NVM subsystem reliability degraded",
	"media placed in read only mode",
	"volatile memory backup device failed",
	"persistent memory region read only or unreliable",
	"reserved warning bit 6",
	"reserved warning bit 7",
};

static const char *const nvme_hwmon_sensor_names[NVME_HWMON_CHANNELS] = {
	"Composite", "Sensor 1", "Sensor 2", "Sensor 3", "Sensor 4",
	"Sensor 5",  "Sensor 6", "Sensor 7", "Sensor 8",
};

const char *nvme_hwmon_label(int channel)
{
	if (channel < 0 || channel >= NVME_HWMON_CHANNELS)
		return "?";
	return nvme_hwmon_sensor_names[channel];
}

/* ==========================================================================
 * Thresholds
 * ========================================================================== */

int nvme_get_temp_thresh(struct nvme_ctrl *ctrl, int sensor, bool under,
			 uint16_t *kelvin)
{
	unsigned int threshold;
	uint32_t status = 0;
	int ret;

	might_sleep();
	if (sensor < 0 || sensor > 8)
		return -EINVAL;
	threshold = (unsigned int)sensor << NVME_TEMP_THRESH_SELECT_SHIFT;
	if (under)
		threshold |= NVME_TEMP_THRESH_TYPE_UNDER;
	ret = nvme_get_features(ctrl, NVME_FEAT_TEMP_THRESH, threshold, NULL,
				0, &status);
	if (ret > 0)
		return -EIO;
	if (ret < 0)
		return ret;
	*kelvin = (uint16_t)(status & NVME_TEMP_THRESH_MASK);
	return 0;
}

int nvme_set_temp_thresh(struct nvme_ctrl *ctrl, int sensor, bool under,
			 uint16_t kelvin)
{
	unsigned int threshold;
	int ret;

	might_sleep();
	if (sensor < 0 || sensor > 8)
		return -EINVAL;
	if (ctrl->quirks & NVME_QUIRK_NO_TEMP_THRESH_CHANGE)
		return -EPERM;
	if (sensor && (ctrl->quirks & NVME_QUIRK_NO_SECONDARY_TEMP_THRESH))
		return -EOPNOTSUPP;
	threshold = (unsigned int)sensor << NVME_TEMP_THRESH_SELECT_SHIFT;
	threshold |= kelvin & NVME_TEMP_THRESH_MASK;
	if (under)
		threshold |= NVME_TEMP_THRESH_TYPE_UNDER;
	ret = nvme_set_features(ctrl, NVME_FEAT_TEMP_THRESH, threshold, NULL,
				0, NULL);
	if (ret > 0)
		return -EIO;
	return ret;
}

/* ==========================================================================
 * SMART / health log
 * ========================================================================== */

/* Read the log into `log' (DMA-able) and record what the core keeps of
 * it. */
static int nvme_hwmon_read_smart(struct nvme_ctrl *ctrl,
				 struct nvme_smart_log *log)
{
	int ret;

	BUILD_BUG_ON(sizeof(struct nvme_smart_log) != 512);
	ret = nvme_get_log(ctrl, NVME_NSID_ALL, NVME_LOG_SMART, 0,
			   NVME_CSI_NVM, log, sizeof(*log), 0);
	if (ret)
		return ret;
	ctrl->smart_crit_warning = log->critical_warning;
	ctrl->smart_composite_temp = get_unaligned_le16(log->temperature);
	ctrl->smart_last_ns = hrtimer_now_ns();
	return 0;
}

int nvme_get_smart_log(struct nvme_ctrl *ctrl, struct nvme_smart_log *log)
{
	struct nvme_hwmon_data *data = ctrl->hwmon;
	int ret;

	might_sleep();
	if (log)
		return nvme_hwmon_read_smart(ctrl, log);
	/* NULL: refresh the cached copy. */
	if (!data)
		return -ENODEV;
	mutex_lock(&data->read_lock);
	if (data->dead) {
		ret = -ENODEV;
	} else {
		ret = nvme_hwmon_read_smart(ctrl, data->log);
		data->log_valid = ret == 0;
	}
	mutex_unlock(&data->read_lock);
	return ret;
}

/* Report the warning bits newly set since the last report (and the ones
 * cleared).  read_lock held. */
static void nvme_hwmon_report_crit_locked(struct nvme_hwmon_data *data,
					  bool initial)
{
	struct nvme_ctrl *ctrl = data->ctrl;
	uint8_t now = data->log->critical_warning;
	uint8_t raised = (uint8_t)(now & ~data->last_crit);
	uint8_t cleared = (uint8_t)(data->last_crit & ~now);
	int bit;

	lockdep_assert_held(&data->read_lock);
	for (bit = 0; bit < 8; bit++) {
		if (raised & (1u << bit))
			kprintf(NVME_LOG_PREFIX
				"%s: %scritical warning: %s\n",
				ctrl->name, initial ? "" : "new ",
				nvme_crit_warning_names[bit]);
		if (cleared & (1u << bit))
			kprintf(NVME_LOG_PREFIX
				"%s: critical warning cleared: %s\n",
				ctrl->name, nvme_crit_warning_names[bit]);
	}
	if ((raised & NVME_SMART_CRIT_TEMPERATURE) ||
	    (initial && (now & NVME_SMART_CRIT_TEMPERATURE))) {
		uint16_t k = get_unaligned_le16(data->log->temperature);

		kprintf(NVME_LOG_PREFIX
			"%s: composite temperature %ld C (warning %ld C, "
			"critical %ld C)\n",
			ctrl->name, nvme_kelvin_to_millicelsius(k) / 1000,
			ctrl->wctemp ?
				nvme_kelvin_to_millicelsius(ctrl->wctemp) /
					1000 :
				0L,
			ctrl->cctemp ?
				nvme_kelvin_to_millicelsius(ctrl->cctemp) /
					1000 :
				0L);
	}
	if (raised & NVME_SMART_CRIT_SPARE)
		kprintf(NVME_LOG_PREFIX
			"%s: available spare %u%% (threshold %u%%)\n",
			ctrl->name, data->log->avail_spare,
			data->log->spare_thresh);
	data->last_crit = now;
}

void nvme_hwmon_smart_event(struct nvme_ctrl *ctrl, uint32_t aen_result)
{
	struct nvme_hwmon_data *data = ctrl->hwmon;
	struct nvme_smart_log *log = NULL;
	uint32_t type = NVME_AER_RESULT_TYPE(aen_result);
	uint32_t info = NVME_AER_RESULT_INFO(aen_result);
	int ret;

	might_sleep();
	if (!data) {
		/* No hwmon (init failed): still read the log to end the
		 * event. */
		log = kalloc(sizeof(*log));
		if (!log)
			return;
		ret = nvme_hwmon_read_smart(ctrl, log);
		if (!ret && log->critical_warning)
			kprintf(NVME_LOG_PREFIX
				"%s: critical warning byte 0x%02x\n",
				ctrl->name, log->critical_warning);
		kfree(log);
		return;
	}

	mutex_lock(&data->read_lock);
	if (data->dead) {
		mutex_unlock(&data->read_lock);
		return;
	}
	ret = nvme_hwmon_read_smart(ctrl, data->log);
	data->log_valid = ret == 0;
	if (ret) {
		mutex_unlock(&data->read_lock);
		kprintf(NVME_LOG_PREFIX
			"%s: SMART log read after event failed (%d)\n",
			ctrl->name, ret);
		return;
	}
	if (type == NVME_AER_SMART) {
		switch (info) {
		case NVME_AER_SMART_SUBSYSTEM_RELIABILITY:
			kprintf(NVME_LOG_PREFIX
				"%s: SMART event: NVM subsystem reliability\n",
				ctrl->name);
			break;
		case NVME_AER_SMART_TEMPERATURE_THRESHOLD:
			kprintf(NVME_LOG_PREFIX
				"%s: SMART event: temperature threshold\n",
				ctrl->name);
			break;
		case NVME_AER_SMART_SPARE_THRESHOLD:
			kprintf(NVME_LOG_PREFIX
				"%s: SMART event: spare below threshold\n",
				ctrl->name);
			break;
		default:
			kprintf(NVME_LOG_PREFIX
				"%s: SMART event: info 0x%02x\n",
				ctrl->name, info);
			break;
		}
	} else if (type == NVME_AER_ONESHOT &&
		   info == NVME_AER_ONESHOT_TEMP_HYSTERESIS) {
		kprintf(NVME_LOG_PREFIX
			"%s: temperature back within its threshold "
			"hysteresis (composite %ld C)\n",
			ctrl->name,
			nvme_kelvin_to_millicelsius(
				get_unaligned_le16(data->log->temperature)) /
				1000);
	}
	nvme_hwmon_report_crit_locked(data, false);
	mutex_unlock(&data->read_lock);
}

/* ==========================================================================
 * Error log
 * ========================================================================== */

void nvme_hwmon_error_event(struct nvme_ctrl *ctrl)
{
	struct nvme_hwmon_data *data = ctrl->hwmon;
	struct nvme_error_slot *slots;
	unsigned int n = (unsigned int)ctrl->elpe + 1, i, shown = 0;
	uint64_t newest = 0, seen;
	bool own = false;
	int ret;

	might_sleep();
	BUILD_BUG_ON(sizeof(struct nvme_error_slot) != 64);
	if (n > NVME_HWMON_ERRLOG_ENTRIES)
		n = NVME_HWMON_ERRLOG_ENTRIES;

	if (data) {
		mutex_lock(&data->read_lock);
		if (data->dead) {
			mutex_unlock(&data->read_lock);
			return;
		}
		slots = data->errlog;
		seen = data->errors_seen;
	} else {
		slots = kcalloc(n, sizeof(*slots));
		if (!slots)
			return;
		own = true;
		seen = ctrl->error_count;
	}

	ret = nvme_get_log(ctrl, NVME_NSID_ALL, NVME_LOG_ERROR, 0,
			   NVME_CSI_NVM, slots, n * sizeof(*slots), 0);
	if (ret) {
		kprintf(NVME_LOG_PREFIX "%s: error log read failed (%d)\n",
			ctrl->name, ret);
		goto out;
	}
	for (i = 0; i < n; i++) {
		const struct nvme_error_slot *e = &slots[i];
		uint64_t cnt = le64_to_cpu(e->error_count);
		uint16_t st = le16_to_cpu(e->status_field) >> 1;

		if (!cnt || cnt <= seen)
			continue;
		if (cnt > newest)
			newest = cnt;
		if (shown++ >= 8)
			continue;
		kprintf(NVME_LOG_PREFIX
			"%s: error #%llu: sqid %u cid %#x opcode 0x%02x nsid "
			"%u lba %llu: %s (sct 0x%x / sc 0x%x)\n",
			ctrl->name, (unsigned long long)cnt,
			le16_to_cpu(e->sqid), le16_to_cpu(e->cmdid),
			e->opcode, le32_to_cpu(e->nsid),
			(unsigned long long)le64_to_cpu(e->lba),
			nvme_get_error_status_str(st), NVME_SCT(st),
			st & NVME_SC_MASK);
	}
	if (shown > 8)
		kprintf(NVME_LOG_PREFIX "%s: ... %u more error entries\n",
			ctrl->name, shown - 8);
	if (newest > seen) {
		if (data)
			data->errors_seen = newest;
		if (newest > ctrl->error_count)
			ctrl->error_count = newest;
	}
out:
	if (own)
		kfree(slots);
	else
		mutex_unlock(&data->read_lock);
}

/* ==========================================================================
 * Temperature attributes
 * ========================================================================== */

unsigned int nvme_hwmon_is_visible(struct nvme_ctrl *ctrl,
				   enum nvme_hwmon_attr attr, int channel)
{
	struct nvme_hwmon_data *data = ctrl->hwmon;
	unsigned int mode = 0;

	if (!data || channel < 0 || channel >= NVME_HWMON_CHANNELS)
		return 0;
	mutex_lock(&data->read_lock);
	if (data->dead || !data->log_valid)
		goto out;
	switch (attr) {
	case NVME_HWMON_TEMP_CRIT:
		if (!channel && ctrl->cctemp)
			mode = 0444;
		break;
	case NVME_HWMON_TEMP_MAX:
	case NVME_HWMON_TEMP_MIN:
		if ((!channel && ctrl->wctemp) ||
		    (channel && data->log->temp_sensor[channel - 1] &&
		     !(ctrl->quirks & NVME_QUIRK_NO_SECONDARY_TEMP_THRESH)))
			mode = (ctrl->quirks &
				NVME_QUIRK_NO_TEMP_THRESH_CHANGE) ?
				       0444 :
				       0644;
		break;
	case NVME_HWMON_TEMP_ALARM:
		if (!channel)
			mode = 0444;
		break;
	case NVME_HWMON_TEMP_INPUT:
	case NVME_HWMON_TEMP_LABEL:
		if (!channel || data->log->temp_sensor[channel - 1])
			mode = 0444;
		break;
	}
out:
	mutex_unlock(&data->read_lock);
	return mode;
}

int nvme_hwmon_read(struct nvme_ctrl *ctrl, enum nvme_hwmon_attr attr,
		    int channel, long *val)
{
	struct nvme_hwmon_data *data = ctrl->hwmon;
	uint16_t k;
	int err;

	might_sleep();
	if (!data)
		return -ENODEV;
	if (channel < 0 || channel >= NVME_HWMON_CHANNELS)
		return -EINVAL;

	/* What needs no SMART log first. */
	switch (attr) {
	case NVME_HWMON_TEMP_MAX:
		err = nvme_get_temp_thresh(ctrl, channel, false, &k);
		if (!err)
			*val = nvme_kelvin_to_millicelsius(k);
		return err;
	case NVME_HWMON_TEMP_MIN:
		err = nvme_get_temp_thresh(ctrl, channel, true, &k);
		if (!err)
			*val = nvme_kelvin_to_millicelsius(k);
		return err;
	case NVME_HWMON_TEMP_CRIT:
		*val = nvme_kelvin_to_millicelsius(ctrl->cctemp);
		return 0;
	case NVME_HWMON_TEMP_LABEL:
		return -EOPNOTSUPP; /* a string: nvme_hwmon_label() */
	default:
		break;
	}

	mutex_lock(&data->read_lock);
	if (data->dead) {
		err = -ENODEV;
		goto unlock;
	}
	err = nvme_hwmon_read_smart(ctrl, data->log);
	data->log_valid = err == 0;
	if (err) {
		if (err > 0)
			err = -EIO;
		goto unlock;
	}
	switch (attr) {
	case NVME_HWMON_TEMP_INPUT:
		if (!channel)
			k = get_unaligned_le16(data->log->temperature);
		else
			k = le16_to_cpu(data->log->temp_sensor[channel - 1]);
		*val = nvme_kelvin_to_millicelsius(k);
		break;
	case NVME_HWMON_TEMP_ALARM:
		*val = !!(data->log->critical_warning &
			  NVME_SMART_CRIT_TEMPERATURE);
		break;
	default:
		err = -EOPNOTSUPP;
		break;
	}
unlock:
	mutex_unlock(&data->read_lock);
	return err;
}

int nvme_hwmon_write(struct nvme_ctrl *ctrl, enum nvme_hwmon_attr attr,
		     int channel, long val)
{
	long k;

	might_sleep();
	if (channel < 0 || channel >= NVME_HWMON_CHANNELS)
		return -EINVAL;
	k = nvme_millicelsius_to_kelvin(val);
	if (k < 0)
		k = 0;
	if (k > NVME_TEMP_THRESH_MASK)
		k = NVME_TEMP_THRESH_MASK;
	switch (attr) {
	case NVME_HWMON_TEMP_MAX:
		return nvme_set_temp_thresh(ctrl, channel, false,
					    (uint16_t)k);
	case NVME_HWMON_TEMP_MIN:
		return nvme_set_temp_thresh(ctrl, channel, true, (uint16_t)k);
	default:
		return -EOPNOTSUPP;
	}
}

/* ==========================================================================
 * Lifetime
 * ========================================================================== */

int nvme_hwmon_init(struct nvme_ctrl *ctrl)
{
	struct nvme_hwmon_data *data;
	int err;

	might_sleep();
	if (ctrl->hwmon)
		return 0;
	data = kcalloc(1, sizeof(*data));
	if (!data)
		return -ENOMEM;
	data->log = kcalloc(1, sizeof(*data->log));
	data->errlog = kcalloc(NVME_HWMON_ERRLOG_ENTRIES,
			       sizeof(*data->errlog));
	if (!data->log || !data->errlog) {
		err = -ENOMEM;
		goto err_free;
	}
	data->ctrl = ctrl;
	data->hwmon_id = -1;
	mutex_init(&data->read_lock, "nvme_hwmon");

	err = nvme_hwmon_read_smart(ctrl, data->log);
	if (err) {
		kprintf(NVME_LOG_PREFIX
			"%s: failed to read smart log (error %d)\n",
			ctrl->name, err);
		goto err_free;
	}
	data->log_valid = true;
	/* The error entries already counted are not "new". */
	data->errors_seen = get_unaligned_le64(data->log->num_err_log_entries);
	ctrl->error_count = data->errors_seen;

	mutex_lock(&data->read_lock);
	data->last_crit = 0;
	if (data->log->critical_warning)
		nvme_hwmon_report_crit_locked(data, true);
	mutex_unlock(&data->read_lock);

	/* The monitor's directory is /sys/class/nvme/nvmeX/hwmon<N>, and
	 * /sys/class/hwmon/hwmon<N> leads to it, where sensor tools look. */
	{
		char parent[48];

		ksnprintf(parent, sizeof(parent), "/sys/class/nvme/%s",
			  ctrl->name);
		data->hwmon_id = sysfs_hwmon_register(parent);
		if (data->hwmon_id < 0)
			kprintf(NVME_LOG_PREFIX
				"%s: no hwmon class entry (%d)\n", ctrl->name,
				data->hwmon_id);
	}

	__atomic_store_n(&ctrl->hwmon, data, __ATOMIC_RELEASE);
	return 0;

err_free:
	if (data->errlog)
		kfree(data->errlog);
	if (data->log)
		kfree(data->log);
	kfree(data);
	return err;
}

void nvme_hwmon_exit(struct nvme_ctrl *ctrl)
{
	struct nvme_hwmon_data *data = ctrl->hwmon;

	if (!data)
		return;
	mutex_lock(&data->read_lock);
	data->dead = true;
	mutex_unlock(&data->read_lock);
	{
		/* Gone from the controller's directory (readers see -1),
		 * then from the hwmon class. */
		int id = __atomic_exchange_n(&data->hwmon_id, -1,
					     __ATOMIC_ACQ_REL);

		if (id >= 0)
			sysfs_hwmon_unregister(id);
	}
}

int nvme_hwmon_id(struct nvme_ctrl *ctrl)
{
	struct nvme_hwmon_data *data =
		__atomic_load_n(&ctrl->hwmon, __ATOMIC_ACQUIRE);

	return data ? __atomic_load_n(&data->hwmon_id, __ATOMIC_RELAXED) : -1;
}

void nvme_hwmon_free(struct nvme_ctrl *ctrl)
{
	struct nvme_hwmon_data *data =
		__atomic_exchange_n(&ctrl->hwmon, NULL, __ATOMIC_ACQ_REL);

	if (!data)
		return;
	/* Nobody holds a controller reference any more: no reader. */
	BUG_ON(mutex_is_locked(&data->read_lock));
	kfree(data->errlog);
	kfree(data->log);
	kfree(data);
}
