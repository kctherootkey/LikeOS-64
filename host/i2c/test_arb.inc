/* Host tests of the firmware bus arbitration (the IOSF sideband mailbox and
 * the P-Unit PMIC bus semaphore of Bay Trail / Cherry Trail, the AMD PSP
 * platform access mailbox and doorbell with the I2C arbitration protocol),
 * the refusal of adapters whose arbitration is unavailable, the DMI
 * matcher with fake SMBIOS tables and the DMI-keyed quirks of the I2C
 * path, the bus lock operations, and the suspend test chain.  Part of
 * host/i2c/test_i2c.c (its helpers), models in host/i2c/arb.c.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include "host/i2c/arb.h"
#include <kernel/hal/iosf_mbi.h>
#include <kernel/hal/amd_psp.h>
#include <kernel/hal/dmi.h>

/* The firmware's SMBIOS entry points (none: the tests load tables). */
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
 * SMBIOS tables
 * ========================================================================== */

struct smb {
	uint8_t t[2048];
	int n;
};

/* A structure: `f' the formatted area after the 4-byte header (len-4
 * bytes), then the strings (NULL-terminated list). */
static void smb_add(struct smb *s, uint8_t type, const uint8_t *f, int flen,
		    const char *const *strs)
{
	int start = s->n;

	s->t[s->n++] = type;
	s->t[s->n++] = (uint8_t)(flen + 4);
	s->t[s->n++] = (uint8_t)start;
	s->t[s->n++] = 0;
	kmemcpy(s->t + s->n, f, (size_t)flen);
	s->n += flen;
	if (!strs || !strs[0]) {
		s->t[s->n++] = 0;
		s->t[s->n++] = 0;
		return;
	}
	for (int i = 0; strs[i]; i++) {
		size_t l = kstrlen(strs[i]);

		kmemcpy(s->t + s->n, strs[i], l + 1);
		s->n += (int)l + 1;
	}
	s->t[s->n++] = 0;
	T_ASSERT(s->n < (int)sizeof(s->t) - 64);
}

static void smb_end(struct smb *s)
{
	smb_add(s, 127, NULL, 0, NULL);
}

static void dmi_none(void)
{
	struct smb s = { .n = 0 };

	smb_end(&s);
	dmi_set_version(0x030000);
	dmi_load_table(s.t, (uint32_t)s.n, 0);
}

/* System information only (vendor, product, version). */
static void dmi_system(const char *vendor, const char *product,
		       const char *version)
{
	struct smb s = { .n = 0 };
	uint8_t f1[0x1b - 4];
	const char *strs[] = { vendor, product, version, NULL };

	kmemset(f1, 0, sizeof(f1));
	f1[0] = 1;
	f1[1] = 2;
	f1[2] = 3;
	smb_add(&s, 1, f1, (int)sizeof(f1), strs);
	smb_end(&s);
	dmi_set_version(0x030000);
	dmi_load_table(s.t, (uint32_t)s.n, 0);
}

/* ==========================================================================
 * DMI matcher and modalias
 * ========================================================================== */

