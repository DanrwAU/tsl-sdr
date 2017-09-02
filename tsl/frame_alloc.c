/*
 *  frame_alloc.c - Simple block-oriented concurrent memory allocator.
 *
 *  Copyright (c)2017 Phil Vachon <phil@security-embedded.com>
 *
 *  This file is a part of The Standard Library (TSL)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <tsl/frame_alloc.h>

#include <tsl/assert.h>
#include <tsl/diag.h>
#include <tsl/errors.h>
#include <tsl/safe_alloc.h>

#include <sys/mman.h>
#include <errno.h>

#include <stdatomic.h>

#include <ck_pr.h>

#ifdef __ARM_ARCH
#include <tsl/atomic_arm.h>
#endif

#include <stdlib.h>


#define _FRAME_ALLOC_COUNTS

#if defined(__x86_64__) || defined(__ARM_ARCH_ISA_A64) || defined(__aarch64__)
typedef uint64_t counter_t;
#elif defined(__ARM_ARCH)
typedef uint32_t counter_t;
#else
#error Unknown target CPU architecture, aborting.
#endif

struct frame_alloc_free {
    struct frame_alloc_free *next;
} CAL_CACHE_ALIGNED;

struct frame_alloc_head {
    struct frame_alloc_free *free;
    counter_t counter;
} CAL_PACKED;

struct frame_alloc {
    struct frame_alloc_head hd CAL_ALIGN(16);

    void *rgn;
    size_t rgn_len;
    size_t frame_size;
    size_t nr_frames;
#ifdef _FRAME_ALLOC_COUNTS
    size_t frames_outstanding;
    size_t nr_frees;
    size_t nr_allocs;
#endif
} CAL_CACHE_ALIGNED;

aresult_t frame_alloc_get_counts(struct frame_alloc *alloc, size_t *nr_frees, size_t *nr_allocs)
{
    aresult_t ret = A_OK;

    TSL_ASSERT_ARG_DEBUG(NULL != alloc);
    TSL_ASSERT_ARG_DEBUG(NULL != nr_frees);
    TSL_ASSERT_ARG_DEBUG(NULL != nr_allocs);

#ifdef _FRAME_ALLOC_COUNTS
    *nr_frees = alloc->nr_frees;
    *nr_allocs = alloc->nr_allocs;
#endif

    return ret;
}

static
aresult_t _frame_alloc_pop_free(struct frame_alloc *alloc, void **top_free)
{
    aresult_t ret = A_OK;

    struct frame_alloc_head cur CAL_ALIGN(SYS_CACHE_LINE_LENGTH),
                            new CAL_ALIGN(SYS_CACHE_LINE_LENGTH);
    struct frame_alloc_free *sn = NULL;

    TSL_ASSERT_ARG_DEBUG(NULL != alloc);
    TSL_ASSERT_ARG_DEBUG(NULL != top_free);

    ck_pr_load_ptr_2(&alloc->hd, &cur);

    if (NULL == cur.free) {
        DIAG("no more space in allocator");
        ret = A_E_NOMEM;
        goto done;
    }

    do {
        sn = cur.free;
        new.free = cur.free->next;
        new.counter = cur.counter + 1;
    } while (!ck_pr_cas_ptr_2_value(&alloc->hd, &cur, &new, &cur));

#ifdef _FRAME_ALLOC_COUNTS
    atomic_fetch_add(&alloc->nr_allocs, 1);
#endif

    *top_free = sn;

done:
    return ret;
}

static
aresult_t _frame_alloc_push_free(struct frame_alloc *alloc, void *new_top)
{
    aresult_t ret = A_OK;

    struct frame_alloc_head head CAL_ALIGN(SYS_CACHE_LINE_LENGTH),
                            new CAL_ALIGN(SYS_CACHE_LINE_LENGTH);
    struct frame_alloc_free *phead = NULL;

    TSL_ASSERT_ARG_DEBUG(NULL != alloc);
    TSL_ASSERT_ARG_DEBUG(NULL != new_top);

    new.free = new_top;
    phead = new_top;

    ck_pr_load_ptr_2(&alloc->hd, &head);

    do {
        /* Splice in the new head of the free list */
        phead->next = head.free;
        new.counter = head.counter + 1;
    } while (!ck_pr_cas_ptr_2_value(&alloc->hd, &head, &new, &head));

