/* Host test of the ACPI device layer (kernel/hal/acpi_dev.c) over a model
 * of the ACPICA namespace interfaces (host/kshim/acpica_mock.c): resource
 * decoding as ACPICA hands the descriptors over (the touchpad's
 * I2cSerialBusV2 and GpioInt, interrupt lists, memory/address/IO, ISA
 * IRQs through the MADT overrides), enumeration by _HID/_CID with _STA,
 * _DSM with argument packages, _DSD properties, power resources and
 * D-state transitions, PCI companions, _DEP, and the "no ACPI" paths.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include "host/kshim/acpica_mock.h"
#include <kernel/hal/acpi_dev.h>
#include <kernel/ke/irq.h>

extern int g_kshim_acpi_ready;

union rslot {
	ACPI_RESOURCE r;
	UINT8 pad[sizeof(ACPI_RESOURCE) + 32];
};

static ACPI_STATUS sta_absent(struct mock_node *n, const ACPI_OBJECT_LIST *a,
			      ACPI_OBJECT *o)
{
	(void)n;
	(void)a;
	if (o)
		mock_obj_int(o, 0);
	return AE_OK;
}

static ACPI_STATUS sta_present_not_functioning(struct mock_node *n,
					       const ACPI_OBJECT_LIST *a,
					       ACPI_OBJECT *o)
{
	(void)n;
	(void)a;
	if (o)
		mock_obj_int(o, 0x01);
	return AE_OK;
}

/* ---- resources --------------------------------------------------------- */

static UINT16 g_tp_pin[] = { 0x147 };
static UINT16 g_io_pins[] = { 5, 6, 7 };
static union rslot g_tp_crs[16];

static struct mock_node *build_touchpad(void)
{
	struct mock_node *pc00, *i2c0, *tpd0;
	static const UINT32 ext[] = { 0x35, 0x36, 0x37 };
	static const UINT8 isa9[] = { 9 }, isa0[] = { 0 };
	int i = 0;

	mock_ns_reset();
	pc00 = mock_device(NULL, "PC00", "PNP0A08", NULL);
	mock_add_cid(pc00, "PNP0A03");
	i2c0 = mock_device(pc00, "I2C0", NULL, NULL);
	mock_set_adr(i2c0, 0x00150000);
	tpd0 = mock_device(i2c0, "TPD0", "ELAN073B", "1");
	mock_add_cid(tpd0, "PNP0C50");
	mock_res_i2c(&g_tp_crs[i++].r, 0x15, 400000, 0, "\\_SB.PC00.I2C0", 1);
	mock_res_gpio(&g_tp_crs[i++].r, 1, 1, ACPI_ACTIVE_LOW, 0, 1,
		      ACPI_PIN_CONFIG_PULLUP, 0, 0, "\\_SB.GPI0", g_tp_pin, 1);
	mock_res_extirq(&g_tp_crs[i++].r, 1, ACPI_ACTIVE_LOW, 1, 0, ext, 3);
	mock_res_mem32fixed(&g_tp_crs[i++].r, 0xfed00000, 0x1000, 1);
	mock_res_mem32(&g_tp_crs[i++].r, 0xfe000000, 0x10000, 0);
	mock_res_addr64(&g_tp_crs[i++].r, 0x4000000000ull, 0x40000fffffull, 0,
			0x100, 1);
	mock_res_irq(&g_tp_crs[i++].r, 0, ACPI_ACTIVE_HIGH, 0, isa9, 1);
	mock_res_irq(&g_tp_crs[i++].r, 0, ACPI_ACTIVE_HIGH, 0, isa0, 1);
	mock_res_gpio(&g_tp_crs[i++].r, 0, 0, 0, 0, 0, ACPI_PIN_CONFIG_PULLDOWN,
		      ACPI_IO_RESTRICT_OUTPUT, 50, "\\_SB.GPI0", g_io_pins, 3);
	mock_res_io(&g_tp_crs[i++].r, 0x60, 1);
	mock_res_fixeddma(&g_tp_crs[i++].r, 7, 3);
	mock_res_i2c(&g_tp_crs[i++].r, 0x2a5, 1000000, 1, "\\_SB.PC00.I2C1", 0);
	mock_res_end(&g_tp_crs[i++].r);
	(void)mock_resources_stride(tpd0, "_CRS", g_tp_crs, i,
				    (int)sizeof(union rslot));
	(void)mock_device(NULL, "GPI0", "INT34C5", NULL);
	(void)mock_device(pc00, "I2C1", NULL, NULL);
	return tpd0;
}