static void t_dmi(void)
{
	struct smb s = { .n = 0 };
	static const char *const bios[] = { "LENOVO", "N1EET89W (1.62 )",
					    "05/14/2021", NULL };
	static const char *const sys[] = { "LENOVO", "20EN0013GE",
					   "ThinkPad P50", "PF0ABCDE",
					   "LENOVO_MT_20EN", "ThinkPad P50",
					   NULL };
	static const char *const sys2[] = { "OTHER", "OTHER", NULL };
	static const char *const board[] = { "LENOVO", "20EN0013GE",
					     "SDK0J40697 WIN", "L1HF65X00XX",
					     "   ", NULL };
	static const char *const chassis[] = { "LENOVO", "None", "PF0ABCDE",
					       "No Asset Information", NULL };
	static const struct dmi_system_id ids[] = {
		{ .ident = "exact miss",
		  .matches = { DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "20EN") } },
		{ .ident = "absent field",
		  .matches = { DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			       DMI_MATCH(DMI_EC_FIRMWARE_RELEASE, "") } },
		{ .ident = "p50",
		  .matches = { DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			       DMI_MATCH(DMI_PRODUCT_VERSION, "P50"),
			       DMI_EXACT_MATCH(DMI_BOARD_NAME, "20EN0013GE"),
			       DMI_MATCH(DMI_BIOS_VERSION, "N1EET") } },
		{ 0 }
	};
	uint8_t f0[0x18 - 4], f1[0x1b - 4], f2[0x0f - 4], f3[0x15 - 4];
	char ma[512];
	long n;

	kmemset(f0, 0, sizeof(f0));
	f0[0] = 1; /* vendor */
	f0[1] = 2; /* version */
	f0[4] = 3; /* date */
	f0[0x14 - 4] = 1; /* release 1.62 */
	f0[0x15 - 4] = 62;
	f0[0x16 - 4] = 0xff; /* no EC release */
	f0[0x17 - 4] = 0xff;
	smb_add(&s, 0, f0, (int)sizeof(f0), bios);
	kmemset(f1, 0, sizeof(f1));
	f1[0] = 1;
	f1[1] = 2;
	f1[2] = 3;
	f1[3] = 4;
	{
		static const uint8_t uuid[16] = { 0x33, 0x22, 0x11, 0x00,
						  0x55, 0x44, 0x77, 0x66,
						  0x88, 0x99, 0xaa, 0xbb,
						  0xcc, 0xdd, 0xee, 0xff };

		kmemcpy(&f1[4], uuid, 16);
	}
	f1[0x19 - 4] = 5;
	f1[0x1a - 4] = 6;
	smb_add(&s, 1, f1, (int)sizeof(f1), sys);
	/* a second system structure: the first one wins */
	kmemset(f1, 0, sizeof(f1));
	f1[0] = 1;
	f1[1] = 2;
	smb_add(&s, 1, f1, (int)sizeof(f1), sys2);
	kmemset(f2, 0, sizeof(f2));
	f2[0] = 1;
	f2[1] = 2;
	f2[2] = 3;
	f2[3] = 4;
	f2[4] = 5; /* asset tag: blanks only */
	smb_add(&s, 2, f2, (int)sizeof(f2), board);
	kmemset(f3, 0, sizeof(f3));
	f3[0] = 1;
	f3[1] = 0x8a; /* lock bit + 10 (notebook) */
	f3[2] = 2;
	f3[3] = 3;
	f3[4] = 4;
	smb_add(&s, 3, f3, (int)sizeof(f3), chassis);
	smb_end(&s);

	dmi_set_version(0x030200);
	dmi_load_table(s.t, (uint32_t)s.n, 0);
	T_STR(dmi_get_system_info(DMI_BIOS_VERSION), "N1EET89W (1.62 )");
	T_STR(dmi_get_system_info(DMI_BIOS_RELEASE), "1.62");
	T_ASSERT(!dmi_field_present(DMI_EC_FIRMWARE_RELEASE));
	T_STR(dmi_get_system_info(DMI_EC_FIRMWARE_RELEASE), "");
	T_STR(dmi_get_system_info(DMI_SYS_VENDOR), "LENOVO");
	T_STR(dmi_get_system_info(DMI_PRODUCT_SERIAL), "PF0ABCDE");
	T_STR(dmi_get_system_info(DMI_PRODUCT_UUID),
	      "00112233-4455-6677-8899-aabbccddeeff");
	T_STR(dmi_get_system_info(DMI_PRODUCT_SKU), "LENOVO_MT_20EN");
	T_STR(dmi_get_system_info(DMI_BOARD_SERIAL), "L1HF65X00XX");
	T_ASSERT(dmi_field_present(DMI_BOARD_ASSET_TAG));
	T_STR(dmi_get_system_info(DMI_BOARD_ASSET_TAG), "");
	T_STR(dmi_get_system_info(DMI_CHASSIS_TYPE), "10");
	T_STR(dmi_get_system_info(DMI_CHASSIS_VERSION), "None");
	T_STR(dmi_get_system_info(DMI_CHASSIS_ASSET_TAG),
	      "No Asset Information");
	T_STR(dmi_first_match(ids)->ident, "p50");

	n = dmi_get_modalias(ma, sizeof(ma));
	T_STR(ma,
	      "dmi:bvnLENOVO:bvrN1EET89W(1.62):bd05/14/2021:br1.62:svnLENOVO:pn20EN0013GE:pvrThinkPadP50:rvnLENOVO:rn20EN0013GE:rvrSDK0J40697WIN:cvnLENOVO:ct10:cvrNone:skuLENOVO_MT_20EN:pfaThinkPadP50:");
	T_EQ(n, (long)kstrlen(ma));
	/* a short buffer: terminated, the full length reported */
	T_EQ(dmi_get_modalias(ma, 8), n);
	T_STR(ma, "dmi:bvn");
	/* SMBIOS before 2.6: the UUID in the order stored */
	dmi_set_version(0x020500);
	dmi_load_table(s.t, (uint32_t)s.n, 0);
	T_STR(dmi_get_system_info(DMI_PRODUCT_UUID),
	      "33221100-5544-7766-8899-aabbccddeeff");
	/* a structure count stops before the end-of-table structure */
	dmi_load_table(s.t, (uint32_t)s.n, 1);
	T_ASSERT(dmi_field_present(DMI_BIOS_VENDOR));
	T_ASSERT(!dmi_field_present(DMI_SYS_VENDOR));
	/* a table cut inside a string set is not decoded past it */
	dmi_load_table(s.t, 12, 0);
	T_ASSERT(!dmi_field_present(DMI_BIOS_VENDOR));
	/* no SMBIOS: nothing matches, the modalias is "dmi:" */
	dmi_none();
	T_ASSERT(!dmi_first_match(ids));
	dmi_get_modalias(ma, sizeof(ma));
	T_STR(ma, "dmi:");
}

/* ==========================================================================
 * IOSF sideband and the P-Unit semaphore
 * ========================================================================== */

static void iosf_machine(uint16_t device, uint32_t sem)
{
	arb_iosf_reset();
	arb_set_root_complex(0x8086, device);
	g_arb_iosf.sem_reg = sem;
	T_EQ(iosf_mbi_init(), 0);
}

static unsigned long g_nb_begin, g_nb_end;

static int nb_fn(struct iosf_mbi_notifier *nb, unsigned long action)
{
	(void)nb;
	if (action == MBI_PMIC_BUS_ACCESS_BEGIN)
		g_nb_begin++;
	else if (action == MBI_PMIC_BUS_ACCESS_END)
		g_nb_end++;
	return 0;
}

static volatile int g_punit_state;

static void punit_holder(void *arg)
{
	(void)arg;
	iosf_mbi_punit_acquire();
	g_punit_state = 1;
	h_usleep(30000);
	g_punit_state = 2;
	iosf_mbi_punit_release();
}