#ifdef _FRAME_ALLOC_COUNTS
    atomic_fetch_add(&alloc->nr_frees, 1);
#endif

    return ret;
}

static
aresult_t _frame_alloc_init(struct frame_alloc *alloc)
{
    aresult_t ret = A_OK;

    TSL_ASSERT_ARG_DEBUG(NULL != alloc);

    for (void *ptr = alloc->rgn; ptr < (alloc->rgn + alloc->rgn_len); ptr += alloc->frame_size) {
        struct frame_alloc_free *itm = ptr;

        itm->next = alloc->hd.free;
        alloc->hd.free = itm;
    }

    return ret;
}

aresult_t frame_alloc_new(struct frame_alloc **palloc, size_t frame_bytes, size_t nr_frames)
{
    aresult_t ret = A_OK;

    struct frame_alloc *fa = NULL;
    size_t rgn_size = 0;
    void *rgn = MAP_FAILED;

    TSL_ASSERT_ARG(NULL != palloc);
    TSL_ASSERT_ARG(0 < frame_bytes);
    TSL_ASSERT_ARG(0 < nr_frames);

    frame_bytes = (frame_bytes + SYS_CACHE_LINE_LENGTH - 1) & (~(SYS_CACHE_LINE_LENGTH - 1));

    DIAG("Creating new frame allocator, %zu frames of %zu bytes",
            nr_frames, frame_bytes);

    *palloc = NULL;

    if (frame_bytes < sizeof(struct frame_alloc_free)) {
        frame_bytes = sizeof(struct frame_alloc_free);
    }

    /* Allocate the frame allocator object */
    if (FAILED(ret = TZAALLOC(fa, SYS_CACHE_LINE_LENGTH))) {
        goto done;
    }

    rgn_size = nr_frames * frame_bytes;

    /* Allocate the frame allocator memory behind it */
    if (MAP_FAILED == (rgn = mmap(NULL, rgn_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)))
    {
        PDIAG("Could not allocate %zu bytes of pages.", rgn_size);
        goto done;
    }

    fa->rgn = rgn;
    fa->rgn_len = rgn_size;
    fa->frame_size = frame_bytes;
    fa->nr_frames = nr_frames;

    TSL_BUG_IF_FAILED(_frame_alloc_init(fa));

    *palloc = fa;

done:
    if (FAILED(ret)) {
        TFREE(fa);

        if (MAP_FAILED != rgn) {
            munmap(rgn, rgn_size);
            rgn = MAP_FAILED;
            rgn_size = 0;
        }
    }
    return ret;
}

aresult_t frame_alloc_delete(struct frame_alloc **palloc)
{
    aresult_t ret = A_OK;

    struct frame_alloc *alloc = NULL;

    TSL_ASSERT_ARG(NULL != palloc);
    TSL_ASSERT_ARG(NULL != *palloc);

    alloc = *palloc;

    if (NULL != alloc->rgn) {
        munmap(alloc->rgn, alloc->rgn_len);
        alloc->rgn = NULL;
        alloc->rgn_len = 0;
    }

    TFREE(*palloc);

    return ret;
}

aresult_t frame_alloc(struct frame_alloc *alloc, void **pframe)
{
    aresult_t ret = A_OK;

    TSL_ASSERT_ARG(NULL != alloc);
    TSL_ASSERT_ARG(NULL != pframe);

    *pframe = NULL;

    ret = _frame_alloc_pop_free(alloc, pframe);

    return ret;
}

aresult_t frame_free(struct frame_alloc *alloc, void **pframe)
{
    aresult_t ret = A_OK;

    TSL_ASSERT_ARG(NULL != alloc);
    TSL_ASSERT_ARG(NULL != pframe);
    TSL_ASSERT_ARG(NULL != *pframe);

    if (FAILED_UNLIKELY(ret = _frame_alloc_push_free(alloc, *pframe))) {
        goto done;
    }

    *pframe = NULL;

done:
    return ret;
}

aresult_t frame_alloc_get_frame_size(struct frame_alloc *alloc, size_t *pframe_bytes)
{
    aresult_t ret = A_OK;

    TSL_ASSERT_ARG(NULL != alloc);
    TSL_ASSERT_ARG(NULL != pframe_bytes);

    *pframe_bytes = alloc->frame_size;

    return ret;
}

