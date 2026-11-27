/* Host test of the I/O APIC line vector policy (kernel/ke/irq_gsi.c):
 * which vector a GSI gets, given the vectors already taken.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include <kernel/ke/irq.h>

static unsigned char g_busy[256];

static int busy(int v, void *ctx)
{
	(void)ctx;
	T_ASSERT(v >= 32 && v < 256);
	return g_busy[v];
}

/* Assign like irq.c does: the chosen vector is taken from then on. */
static int assign(int gsi)
{
	int v = irq_gsi_choose_vector(gsi, busy, NULL);

	if (v > 0)
		g_busy[v] = 1;
	return v;
}

static void reset(void)
{
	kmemset(g_busy, 0, sizeof(g_busy));
}

static void t_fixed(void)
{
	static const int fixed[] = { 32, 47, 58, 59, 60, 61, 62 };

	for (unsigned int i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++)
		T_ASSERT(irq_vector_is_fixed(fixed[i]));
	T_ASSERT(!irq_vector_is_fixed(33)); /* PS/2 keyboard, GSI 1's own */
	T_ASSERT(!irq_vector_is_fixed(44)); /* PS/2 aux, GSI 12's own */
	T_ASSERT(!irq_vector_is_fixed(48));
	T_ASSERT(!irq_vector_is_fixed(63));
	T_ASSERT(!irq_vector_is_fixed(57));
}

static void t_natural(void)
{
	reset();
	/* low lines keep 32 + GSI: the legacy INTx arms match on it */
	T_EQ(assign(1), 33);
	T_EQ(assign(9), 41);
	T_EQ(assign(11), 43);
	T_EQ(assign(16), 48);
	T_EQ(assign(17), 49);
	T_EQ(assign(23), 55);
	T_EQ(assign(25), 57);
}

static void t_relocated(void)
{
	reset();
	/* fixed natural vectors: the LAPIC timer and error vectors */
	T_EQ(assign(0), IRQ_DYN_VECTOR_FIRST);
	T_EQ(assign(15), IRQ_DYN_VECTOR_FIRST + 1);
	/* 26..30 would land on the SCI, the NIC MSI and the HPET vectors */
	for (int g = 26; g <= 30; g++) {
		int v = assign(g);

		T_ASSERT(v >= IRQ_DYN_VECTOR_FIRST && v <= IRQ_DYN_VECTOR_LAST);
		T_ASSERT(!irq_vector_is_fixed(v));
	}
	/* above the natural range: the dynamic range, lowest free first,
	 * never the identity vector inside it */
	reset();
	g_busy[IRQ_DYN_VECTOR_FIRST] = 1; /* an MSI took it */
	T_EQ(assign(40), IRQ_DYN_VECTOR_FIRST + 1);
	T_EQ(assign(31), IRQ_DYN_VECTOR_FIRST + 2);
	T_EQ(assign(200), IRQ_DYN_VECTOR_FIRST + 3);
	/* a natural vector that is taken: relocated too */
	g_busy[42] = 1;
	T_EQ(assign(10), IRQ_DYN_VECTOR_FIRST + 4);
}

static void t_exhausted_and_bad(void)
{
	reset();
	for (int v = IRQ_DYN_VECTOR_FIRST; v <= IRQ_DYN_VECTOR_LAST; v++)
		g_busy[v] = 1;
	T_EQ(assign(40), -1);
	T_EQ(assign(0), -1);
	T_EQ(assign(5), 37); /* the natural one is still there */
	T_EQ(irq_gsi_choose_vector(-1, busy, NULL), -1);
	T_EQ(irq_gsi_choose_vector(IRQ_GSI_MAX, busy, NULL), -1);
	T_EQ(irq_gsi_choose_vector(3, NULL, NULL), -1);
}

/* Every GSI, in any order: never a fixed vector, never twice the same. */
static void t_all_lines(void)
{
	unsigned char seen[256];
	int n = 0;

	reset();
	kmemset(seen, 0, sizeof(seen));
	for (int g = IRQ_GSI_MAX - 1; g >= 0; g--) {
		int v = assign(g);

		if (v < 0)
			continue;
		T_ASSERT(!irq_vector_is_fixed(v));
		T_EQ(seen[v], 0);
		seen[v] = 1;
		n++;
		if (g <= IRQ_GSI_NATURAL_LAST && !irq_vector_is_fixed(32 + g))
			T_EQ(v, 32 + g);
	}
	/* the dynamic range ran out before the high lines did */
	T_EQ(n, (IRQ_DYN_VECTOR_LAST - IRQ_DYN_VECTOR_FIRST + 1) + 24);
}

int main(void);
int main(void)
{
	kshim_init_main();
	test_case("fixed", t_fixed);
	test_case("natural", t_natural);
	test_case("relocated", t_relocated);
	test_case("exhausted_and_bad", t_exhausted_and_bad);
	test_case("all_lines", t_all_lines);
	test_summary_and_exit();
}