static void t_iosf(void)
{
	struct iosf_mbi_notifier nb = { .fn = nb_fn };
	struct iosf_mbi_stats st;
	uint32_t v;
	int w0;

	/* no host bridge: nothing */
	arb_iosf_reset();
	T_EQ(iosf_mbi_init(), -ENODEV);
	T_ASSERT(!iosf_mbi_available());
	T_EQ(iosf_mbi_read(BT_MBI_UNIT_BUNIT, MBI_REG_READ, 0x10, &v),
	     -ENODEV);
	/* an unrelated host bridge */
	arb_set_root_complex(0x8086, 0x9b61);
	T_EQ(iosf_mbi_init(), -ENODEV);

	/* Bay Trail: the mailbox, a register beyond 8 offset bits */
	iosf_machine(PCI_DEVICE_ID_INTEL_BAYTRAIL, PUNIT_SEMAPHORE_BYT);
	T_ASSERT(iosf_mbi_available());
	T_ASSERT(iosf_mbi_punit_semaphore_available());
	T_EQ(iosf_mbi_write(BT_MBI_UNIT_BUNIT, MBI_REG_WRITE, 0x1a4,
			    0x12345678),
	     0);
	T_EQ(g_arb_iosf.regs[BT_MBI_UNIT_BUNIT][0x1a4], 0x12345678);
	T_EQ(iosf_mbi_read(BT_MBI_UNIT_BUNIT, MBI_REG_READ, 0x1a4, &v), 0);
	T_EQ(v, 0x12345678);
	T_EQ(iosf_mbi_modify(BT_MBI_UNIT_BUNIT, MBI_REG_READ, 0x1a4, 0xff,
			     0x0f0f),
	     0);
	T_EQ(g_arb_iosf.regs[BT_MBI_UNIT_BUNIT][0x1a4], 0x1234507f);
	/* the GPU's port is not ours */
	w0 = g_test_warnings;
	T_EQ(iosf_mbi_read(BT_MBI_UNIT_GFX, MBI_REG_READ, 0, &v), -EPERM);
	EXPECT_WARNS(w0, 1);
	/* config space failing */
	g_arb_iosf.cfg_fail = 1;
	T_EQ(iosf_mbi_read(BT_MBI_UNIT_BUNIT, MBI_REG_READ, 0x10, &v), -EIO);
	g_arb_iosf.cfg_fail = 0;

	/* the semaphore: granted after a few polls, nested users share
	 * one grant, the notifiers and the C-state constraint bracket the
	 * outermost period */
	T_EQ(iosf_mbi_register_pmic_bus_access_notifier(&nb), 0);
	g_nb_begin = g_nb_end = 0;
	g_arb_iosf.grant_after = 3;
	T_ASSERT(iosf_mbi_deep_cstates_allowed());
	T_EQ(iosf_mbi_block_punit_i2c_access(), 0);
	T_EQ(g_arb_iosf.granted, 1);
	T_EQ(iosf_mbi_punit_semaphore_held(), 1);
	T_ASSERT(!iosf_mbi_deep_cstates_allowed());
	T_EQ(iosf_mbi_block_punit_i2c_access(), 0);
	T_EQ(g_arb_iosf.requests, 1);
	iosf_mbi_unblock_punit_i2c_access();
	T_EQ(g_arb_iosf.granted, 1);
	iosf_mbi_unblock_punit_i2c_access();
	T_EQ(g_arb_iosf.granted, 0);
	T_EQ(g_arb_iosf.releases, 1);
	T_EQ(iosf_mbi_punit_semaphore_held(), 0);
	T_ASSERT(iosf_mbi_deep_cstates_allowed());
	T_EQ(g_nb_begin, 1);
	T_EQ(g_nb_end, 1);
	iosf_mbi_get_stats(&st);
	T_ASSERT(st.acquires >= 1 && st.nested >= 1 && st.releases >= 1);
	/* the firmware still uses the bus for a while: the OS waits */
	g_arb_iosf.fw_hold_polls = 20;
	T_EQ(iosf_mbi_block_punit_i2c_access(), 0);
	T_EQ(g_arb_iosf.fw_hold_polls, 0);
	iosf_mbi_unblock_punit_i2c_access();
	/* never granted: -ETIMEDOUT after 500 ms, the request withdrawn,
	 * the constraint and the notifiers balanced */
	g_arb_iosf.never_grant = 1;
	T_EQ(iosf_mbi_block_punit_i2c_access(), -ETIMEDOUT);
	T_EQ(g_arb_iosf.requested, 0);
	T_ASSERT(iosf_mbi_deep_cstates_allowed());
	T_EQ(g_nb_begin, g_nb_end);
	g_arb_iosf.never_grant = 0;
	/* a mailbox failing: -EIO, nothing held */
	g_arb_iosf.cfg_fail = 1;
	T_ASSERT(iosf_mbi_block_punit_i2c_access() < 0);
	g_arb_iosf.cfg_fail = 0;
	T_ASSERT(iosf_mbi_deep_cstates_allowed());
	/* P-Unit requests and OS blocks exclude each other */
	g_punit_state = 0;
	T_ASSERT(sched_add_task(punit_holder, NULL, NULL, 0));
	while (!g_punit_state)
		h_usleep(1000);
	T_EQ(iosf_mbi_block_punit_i2c_access(), 0);
	T_EQ(g_punit_state, 2); /* waited for the P-Unit request */
	iosf_mbi_unblock_punit_i2c_access();
	T_EQ(iosf_mbi_unregister_pmic_bus_access_notifier(&nb), 0);
	T_EQ(iosf_mbi_unregister_pmic_bus_access_notifier(&nb), -ENOENT);

	/* Cherry Trail: the semaphore at 0x10e (the extended offset) */
	iosf_machine(PCI_DEVICE_ID_INTEL_BRASWELL, PUNIT_SEMAPHORE_CHT);
	T_EQ(iosf_mbi_block_punit_i2c_access(), 0);
	T_EQ(g_arb_iosf.granted, 1);
	iosf_mbi_unblock_punit_i2c_access();
	T_EQ(g_arb_iosf.releases, 1);
	/* a P-Unit with its semaphore elsewhere never grants the Cherry
	 * Trail address */
	iosf_machine(PCI_DEVICE_ID_INTEL_BRASWELL, PUNIT_SEMAPHORE_BYT);
	T_EQ(iosf_mbi_block_punit_i2c_access(), -ETIMEDOUT);
	T_EQ(g_arb_iosf.grants, 0);

	/* Quark: a mailbox without a semaphore */
	iosf_machine(PCI_DEVICE_ID_INTEL_QUARK_X1000, 0);
	T_ASSERT(iosf_mbi_available());
	T_ASSERT(!iosf_mbi_punit_semaphore_available());
	w0 = g_test_warnings;
	T_EQ(iosf_mbi_block_punit_i2c_access(), -ENXIO);
	EXPECT_WARNS(w0, 1);
	arb_iosf_reset();
}

/* ==========================================================================
 * Bay Trail / Cherry Trail adapters with _SEM
 * ========================================================================== */

static union rslot g_arb_crs[4][4], g_arb_tp[4][4];

