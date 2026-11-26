/* Per-instance harness state shared by kstubs.c and glue.c (kernel side
 * of the network harness, host/test-net.sh).
 *
 * Copyright (C) 2026 The LikeOS Project */
#ifndef HOST_NET_KSTUBS_H
#define HOST_NET_KSTUBS_H

extern int g_inst;
extern uint64_t test_warnings;
extern int test_quiet;
extern uint64_t test_kalloc_live, test_kalloc_bytes, test_kalloc_peak;
extern uint64_t test_softirq_runs[];
extern uint64_t test_yields;

void test_task_init(void);
void test_alloc_fail(uint64_t nth, uint64_t every);
void test_skb_fail(uint64_t nth, uint64_t every);
void softirq_drain(void);
extern uint64_t test_slab_irqoff;
extern int test_slab_strict;
extern int test_sip12_fixed;
extern uint32_t test_sip12_value;
struct wait_queue_head;
int glue_sock_index(struct wait_queue_head *h);

#endif