static void t_resources(void)
{
	struct mock_node *tpd0 = build_touchpad();
	struct acpi_device *adev = acpi_dev_from_path(NULL, "\\_SB.PC00.I2C0.TPD0");
	struct acpi_res_list l;
	const struct acpi_res *r;
	const struct acpi_res_gpio *g;
	struct acpi_res_gpio gd;
	void *ctrl;
	unsigned int pin;

	T_ASSERT(adev);
	T_ASSERT(adev->handle == tpd0);
	T_ASSERT(!kstrcmp(adev->hid, "ELAN073B"));
	T_ASSERT(!kstrcmp(adev->uid, "1"));
	T_EQ(adev->ncids, 1);
	T_ASSERT(acpi_dev_has_id(adev, "PNP0C50"));
	T_ASSERT(!kstrcmp(adev->path, "\\_SB.PC00.I2C0.TPD0"));
	T_EQ(adev->sta, ACPI_DEV_STA_DEFAULT);

	T_EQ(acpi_dev_get_resources(adev, &l), 0);
	/* 3 extended interrupts expand into 3 entries */
	T_EQ(acpi_res_count(&l, ACPI_RES_IRQ), 5);
	T_EQ(acpi_res_count(&l, ACPI_RES_I2C), 2);
	T_EQ(acpi_res_count(&l, ACPI_RES_GPIO), 2);
	T_EQ(acpi_res_count(&l, ACPI_RES_MEM), 3);
	T_EQ(acpi_res_count(&l, ACPI_RES_IO), 1);
	T_EQ(acpi_res_count(&l, ACPI_RES_DMA), 1);

	r = acpi_res_find(&l, ACPI_RES_I2C, 0);
	T_ASSERT(r);
	T_EQ(r->i2c.address, 0x15);
	T_EQ(r->i2c.speed_hz, 400000);
	T_EQ(r->i2c.ten_bit, 0);
	T_EQ(r->i2c.revision, 2);
	T_ASSERT(!kstrcmp(r->i2c.source, "\\_SB.PC00.I2C0"));
	r = acpi_res_find(&l, ACPI_RES_I2C, 1);
	T_EQ(r->i2c.address, 0x2a5);
	T_EQ(r->i2c.ten_bit, 1);
	T_EQ(r->i2c.speed_hz, 1000000);

	g = acpi_res_find_gpio(&l, ACPI_RES_GPIO_INT, 0);
	T_ASSERT(g);
	T_EQ(g->pins[0], 0x147);
	T_EQ(g->npins, 1);
	T_EQ(g->triggering, ACPI_RES_LEVEL);
	T_EQ(g->polarity, ACPI_RES_ACTIVE_LOW);
	T_EQ(g->wake_capable, 1);
	T_EQ(g->shareable, 0);
	T_EQ(g->pin_config, ACPI_RES_PULL_UP);
	T_ASSERT(!kstrcmp(g->source, "\\_SB.GPI0"));
	T_EQ(acpi_res_irq_type(g->triggering, g->polarity), IRQ_TYPE_LEVEL_LOW);
	g = acpi_res_find_gpio(&l, ACPI_RES_GPIO_IO, 0);
	T_ASSERT(g);
	T_EQ(g->npins, 3);
	T_EQ(g->pins[2], 7);
	T_EQ(g->io_restriction, ACPI_RES_IO_RESTRICT_OUTPUT);
	T_EQ(g->debounce_timeout, 50);
	T_ASSERT(acpi_res_find_gpio(&l, -1, 1) == g);
	T_ASSERT(!acpi_res_find_gpio(&l, ACPI_RES_GPIO_INT, 1));

	r = acpi_res_find(&l, ACPI_RES_IRQ, 0);
	T_EQ(r->irq.gsi, 0x35);
	T_EQ(r->irq.triggering, ACPI_RES_LEVEL);
	T_EQ(r->irq.polarity, ACPI_RES_ACTIVE_LOW);
	T_EQ(r->irq.shareable, 1);
	T_EQ(r->irq.extended, 1);
	T_EQ(acpi_res_find(&l, ACPI_RES_IRQ, 2)->irq.gsi, 0x37);
	/* ISA 9 through the override: level, low */
	r = acpi_res_find(&l, ACPI_RES_IRQ, 3);
	T_EQ(r->irq.gsi, 9);
	T_EQ(r->irq.triggering, ACPI_RES_LEVEL);
	T_EQ(r->irq.polarity, ACPI_RES_ACTIVE_LOW);
	T_EQ(r->irq.extended, 0);
	/* ISA 0 -> GSI 2, trigger as described (override says default) */
	r = acpi_res_find(&l, ACPI_RES_IRQ, 4);
	T_EQ(r->irq.gsi, 2);
	T_EQ(r->irq.triggering, ACPI_RES_EDGE);

	r = acpi_res_find(&l, ACPI_RES_MEM, 0);
	T_EQ(r->mem.start, 0xfed00000);
	T_EQ(r->mem.length, 0x1000);
	T_EQ(r->mem.writeable, 1);
	r = acpi_res_find(&l, ACPI_RES_MEM, 1);
	T_EQ(r->mem.start, 0xfe000000);
	T_EQ(r->mem.writeable, 0);
	r = acpi_res_find(&l, ACPI_RES_MEM, 2);
	T_EQ(r->mem.start, 0x4000000000ull + 0x100);
	T_EQ(r->mem.length, 0x100000);
	T_EQ(r->mem.producer, 1);
	r = acpi_res_find(&l, ACPI_RES_IO, 0);
	T_EQ(r->io.start, 0x60);
	r = acpi_res_find(&l, ACPI_RES_DMA, 0);
	T_EQ(r->dma.request_line, 7);
	T_EQ(r->dma.channel, 3);
	acpi_dev_free_resource_list(&l);
	T_EQ(l.count, 0);

	/* GpioInt index -> controller handle and pin */
	T_EQ(acpi_dev_gpio_int(adev, 0, &ctrl, &pin, &gd), 0);
	T_ASSERT(ctrl == mock_lookup("\\_SB.GPI0"));
	T_EQ(pin, 0x147);
	T_EQ(acpi_dev_gpio_int(adev, 1, &ctrl, &pin, &gd), -ENOENT);
	acpi_dev_put(adev);

	/* no _CRS */
	adev = acpi_dev_from_path(NULL, "\\_SB.GPI0");
	T_ASSERT(adev);
	T_EQ(acpi_dev_get_resources(adev, &l), -ENOENT);
	acpi_dev_put(adev);
	T_ASSERT(acpi_dev_from_path(NULL, "\\_SB.NOPE") == NULL);
}

/* ---- enumeration ---------------------------------------------------------- */

struct enum_ctx {
	int n;
	char seen[8][ACPI_PATH_LEN];
	unsigned long data[8];
	struct acpi_device *kept;
};