/* An ACPI controller `hid' at `m' (polled), with _SEM = sem (-1: none). */
static struct mock_node *arb_ctrl(struct dw_model *m, const char *name,
				  const char *hid, int slot, int sem)
{
	struct mock_node *d = mock_device(NULL, name, hid, "1");
	int i = 0;

	mock_res_addr64(&g_arb_crs[slot][i++].r,
			(UINT64)(unsigned long)m->window,
			(UINT64)(unsigned long)m->window + 0xfff, 0x1000, 0, 0);
	mock_res_end(&g_arb_crs[slot][i++].r);
	(void)mock_resources_stride(d, "_CRS", g_arb_crs[slot], i,
				    (int)sizeof(union rslot));
	if (sem >= 0)
		(void)mock_int(d, "_SEM", (UINT64)sem);
	return d;
}

/* A client of `ctrl' at 0x50 (its ACPI id `hid'). */
static struct mock_node *arb_client(struct mock_node *ctrl, int slot,
				    const char *name, const char *hid,
				    const char *path)
{
	struct mock_node *c = mock_device(ctrl, name, hid, NULL);
	int i = 0;

	mock_res_i2c(&g_arb_tp[slot][i++].r, 0x50, 400000, 0, path, 1);
	mock_res_end(&g_arb_tp[slot][i++].r);
	(void)mock_resources_stride(c, "_CRS", g_arb_tp[slot], i,
				    (int)sizeof(union rslot));
	return c;
}

static void t_byt_sem(void)
{
	struct dw_model *m = model_std(32);
	struct mock_node *ctrl;
	struct i2c_adapter *a;
	struct dw_i2c_dev *dev;
	uint8_t w[2] = { 0x10, 0x5a }, r[1];
	char *buf;
	long len;

	/* the PMIC bus with the mailbox: arbitrated, every transfer
	 * inside a granted period */
	mock_ns_reset();
	i2c_acpi_invalidate_cache();
	iosf_machine(PCI_DEVICE_ID_INTEL_BAYTRAIL, PUNIT_SEMAPHORE_BYT);
	g_arb_iosf.grant_after = 2;
	ctrl = arb_ctrl(m, "I2C7", "80860F41", 0, 1);
	arb_watch_bus(m, 1);
	T_EQ(i2c_dw_platform_init(), 1);
	a = i2c_find_adapter_by_acpi_handle(ctrl);
	T_ASSERT(a);
	dev = i2c_get_adapdata(a);
	T_ASSERT(dev->shared_with_punit);
	T_ASSERT(dev->acquire_lock && dev->release_lock);
	T_ASSERT(!dev->rpm_enabled); /* the P-Unit wants it powered */
	for (int i = 0; i < 20; i++) {
		T_EQ(wr(a, 0x50, 0, w, 2, NULL, 0), 1);
		T_EQ(wr(a, 0x50, 0, w, 1, r, 1), 2);
		T_EQ(r[0], 0x5a);
	}
	T_EQ(g_arb_iosf.violations, 0);
	T_ASSERT(g_arb_iosf.grants >= 40);
	T_EQ(g_arb_iosf.granted, 0); /* released between transfers */
	T_ASSERT(iosf_mbi_deep_cstates_allowed());
	/* a client's multi-transfer sequence under the bus lock */
	buf = kcalloc(1, 8192);
	len = i2c_core_show(buf, 8192);
	T_ASSERT(len > 0 && str_has(buf, "punit semaphore free"));
	kfree(buf);
	/* the firmware busy for a while: the transfer waits for it */
	g_arb_iosf.fw_hold_polls = 10;
	T_EQ(wr(a, 0x50, 0, w, 1, r, 1), 2);
	T_EQ(g_arb_iosf.violations, 0);
	/* the P-Unit stops answering: the transfer fails, nothing goes
	 * on the bus */
	g_arb_iosf.never_grant = 1;
	dwm_log_clear(m);
	T_ASSERT(wr(a, 0x50, 0, w, 1, r, 1) < 0);
	T_STR(bus_log(m), "");
	g_arb_iosf.never_grant = 0;
	T_EQ(wr(a, 0x50, 0, w, 1, r, 1), 2);
	i2c_put_adapter(a);
	/* system suspend/resume of a shared bus: no power-down, re-init */
	T_EQ(i2c_pm_suspend_all(), 0);
	T_EQ(i2c_pm_resume_all(), 0);
	T_EQ(wr(a, 0x50, 0, w, 1, r, 1), 2);
	T_EQ(g_arb_iosf.violations, 0);
	i2c_dw_platform_exit();
	arb_watch_bus(m, 0);

	/* _SEM but no mailbox: refused, the controller left alone */
	mock_ns_reset();
	i2c_acpi_invalidate_cache();
	arb_iosf_reset();
	ctrl = arb_ctrl(m, "I2C7", "80860F41", 0, 1);
	T_EQ(i2c_dw_platform_init(), 0);
	T_ASSERT(!i2c_find_adapter_by_acpi_handle(ctrl));
	/* a P-Unit that never grants: refused */
	iosf_machine(PCI_DEVICE_ID_INTEL_BAYTRAIL, PUNIT_SEMAPHORE_BYT);
	g_arb_iosf.never_grant = 1;
	T_EQ(i2c_dw_platform_init(), 0);
	T_ASSERT(!i2c_find_adapter_by_acpi_handle(ctrl));
	T_ASSERT(iosf_mbi_deep_cstates_allowed());
	/* Cherry Trail with _SEM = 1: arbitrated at its own address */
	mock_ns_reset();
	i2c_acpi_invalidate_cache();
	iosf_machine(PCI_DEVICE_ID_INTEL_BRASWELL, PUNIT_SEMAPHORE_CHT);
	ctrl = arb_ctrl(m, "I2C7", "808622C1", 0, 1);
	arb_watch_bus(m, 1);
	T_EQ(i2c_dw_platform_init(), 1);
	a = i2c_find_adapter_by_acpi_handle(ctrl);
	T_ASSERT(a);
	T_EQ(wr(a, 0x50, 0, w, 1, r, 1), 2);
	T_EQ(g_arb_iosf.violations, 0);
	i2c_put_adapter(a);
	i2c_dw_platform_exit();
	arb_watch_bus(m, 0);
	/* _SEM = 0 (a bus of the OS alone): no semaphore, as before */
	mock_ns_reset();
	i2c_acpi_invalidate_cache();
	ctrl = arb_ctrl(m, "I2C7", "80860F41", 0, 0);
	T_EQ(i2c_dw_platform_init(), 1);
	a = i2c_find_adapter_by_acpi_handle(ctrl);
	T_ASSERT(a);
	dev = i2c_get_adapdata(a);
	T_ASSERT(!dev->acquire_lock && !dev->shared_with_punit);
	T_ASSERT(dev->rpm_enabled);
	T_EQ(wr(a, 0x50, 0, w, 1, r, 1), 2);
	i2c_put_adapter(a);
	i2c_dw_platform_exit();
	arb_iosf_reset();
	dwm_destroy(m);
	i2c_acpi_invalidate_cache();
}

