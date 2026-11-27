/* A small model of the ACPI namespace behind the ACPICA interfaces that
 * kernel/hal/acpi_dev.c and the GPIO drivers call (host/kshim).
 *
 * A test builds a tree of nodes: devices (with _HID/_CID/_UID/_ADR and
 * _STA), methods (an evaluation callback, or a fixed integer, or a
 * resource template for _CRS/_AEI/...) and power resources.  Handles are
 * node pointers.  Evaluation results are returned the way ACPICA returns
 * them: one allocation holding the object tree, freed with AcpiOsFree.
 *
 * Copyright (C) 2026 The LikeOS Project */
#ifndef HOST_KSHIM_ACPICA_MOCK_H
#define HOST_KSHIM_ACPICA_MOCK_H

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "kernel/hal/acpica/include/acpi.h"
#pragma GCC diagnostic pop

#define MOCK_MAX_CHILD_RES 24

struct mock_node;
/* Evaluate: fill *out (a tree the callback owns, copied by the mock) and
 * return AE_OK, or an error status.  `out' is NULL when the caller does
 * not want a result. */
typedef ACPI_STATUS (*mock_eval_fn)(struct mock_node *self,
				    const ACPI_OBJECT_LIST *args,
				    ACPI_OBJECT *out);

struct mock_node {
	char name[5];
	char path[128];
	struct mock_node *parent;
	struct mock_node *first_child, *next_sibling;
	ACPI_OBJECT_TYPE type;
	/* devices */
	const char *hid, *uid;
	const char *cids[4];
	int ncids;
	int has_adr;
	UINT64 adr;
	/* methods */
	mock_eval_fn eval;
	int has_int;
	UINT64 int_value;
	const ACPI_RESOURCE *res; /* resource template (EndTag last) */
	int nres;
	int res_stride; /* bytes from one descriptor to the next */
	void *priv;
	int calls; /* evaluations so far */
};

/* Reset the namespace (frees everything). */
void mock_ns_reset(void);
struct mock_node *mock_root(void);
/* A device below `parent' (NULL: \_SB). */
struct mock_node *mock_device(struct mock_node *parent, const char *name,
			      const char *hid, const char *uid);
void mock_add_cid(struct mock_node *dev, const char *cid);
void mock_set_adr(struct mock_node *dev, UINT64 adr);
/* Methods / named objects below `parent'. */
struct mock_node *mock_method(struct mock_node *parent, const char *name,
			      mock_eval_fn eval, void *priv);
struct mock_node *mock_int(struct mock_node *parent, const char *name,
			   UINT64 value);
struct mock_node *mock_resources(struct mock_node *parent, const char *name,
				 const ACPI_RESOURCE *res, int nres);
/* The same with descriptors `stride' bytes apart (room for interrupt
 * lists beyond the first entry). */
struct mock_node *mock_resources_stride(struct mock_node *parent,
					const char *name, const void *res,
					int nres, int stride);
struct mock_node *mock_power_resource(struct mock_node *parent,
				      const char *name);
struct mock_node *mock_lookup(const char *path);

/* Address space handlers installed through AcpiInstallAddressSpaceHandler. */
struct mock_region {
	ACPI_HANDLE device;
	ACPI_ADR_SPACE_TYPE space;
	ACPI_ADR_SPACE_HANDLER handler;
	void *context;
};
struct mock_region *mock_find_region(ACPI_HANDLE device,
				     ACPI_ADR_SPACE_TYPE space);

/* Resource builders (fill *r; strings/arrays must outlive the use). */
void mock_res_end(ACPI_RESOURCE *r);
void mock_res_gpio(ACPI_RESOURCE *r, int interrupt, int level, int polarity,
		   int shared, int wake, int pull, int iorestrict,
		   UINT16 debounce, const char *source, UINT16 *pins,
		   int npins);
void mock_res_i2c(ACPI_RESOURCE *r, UINT16 addr, UINT32 speed, int tenbit,
		  const char *source, int v2);
void mock_res_extirq(ACPI_RESOURCE *r, int level, int polarity, int shared,
		     int wake, const UINT32 *irqs, int n);
void mock_res_irq(ACPI_RESOURCE *r, int level, int polarity, int shared,
		  const UINT8 *irqs, int n);
void mock_res_mem32fixed(ACPI_RESOURCE *r, UINT32 addr, UINT32 len, int rw);
void mock_res_mem32(ACPI_RESOURCE *r, UINT32 min, UINT32 len, int rw);
void mock_res_addr64(ACPI_RESOURCE *r, UINT64 min, UINT64 max, UINT64 len,
		     UINT64 tra, int producer);
void mock_res_io(ACPI_RESOURCE *r, UINT16 min, UINT8 len);
void mock_res_fixeddma(ACPI_RESOURCE *r, UINT16 line, UINT16 chan);

/* Object builders for evaluation callbacks. */
static inline void mock_obj_int(ACPI_OBJECT *o, UINT64 v)
{
	o->Type = ACPI_TYPE_INTEGER;
	o->Integer.Value = v;
}
static inline void mock_obj_str(ACPI_OBJECT *o, const char *s)
{
	o->Type = ACPI_TYPE_STRING;
	o->String.Pointer = (char *)s;
	o->String.Length = (UINT32)kstrlen(s);
}
static inline void mock_obj_buf(ACPI_OBJECT *o, const void *b, UINT32 len)
{
	o->Type = ACPI_TYPE_BUFFER;
	o->Buffer.Pointer = (UINT8 *)b;
	o->Buffer.Length = len;
}
static inline void mock_obj_pkg(ACPI_OBJECT *o, ACPI_OBJECT *elems,
				UINT32 n)
{
	o->Type = ACPI_TYPE_PACKAGE;
	o->Package.Elements = elems;
	o->Package.Count = n;
}
static inline void mock_obj_ref(ACPI_OBJECT *o, ACPI_HANDLE h)
{
	o->Type = ACPI_TYPE_LOCAL_REFERENCE;
	o->Reference.ActualType = ACPI_TYPE_ANY;
	o->Reference.Handle = h;
}

#endif /* HOST_KSHIM_ACPICA_MOCK_H */