static int enum_cb(struct acpi_device *adev, const struct acpi_device_id *id,
		   void *ctx)
{
	struct enum_ctx *e = ctx;

	if (e->n < 8) {
		ksnprintf(e->seen[e->n], ACPI_PATH_LEN, "%s", adev->path);
		e->data[e->n] = id ? id->driver_data : 0;
	}
	if (!e->kept)
		e->kept = acpi_dev_get(adev);
	e->n++;
	return 0;
}

static void t_enumeration(void)
{
	static const struct acpi_device_id ids[] = {
		{ "PNP0C50", 1 },
		{ "ACPI0C50", 2 },
		{ "ELAN0000", 3 },
		{ "", 0 },
	};
	struct mock_node *a, *b, *off, *offkid, *pnf;
	struct enum_ctx e;

	mock_ns_reset();
	a = mock_device(NULL, "TPD0", "ELAN0000", NULL);
	mock_add_cid(a, "PNP0C50");
	b = mock_device(NULL, "TPD1", "SYNA0001", NULL);
	mock_add_cid(b, "ACPI0C50");
	off = mock_device(NULL, "OFF0", "PNP0C50", NULL);
	(void)mock_method(off, "_STA", sta_absent, NULL);
	offkid = mock_device(off, "KID0", "PNP0C50", NULL);
	pnf = mock_device(NULL, "PNF0", "PNP0C50", NULL);
	(void)mock_method(pnf, "_STA", sta_present_not_functioning, NULL);
	(void)offkid;

	kmemset(&e, 0, sizeof(e));
	T_EQ(acpi_dev_for_each_match(ids, 1, enum_cb, &e), 2);
	T_EQ(e.n, 2);
	/* the device's _HID beats its _CID */
	T_ASSERT(!kstrcmp(e.seen[0], "\\_SB.TPD0"));
	T_EQ(e.data[0], 3);
	T_ASSERT(!kstrcmp(e.seen[1], "\\_SB.TPD1"));
	T_EQ(e.data[1], 2);
	/* a device kept beyond the walk stays valid */
	T_ASSERT(e.kept && !kstrcmp(e.kept->hid, "ELAN0000"));
	acpi_dev_put(e.kept);

	kmemset(&e, 0, sizeof(e));
	T_EQ(acpi_dev_for_each_match(ids, 0, enum_cb, &e), 5);
	acpi_dev_put(e.kept);

	T_ASSERT(acpi_dev_present("SYNA0001", NULL, -1));
	T_ASSERT(!acpi_dev_present("SYNA0001", "7", -1));
	T_ASSERT(!acpi_dev_present("NONE0000", NULL, -1));
}

/* ---- _DSM, packages, _DSD --------------------------------------------------- */

static UINT8 g_dsm_uuid[16];
static int g_dsm_bad_args;

static ACPI_STATUS dsm_eval(struct mock_node *n, const ACPI_OBJECT_LIST *a,
			    ACPI_OBJECT *o)
{
	static const UINT8 funcs[1] = { 0x03 };
	(void)n;

	if (!a || a->Count != 4 || a->Pointer[0].Type != ACPI_TYPE_BUFFER ||
	    a->Pointer[0].Buffer.Length != 16 ||
	    kmemcmp(a->Pointer[0].Buffer.Pointer, g_dsm_uuid, 16) ||
	    a->Pointer[1].Type != ACPI_TYPE_INTEGER ||
	    a->Pointer[2].Type != ACPI_TYPE_INTEGER ||
	    a->Pointer[3].Type != ACPI_TYPE_PACKAGE) {
		g_dsm_bad_args++;
		return AE_AML_OPERAND_TYPE;
	}
	if (!o)
		return AE_OK;
	switch (a->Pointer[2].Integer.Value) {
	case 0:
		mock_obj_buf(o, funcs, 1);
		break;
	case 1:
		/* HID descriptor address; arg package carried through */
		mock_obj_int(o, 0x20 + a->Pointer[3].Package.Count);
		break;
	default:
		mock_obj_int(o, 0);
	}
	return AE_OK;
}

static ACPI_OBJECT g_sscn_e[3];
static ACPI_STATUS sscn_eval(struct mock_node *n, const ACPI_OBJECT_LIST *a,
			     ACPI_OBJECT *o)
{
	(void)n;
	(void)a;
	mock_obj_int(&g_sscn_e[0], 0x200);
	mock_obj_int(&g_sscn_e[1], 0x2c0);
	mock_obj_int(&g_sscn_e[2], 0x1e);
	if (o)
		mock_obj_pkg(o, g_sscn_e, 3);
	return AE_OK;
}

static UINT8 g_dsd_uuid[16], g_other_uuid[16];
static ACPI_OBJECT g_dsd[4], g_props[4], g_kv[4][2], g_arr[3], g_nest[2];

static ACPI_STATUS dsd_eval(struct mock_node *n, const ACPI_OBJECT_LIST *a,
			    ACPI_OBJECT *o)
{
	(void)n;
	(void)a;
	mock_obj_str(&g_kv[0][0], "i2c-hid-quirk");
	mock_obj_int(&g_kv[0][1], 5);
	mock_obj_str(&g_kv[1][0], "name");
	mock_obj_str(&g_kv[1][1], "abc");
	mock_obj_int(&g_arr[0], 1);
	mock_obj_int(&g_arr[1], 2);
	mock_obj_int(&g_arr[2], 0x1ffffffffull);
	mock_obj_str(&g_kv[2][0], "arr");
	mock_obj_pkg(&g_kv[2][1], g_arr, 3);
	mock_obj_str(&g_kv[3][0], "big");
	mock_obj_int(&g_kv[3][1], 0x10000);
	for (int i = 0; i < 4; i++)
		mock_obj_pkg(&g_props[i], g_kv[i], 2);
	mock_obj_buf(&g_dsd[0], g_other_uuid, 16);
	mock_obj_int(&g_nest[0], 0);
	mock_obj_pkg(&g_dsd[1], g_nest, 1);
	mock_obj_buf(&g_dsd[2], g_dsd_uuid, 16);
	mock_obj_pkg(&g_dsd[3], g_props, 4);
	if (o)
		mock_obj_pkg(o, g_dsd, 4);
	return AE_OK;
}