/* ==========================================================================
 * The AMD PSP
 * ========================================================================== */

static void psp_machine(uint16_t psp_device, int doorbell)
{
	const struct amd_psp_vdata *vd = amd_psp_lookup_vdata(psp_device);

	T_ASSERT(vd);
	arb_psp_reset();
	g_arb_psp.doorbell = doorbell;
	T_EQ(amd_psp_attach(g_arb_psp.window, vd, "psp-test"), 0);
}

struct t_req {
	struct psp_req_buffer_hdr hdr;
	uint32_t type;
} __attribute__((packed));

static void t_psp(void)
{
	struct t_req req;
	uint32_t result = 0;

	/* the device ids: only generations with platform access */
	T_ASSERT(amd_psp_lookup_vdata(0x1649));
	T_ASSERT(amd_psp_lookup_vdata(0x17E0));
	T_ASSERT(!amd_psp_lookup_vdata(0x14CA));
	T_ASSERT(!amd_psp_lookup_vdata(0x1537));
	/* a hidden PSP (all-ones) is not used */
	arb_psp_reset();
	g_arb_psp.feature = 0xffffffffu;
	T_EQ(amd_psp_attach(g_arb_psp.window, amd_psp_lookup_vdata(0x15DF),
			    "hidden"),
	     -EIO);
	T_EQ(psp_check_platform_access_status(), -ENODEV);

	/* mailbox */
	psp_machine(0x15DF, 0);
	T_EQ(psp_check_platform_access_status(), 0);
	kmemset(&req, 0, sizeof(req));
	req.hdr.payload_size = sizeof(req);
	req.type = 0;
	T_EQ(psp_send_platform_access_msg(PSP_I2C_REQ_BUS_CMD,
					  (struct psp_request *)&req),
	     0);
	T_EQ(g_arb_psp.held, 1);
	req.type = 1;
	T_EQ(psp_send_platform_access_msg(PSP_I2C_REQ_BUS_CMD,
					  (struct psp_request *)&req),
	     0);
	T_EQ(g_arb_psp.held, 0);
	/* busy: the request ran, its status says busy */
	g_arb_psp.busy = 1;
	req.type = 0;
	T_EQ(psp_send_platform_access_msg(PSP_I2C_REQ_BUS_CMD,
					  (struct psp_request *)&req),
	     -EIO);
	T_EQ(req.hdr.status, 1);
	/* a status in the command register */
	g_arb_psp.mode = PSP_MODE_CMD_STS_ERR;
	T_EQ(psp_send_platform_access_msg(PSP_I2C_REQ_BUS_CMD,
					  (struct psp_request *)&req),
	     -EIO);
	T_EQ(req.hdr.status, 5);
	/* in recovery: busy without sending */
	g_arb_psp.mode = PSP_MODE_RECOVERY;
	T_EQ(psp_send_platform_access_msg(PSP_I2C_REQ_BUS_CMD,
					  (struct psp_request *)&req),
	     -EBUSY);
	/* stuck: the answer never comes; the next command finds the
	 * mailbox busy */
	g_arb_psp.mode = PSP_MODE_STUCK;
	T_EQ(psp_send_platform_access_msg(PSP_I2C_REQ_BUS_CMD,
					  (struct psp_request *)&req),
	     -ETIMEDOUT);
	T_EQ(psp_send_platform_access_msg(PSP_I2C_REQ_BUS_CMD,
					  (struct psp_request *)&req),
	     -EBUSY);
	/* bad sizes */
	req.hdr.payload_size = 4;
	T_EQ(psp_send_platform_access_msg(PSP_I2C_REQ_BUS_CMD,
					  (struct psp_request *)&req),
	     -EINVAL);

	/* doorbell (no mailbox on this generation) */
	psp_machine(0x17E0, 1);
	req.hdr.payload_size = sizeof(req);
	T_EQ(psp_send_platform_access_msg(PSP_I2C_REQ_BUS_CMD,
					  (struct psp_request *)&req),
	     -ENODEV);
	T_EQ(psp_ring_platform_doorbell(0, &result), 0);
	T_EQ(g_arb_psp.held, 1);
	T_EQ(psp_ring_platform_doorbell(1, &result), 0);
	T_EQ(g_arb_psp.held, 0);
	g_arb_psp.busy = 1;
	T_EQ(psp_ring_platform_doorbell(0, &result), -EIO);
	T_EQ(result, 1);
	g_arb_psp.mode = PSP_MODE_STUCK;
	T_EQ(psp_ring_platform_doorbell(0, &result), -ETIMEDOUT);
	T_EQ(psp_ring_platform_doorbell(0, &result), -EBUSY);
	arb_psp_reset();
}

