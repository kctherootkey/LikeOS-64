/* The bus world of the I2C-HID harness (host/test-i2chid.sh): the
 * wrappers the HID world calls (kworld.h) and the machines -- ACPI
 * namespace, PCI functions, DesignWare models and the I2C-HID device
 * model -- shaped like the HP ProBook 650 G8 and the Dell Pro 16 Plus.
 * Compiled with the kshim prelude, like the I2C harness.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include "host/kshim/acpica_mock.h"
#include "host/i2c/model.h"
#include "host/i2c/mocks.h"
#include "host/i2chid/kworld.h"
#include "host/i2chid/kworld_int.h"
#include <kernel/dev/i2c/i2c.h>
#include <kernel/dev/i2c/i2c-core.h>
#include <kernel/dev/i2c/busses/i2c-designware-core.h>
#include <kernel/hal/acpi_dev.h>
#include <kernel/hal/dmi.h>
#include <kernel/ke/irq.h>
#include <kernel/ke/workqueue.h>

union rslot {
	ACPI_RESOURCE r;
	UINT8 pad[sizeof(ACPI_RESOURCE) + 32];
};

/* ==========================================================================
 * Firmware tables the DMI matcher would map: none (kw_dmi_set() loads the
 * machine's identification before anything asks).
 * ========================================================================== */

uint64_t g_boot_smbios_entry;
uint64_t g_boot_smbios3_entry;

void *AcpiOsMapMemory(ACPI_PHYSICAL_ADDRESS where, ACPI_SIZE length)
{
	(void)where;
	(void)length;
	return NULL;
}

void AcpiOsUnmapMemory(void *where, ACPI_SIZE length)
{
	(void)where;
	(void)length;
}

/* ==========================================================================
 * Threads
 * ========================================================================== */

void kw_adopt(void)
{
	static volatile int once;

	/* A thread of the HID world (the scenarios, its work queue) the bus
	 * world has not seen: give it a task and "interrupts on". */
	if (!kshim_current_or_null())
		kshim_init_main();
	/* The bus world's own start: work queues, the I2C core. */
	if (!__atomic_exchange_n(&once, 1, __ATOMIC_ACQ_REL)) {
		workqueue_init();
		i2c_core_init();
	}
}

/* ==========================================================================
 * The transport's calls
 * ========================================================================== */

int kwb_i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	kw_adopt();
	return i2c_transfer(adap, msgs, num);
}

int kwb_i2c_transfer_buffer_flags(const struct i2c_client *client, char *buf,
				  int count, uint16_t flags)
{
	kw_adopt();
	return i2c_transfer_buffer_flags(client, buf, count, flags);
}

int kwb_i2c_smbus_read_byte(const struct i2c_client *client)
{
	kw_adopt();
	return i2c_smbus_read_byte(client);
}

int kwb_i2c_register_driver(struct i2c_driver *drv)
{
	kw_adopt();
	return i2c_register_driver(drv);
}

void kwb_i2c_del_driver(struct i2c_driver *drv)
{
	kw_adopt();
	i2c_del_driver(drv);
}

int kwb_i2c_client_devpath(const struct i2c_client *client, char *buf,
			   size_t len)
{
	kw_adopt();
	return i2c_client_devpath(client, buf, len);
}

int kwb_request_threaded_irq(int irq, int (*fn)(void *),
			     int (*thread_fn)(void *), unsigned long flags,
			     const char *name, void *arg)
{
	kw_adopt();
	return request_threaded_irq(irq, fn, thread_fn, flags, name, arg);
}

const char *kwb_free_irq(int irq, void *arg)
{
	kw_adopt();
	return free_irq(irq, arg);
}

void kwb_disable_irq(int irq)
{
	kw_adopt();
	disable_irq(irq);
}

void kwb_disable_irq_nosync(int irq)
{
	kw_adopt();
	disable_irq_nosync(irq);
}

void kwb_enable_irq(int irq)
{
	kw_adopt();
	enable_irq(irq);
}

void kwb_synchronize_irq(int irq)
{
	kw_adopt();
	synchronize_irq(irq);
}

unsigned int kwb_irq_get_trigger_type(int irq)
{
	kw_adopt();
	return irq_get_trigger_type(irq);
}