static void t_dsm_dsd(void)
{
	static const UINT8 expect[16] = { 0xf7, 0xf6, 0xdf, 0x3c, 0x67, 0x42,
					   0x55, 0x45, 0xad, 0x05, 0xb3, 0x0a,
					   0x3d, 0x89, 0x38, 0xde };
	struct mock_node *dev;
	struct acpi_value *v = NULL, pkg, el[2];
	uint16_t t[3] = { 0 };
	uint32_t u32, arr[4];
	uint16_t u16;
	char s[8];

	T_EQ(acpi_uuid_parse("3cdff6f7-4267-4555-ad05-b30a3d8938de",
			     g_dsm_uuid), 0);
	T_EQ(kmemcmp(g_dsm_uuid, expect, 16), 0);
	T_EQ(acpi_uuid_parse("3cdff6f7-4267-4555-ad05-b30a3d8938d", g_dsm_uuid),
	     -EINVAL);
	T_EQ(acpi_uuid_parse("3cdff6f7x4267-4555-ad05-b30a3d8938de",
			     g_dsm_uuid),
	     -EINVAL);
	T_EQ(acpi_uuid_parse("3cdff6f7-4267-4555-ad05-b30a3d8938de",
			     g_dsm_uuid), 0);
	T_EQ(acpi_uuid_parse("daffd814-6eba-4d8c-8a91-bc9bbf4aa301",
			     g_dsd_uuid), 0);
	T_EQ(acpi_uuid_parse("dbb8e3e6-5886-4ba6-8795-1319f52a966b",
			     g_other_uuid), 0);

	mock_ns_reset();
	dev = mock_device(NULL, "TPD0", "PNP0C50", NULL);
	(void)mock_method(dev, "_DSM", dsm_eval, NULL);
	(void)mock_method(dev, "SSCN", sscn_eval, NULL);
	(void)mock_method(dev, "_DSD", dsd_eval, NULL);

	T_ASSERT(acpi_check_dsm(dev, g_dsm_uuid, 1, 1u << 1));
	T_ASSERT(!acpi_check_dsm(dev, g_dsm_uuid, 1, 1u << 2));
	T_ASSERT(!acpi_check_dsm(dev, g_dsm_uuid, 1, 0));
	T_EQ(acpi_evaluate_dsm_typed(dev, g_dsm_uuid, 1, 1, NULL,
				     ACPI_VALUE_INTEGER, &v),
	     0);
	T_EQ(v->integer, 0x20);
	acpi_value_free(v);
	/* a package argument with nested values goes through */
	kmemset(&pkg, 0, sizeof(pkg));
	kmemset(el, 0, sizeof(el));
	el[0].type = ACPI_VALUE_INTEGER;
	el[0].integer = 7;
	el[1].type = ACPI_VALUE_STRING;
	el[1].string = (char *)"x";
	el[1].length = 1;
	pkg.type = ACPI_VALUE_PACKAGE;
	pkg.length = 2;
	pkg.elements = el;
	T_EQ(acpi_evaluate_dsm(dev, g_dsm_uuid, 1, 1, &pkg, &v), 0);
	T_EQ(v->type, ACPI_VALUE_INTEGER);
	T_EQ(v->integer, 0x22);
	acpi_value_free(v);
	T_EQ(acpi_evaluate_dsm_typed(dev, g_dsm_uuid, 1, 0, NULL,
				     ACPI_VALUE_INTEGER, &v),
	     -EPROTO);
	T_EQ(g_dsm_bad_args, 0);

	T_EQ(acpi_evaluate_u16_package(dev, "SSCN", t, 3), 3);
	T_EQ(t[0], 0x200);
	T_EQ(t[1], 0x2c0);
	T_EQ(t[2], 0x1e);
	T_EQ(acpi_evaluate_u16_package(dev, "FMCN", t, 3), -ENOENT);

	T_EQ(acpi_dev_get_property_u32(dev, "i2c-hid-quirk", &u32), 0);
	T_EQ(u32, 5);
	T_EQ(acpi_dev_get_property_u16(dev, "big", &u16), -EOVERFLOW);
	T_EQ(acpi_dev_get_property_string(dev, "name", s, sizeof(s)), 0);
	T_ASSERT(!kstrcmp(s, "abc"));
	T_EQ(acpi_dev_get_property_u32(dev, "name", &u32), -EPROTO);
	T_EQ(acpi_dev_get_property_u32(dev, "nope", &u32), -ENOENT);
	T_EQ(acpi_dev_get_property_u32_array(dev, "arr", arr, 2), 2);
	T_EQ(arr[1], 2);
	T_EQ(acpi_dev_get_property_u32_array(dev, "arr", arr, 4), -EOVERFLOW);
	T_ASSERT(acpi_dev_has_property(dev, "big"));
	T_ASSERT(!acpi_dev_has_property(dev, "small"));

	/* a nested result keeps every value array aligned */
	T_EQ(acpi_evaluate(dev, "_DSD", NULL, 0, &v), 0);
	T_EQ(v->type, ACPI_VALUE_PACKAGE);
	T_EQ(v->length, 4);
	T_EQ(((unsigned long)v->elements[3].elements & 7), 0);
	T_EQ(((unsigned long)v->elements[3].elements[0].elements & 7), 0);
	T_ASSERT(!kstrcmp(v->elements[3].elements[1].elements[1].string,
			  "abc"));
	T_EQ(v->elements[0].type, ACPI_VALUE_BUFFER);
	T_EQ(v->elements[0].length, 16);
	acpi_value_free(v);
}