static void t_amd_psp_bus(void)
{
	struct dw_model *m = model_std(32), *m2 = model_std(32);
	struct mock_node *ctrl, *ctrl2;
	struct i2c_adapter *a;
	struct dw_i2c_dev *dev;
	uint8_t w[2] = { 0x10, 0x77 }, r[1];
	char *buf;
	unsigned long g0;

	/* Cezanne (root complex 1022:1630): the mailbox */
	mock_ns_reset();
	i2c_acpi_invalidate_cache();
	arb_iosf_reset();
	arb_set_root_complex(0x1022, 0x1630);
	psp_machine(0x15DF, 0);
	ctrl = arb_ctrl(m, "I2CB", "AMDI0019", 0, -1);
	arb_watch_bus(m, 2);
	T_EQ(i2c_dw_platform_init(), 1);
	a = i2c_find_adapter_by_acpi_handle(ctrl);
	T_ASSERT(a);
	dev = i2c_get_adapdata(a);
	T_EQ(dev->clk_khz, 150000);
	T_ASSERT(a->lock_ops && dev->acquire_lock);
	T_EQ(wr(a, 0x50, 0, w, 2, NULL, 0), 1);
	T_EQ(wr(a, 0x50, 0, w, 1, r, 1), 2);
	T_EQ(r[0], 0x77);
	T_EQ(g_arb_psp.violations, 0);
	T_EQ(g_arb_psp.last_type, 0);
	/* the reservation: transfers within it ask nothing */
	g0 = g_arb_psp.requests;
	T_EQ(wr(a, 0x50, 0, w, 1, r, 1), 2);
	T_EQ(g_arb_psp.requests, g0);
	/* ... and the bus goes back when it ends */
	wait_ms(250);
	T_EQ(g_arb_psp.held, 0);
	T_EQ(g_arb_psp.last_type, 1);
	/* a client holding the bus lock over several transfers keeps the
	 * PSP out past the reservation */
	i2c_lock_bus(a, I2C_LOCK_SEGMENT);
	wait_ms(150);
	T_EQ(g_arb_psp.held, 1);
	{
		struct i2c_msg msg = { .addr = 0x50, .flags = 0, .len = 1,
				       .buf = w };

		T_EQ(__i2c_transfer(a, &msg, 1), 1);
	}
	i2c_unlock_bus(a, I2C_LOCK_SEGMENT);
	T_EQ(g_arb_psp.held, 0); /* the last release gives it back */
	/* the PSP busy for a while: retried until granted */
	g_arb_psp.busy = 3;
	T_EQ(wr(a, 0x50, 0, w, 1, r, 1), 2);
	T_EQ(g_arb_psp.busy_answers, 3);
	T_EQ(g_arb_psp.violations, 0);
	buf = kcalloc(1, 8192);
	(void)i2c_core_show(buf, 8192);
	T_ASSERT(str_has(buf, "psp arbitration"));
	kfree(buf);
	wait_ms(250);
	/* a second PSP-arbitrated bus: refused (one per platform) */
	ctrl2 = arb_ctrl(m2, "I2CC", "AMDI0019", 1, -1);
	T_EQ(i2c_dw_platform_init(), 0); /* -EEXIST for both */
	T_ASSERT(!i2c_find_adapter_by_acpi_handle(ctrl2));
	(void)ctrl2;
	i2c_put_adapter(a);
	i2c_dw_platform_exit();
	T_EQ(g_arb_psp.held, 0);

	/* Mendocino and later (any other root complex): the doorbell */
	mock_ns_reset();
	i2c_acpi_invalidate_cache();
	arb_set_root_complex(0x1022, 0x14e8);
	psp_machine(0x17E0, 1);
	ctrl = arb_ctrl(m, "I2CB", "AMDI0019", 0, -1);
	T_EQ(i2c_dw_platform_init(), 1);
	a = i2c_find_adapter_by_acpi_handle(ctrl);
	T_ASSERT(a);
	g_arb_psp.busy = 2;
	T_EQ(wr(a, 0x50, 0, w, 1, r, 1), 2);
	T_EQ(g_arb_psp.violations, 0);
	i2c_put_adapter(a);
	i2c_dw_platform_exit();
	T_EQ(g_arb_psp.held, 0);

	/* no PSP: refused */
	mock_ns_reset();
	i2c_acpi_invalidate_cache();
	arb_psp_reset();
	ctrl = arb_ctrl(m, "I2CB", "AMDI0019", 0, -1);
	T_EQ(i2c_dw_platform_init(), 0);
	T_ASSERT(!i2c_find_adapter_by_acpi_handle(ctrl));
	/* a PSP that does not answer: refused */
	arb_set_root_complex(0x1022, 0x1630);
	psp_machine(0x15DF, 0);
	g_arb_psp.mode = PSP_MODE_STUCK;
	T_EQ(i2c_dw_platform_init(), 0);
	T_ASSERT(!i2c_find_adapter_by_acpi_handle(ctrl));
	/* busy for longer than the request may be retried: refused */
	psp_machine(0x15DF, 0);
	g_arb_psp.busy = 1000;
	T_EQ(i2c_dw_platform_init(), 0);
	T_ASSERT(!i2c_find_adapter_by_acpi_handle(ctrl));
	/* an error status: refused */
	psp_machine(0x15DF, 0);
	g_arb_psp.mode = PSP_MODE_STATUS_ERR;
	T_EQ(i2c_dw_platform_init(), 0);
	T_ASSERT(!i2c_find_adapter_by_acpi_handle(ctrl));
	/* the doorbell layout on a Cezanne root complex (no mailbox):
	 * refused */
	psp_machine(0x17E0, 1);
	T_EQ(i2c_dw_platform_init(), 0);
	T_ASSERT(!i2c_find_adapter_by_acpi_handle(ctrl));
	arb_watch_bus(m, 0);
	/* and a plain AMDI0010 next to it is not affected */
	mock_ns_reset();
	i2c_acpi_invalidate_cache();
	arb_psp_reset();
	ctrl = arb_ctrl(m, "I2CA", "AMDI0010", 0, -1);
	T_EQ(i2c_dw_platform_init(), 1);
	a = i2c_find_adapter_by_acpi_handle(ctrl);
	T_ASSERT(a && !a->lock_ops);
	T_EQ(wr(a, 0x50, 0, w, 1, r, 1), 2);
	i2c_put_adapter(a);
	i2c_dw_platform_exit();
	arb_set_root_complex(0, 0);
	arb_psp_reset();
	dwm_destroy(m);
	dwm_destroy(m2);
	i2c_acpi_invalidate_cache();
}

/* ==========================================================================
 * DMI quirks of the I2C path
 * ========================================================================== */

static ACPI_STATUS eval_counts(struct mock_node *self,
			       const ACPI_OBJECT_LIST *args, ACPI_OBJECT *out)
{
	static ACPI_OBJECT e[3];

	(void)self;
	(void)args;
	mock_obj_int(&e[0], 0x111);
	mock_obj_int(&e[1], 0x222);
	mock_obj_int(&e[2], 0x33);
	if (out)
		mock_obj_pkg(out, e, 3);
	return AE_OK;
}

