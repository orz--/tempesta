/**
 *		Tempesta FW
 *
 * Stack-like region-based memory manager.
 *
 * Pools are used per message (e.g. HTTP request or response).
 * Since Tempesta handles a message only on one CPU, then the pool should not
 * bother about concurrency.
 *
 * The allocator is pure Tempesta specific which is perfect for current
 * workload (i.e. should be reviewed when new usage patterns appear):
 * 1. the typical allocation sequence is:
 *    (a) HTTP message is allocated on the same page as the pool itself;
 *    (b) TfwHttpHdrTbl follows the message descriptor;
 *    (c) many TfwStr's for each HTTP field;
 * 2. TfwHttpHdrTbl can be rarely reallocated - since it's followed by TfwStr
 *    allocations we always lose it's memory. The table grows exponentilly
 *    minimizing number of reallocations and Frang controls number and size
 *    of the reallocations;
 * 3. TfwStr grows while we're reading it, so this is last allocation when we
 *    reallocate it - the operation is pretty fast for current allocation.
 *    Duplicate HTTP headers are likely introduces heavy reallocations like
 *    (2) with losing memory - they're also rare;
 * 4. sometimes we need temporal buffers to do something - the buffers should
 *    be immediately freed to keep stack-like memory management.
 *
 * Copyright (C) 2014 NatSys Lab. (info@natsys-lab.com).
 * Copyright (C) 2015 Tempesta Technologies, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "pool.h"
#include <stdlib.h>

#define TFW_POOL_CHUNK_SZ(p)	(PAGE_SIZE << (p)->order)
#define TFW_POOL_CHUNK_BASE(c)	((unsigned long)(c) & PAGE_MASK)
#define TFW_POOL_CHUNK_END(p)	(TFW_POOL_CHUNK_BASE((p)->curr) + (p)->off)
#define TFW_POOL_ALIGN_SZ(n)	(((n) + 7) & ~7UL)
#define TFW_POOL_HEAD_OFF	(TFW_POOL_ALIGN_SZ(sizeof(TfwPool))	\
				 + TFW_POOL_ALIGN_SZ(sizeof(TfwPoolChunk)))
#define TFW_POOL_PGCACHE_SZ	512

static unsigned int get_order(unsigned long p)  {
    p>>=12;
    unsigned int result = 0;
    while(p & 1) {
        ++result;
        p>>=1;
    }
    return result;
}

static unsigned long
tfw_pool_alloc_pages(unsigned int order)
{
    void * result = 0;
    if (posix_memalign(&result, PAGE_SIZE, PAGE_SIZE<<order))
        return 0;
    return (long)result;
}

static void
tfw_pool_free_pages(unsigned long addr, unsigned int order)
{
    free((void*)addr);
}

void *
tfw_pool_alloc(TfwPool *p, size_t n)
{
	void *a;

	n = TFW_POOL_ALIGN_SZ(n);

	if (unlikely(p->off + n > TFW_POOL_CHUNK_SZ(p))) {
		TfwPoolChunk *c, *curr = p->curr;
		unsigned int off = TFW_POOL_ALIGN_SZ(sizeof(TfwPoolChunk)) + n;
		unsigned int order = get_order(off);

        c = (TfwPoolChunk *)tfw_pool_alloc_pages(order);
        if (!c)
            return NULL;

        c->next = curr;
		c->order = order;

		curr->off = p->off;

		p->order = order;
		p->off = off;
		p->curr = c;

		return (void *)TFW_POOL_ALIGN_SZ((unsigned long)(c + 1));
	}

	a = (char *)TFW_POOL_CHUNK_END(p);
	p->off += n;

	return a;
}
EXPORT_SYMBOL(tfw_pool_alloc);

void *
tfw_pool_realloc(TfwPool *p, void *ptr, size_t old_n, size_t new_n)
{
	void *a;

	BUG_ON(new_n < old_n);

	old_n = TFW_POOL_ALIGN_SZ(old_n);
	new_n = TFW_POOL_ALIGN_SZ(new_n);

	if ((char *)ptr + old_n == (char *)TFW_POOL_CHUNK_END(p)
	    && p->off - old_n + new_n <= TFW_POOL_CHUNK_SZ(p))
	{
		p->off += new_n - old_n;
		return ptr;
	}

	a = tfw_pool_alloc(p, new_n);
    if (likely(a != NULL))
		memcpy(a, ptr, old_n);

	return a;
}
EXPORT_SYMBOL(tfw_pool_realloc);

/**
 * It's good to call the function against just allocated chunk in stack-manner.
 * Consequent free calls can empty the whole pool but the first chunk with
 * the pool header.
 */
void
tfw_pool_free(TfwPool *p, void *ptr, size_t n)
{
	n = TFW_POOL_ALIGN_SZ(n);

	/* Stack-like usage is expected. */
	if (unlikely((char *)ptr + n != (char *)TFW_POOL_CHUNK_END(p)))
		return;

	p->off -= n;

	/* Free empty chunk which doesn't contain the pool header. */
	if (unlikely(p->off == TFW_POOL_ALIGN_SZ(sizeof(TfwPoolChunk)))) {
		TfwPoolChunk *next = p->curr->next;
		tfw_pool_free_pages(TFW_POOL_CHUNK_BASE(p->curr), p->order);
		p->curr = next;
		p->order = next->order;
		p->off = next->off;
	}
}
EXPORT_SYMBOL(tfw_pool_free);

/**
 * Allocate bit more pages than we need.
 */
TfwPool *
__tfw_pool_new(size_t n)
{
	TfwPool *p;
	TfwPoolChunk *c;
	unsigned int order;

	order = get_order(TFW_POOL_ALIGN_SZ(n) + TFW_POOL_HEAD_OFF);

    c = (TfwPoolChunk *)tfw_pool_alloc_pages(order);
    if (unlikely(!c))
        return NULL;

	p = (TfwPool *)((char *)c + TFW_POOL_ALIGN_SZ(sizeof(*c)));

	c->next = NULL;
	p->order = c->order = order;
	p->off = c->off = TFW_POOL_HEAD_OFF;
	p->curr = c;

	return p;
}
EXPORT_SYMBOL(__tfw_pool_new);

void
tfw_pool_destroy(TfwPool *p)
{
	TfwPoolChunk *c, *next;

	if (!p)
		return;

	for (c = p->curr; c; c = next) {
		next = c->next;
        tfw_pool_free_pages(TFW_POOL_CHUNK_BASE(c), c->order);
    }
}
EXPORT_SYMBOL(tfw_pool_destroy);

int
tfw_pool_init(void)
{
	return 0;
}

void
tfw_pool_exit(void)
{
}