/* ---- power --------------------------------------------------------------- */

static char g_plog[512];

static void plog(const char *what, struct mock_node *n)
{
	size_t l = kstrlen(g_plog);

	ksnprintf(g_plog + l, sizeof(g_plog) - l, "%s%s:%s",
		  l ? " " : "", n->parent->name, what);
}

static int g_res_on[2];

static ACPI_STATUS pr_on(struct mock_node *n, const ACPI_OBJECT_LIST *a,
			 ACPI_OBJECT *o)
{
	(void)a;
	(void)o;
	g_res_on[(long)n->priv] = 1;
	plog("ON", n);
	return AE_OK;
}

static ACPI_STATUS pr_off(struct mock_node *n, const ACPI_OBJECT_LIST *a,
			  ACPI_OBJECT *o)
{
	(void)a;
	(void)o;
	g_res_on[(long)n->priv] = 0;
	plog("OFF", n);
	return AE_OK;
}

static ACPI_STATUS pr_sta(struct mock_node *n, const ACPI_OBJECT_LIST *a,
			  ACPI_OBJECT *o)
{
	(void)a;
	if (o)
		mock_obj_int(o, (UINT64)g_res_on[(long)n->priv]);
	return AE_OK;
}

static ACPI_STATUS ps_method(struct mock_node *n, const ACPI_OBJECT_LIST *a,
			     ACPI_OBJECT *o)
{
	(void)a;
	(void)o;
	plog(n->name, n);
	return AE_OK;
}

static struct mock_node *g_pwr[2];
static ACPI_OBJECT g_pr0[2], g_pr3[1], g_pr0b[1];

static ACPI_STATUS pr0_eval(struct mock_node *n, const ACPI_OBJECT_LIST *a,
			    ACPI_OBJECT *o)
{
	(void)n;
	(void)a;
	mock_obj_ref(&g_pr0[0], g_pwr[0]);
	mock_obj_ref(&g_pr0[1], g_pwr[1]);
	if (o)
		mock_obj_pkg(o, g_pr0, 2);
	return AE_OK;
}

static ACPI_STATUS pr3_eval(struct mock_node *n, const ACPI_OBJECT_LIST *a,
			    ACPI_OBJECT *o)
{
	(void)n;
	(void)a;
	mock_obj_ref(&g_pr3[0], g_pwr[1]);
	if (o)
		mock_obj_pkg(o, g_pr3, 1);
	return AE_OK;
}

static ACPI_STATUS pr0b_eval(struct mock_node *n, const ACPI_OBJECT_LIST *a,
			     ACPI_OBJECT *o)
{
	(void)n;
	(void)a;
	mock_obj_ref(&g_pr0b[0], g_pwr[1]);
	if (o)
		mock_obj_pkg(o, g_pr0b, 1);
	return AE_OK;
}

static void t_power(void)
{
	struct mock_node *dev, *dev2;
	struct acpi_device *a, *b;

	mock_ns_reset();
	for (long i = 0; i < 2; i++) {
		g_pwr[i] = mock_power_resource(NULL, i ? "PWR1" : "PWR0");
		(void)mock_method(g_pwr[i], "_ON_", pr_on, (void *)i);
		(void)mock_method(g_pwr[i], "_OFF", pr_off, (void *)i);
		(void)mock_method(g_pwr[i], "_STA", pr_sta, (void *)i);
	}
	dev = mock_device(NULL, "TPD0", "PNP0C50", NULL);
	(void)mock_method(dev, "_PR0", pr0_eval, NULL);
	(void)mock_method(dev, "_PR3", pr3_eval, NULL);
	(void)mock_method(dev, "_PS0", ps_method, NULL);
	(void)mock_method(dev, "_PS3", ps_method, NULL);
	dev2 = mock_device(NULL, "TPD1", "PNP0C50", NULL);
	(void)mock_method(dev2, "_PR0", pr0b_eval, NULL);
	g_res_on[0] = g_res_on[1] = 0;
	g_plog[0] = 0;
	a = acpi_dev_from_handle(dev);
	b = acpi_dev_from_handle(dev2);
	T_ASSERT(acpi_device_power_manageable(a));
	/* resources off: the device is found in D3cold */
	T_EQ(acpi_device_get_power(a), ACPI_DEV_D3_COLD);
	T_EQ(acpi_device_set_power(a, ACPI_DEV_D0), 0);
	T_ASSERT(!kstrcmp(g_plog, "PWR0:ON PWR1:ON TPD0:_PS0"));
	T_EQ(acpi_device_get_power(a), ACPI_DEV_D0);
	g_plog[0] = 0;
	T_EQ(acpi_device_set_power(a, ACPI_DEV_D0), 0);
	T_ASSERT(!g_plog[0]); /* no transition */
	/* D3hot: _PS3 first, then only what _PR3 lacks goes off */
	T_EQ(acpi_device_set_power(a, ACPI_DEV_D3_HOT), 0);
	T_ASSERT(!kstrcmp(g_plog, "TPD0:_PS3 PWR0:OFF"));
	/* a second user of PWR1 */
	g_plog[0] = 0;
	T_EQ(acpi_device_set_power(b, ACPI_DEV_D0), 0);
	T_ASSERT(!g_plog[0]); /* PWR1 already on: only a reference */
	T_EQ(acpi_device_set_power(a, ACPI_DEV_D0), 0);
	T_ASSERT(!kstrcmp(g_plog, "PWR0:ON TPD0:_PS0"));
	g_plog[0] = 0;
	/* D3cold: everything of this device goes, PWR1 stays for TPD1 */
	T_EQ(acpi_device_set_power(a, ACPI_DEV_D3_COLD), 0);
	T_ASSERT(!kstrcmp(g_plog, "TPD0:_PS3 PWR0:OFF"));
	T_EQ(g_res_on[1], 1);
	g_plog[0] = 0;
	T_EQ(acpi_device_set_power(b, ACPI_DEV_D3_COLD), 0);
	T_ASSERT(!kstrcmp(g_plog, "PWR1:OFF"));
	/* D1 is not supported by either */
	T_EQ(acpi_device_set_power(a, ACPI_DEV_D1), -EINVAL);
	acpi_dev_put(a);
	acpi_dev_put(b);
}