static void t_dmi_quirks(void)
{
	struct dw_model *m = model_std(32);
	struct mock_node *ctrl, *bogus, *pmic;
	struct acpi_device *adev;
	struct dw_i2c_dev *dev;
	struct i2c_adapter *a;
	struct i2c_client *c;

	/* ACPI counts used ... */
	mock_ns_reset();
	i2c_acpi_invalidate_cache();
	dmi_none();
	ctrl = mock_device(NULL, "BUSQ", "INT3446", "1");
	(void)mock_method(ctrl, "FMCN", eval_counts, NULL);
	adev = acpi_dev_from_handle(ctrl);
	dev = dw_new_ex(m, 120000, 400000, 1, adev);
	T_EQ(dev->fs_hcnt, 0x111);
	T_EQ(dev->fs_lcnt, 0x222);
	T_EQ(dev->sda_hold_time & 0xffff, 0x33);
	dw_del(dev);
	/* ... except on the Dell Inspiron 7348: computed from the clock */
	dmi_system("Dell Inc.", "Inspiron 7348", "A00");
	dev = dw_new_ex(m, 120000, 400000, 1, adev);
	T_ASSERT(dev->fs_hcnt != 0x111 && dev->fs_hcnt > 0);
	T_ASSERT(dev->fs_lcnt != 0x222 && dev->fs_lcnt > 0);
	dw_del(dev);
	acpi_dev_put(adev);

	/* adapters of the Qtechnology QT5222 carry hardware monitors */
	mock_ns_reset();
	i2c_acpi_invalidate_cache();
	dmi_system("Qtechnology", "QT5222", "1");
	ctrl = arb_ctrl(m, "I2CA", "AMDI0010", 0, -1);
	T_EQ(i2c_dw_platform_init(), 1);
	a = i2c_find_adapter_by_acpi_handle(ctrl);
	T_ASSERT(a);
	T_EQ(a->class, I2C_CLASS_HWMON);
	i2c_put_adapter(a);
	i2c_dw_platform_exit();
	dmi_none();
	T_EQ(i2c_dw_platform_init(), 1);
	a = i2c_find_adapter_by_acpi_handle(ctrl);
	T_EQ(a->class, I2C_CLASS_DEPRECATED);
	i2c_put_adapter(a);
	i2c_dw_platform_exit();

	/* Android-only tablets: the bogus ACPI clients are skipped, the
	 * known good ones (a PMIC) still made */
	mock_ns_reset();
	i2c_acpi_invalidate_cache();
	ctrl = arb_ctrl(m, "I2C7", "80860F41", 0, -1);
	bogus = arb_client(ctrl, 0, "TCH0", "BOGU0001", "\\_SB.I2C7");
	pmic = arb_client(ctrl, 1, "PMI0", "INT33F4", "\\_SB.I2C7");
	(void)bogus;
	(void)pmic;
	dmi_system("Insyde", "VESPA2", "1");
	T_EQ(i2c_dw_platform_init(), 1);
	a = i2c_find_adapter_by_acpi_handle(ctrl);
	T_ASSERT(a);
	c = i2c_find_client(a, 0x50);
	T_ASSERT(c);
	T_STR(c->name, "INT33F4:00");
	i2c_client_put(c);
	i2c_put_adapter(a);
	i2c_dw_platform_exit();
	/* elsewhere the first device listed gets the address */
	dmi_none();
	i2c_acpi_invalidate_cache();
	T_EQ(i2c_dw_platform_init(), 1);
	a = i2c_find_adapter_by_acpi_handle(ctrl);
	c = i2c_find_client(a, 0x50);
	T_ASSERT(c);
	T_STR(c->name, "BOGU0001:00");
	i2c_client_put(c);
	i2c_put_adapter(a);
	i2c_dw_platform_exit();
	dwm_destroy(m);
	i2c_acpi_invalidate_cache();
}

/* ==========================================================================
 * Bus lock operations
 * ========================================================================== */

static int g_lo_lock, g_lo_try, g_lo_unlock;

static void lo_lock(struct i2c_adapter *a, unsigned int f)
{
	g_lo_lock++;
	i2c_adapter_lock_bus(a, f);
}

static int lo_trylock(struct i2c_adapter *a, unsigned int f)
{
	g_lo_try++;
	return i2c_adapter_trylock_bus(a, f);
}

static void lo_unlock(struct i2c_adapter *a, unsigned int f)
{
	g_lo_unlock++;
	i2c_adapter_unlock_bus(a, f);
}

static const struct i2c_lock_operations g_lo_ops = {
	.lock_bus = lo_lock,
	.trylock_bus = lo_trylock,
	.unlock_bus = lo_unlock,
};

static void t_lock_ops(void)
{
	struct dw_model *m = model_std(32);
	struct dw_i2c_dev *dev = dw_new(m, 100000, 400000);
	uint8_t w[2] = { 0x10, 0x01 }, r[1];

	dev->adapter.lock_ops = &g_lo_ops;
	T_EQ(wr(&dev->adapter, 0x50, 0, w, 1, r, 1), 2);
	T_ASSERT(g_lo_lock >= 1 && g_lo_unlock == g_lo_lock);
	T_EQ(i2c_trylock_bus(&dev->adapter, I2C_LOCK_SEGMENT), 1);
	T_ASSERT(i2c_bus_locked_by_me(&dev->adapter));
	i2c_unlock_bus(&dev->adapter, I2C_LOCK_SEGMENT);
	T_EQ(g_lo_try, 1);
	T_EQ(g_lo_unlock, g_lo_lock + g_lo_try);
	dev->adapter.lock_ops = NULL;
	dw_del(dev);
	dwm_destroy(m);
}

/* ==========================================================================
 * The suspend test chain
 * ========================================================================== */

static char g_pm_log[64];
static int g_pm_nlog;
static struct dw_i2c_dev *g_pm_dev;
static struct i2c_client *g_pm_client;
static int g_pm_fail_suspend;

