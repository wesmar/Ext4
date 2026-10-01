/**
 * jbd2_revoke.c - jbd2 revoke table used during replay.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 * Linux jbd2 origin: Written by Stephen C. Tweedie <sct@redhat.com>, 2000.
 * Copyright 2000 Red Hat corp --- All Rights Reserved
 */

#include <linux/fs.h>
#include <linux/jbd2.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/log2.h>

//#include <linux/hash.h>

static struct kmem_cache *jbd2_revoke_record_cache;
static struct kmem_cache *jbd2_revoke_table_cache;

/* Each revoke record represents one single revoked block.  During
   journal replay, this involves recording the transaction ID of the
   last transaction to revoke this block. */

struct jbd2_revoke_record_s
{
	struct list_head  hash;
	tid_t		  sequence;	/* Used for recovery only */
	unsigned long long	  blocknr;
};

/* The revoke table is just a simple hash table of revoke records. */
struct jbd2_revoke_table_s
{
	/* It is conceivable that we might want a larger hash table
	 * for recovery.  Must be a power of two. */
	int		  hash_size;
	int		  hash_shift;
	struct list_head *hash_table;
};

/* Utility functions to maintain the revoke table */

#define GOLDEN_RATIO_32 0x61C88647

static inline u32 __hash_32(u32 val)
{
	return val * GOLDEN_RATIO_32;
}

static inline u32 hash_32(u32 val, unsigned int bits)
{
	/* High bits are more random, so use them. */
	return __hash_32(val) >> (32 - bits);
}

static inline u32 hash_64(u64 val, unsigned int bits)
{
	/* Hash 64 bits using only 32x32-bit multiply. */
	return hash_32((u32)val ^ __hash_32(val >> 32), bits);
}

static inline int hash(journal_t *journal, unsigned long long block)
{
	return hash_64(block, journal->j_revoke->hash_shift);
}

static int insert_revoke_hash(journal_t *journal, unsigned long long blocknr,
			      tid_t seq)
{
	struct list_head *hash_list;
	struct jbd2_revoke_record_s *record;
	gfp_t gfp_mask = GFP_NOFS;

	if (journal_oom_retry)
		gfp_mask |= __GFP_NOFAIL;
	record = kmem_cache_alloc(jbd2_revoke_record_cache, gfp_mask);
	if (!record)
		return -ENOMEM;

	record->sequence = seq;
	record->blocknr = blocknr;
	hash_list = &journal->j_revoke->hash_table[hash(journal, blocknr)];
	spin_lock(&journal->j_revoke_lock);
	list_add(&record->hash, hash_list);
	spin_unlock(&journal->j_revoke_lock);
	return 0;
}

/* Find a revoke record in the journal's hash table. */

static struct jbd2_revoke_record_s *find_revoke_record(journal_t *journal,
						      unsigned long long blocknr)
{
	struct list_head *hash_list;
	struct jbd2_revoke_record_s *record;

	hash_list = &journal->j_revoke->hash_table[hash(journal, blocknr)];

	spin_lock(&journal->j_revoke_lock);
	record = (struct jbd2_revoke_record_s *) hash_list->next;
	while (&(record->hash) != hash_list) {
		if (record->blocknr == blocknr) {
			spin_unlock(&journal->j_revoke_lock);
			return record;
		}
		record = (struct jbd2_revoke_record_s *) record->hash.next;
	}
	spin_unlock(&journal->j_revoke_lock);
	return NULL;
}

void jbd2_journal_destroy_revoke_caches(void)
{
	kmem_cache_destroy(jbd2_revoke_record_cache);
	jbd2_revoke_record_cache = NULL;
	kmem_cache_destroy(jbd2_revoke_table_cache);
	jbd2_revoke_table_cache = NULL;
}

int __init jbd2_journal_init_revoke_caches(void)
{
	J_ASSERT(!jbd2_revoke_record_cache);
	J_ASSERT(!jbd2_revoke_table_cache);

	jbd2_revoke_record_cache = KMEM_CACHE(jbd2_revoke_record_s,
					SLAB_HWCACHE_ALIGN|SLAB_TEMPORARY);
	if (!jbd2_revoke_record_cache)
		goto record_cache_failure;

	jbd2_revoke_table_cache = KMEM_CACHE(jbd2_revoke_table_s,
					     SLAB_TEMPORARY);
	if (!jbd2_revoke_table_cache)
		goto table_cache_failure;
	return 0;
table_cache_failure:
	jbd2_journal_destroy_revoke_caches();
record_cache_failure:
		return -ENOMEM;
}

static struct jbd2_revoke_table_s *jbd2_journal_init_revoke_table(int hash_size)
{
	int shift = 0;
	int tmp = hash_size;
	struct jbd2_revoke_table_s *table;