/* Reference counts start from every present device that is on, and a
 * resource the firmware has on with nobody listed using it stays on. */
static struct mock_node *g_shr, *g_orph;
static int g_shr_on, g_orph_on;
static ACPI_OBJECT g_pr_shr[1], g_pr_orph[1];

static ACPI_STATUS shr_sta(struct mock_node *n, const ACPI_OBJECT_LIST *a,
			   ACPI_OBJECT *o)
{
	(void)a;
	if (o)
		mock_obj_int(o, (UINT64)(n->priv ? g_orph_on : g_shr_on));
	return AE_OK;
}

static ACPI_STATUS shr_on(struct mock_node *n, const ACPI_OBJECT_LIST *a,
			  ACPI_OBJECT *o)
{
	(void)a;
	(void)o;
	*(n->priv ? &g_orph_on : &g_shr_on) = 1;
	plog("ON", n);
	return AE_OK;
}

static ACPI_STATUS shr_off(struct mock_node *n, const ACPI_OBJECT_LIST *a,
			   ACPI_OBJECT *o)
{
	(void)a;
	(void)o;
	*(n->priv ? &g_orph_on : &g_shr_on) = 0;
	plog("OFF", n);
	return AE_OK;
}

static ACPI_STATUS pr_shr_eval(struct mock_node *n, const ACPI_OBJECT_LIST *a,
			       ACPI_OBJECT *o)
{
	(void)n;
	(void)a;
	mock_obj_ref(&g_pr_shr[0], g_shr);
	if (o)
		mock_obj_pkg(o, g_pr_shr, 1);
	return AE_OK;
}

static ACPI_STATUS pr_orph_eval(struct mock_node *n, const ACPI_OBJECT_LIST *a,
				ACPI_OBJECT *o)
{
	(void)n;
	(void)a;
	mock_obj_ref(&g_pr_orph[0], g_orph);
	if (o)
		mock_obj_pkg(o, g_pr_orph, 1);
	return AE_OK;
}

static struct mock_node *power_res(const char *name, long orphan)
{
	struct mock_node *r = mock_power_resource(NULL, name);

	(void)mock_method(r, "_ON_", shr_on, (void *)orphan);
	(void)mock_method(r, "_OFF", shr_off, (void *)orphan);
	(void)mock_method(r, "_STA", shr_sta, (void *)orphan);
	return r;
}

static void t_power_refs(void)
{
	struct mock_node *dva, *dvb, *dvc;
	struct acpi_device *a, *b, *c;

	mock_ns_reset();
	g_shr = power_res("PWRS", 0);
	g_orph = power_res("PWRO", 1);
	g_shr_on = 1; /* firmware left both on */
	g_orph_on = 1;
	dva = mock_device(NULL, "DVA0", "PNP0C50", NULL);
	(void)mock_method(dva, "_PR0", pr_shr_eval, NULL);
	(void)mock_method(dva, "_PS0", ps_method, NULL);
	(void)mock_method(dva, "_PS3", ps_method, NULL);
	/* a second user the kernel never touches */
	dvb = mock_device(NULL, "DVB0", "INT0000", NULL);
	(void)mock_method(dvb, "_PR0", pr_shr_eval, NULL);
	/* the only device listing PWRO is not there */
	dvc = mock_device(NULL, "DVC0", "INT0001", NULL);
	(void)mock_method(dvc, "_STA", sta_absent, NULL);
	(void)mock_method(dvc, "_PR0", pr_orph_eval, NULL);
	(void)mock_method(dvc, "_PS3", ps_method, NULL);
	g_plog[0] = 0;
	a = acpi_dev_from_handle(dva);
	b = acpi_dev_from_handle(dvb);
	c = acpi_dev_from_handle(dvc);

	/* on through the shared resource: D0, no method run to find out */
	T_EQ(acpi_device_get_power(a), ACPI_DEV_D0);
	T_ASSERT(!g_plog[0]);
	/* DVA off: _PS3, but PWRS stays for DVB */
	T_EQ(acpi_device_set_power(a, ACPI_DEV_D3_COLD), 0);
	T_ASSERT(!kstrcmp(g_plog, "DVA0:_PS3"));
	T_EQ(g_shr_on, 1);
	/* and back: the resource is on, only _PS0 */
	g_plog[0] = 0;
	T_EQ(acpi_device_set_power(a, ACPI_DEV_D0), 0);
	T_ASSERT(!kstrcmp(g_plog, "DVA0:_PS0"));
	/* both off: now PWRS goes */
	g_plog[0] = 0;
	T_EQ(acpi_device_set_power(b, ACPI_DEV_D3_COLD), 0);
	T_ASSERT(!g_plog[0]);
	T_EQ(acpi_device_set_power(a, ACPI_DEV_D3_COLD), 0);
	T_ASSERT(!kstrcmp(g_plog, "DVA0:_PS3 PWRS:OFF"));
	T_EQ(g_shr_on, 0);
	/* the firmware's resource: never switched off by a device */
	g_plog[0] = 0;
	T_EQ(acpi_device_set_power(c, ACPI_DEV_D0), 0);
	T_ASSERT(!g_plog[0]); /* on already: no _ON */
	T_EQ(acpi_device_set_power(c, ACPI_DEV_D3_COLD), 0);
	T_ASSERT(!kstrcmp(g_plog, "DVC0:_PS3"));
	T_EQ(g_orph_on, 1);
	acpi_dev_put(a);
	acpi_dev_put(b);
	acpi_dev_put(c);
}