static void pm_log(char c)
{
	if (g_pm_nlog < (int)sizeof(g_pm_log) - 1)
		g_pm_log[g_pm_nlog++] = c;
	g_pm_log[g_pm_nlog] = 0;
}

static int pmd_probe(struct i2c_client *c)
{
	g_pm_client = c;
	return 0;
}

static int pmd_suspend(struct i2c_client *c)
{
	uint8_t w[1] = { 0x10 };

	/* still talks to its device: the adapter is up */
	T_EQ(i2c_master_send(c, (const char *)w, 1), 1);
	T_ASSERT(!g_pm_dev->sys_suspended);
	pm_log('c');
	return g_pm_fail_suspend ? -EIO : 0;
}

static int pmd_resume(struct i2c_client *c)
{
	uint8_t w[1] = { 0x10 };

	T_ASSERT(!g_pm_dev->sys_suspended);
	T_EQ(i2c_master_send(c, (const char *)w, 1), 1);
	pm_log('C');
	return 0;
}

static long pmd_show(struct i2c_client *c, char *buf, long cap, long pos)
{
	(void)c;
	return i2c_show_printf(buf, cap, pos, "    pmd: state %s\n",
			       g_pm_log);
}

static const struct i2c_device_id g_pmd_ids[] = {
	{ "pmdev", 0 },
	{ "", 0 },
};

static struct i2c_driver g_pmd = {
	.name = "pmd",
	.probe = pmd_probe,
	.suspend = pmd_suspend,
	.resume = pmd_resume,
	.show = pmd_show,
	.id_table = g_pmd_ids,
};

static void pm_gpio_suspend(void)
{
	/* the GPIO controllers go last: devices and adapters are down */
	T_ASSERT(g_pm_dev->sys_suspended && g_pm_dev->rpm_suspended);
	T_ASSERT(g_pm_client->suspended);
	pm_log('g');
}

static void pm_gpio_resume(void)
{
	/* and come back first */
	T_ASSERT(g_pm_dev->sys_suspended);
	T_ASSERT(g_pm_client->suspended);
	pm_log('G');
	/* a second test meanwhile is refused */
	T_EQ(i2c_proc_store(NULL, "suspend-test 1", 14), -EBUSY);
}

static void t_suspend_test(void)
{
	struct dw_model *m = model_std(32);
	struct i2c_board_info info;
	struct i2c_client *c;
	char *buf;
	int w0;

	m->lpss = 1;
	g_pm_dev = dw_new(m, 120000, 400000);
	i2c_dw_rpm_enable(g_pm_dev, 1000);
	T_EQ(i2c_register_driver(&g_pmd), 0);
	kmemset(&info, 0, sizeof(info));
	i2c_strlcpy(info.type, "pmdev", sizeof(info.type));
	info.addr = 0x50;
	c = i2c_new_client_device(&g_pm_dev->adapter, &info);
	T_ASSERT(!I2C_IS_ERR(c));
	T_ASSERT(c->driver == &g_pmd);
	g_arb_pinctrl_suspend_hook = pm_gpio_suspend;
	g_arb_pinctrl_resume_hook = pm_gpio_resume;

	/* the order: client, adapter (LPSS state saved), GPIO; back in
	 * reverse; the device usable after */
	g_pm_nlog = 0;
	T_EQ(i2c_pm_suspend_test(20), 0);
	T_STR(g_pm_log, "cgGC");
	T_ASSERT(!g_pm_dev->sys_suspended);
	T_ASSERT(g_pm_dev->st.rpm_suspends >= 1);
	T_ASSERT(g_pm_dev->st.rpm_resumes >= 1);
	{
		uint8_t w[2] = { 0x10, 0x42 }, r[1];

		T_EQ(wr(&g_pm_dev->adapter, 0x50, 0, w, 2, NULL, 0), 1);
		T_EQ(wr(&g_pm_dev->adapter, 0x50, 0, w, 1, r, 1), 2);
		T_EQ(r[0], 0x42);
	}
	/* the trigger: root only, "suspend-test [ms]" */
	g_pm_nlog = 0;
	T_EQ(i2c_proc_store(NULL, "suspend-test 5\n", 15), 15);
	T_STR(g_pm_log, "cgGC");
	g_pm_nlog = 0;
	T_EQ(i2c_proc_store(NULL, "suspend-test", 12), 12);
	T_STR(g_pm_log, "cgGC");
	T_EQ(i2c_proc_store(NULL, "suspend-test x", 14), -EINVAL);
	T_EQ(i2c_proc_store(NULL, "suspend-test 99999", 18), -EINVAL);
	T_EQ(i2c_proc_store(NULL, "suspend-testing", 15), -EINVAL);
	T_EQ(i2c_proc_store(NULL, "suspend-test ", 13), 13);
	T_EQ(i2c_proc_store(NULL, "resume", 6), -EINVAL);
	g_arb_capable = 0;
	T_EQ(i2c_proc_store(NULL, "suspend-test", 12), -EPERM);
	g_arb_capable = 1;
	/* a client that fails to suspend: nothing stays suspended */
	g_pm_fail_suspend = 1;
	g_pm_nlog = 0;
	T_EQ(i2c_pm_suspend_test(5), -EIO);
	T_STR(g_pm_log, "c");
	T_ASSERT(!c->suspended && !g_pm_dev->sys_suspended);
	g_pm_fail_suspend = 0;
	/* /proc/i2c: the test's record and the driver's line */
	buf = kcalloc(1, 8192);
	(void)i2c_core_show(buf, 8192);
	T_ASSERT(str_has(buf, "suspend-test: runs 5 failures 1 last -5"));
	T_ASSERT(str_has(buf, "pmd: state c"));
	kfree(buf);

	g_arb_pinctrl_suspend_hook = NULL;
	g_arb_pinctrl_resume_hook = NULL;
	w0 = g_test_warnings;
	i2c_unregister_device(c);
	i2c_del_driver(&g_pmd);
	dw_del(g_pm_dev);
	g_pm_dev = NULL;
	dwm_destroy(m);
	EXPECT_WARNS(w0, 0);
}