int kwb_irq_set_irq_type(int irq, unsigned int type)
{
	kw_adopt();
	return irq_set_irq_type(irq, type);
}

int kwb_irq_set_irq_wake(int irq, unsigned int on)
{
	kw_adopt();
	return irq_set_irq_wake(irq, on);
}

int kwb_acpi_uuid_parse(const char *str, uint8_t out[16])
{
	kw_adopt();
	return acpi_uuid_parse(str, out);
}

int kwb_acpi_evaluate_dsm_typed(void *handle, const uint8_t uuid[16],
				uint64_t rev, uint64_t func,
				const struct acpi_value *arg, int type,
				struct acpi_value **out)
{
	kw_adopt();
	return acpi_evaluate_dsm_typed(handle, uuid, rev, func, arg,
				       (enum acpi_value_type)type, out);
}

void kwb_acpi_value_free(struct acpi_value *v)
{
	kw_adopt();
	acpi_value_free(v);
}

const struct acpi_device_id *
kwb_acpi_match_device_ids(const struct acpi_device *adev,
			  const struct acpi_device_id *ids)
{
	kw_adopt();
	return acpi_match_device_ids(adev, ids);
}

int kwb_acpi_evaluate_integer(void *handle, const char *method,
			      const struct acpi_value *args, int nargs,
			      uint64_t *val)
{
	kw_adopt();
	return acpi_evaluate_integer(handle, method, args, nargs, val);
}

int kwb_acpi_evaluate(void *handle, const char *method,
		      const struct acpi_value *args, int nargs,
		      struct acpi_value **out)
{
	kw_adopt();
	return acpi_evaluate(handle, method, args, nargs, out);
}

int kwb_acpi_device_set_power(struct acpi_device *adev, int state)
{
	kw_adopt();
	return acpi_device_set_power(adev, state);
}

struct acpi_device *kwb_acpi_dev_get(struct acpi_device *adev)
{
	kw_adopt();
	return acpi_dev_get(adev);
}

void kwb_acpi_dev_put(struct acpi_device *adev)
{
	kw_adopt();
	acpi_dev_put(adev);
}

const struct dmi_system_id *kwb_dmi_first_match(const struct dmi_system_id *l)
{
	kw_adopt();
	return dmi_first_match(l);
}

/* ==========================================================================
 * Machines
 * ========================================================================== */

static struct kw_cfg g_cfg;
static struct dw_model *g_m0, *g_m;
static struct mock_pci g_p0, g_p;
static struct i2c_adapter *g_adap;
static struct i2c_client *g_client;
static int g_gpio_irq;
static uint16_t g_desc_reg;
static union rslot g_crs[8];
static UINT16 g_pin[] = { 0x147 };
static volatile int g_ticker_run, g_ticker_on;

/* _DSM of the touchpad: function 1 of the HID-over-I2C UUID returns the
 * descriptor register. */
static ACPI_STATUS dsm_eval(struct mock_node *self, const ACPI_OBJECT_LIST *args,
			    ACPI_OBJECT *out)
{
	static const UINT8 guid[16] = { 0xf7, 0xf6, 0xdf, 0x3c, 0x67, 0x42,
					0x55, 0x45, 0xad, 0x05, 0xb3, 0x0a,
					0x3d, 0x89, 0x38, 0xde };

	(void)self;
	if (!args || args->Count != 4 ||
	    args->Pointer[0].Type != ACPI_TYPE_BUFFER ||
	    args->Pointer[0].Buffer.Length != 16 ||
	    kmemcmp(args->Pointer[0].Buffer.Pointer, guid, 16) ||
	    args->Pointer[1].Type != ACPI_TYPE_INTEGER ||
	    args->Pointer[1].Integer.Value != 1 ||
	    args->Pointer[2].Type != ACPI_TYPE_INTEGER)
		return AE_BAD_PARAMETER;
	if (g_cfg.dsm_mode == 1)
		return AE_ERROR;
	if (!out)
		return AE_OK;
	switch (args->Pointer[2].Integer.Value) {
	case 0: {
		static UINT8 funcs[1] = { 0x03 };

		mock_obj_buf(out, funcs, 1);
		return AE_OK;
	}
	case 1:
		mock_obj_int(out, g_cfg.dsm_mode == 2 ? 0x10001 : g_desc_reg);
		return AE_OK;
	default:
		return AE_BAD_PARAMETER;
	}
}

