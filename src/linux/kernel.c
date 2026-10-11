/**
 * kernel.c - Linux kernel services: memory caches, wait queues, string helpers.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include <linux/errno.h>
#include "linux_internal.h"

struct task_struct current_task = {
    /* pid  */ 0,
    /* tid  */ 1,
    /* comm */ "current\0",
    /* journal_info */ NULL
};
struct task_struct *current = &current_task;

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, kzalloc)
#endif

void *kzalloc(size_t size, int flags)
{
    UNREFERENCED_PARAMETER(flags);
    void *buffer = kmalloc(size, flags);
    if (buffer) {
        memset(buffer, 0, size);
    }
    return buffer;
}

/* slab routines */

kmem_cache_t *
kmem_cache_create(
    const char *    name,
    size_t          size,
    size_t          offset,
    unsigned long   flags,
    kmem_cache_cb_t ctor
)
{
    UNREFERENCED_PARAMETER(offset);
    UNREFERENCED_PARAMETER(flags);
    kmem_cache_t *kc = NULL;

    kc = kmalloc(sizeof(kmem_cache_t), GFP_KERNEL);
    if (kc == NULL) {
        goto errorout;
    }

    memset(kc, 0, sizeof(kmem_cache_t));
    ExInitializeNPagedLookasideList(
        &kc->la,
        NULL,
        NULL,
        0,
        size,
        'JBKC',
        0);

    kc->size = (ULONG)size;
    strncpy(kc->name, name, 31);
    kc->constructor = ctor;

errorout:

    return kc;
}

int kmem_cache_destroy(kmem_cache_t * kc)
{
    /* like Linux: destroying a cache that was never created is a no-op
       (jbd2 tears down its slab table unconditionally on unload) */
    if (kc == NULL) {
        return 0;
    }

    ExDeleteNPagedLookasideList(&(kc->la));
    kfree(kc);

    return 0;
}

void* kmem_cache_alloc(kmem_cache_t *kc, int flags)
{
    UNREFERENCED_PARAMETER(flags);
    PVOID  ptr = NULL;
    ptr = ExAllocateFromNPagedLookasideList(&(kc->la));
    if (ptr) {
        atomic_inc(&kc->count);
        atomic_inc(&kc->acount);
    }
    return ptr;
}

void kmem_cache_free(kmem_cache_t *kc, void *p)
{
    if (p) {
        atomic_dec(&kc->count);
        ExFreeToNPagedLookasideList(&(kc->la), p);
    }
}

/* wait queue routines */

void init_waitqueue_head(wait_queue_head_t *q)
{
    spin_lock_init(&q->lock);
    INIT_LIST_HEAD(&q->task_list);
}

int wake_up(wait_queue_head_t *queue)
{
    UNREFERENCED_PARAMETER(queue);
    return 0; /* KeSetEvent(&wait->event, 0, FALSE); */
}

void iput(struct inode *inode)
{
    if (atomic_dec_and_test(&inode->i_count)) {
        kfree(inode);
    }
}

/* string functions from linux/lib/string.c */

#if _MSC_VER > 1900

int _strnicmp(const char* str1, const char* str2, size_t count)
{
	unsigned int c1, c2;

	if (!count)
		return 0;

	do {
		c1 = *str1++;
		c2 = *str2++;
		if (!c1 || !c2)
			break;
		if (c1 == c2)
			continue;
		c1 = tolower(c1);
		c2 = tolower(c2);
		if (c1 != c2)
			break;
	} while (--count);
	return (int)c1 - (int)c2;
}

#if !defined(_M_ARM64) && !defined(_M_ARM)

/* Newer MSVC toolsets (VS 2022 17.x and later) treat strncmp/strncpy as
   intrinsics, which makes redefining them a C2169 error. Force a real function
   so these kernel-side implementations are used as before. */
#pragma function(strncmp, strncpy)

int strncmp(const char* str1, const char* str2, size_t count)
{
	unsigned char c1, c2;

	while (count) {
		c1 = *str1++;
		c2 = *str2++;
		if (c1 != c2)
			return c1 < c2 ? -1 : 1;
		if (!c1)
			break;
		count--;
	}
	return 0;
}

char* strncpy(char* dest, const char* src, size_t count)
{
	char *p = dest;

	while (count) {
		if ((*p = *src) != 0)
			src++;
		p++;
		count--;
	}
	return dest;
}

#endif

#endif

/* initialzer and destructor */

int
ext2_init_linux()
{
    int rc = 0;

    rc = ext2_init_bh();
    if (rc != 0) {
        goto errorout;
    }

errorout:

    return rc;
}

void
ext2_destroy_linux()
{
    ext2_destroy_bh();
}