	table = kmem_cache_alloc(jbd2_revoke_table_cache, GFP_KERNEL);
	if (!table)
		goto out;

	while((tmp >>= 1UL) != 0UL)
		shift++;

	table->hash_size = hash_size;
	table->hash_shift = shift;
	table->hash_table =
		kmalloc_array(hash_size, sizeof(struct list_head), GFP_KERNEL);
	if (!table->hash_table) {
		kmem_cache_free(jbd2_revoke_table_cache, table);
		table = NULL;
		goto out;
	}

	for (tmp = 0; tmp < hash_size; tmp++)
		INIT_LIST_HEAD(&table->hash_table[tmp]);

out:
	return table;
}

static void jbd2_journal_destroy_revoke_table(struct jbd2_revoke_table_s *table)
{
	int i;
	struct list_head *hash_list;

	for (i = 0; i < table->hash_size; i++) {
		hash_list = &table->hash_table[i];
		J_ASSERT(list_empty(hash_list));
	}

	kfree(table->hash_table);
	kmem_cache_free(jbd2_revoke_table_cache, table);
}

/* Initialise the revoke table for a given journal to a given size. */
int jbd2_journal_init_revoke(journal_t *journal, int hash_size)
{
	J_ASSERT(journal->j_revoke_table[0] == NULL);
	J_ASSERT(is_power_of_2(hash_size));

	journal->j_revoke_table[0] = jbd2_journal_init_revoke_table(hash_size);
	if (!journal->j_revoke_table[0])
		goto fail0;

	journal->j_revoke_table[1] = jbd2_journal_init_revoke_table(hash_size);
	if (!journal->j_revoke_table[1])
		goto fail1;

	journal->j_revoke = journal->j_revoke_table[1];

	spin_lock_init(&journal->j_revoke_lock);

	return 0;

fail1:
	jbd2_journal_destroy_revoke_table(journal->j_revoke_table[0]);
	journal->j_revoke_table[0] = NULL;
fail0:
	return -ENOMEM;
}

/* Destroy a journal's revoke table.  The table must already be empty! */
void jbd2_journal_destroy_revoke(journal_t *journal)
{
	journal->j_revoke = NULL;
	if (journal->j_revoke_table[0])
		jbd2_journal_destroy_revoke_table(journal->j_revoke_table[0]);
	if (journal->j_revoke_table[1])
		jbd2_journal_destroy_revoke_table(journal->j_revoke_table[1]);
}

/*
 * Revoke support for recovery.
 *
 * Recovery needs to be able to:
 *
 *  record all revoke records, including the tid of the latest instance
 *  of each revoke in the journal
 *
 *  check whether a given block in a given transaction should be replayed
 *  (ie. has not been revoked by a revoke record in that or a subsequent
 *  transaction)
 *
 *  empty the revoke table after recovery.
 */

/*
 * First, setting revoke records.  We create a new revoke record for
 * every block ever revoked in the log as we scan it for recovery, and
 * we update the existing records if we find multiple revokes for a
 * single block.
 */

int jbd2_journal_set_revoke(journal_t *journal,
		       unsigned long long blocknr,
		       tid_t sequence)
{
	struct jbd2_revoke_record_s *record;

	record = find_revoke_record(journal, blocknr);
	if (record) {
		/* If we have multiple occurrences, only record the
		 * latest sequence number in the hashed record */
		if (tid_gt(sequence, record->sequence))
			record->sequence = sequence;
		return 0;
	}
	return insert_revoke_hash(journal, blocknr, sequence);
}

/*
 * Test revoke records.  For a given block referenced in the log, has
 * that block been revoked?  A revoke record with a given transaction
 * sequence number revokes all blocks in that transaction and earlier
 * ones, but later transactions still need replayed.
 */

int jbd2_journal_test_revoke(journal_t *journal,
			unsigned long long blocknr,
			tid_t sequence)
{
	struct jbd2_revoke_record_s *record;

	record = find_revoke_record(journal, blocknr);
	if (!record)
		return 0;
	if (tid_gt(sequence, record->sequence))
		return 0;
	return 1;
}

/*
 * Finally, once recovery is over, we need to clear the revoke table so
 * that it can be reused by the running filesystem.
 */

void jbd2_journal_clear_revoke(journal_t *journal)
{
	int i;
	struct list_head *hash_list;
	struct jbd2_revoke_record_s *record;
	struct jbd2_revoke_table_s *revoke;

	revoke = journal->j_revoke;

	for (i = 0; i < revoke->hash_size; i++) {
		hash_list = &revoke->hash_table[i];
		while (!list_empty(hash_list)) {
			record = (struct jbd2_revoke_record_s*) hash_list->next;
			list_del(&record->hash);
			kmem_cache_free(jbd2_revoke_record_cache, record);
		}
	}
}