static void ticker(void *arg)
{
	(void)arg;
	while (__atomic_load_n(&g_ticker_run, __ATOMIC_ACQUIRE)) {
		hidm_tick();
		h_usleep(500);
	}
	__atomic_store_n(&g_ticker_on, 0, __ATOMIC_RELEASE);
}

static struct dw_model *lpss_model(void)
{
	struct dw_model *m = dwm_create(64, 64);

	m->lpss = 1;
	m->lpss_caps = 0x200; /* I2C, iDMA present */
	m->priv[0xfc / 4] = m->lpss_caps;
	return m;
}

static void touchpad_node(struct mock_node *bus, const char *name,
			  const char *hid, const char *bus_path, uint16_t addr,
			  uint32_t speed, int gsi)
{
	struct mock_node *tpd = mock_device(bus, name, hid, "1");
	int i = 0;

	mock_add_cid(tpd, "PNP0C50");
	mock_res_i2c(&g_crs[i++].r, addr, speed, 0, bus_path, 1);
	if (gsi >= 0) {
		static UINT32 gsis[1];

		gsis[0] = (UINT32)gsi;
		mock_res_extirq(&g_crs[i++].r, 1, ACPI_ACTIVE_LOW, 0, 0, gsis,
				1);
	} else {
		mock_res_gpio(&g_crs[i++].r, 1, 1, ACPI_ACTIVE_LOW, 0, 1,
			      ACPI_PIN_CONFIG_PULLUP, 0, 0, "\\_SB.GPI0", g_pin,
			      1);
	}
	mock_res_end(&g_crs[i++].r);
	(void)mock_resources_stride(tpd, "_CRS", g_crs, i,
				    (int)sizeof(union rslot));
	(void)mock_method(tpd, "_DSM", dsm_eval, NULL);
}