/* A slow _PS0 holds up its own device only. */
static volatile int g_slow_started, g_slow_done;

static ACPI_STATUS slow_ps0(struct mock_node *n, const ACPI_OBJECT_LIST *a,
			    ACPI_OBJECT *o)
{
	(void)n;
	(void)a;
	(void)o;
	__atomic_store_n(&g_slow_started, 1, __ATOMIC_SEQ_CST);
	h_usleep(300000);
	return AE_OK;
}

static void slow_task(void *p)
{
	T_EQ(acpi_device_set_power(p, ACPI_DEV_D0), 0);
	__atomic_store_n(&g_slow_done, 1, __ATOMIC_SEQ_CST);
}

static void t_power_parallel(void)
{
	struct mock_node *s0, *f0;
	struct acpi_device *a, *b;
	uint64_t t0;

	mock_ns_reset();
	g_shr = power_res("PWRS", 0);
	g_orph = power_res("PWRO", 1);
	g_shr_on = 0;
	g_orph_on = 0;
	s0 = mock_device(NULL, "SLW0", "PNP0C50", NULL);
	(void)mock_method(s0, "_PR0", pr_shr_eval, NULL);
	(void)mock_method(s0, "_PS0", slow_ps0, NULL);
	f0 = mock_device(NULL, "FST0", "PNP0C50", NULL);
	(void)mock_method(f0, "_PR0", pr_orph_eval, NULL);
	(void)mock_method(f0, "_PS0", ps_method, NULL);
	g_plog[0] = 0;
	a = acpi_dev_from_handle(s0);
	b = acpi_dev_from_handle(f0);
	T_EQ(acpi_device_get_power(a), ACPI_DEV_D3_COLD);
	g_slow_started = 0;
	g_slow_done = 0;
	(void)sched_add_task(slow_task, a, NULL, 0);
	while (!__atomic_load_n(&g_slow_started, __ATOMIC_SEQ_CST))
		h_usleep(100);
	t0 = h_now_ns();
	T_EQ(acpi_device_set_power(b, ACPI_DEV_D0), 0);
	T_ASSERT(h_now_ns() - t0 < 150000000ull);
	T_EQ(g_slow_done, 0);
	while (!__atomic_load_n(&g_slow_done, __ATOMIC_SEQ_CST))
		h_usleep(1000);
	T_EQ(acpi_device_get_power(a), ACPI_DEV_D0);
	acpi_dev_put(a);
	acpi_dev_put(b);
}

/* ---- device names -------------------------------------------------------- */

static struct mock_node *g_dep_sup;
static ACPI_OBJECT g_dep_one[1];

static ACPI_STATUS dep_one_eval(struct mock_node *n, const ACPI_OBJECT_LIST *a,
				ACPI_OBJECT *o)
{
	(void)a;
	mock_obj_ref(&g_dep_one[0], n->priv);
	if (o)
		mock_obj_pkg(o, g_dep_one, 1);
	return AE_OK;
}

static ACPI_STATUS sta_fails(struct mock_node *n, const ACPI_OBJECT_LIST *a,
			     ACPI_OBJECT *o)
{
	(void)n;
	(void)a;
	(void)o;
	return AE_AML_INTERNAL;
}

static void name_is(struct mock_node *n, const char *want)
{
	char buf[32];

	T_EQ(acpi_dev_name(n, buf, sizeof(buf)), 0);
	if (kstrcmp(buf, want)) {
		kprintf("  %s: \"%s\", expected \"%s\"\n", n->path, buf, want);
		T_ASSERT(0);
	}
}

static void t_names(void)
{
	struct mock_node *d0, *d1, *d2, *chld, *d3, *d4, *kid, *d5, *pep, *cid,
		*sup;
	struct mock_node *many[18];
	char buf[32];

	mock_ns_reset();
	d0 = mock_device(NULL, "DEV0", "ABCD0001", NULL);
	d1 = mock_device(NULL, "DEV1", "ABCD0001", NULL);
	(void)mock_method(d1, "_STA", sta_absent, NULL); /* still counted */
	d2 = mock_device(NULL, "DEV2", "ABCD0001", NULL); /* _DEP: pass 2 */
	chld = mock_device(d2, "CHLD", "ABCD0001", NULL);
	d3 = mock_device(NULL, "DEV3", "ABCD0001", NULL);
	d4 = mock_device(NULL, "DEV4", "ABCD0001", NULL); /* _STA fails */
	(void)mock_method(d4, "_STA", sta_fails, NULL);
	kid = mock_device(d4, "KID0", "ABCD0001", NULL);
	d5 = mock_device(NULL, "DEV5", "ABCD0001", NULL); /* ignored _DEP */
	sup = mock_device(NULL, "SUP0", "SUPP0001", NULL);
	pep = mock_device(NULL, "PEPD", "PNP0D80", NULL);
	cid = mock_device(NULL, "CIDD", NULL, NULL); /* first _CID counts */
	mock_add_cid(cid, "ABCD0001");
	g_dep_sup = sup;
	(void)mock_method(d2, "_DEP", dep_one_eval, sup);
	(void)mock_method(d5, "_DEP", dep_one_eval, pep);
	(void)kid;

	name_is(d0, "ABCD0001:00");
	name_is(d1, "ABCD0001:01");
	name_is(d3, "ABCD0001:02");
	name_is(d5, "ABCD0001:03");
	name_is(cid, "ABCD0001:04");
	name_is(d2, "ABCD0001:05");
	name_is(chld, "ABCD0001:06");
	/* no device object: the number a new one would get */
	name_is(d4, "ABCD0001:07");
	name_is(sup, "SUPP0001:00");
	name_is(pep, "PNP0D80:00");
	T_EQ(acpi_dev_name(mock_root(), buf, sizeof(buf)), -ENOENT);

	/* two hex digits, more when needed */
	mock_ns_reset();
	for (int i = 0; i < 18; i++) {
		char nm[5] = { 'X', 'X', (char)('A' + i / 10),
			       (char)('0' + i % 10), 0 };

		many[i] = mock_device(NULL, nm, "MANY0001", NULL);
	}
	name_is(many[9], "MANY0001:09");
	name_is(many[10], "MANY0001:0a");
	name_is(many[17], "MANY0001:11");
}

