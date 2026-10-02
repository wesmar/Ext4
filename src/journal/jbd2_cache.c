/**
 * jbd2_cache.c - jbd2 slab caches and module initialisation.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 * Linux jbd2 origin: Written by Stephen C. Tweedie <sct@redhat.com>, 1998.
 * Copyright 1998 Red Hat corp --- All Rights Reserved
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/jbd2.h>
#include <linux/errno.h>
#include <linux/log2.h>
#include <linux/bitops.h>
#include "jbd2_internal.h"

/*
 * JBD memory management
 *
 * These functions are used to allocate block-sized chunks of memory
 * used for making copies of buffer_head data.  Very often it will be
 * page-sized chunks of data, but sometimes it will be in
 * sub-page-size chunks.  (For example, 16k pages on Power systems
 * with a 4k block file system.)  For blocks smaller than a page, we
 * use a SLAB allocator.  There are slab caches for each block size,
 * which are allocated at mount time, if necessary, and we only free
 * (all of) the slab caches when/if the jbd2 module is unloaded.  For
 * this reason we don't need to a mutex to protect access to
 * jbd2_slab[] allocating or releasing memory; only in
 * jbd2_journal_create_slab().
 */
#define JBD2_MAX_SLABS 8
static struct kmem_cache *jbd2_slab[JBD2_MAX_SLABS];

static const char *jbd2_slab_names[JBD2_MAX_SLABS] = {
	"jbd2_1k", "jbd2_2k", "jbd2_4k", "jbd2_8k",
	"jbd2_16k", "jbd2_32k", "jbd2_64k", "jbd2_128k"
};

/*
 * Journal_head storage management
 */
static struct kmem_cache *jbd2_journal_head_cache;

#define jbd2_create_jbd_stats_proc_entry() do {} while (0)
#define jbd2_remove_jbd_stats_proc_entry() do {} while (0)

struct kmem_cache *jbd2_handle_cache, *jbd2_inode_cache;

static struct kmem_cache *transaction_cache;

/*
 * helper functions to deal with 32 or 64bit block numbers.
 */
size_t journal_tag_bytes(journal_t *journal)
{
	size_t sz;

	if (jbd2_has_feature_csum3(journal))
		return sizeof(journal_block_tag3_t);

	sz = sizeof(journal_block_tag_t);

	if (jbd2_has_feature_csum2(journal))
		sz += sizeof(__u16);

	if (jbd2_has_feature_64bit(journal))
		return sz;
	else
		return sz - sizeof(__u32);
}

static void jbd2_journal_destroy_slabs(void)
{
	int i;

	for (i = 0; i < JBD2_MAX_SLABS; i++) {
		kmem_cache_destroy(jbd2_slab[i]);
		jbd2_slab[i] = NULL;
	}
}

int jbd2_journal_create_slab(size_t size)
{
	/*static DEFINE_MUTEX(jbd2_slab_create_mutex);*/
    int i = ilog2(__roundup_pow_of_two((unsigned long)size)) - 10;
	size_t slab_size;

	if (size == PAGE_SIZE)
		return 0;

	if (i >= JBD2_MAX_SLABS)
		return -EINVAL;

	if (/*unlikely*/(i < 0))
		i = 0;
	/*mutex_lock(&jbd2_slab_create_mutex);*/
	if (jbd2_slab[i]) {
		/*mutex_unlock(&jbd2_slab_create_mutex);*/
		return 0;	/* Already created */
	}

	slab_size = 1ll << (i+10);
	jbd2_slab[i] = kmem_cache_create(jbd2_slab_names[i], slab_size,
					 slab_size, 0, NULL);
	/*mutex_unlock(&jbd2_slab_create_mutex);*/
	if (!jbd2_slab[i]) {
		printk(KERN_EMERG "JBD2: no memory for jbd2_slab cache\n");
		return -ENOMEM;
	}
	return 0;
}

static int jbd2_journal_init_journal_head_cache(void)
{
	int retval;

	J_ASSERT(jbd2_journal_head_cache == NULL);
	jbd2_journal_head_cache = kmem_cache_create("jbd2_journal_head",
				sizeof(struct journal_head),
				0,		/* offset */
				SLAB_TEMPORARY /*| SLAB_TYPESAFE_BY_RCU*/,
				NULL);		/* ctor */
	retval = 0;
	if (!jbd2_journal_head_cache) {
		retval = -ENOMEM;
		printk(KERN_EMERG "JBD2: no memory for journal_head cache\n");
	}
	return retval;
}

static void jbd2_journal_destroy_journal_head_cache(void)
{
	kmem_cache_destroy(jbd2_journal_head_cache);
	jbd2_journal_head_cache = NULL;
}

static int __init jbd2_journal_init_handle_cache(void)
{
	jbd2_handle_cache = KMEM_CACHE(jbd2_journal_handle, SLAB_TEMPORARY);
	if (jbd2_handle_cache == NULL) {
		printk(KERN_EMERG "JBD2: failed to create handle cache\n");
		return -ENOMEM;
	}
	jbd2_inode_cache = KMEM_CACHE(jbd2_inode, 0);
	if (jbd2_inode_cache == NULL) {
		printk(KERN_EMERG "JBD2: failed to create inode cache\n");
		kmem_cache_destroy(jbd2_handle_cache);
		return -ENOMEM;
	}
	return 0;
}

static void jbd2_journal_destroy_handle_cache(void)
{
	kmem_cache_destroy(jbd2_handle_cache);
	jbd2_handle_cache = NULL;
	kmem_cache_destroy(jbd2_inode_cache);
	jbd2_inode_cache = NULL;
}

/*
 * Module startup and shutdown
 */

static int __init journal_init_caches(void)
{
	int ret;

	ret = jbd2_journal_init_revoke_caches();
	if (ret == 0)
		ret = jbd2_journal_init_journal_head_cache();
	if (ret == 0)
		ret = jbd2_journal_init_handle_cache();
	if (ret == 0)
		ret = jbd2_journal_init_transaction_cache();
	return ret;
}

static void jbd2_journal_destroy_caches(void)
{
	jbd2_journal_destroy_revoke_caches();
	jbd2_journal_destroy_journal_head_cache();
	jbd2_journal_destroy_handle_cache();
	jbd2_journal_destroy_transaction_cache();
	jbd2_journal_destroy_slabs();
}

int __init journal_init(void)
{
	int ret;


	/*BUILD_BUG_ON(sizeof(struct journal_superblock_s) != 1024);*/

	ret = journal_init_caches();
	if (ret == 0) {
		jbd2_create_jbd_stats_proc_entry();
	} else {
		jbd2_journal_destroy_caches();
	}
	return ret;
}

void __exit journal_exit(void)
{
	jbd2_remove_jbd_stats_proc_entry();
	jbd2_journal_destroy_caches();
}

int __init jbd2_journal_init_transaction_cache(void)
{
	J_ASSERT(!transaction_cache);
	transaction_cache = kmem_cache_create("jbd2_transaction_s",
					sizeof(transaction_t),
					0,
					SLAB_HWCACHE_ALIGN|SLAB_TEMPORARY,
					NULL);
	if (transaction_cache)
		return 0;
	return -ENOMEM;
}

void jbd2_journal_destroy_transaction_cache(void)
{
	kmem_cache_destroy(transaction_cache);
	transaction_cache = NULL;
}