void kw_setup(const struct kw_cfg *cfg)
{
	struct mock_node *pc00, *i2c0, *i2c3;
	const char *hid;
	uint16_t addr;

	kw_adopt();
	T_ASSERT(!g_m);
	g_cfg = *cfg;
	mock_ns_reset();
	i2c_acpi_invalidate_cache();
	mock_pci_reset();
	pc00 = mock_device(NULL, "PC00", "PNP0A08", NULL);
	mock_add_cid(pc00, "PNP0A03");
	(void)mock_device(NULL, "GPI0", "INT34C5", NULL);
	kmemset(&g_mock_gpioint[0], 0, sizeof(g_mock_gpioint[0]));
	g_gpio_irq = 0;

	switch (cfg->machine) {
	case KW_HP:
	case KW_SIPODEV:
		hid = cfg->hid ? cfg->hid :
		      cfg->machine == KW_HP ? "ELAN073B" :
					     "SYNA3602";
		addr = cfg->machine == KW_HP ? 0x15 : 0x2c;
		g_desc_reg = cfg->machine == KW_HP ? 0x0001 : 0x0020;
		i2c0 = mock_device(pc00, "I2C0", NULL, NULL);
		mock_set_adr(i2c0, 0x00150000);
		touchpad_node(i2c0, "TPD0", hid, "\\_SB.PC00.I2C0", addr,
			      400000, -1);
		/* The GpioInt: its interrupt exists before the probe. */
		i2c_strlcpy(g_mock_gpioint[0].hid, hid,
			    sizeof(g_mock_gpioint[0].hid));
		g_mock_gpioint[0].defer = cfg->gpio_defer;
		g_mock_gpioint[0].wake = 1;
		g_gpio_irq = mock_irq_new(NULL);
		T_ASSERT(g_gpio_irq > 0);
		g_mock_gpioint[0].irq = g_gpio_irq;
		g_m = lpss_model();
		hidm_attach(g_m, addr, cfg, g_desc_reg, g_gpio_irq, -1);
		kmemset(&g_p, 0, sizeof(g_p));
		g_p.vendor = 0x8086;
		g_p.device_id = 0xa0e8; /* Tiger Lake-LP I2C0 */
		g_p.slot = 0x15;
		g_p.m = g_m;
		g_p.bar = (uint64_t)(unsigned long)g_m->window;
		mock_pci_add(&g_p);
		break;
	case KW_DELL:
		hid = cfg->hid ? cfg->hid : "VEN_0488";
		addr = 0x2c;
		g_desc_reg = 0x0020;
		i2c0 = mock_device(pc00, "I2C0", NULL, NULL);
		mock_set_adr(i2c0, 0x00150000);
		i2c3 = mock_device(pc00, "I2C3", NULL, NULL);
		mock_set_adr(i2c3, 0x00150003);
		touchpad_node(i2c3, "TPD0", hid, "\\_SB.PC00.I2C3", addr,
			      1000000, 27);
		mock_gsi_bind(27, NULL);
		mock_gsi_set_level(27, 0);
		g_m0 = lpss_model();
		g_m = lpss_model();
		hidm_attach(g_m, addr, cfg, g_desc_reg, -1, 27);
		kmemset(&g_p0, 0, sizeof(g_p0));
		kmemset(&g_p, 0, sizeof(g_p));
		g_p0.vendor = 0x8086;
		g_p0.device_id = 0x7778; /* Arrow Lake I2C0 */
		g_p0.slot = 0x15;
		g_p0.m = g_m0;
		g_p0.bar = (uint64_t)(unsigned long)g_m0->window;
		g_p0.msi = 1;
		g_p.vendor = 0x8086;
		g_p.device_id = 0x777b; /* Arrow Lake I2C3 */
		g_p.slot = 0x15;
		g_p.func = 3;
		g_p.m = g_m;
		g_p.bar = (uint64_t)(unsigned long)g_m->window;
		mock_pci_add(&g_p0);
		mock_pci_add(&g_p);
		break;
	default:
		T_ASSERT(0);
		return;
	}

	T_EQ(i2c_dw_pci_init(), 0);
	g_adap = i2c_find_adapter_by_acpi_handle(mock_lookup(
		cfg->machine == KW_DELL ? "\\_SB.PC00.I2C3" :
					  "\\_SB.PC00.I2C0"));
	T_ASSERT(g_adap);
	g_client = i2c_find_client(g_adap, addr);

	if (!__atomic_load_n(&g_ticker_on, __ATOMIC_ACQUIRE)) {
		__atomic_store_n(&g_ticker_run, 1, __ATOMIC_RELEASE);
		__atomic_store_n(&g_ticker_on, 1, __ATOMIC_RELEASE);
		T_ASSERT(sched_add_task(ticker, NULL, NULL, 0));
	}
}

void kw_teardown(void)
{
	kw_adopt();
	if (!g_m)
		return;
	if (g_client) {
		i2c_client_put(g_client);
		g_client = NULL;
	}
	if (g_adap) {
		i2c_put_adapter(g_adap);
		g_adap = NULL;
	}
	i2c_dw_pci_exit();
	hidm_detach();
	__atomic_store_n(&g_ticker_run, 0, __ATOMIC_RELEASE);
	while (__atomic_load_n(&g_ticker_on, __ATOMIC_ACQUIRE))
		h_usleep(200);
	if (g_gpio_irq > 0) {
		mock_irq_set_ext(g_gpio_irq, 0);
		irq_free_desc(g_gpio_irq);
		g_gpio_irq = 0;
	}
	mock_gsi_set_level(27, 0);
	dwm_destroy(g_m);
	g_m = NULL;
	if (g_m0) {
		dwm_destroy(g_m0);
		g_m0 = NULL;
	}
	mock_pci_reset();
	kmemset(&g_mock_gpioint[0], 0, sizeof(g_mock_gpioint[0]));
	i2c_acpi_invalidate_cache();
}

struct i2c_client *kw_client(void)
{
	return g_client;
}

int kw_irq(void)
{
	if (g_client && g_client->irq > 0)
		return g_client->irq;
	return g_gpio_irq;
}

void kw_gpio_arrives(void)
{
	kw_adopt();
	g_mock_gpioint[0].defer = 0;
	i2c_retry_deferred_probes();
}

int kw_suspend_all(void)
{
	kw_adopt();
	return i2c_pm_suspend_all();
}

int kw_resume_all(void)
{
	kw_adopt();
	return i2c_pm_resume_all();
}

void kw_shutdown_all(void)
{
	kw_adopt();
	i2c_shutdown_all();
}