/* ---- PCI companions and _DEP ------------------------------------------------ */

static ACPI_OBJECT g_dep[2];
static struct mock_node *g_dep_nodes[2];

static ACPI_STATUS dep_eval(struct mock_node *n, const ACPI_OBJECT_LIST *a,
			    ACPI_OBJECT *o)
{
	(void)n;
	(void)a;
	mock_obj_ref(&g_dep[0], g_dep_nodes[0]);
	mock_obj_ref(&g_dep[1], g_dep_nodes[1]);
	if (o)
		mock_obj_pkg(o, g_dep, 2);
	return AE_OK;
}

static void t_companion_dep(void)
{
	struct mock_node *pc00, *i2c0, *rp, *nvme, *dup, *cam, *pmic, *pep;
	struct pci_device root_dev = { 0 }, bridge = { 0 }, ep = { 0 };
	struct acpi_device *c;

	mock_ns_reset();
	pc00 = mock_device(NULL, "PC00", "PNP0A08", NULL);
	(void)mock_int(pc00, "_BBN", 0);
	dup = mock_device(pc00, "I2CX", NULL, NULL);
	mock_set_adr(dup, 0x00150000);
	(void)mock_method(dup, "_STA", sta_absent, NULL);
	i2c0 = mock_device(pc00, "I2C0", NULL, NULL);
	mock_set_adr(i2c0, 0x00150000);
	rp = mock_device(pc00, "RP01", NULL, NULL);
	mock_set_adr(rp, 0x001c0000);
	nvme = mock_device(rp, "PXSX", NULL, NULL);
	mock_set_adr(nvme, 0);

	root_dev.bus = 0;
	root_dev.device = 0x15;
	root_dev.function = 0;
	c = acpi_pci_find_companion(&root_dev);
	T_ASSERT(c && c->handle == i2c0); /* the present duplicate wins */
	acpi_dev_put(c);
	bridge.bus = 0;
	bridge.device = 0x1c;
	ep.bus = 1;
	ep.bus_self = &bridge;
	c = acpi_pci_find_companion(&ep);
	T_ASSERT(c && c->handle == nvme);
	acpi_dev_put(c);
	root_dev.bus = 3; /* no host bridge with _BBN 3 */
	T_ASSERT(acpi_pci_find_companion(&root_dev) == NULL);
	root_dev.bus = 0;
	root_dev.segment = 0x10000; /* VMD */
	T_ASSERT(acpi_pci_find_companion(&root_dev) == NULL);

	cam = mock_device(NULL, "CAM0", "OVTI01A0", NULL);
	pmic = mock_device(NULL, "PMI0", "INT3472", NULL);
	pep = mock_device(NULL, "PEPD", "PNP0D80", NULL);
	g_dep_nodes[0] = pep;
	g_dep_nodes[1] = pmic;
	(void)mock_method(cam, "_DEP", dep_eval, NULL);
	c = acpi_dev_from_handle(cam);
	{
		void *h[4];

		T_EQ(acpi_dev_get_dependencies(c, h, 4), 2);
		T_ASSERT(h[1] == pmic);
	}
	T_EQ(acpi_dev_ready_for_enumeration(c), 0);
	acpi_dev_supplier_ready(pmic);
	T_EQ(acpi_dev_ready_for_enumeration(c), 1);
	acpi_dev_put(c);
}

/* ---- no ACPI ----------------------------------------------------------------- */

static void t_not_ready(void)
{
	struct acpi_res_list l;
	struct acpi_value *v = (struct acpi_value *)1;
	uint32_t u;

	g_kshim_acpi_ready = 0;
	T_ASSERT(!acpi_dev_ready());
	T_ASSERT(acpi_dev_from_path(NULL, "\\_SB") == NULL);
	T_EQ(acpi_get_resources((void *)1, "_CRS", &l), -ENODEV);
	T_EQ(l.count, 0);
	T_EQ(acpi_evaluate((void *)1, "_STA", NULL, 0, &v), -ENODEV);
	T_ASSERT(v == NULL);
	T_EQ(acpi_dev_get_property_u32((void *)1, "x", &u), -ENODEV);
	T_EQ(acpi_dev_for_each_match(NULL, 1, NULL, NULL), -ENODEV);
	T_ASSERT(!acpi_check_dsm((void *)1, g_dsm_uuid, 1, 2));
	g_kshim_acpi_ready = 1;
}

int main(void);
int main(void)
{
	kshim_init_main();
	test_case("resources", t_resources);
	test_case("enumeration", t_enumeration);
	test_case("dsm_dsd", t_dsm_dsd);
	test_case("power", t_power);
	test_case("power_refs", t_power_refs);
	test_case("power_parallel", t_power_parallel);
	test_case("names", t_names);
	test_case("companion_dep", t_companion_dep);
	test_case("not_ready", t_not_ready);
	test_summary_and_exit();
	return 0;
}
