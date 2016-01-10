/**
 *		Tempesta FW
 *
 * Copyright (C) 2016 Tempesta Technologies, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */
#include <linux/interrupt.h>
#include <linux/percpu.h>
#include <linux/slab.h>

#include "work_queue.h"

/*
 * ------------------------------------------------------------------------
 *	MPMC queue on lock-free ring buffer.
 *
 * INVARIANTS:
 * 	1. tail <= head
 * 	2. last_tail <= tail
 * 	3. last_head <= head
 * 	4. tail == head <=> queue is empty
 * 	5. tail + size == head <=> queue is full
 * 	6. head <= last_tail + size
 * 	7. tail <= last_head
 *
 * 	2,6,7: head <= last_head + size
 *	3,6,7: tail <= last_tail + size
 *
 * ------------------------------------------------------------------------
 */
#define QSZ		1024
#define QMASK		(QSZ - 1)

static int
tfw_q_init(TfwRBQueue *q, int node)
{
	int cpu;

	q->thr_pos = alloc_percpu(struct __ThrPos);
	if (!q->thr_pos)
		return -ENOMEM;
	for_each_possible_cpu(cpu) {
		__ThrPos *pos = per_cpu_ptr(q->thr_pos, cpu);
		atomic64_set(&pos->head, LLONG_MAX);
		atomic64_set(&pos->tail, LLONG_MAX);
	}

	atomic64_set(&q->head, 0);
	atomic64_set(&q->tail, 0);
	atomic64_set(&q->last_head, 0);
	atomic64_set(&q->last_tail, 0);

	q->array = kmalloc_node(QSZ * sizeof(void *), GFP_KERNEL, node);
	if (!q->array) {
		free_percpu(q->thr_pos);
		return -ENOMEM;
	}

	return 0;
}

static void
tfw_q_destroy(TfwRBQueue *q)
{
	kfree(q->array);
	free_percpu(q->thr_pos);
}

/**
 * Called when the caller need to wait for tail or head guard progress.
 * Move head and tail guards simultaneously to reduce overall number of the
 * per-cpu guards array traversing and number of slow path passing in callers.
 */
static void
__update_guards(TfwRBQueue *q)
{
	long long last_head = atomic64_read(&q->head);
	long long last_tail = atomic64_read(&q->tail);
	int cpu;

	/* Don't support switching off cpus in runtime. */
	for_each_online_cpu(cpu) {
		__ThrPos *pos = per_cpu_ptr(q->thr_pos, cpu);
		long long curr_h = atomic64_read(&pos->head);
		long long curr_t = atomic64_read(&pos->tail);

		/* Force compiler to use curr_h and curr_t only once. */
		barrier();

		if (curr_h < last_head)
			last_head = curr_h;
		if (curr_t < last_tail)
			last_tail = curr_t;
	}

	/*
	 * Avoid unnecessary cache lines bouncing: write to the shared memory
	 * only if head and tail pointers were changed.
	 */
	if (atomic64_read(&q->last_head) != last_head)
		atomic64_set(&q->last_head, last_head);
	if (atomic64_read(&q->last_tail) != last_tail)
		atomic64_set(&q->last_tail, last_tail);
}

/**
 * @return false if the queue is full and true otherwise.
 */
static int
tfw_q_push(TfwRBQueue *q, void *ptr)
{
	unsigned long long head;
	__ThrPos *pos;

	/*
	 * Producers can run on the same CPU (softirq and user space process),
	 * so they will write to the same q->thr_pos[cpu_id].
	 * This way we have to disable preemtion.
	 */
	local_bh_disable();

	pos = this_cpu_ptr(q->thr_pos);

	while (1) {
		head = atomic64_read(&q->head);

		if (unlikely(head >= atomic64_read(&q->last_tail) + QSZ)) {
			__update_guards(q);
			/* Second try. */
			if (head >= atomic64_read(&q->last_tail) + QSZ) {
				/* The queue is full, don't wait consumers. */
				local_bh_enable();
				atomic64_set(&pos->head, LLONG_MAX);
				return -ENOMEM;
			}
		}

		/* Set a guard for current position and move global head. */
		atomic64_set(&pos->head, head);
		if (atomic64_cmpxchg(&q->head, head, head + 1) == head)
			break;
	}

	q->array[head & QMASK] = ptr;

	atomic64_set(&pos->head, LLONG_MAX);

	local_bh_enable();

	return 0;
}

/**
 * N producers, M consumers. Used for work queue.
 *
 * @return NULL on shutdown only.
 */
static void *
tfw_q_pop(TfwRBQueue *q)
{
	unsigned long long tail;
	__ThrPos *pos;
	void *ret = NULL;

	local_bh_disable();

	pos = this_cpu_ptr(q->thr_pos);

	while (1) {
		tail = atomic64_read(&q->tail);

		if (unlikely(tail >= atomic64_read(&q->last_head))) {
			__update_guards(q);
			/* Second try. */
			if (tail >= atomic64_read(&q->last_head))
				/* The queue is empty, don't wait producers. */
				goto out;
		}

		/* Set a guard for current position and move global head. */
		atomic64_set(&pos->tail, tail);
		if (atomic64_cmpxchg(&q->tail, tail, tail + 1) == tail)
			break;
	}

	ret = q->array[tail & QMASK];
out:
	atomic64_set(&pos->tail, LLONG_MAX);

	local_bh_enable();

	return ret;
}

/*
 * ------------------------------------------------------------------------
 * 	SoftIRQ work queue
 *
 * TODO should be per-cpu, and correspondingly linked list should be used.
 * ------------------------------------------------------------------------
 */
int
tfw_wq_si_init(TfwRBQueue *wq)
{
	return tfw_q_init(wq, numa_node_id());
}

void
tfw_wq_si_destroy(TfwRBQueue *wq)
{
	tfw_q_destroy(wq);
}

int
tfw_wq_si_push(TfwRBQueue *wq, void *ptr)
{
	int r = tfw_q_push(wq, ptr);

	if (unlikely(!in_softirq()))
		raise_softirq(NET_TX_SOFTIRQ);

	return r;
}

void *
tfw_wq_si_pop(TfwRBQueue *wq)
{
	return tfw_q_pop(wq);
}