long kw_warnings(void)
{
	return __atomic_load_n(&g_test_warnings, __ATOMIC_ACQUIRE);
}

unsigned long kw_irq_threaded(int irq)
{
	return mock_irq_threaded(irq);
}

static struct kw_pm_snap *g_snap;

static void kw_pm_gpio_suspend(void)
{
	struct dw_i2c_dev *dev = g_adap ? i2c_get_adapdata(g_adap) : NULL;
	struct kw_pm_snap *s = g_snap;

	if (!s || !dev)
		return;
	s->taken = 1;
	s->client_suspended = g_client ? g_client->suspended : -1;
	s->adapter_suspended = dev->sys_suspended;
	s->controller_off = dev->rpm_suspended && g_m->lpss &&
			    (g_m->priv[0x04 / 4] & 3) != 3;
	s->irq_depth = g_client ? mock_irq_depth(g_client->irq) : -1;
	s->irq_wake = g_client ? irq_is_wake_enabled(g_client->irq) : -1;
	s->device_power = hidm_get(HIDM_POWER);
	s->gpio_suspends = g_arb_pinctrl_suspends;
	s->gpio_resumes = g_arb_pinctrl_resumes;
}

int kw_suspend_test(unsigned int ms, struct kw_pm_snap *during)
{
	int ret;

	kw_adopt();
	kmemset(during, 0, sizeof(*during));
	g_snap = during;
	g_arb_pinctrl_suspend_hook = kw_pm_gpio_suspend;
	ret = i2c_pm_suspend_test(ms);
	g_arb_pinctrl_suspend_hook = NULL;
	g_snap = NULL;
	return ret;
}

unsigned long kw_gpio_resumes(void)
{
	return g_arb_pinctrl_resumes;
}

long kw_proc_i2c(char *buf, long cap)
{
	kw_adopt();
	return i2c_proc_show(NULL, buf, cap);
}

long kw_proc_i2c_write(const char *s)
{
	kw_adopt();
	return i2c_proc_store(NULL, s, (long)kstrlen(s));
}

int kw_irq_depth(int irq)
{
	return mock_irq_depth(irq);
}

int kw_irq_wake_enabled(int irq)
{
	kw_adopt();
	return irq_is_wake_enabled(irq);
}

unsigned int kw_irq_trigger(int irq)
{
	kw_adopt();
	return irq_get_trigger_type(irq);
}

int kw_irqs_in_use(void)
{
	return mock_irqs_in_use();
}

void kw_adapter_stats(unsigned long *xfers, unsigned long *nacks,
		      unsigned long *timeouts)
{
	*xfers = g_adap ? (unsigned long)g_adap->stats.xfers : 0;
	*nacks = g_adap ? (unsigned long)g_adap->stats.err_nack : 0;
	*timeouts = g_adap ? (unsigned long)g_adap->stats.err_timeout : 0;
}

int kw_client_power_state(void)
{
	kw_adopt();
	if (!g_client || !g_client->acpi_dev)
		return -1;
	return acpi_device_get_power(g_client->acpi_dev);
}

void kw_dmi_set(const char *vendor, const char *product)
{
	uint8_t t[256];
	size_t n = 0, lv = kstrlen(vendor), lp = kstrlen(product);

	kw_adopt();
	kmemset(t, 0, sizeof(t));
	if (lv || lp) {
		T_ASSERT(lv + lp + 0x1b + 16 < sizeof(t));
		/* Type 1, system information: manufacturer string 1,
		 * product name string 2. */
		t[0] = 1;
		t[1] = 0x1b;
		t[2] = 0x01;
		t[4] = lv ? 1 : 0;
		t[5] = lp ? (lv ? 2 : 1) : 0;
		n = 0x1b;
		kmemcpy(t + n, vendor, lv);
		n += lv;
		if (lv)
			t[n++] = 0;
		kmemcpy(t + n, product, lp);
		n += lp;
		if (lp)
			t[n++] = 0;
		t[n++] = 0; /* the string set's end */
		if (!lv && !lp)
			t[n++] = 0;
	}
	/* The end-of-table structure. */
	t[n] = 127;
	t[n + 1] = 4;
	n += 4;
	t[n++] = 0;
	t[n++] = 0;
	dmi_load_table(t, (uint32_t)n, 0);
}
