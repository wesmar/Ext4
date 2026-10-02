/**
 * jbd2_journal.c - jbd2 journal object: superblock, load, wipe, abort, log start.
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


static void __journal_abort_soft (journal_t *journal, int err);

module_init(journal_init);
module_exit(journal_exit);

/* Checksumming functions */
static int jbd2_verify_csum_type(journal_t *j, journal_superblock_t *sb)
{
	if (!jbd2_journal_has_csum_v2or3_feature(j))
		return 1;

	return sb->s_checksum_type == JBD2_CRC32C_CHKSUM;
}

static __be32 jbd2_superblock_csum(journal_t *j, journal_superblock_t *sb)
{
	__u32 csum;
	__be32 old_csum;

	old_csum = sb->s_checksum;
	sb->s_checksum = 0;
	csum = jbd2_chksum(j, ~0U, (char *)sb, sizeof(journal_superblock_t));
	sb->s_checksum = old_csum;

	return cpu_to_be32(csum);
}

static int jbd2_superblock_csum_verify(journal_t *j, journal_superblock_t *sb)
{
	if (!jbd2_journal_has_csum_v2or3_feature(j))
		return 1;

	return sb->s_checksum == jbd2_superblock_csum(j, sb);
}

static void jbd2_superblock_csum_set(journal_t *j, journal_superblock_t *sb)
{
	if (!jbd2_journal_has_csum_v2or3_feature(j))
		return;

	sb->s_checksum = jbd2_superblock_csum(j, sb);
}

/*
 * Allocation code for the journal file.  Manage the space left in the
 * journal, so that we can begin checkpointing when appropriate.
 */

/*
 * Called with j_state_lock locked for writing.
 * Returns true if a transaction commit was started.
 */
int __jbd2_log_start_commit(journal_t *journal, tid_t target)
{
	/* Return if the txn has already requested to be committed */
	if (journal->j_commit_request == target)
		return 0;

	/*
	 * The only transaction we can possibly wait upon is the
	 * currently running transaction (if it exists).  Otherwise,
	 * the target tid must be an old one.
	 */
	if (journal->j_running_transaction &&
	    journal->j_running_transaction->t_tid == target) {
		/*
		 * We want a new commit: OK, mark the request and wakeup the
		 * commit thread.  We do _not_ do the commit ourselves.
		 */

		journal->j_commit_request = target;
		jbd_debug(1, "JBD2: requesting commit %d/%d\n",
			  journal->j_commit_request,
			  journal->j_commit_sequence);
		journal->j_running_transaction->t_requested = jiffies;
		wake_up(&journal->j_wait_commit);
		return 1;
	} else if (!tid_geq(journal->j_commit_request, target))
		/* This should never happen, but if it does, preserve
		   the evidence before kjournald goes into a loop and
		   increments j_commit_sequence beyond all recognition. */
		/*WARN_ONCE(1, "JBD2: bad log_start_commit: %u %u %u %u\n",
			  journal->j_commit_request,
			  journal->j_commit_sequence,
			  target, journal->j_running_transaction ? 
			  journal->j_running_transaction->t_tid : 0);*/;
	return 0;
}

int jbd2_log_start_commit(journal_t *journal, tid_t tid)
{
	int ret;

	ret = __jbd2_log_start_commit(journal, tid);
	return ret;
}

/*
 * Conversion of logical to physical block numbers for the journal
 *
 * On external journals the journal blocks are identity-mapped, so
 * this is a no-op.  If needed, we can use j_blk_offset - everything is
 * ready.
 */
int jbd2_journal_bmap(journal_t *journal, unsigned long blocknr,
		 unsigned long long *retp)
{
	int err = 0;
	unsigned long long ret;

	if (journal->j_inode) {
		ret = bmap(journal->j_inode, blocknr);
		if (ret)
			*retp = ret;
		else {
			/*printk(KERN_ALERT "%s: journal block not found "
					"at offset %lu on %s\n",
			       __func__, blocknr, journal->j_devname);*/
			err = -EIO;
			__journal_abort_soft(journal, err);
		}
	} else {
		*retp = blocknr; /* +journal->j_blk_offset */
	}
	return err;
}

static void jbd2_stats_proc_init(journal_t *journal)
{
    UNREFERENCED_PARAMETER(journal);
}

static void jbd2_stats_proc_exit(journal_t *journal)
{
    UNREFERENCED_PARAMETER(journal);
}

/*
 * Management for journal control blocks: functions to create and
 * destroy journal_t structures, and to initialise and read existing
 * journal blocks from disk.  */

/* First: create and setup a journal_t object in memory.  We initialise
 * very few fields yet: that has to wait until we have created the
 * journal structures from from scratch, or loaded them from disk. */

static journal_t *journal_init_common(struct block_device *bdev,
			struct block_device *fs_dev,
			unsigned long long start, int len, int blocksize)
{
	static struct lock_class_key jbd2_trans_commit_key;
	journal_t *journal;
	int err;
	struct buffer_head *bh;
	int n;

	journal = kzalloc(sizeof(*journal), GFP_KERNEL);
	if (!journal)
		return NULL;

	init_waitqueue_head(&journal->j_wait_transaction_locked);
	init_waitqueue_head(&journal->j_wait_done_commit);
	init_waitqueue_head(&journal->j_wait_commit);
	init_waitqueue_head(&journal->j_wait_updates);
	init_waitqueue_head(&journal->j_wait_reserved);
	mutex_init(&journal->j_barrier);
	mutex_init(&journal->j_checkpoint_mutex);
	spin_lock_init(&journal->j_revoke_lock);
	spin_lock_init(&journal->j_list_lock);

	journal->j_commit_interval = (HZ * JBD2_DEFAULT_MAX_COMMIT_AGE);
	journal->j_min_batch_time = 0;
	journal->j_max_batch_time = 15000; /* 15ms */
	atomic_set(&journal->j_reserved_credits, 0);

	/* The journal is marked for error until we succeed with recovery! */
	journal->j_flags = JBD2_ABORT;

	/* Set up a default-sized revoke table for the new mount. */
	err = jbd2_journal_init_revoke(journal, JOURNAL_REVOKE_DEFAULT_HASH);
	if (err)
		goto err_cleanup;

	spin_lock_init(&journal->j_history_lock);

	/*lockdep_init_map(&journal->j_trans_commit_map, "jbd2_handle",
			 &jbd2_trans_commit_key, 0);*/

	/* journal descriptor can store up to n blocks -bzzz */
	journal->j_blocksize = blocksize;
	journal->j_dev = bdev;
	journal->j_fs_dev = fs_dev;
	journal->j_blk_offset = start;
	journal->j_maxlen = len;
	n = journal->j_blocksize / sizeof(journal_block_tag_t);
	journal->j_wbufsize = n;
	journal->j_wbuf = kmalloc_array(n, sizeof(struct buffer_head *),
					GFP_KERNEL);
	if (!journal->j_wbuf)
		goto err_cleanup;

	/*bh = getblk_unmovable(journal->j_dev, start, journal->j_blocksize);*/
    bh = __getblk(journal->j_dev, start, journal->j_blocksize);
	if (!bh) {
		/*pr_err("%s: Cannot get buffer for journal superblock\n",
			__func__);*/
		goto err_cleanup;
	}
	journal->j_sb_buffer = bh;
	journal->j_superblock = (journal_superblock_t *)bh->b_data;

	return journal;

err_cleanup:
	kfree(journal->j_wbuf);
	jbd2_journal_destroy_revoke(journal);
	kfree(journal);
	return NULL;
}

/**
 *  journal_t * jbd2_journal_init_inode () - creates a journal which maps to a inode.
 *  @inode: An inode to create the journal in
 *
 * jbd2_journal_init_inode creates a journal which maps an on-disk inode as
 * the journal.  The inode must exist already, must support bmap() and
 * must have all data blocks preallocated.
 */
journal_t *jbd2_journal_init_inode(struct inode *inode)
{
	journal_t *journal;
	unsigned long long blocknr;

	blocknr = bmap(inode, 0);
	if (!blocknr) {
		/*pr_err("%s: Cannot locate journal superblock\n",
			__func__);*/
		return NULL;
	}

	jbd_debug(1, "JBD2: inode %s/%ld, size %lld, bits %d, blksize %ld\n",
		  inode->i_sb->s_id, inode->i_ino, (long long) inode->i_size,
		  inode->i_sb->s_blocksize_bits, inode->i_sb->s_blocksize);

	journal = journal_init_common(inode->i_sb->s_bdev, inode->i_sb->s_bdev,
			blocknr, (int)(inode->i_size >> inode->i_sb->s_blocksize_bits),
			inode->i_sb->s_blocksize);
	if (!journal)
		return NULL;

	journal->j_inode = inode;
	/*bdevname(journal->j_dev, journal->j_devname);
	p = strreplace(journal->j_devname, '/', '!');
	sprintf(p, "-%lu", journal->j_inode->i_ino);*/
	jbd2_stats_proc_init(journal);

	return journal;
}

/*
 * If the journal init or create aborts, we need to mark the journal
 * superblock as being NULL to prevent the journal destroy from writing
 * back a bogus superblock.
 */
static void journal_fail_superblock (journal_t *journal)
{
	struct buffer_head *bh = journal->j_sb_buffer;
	brelse(bh);
	journal->j_sb_buffer = NULL;
}

/*
 * Given a journal_t structure, initialise the various fields for
 * startup of a new journaling session.  We use this both when creating
 * a journal, and after recovering an old journal to reset it for
 * subsequent use.
 */

static int journal_reset(journal_t *journal)
{
	journal_superblock_t *sb = journal->j_superblock;
	unsigned long /*long*/ first, last;

	first = be32_to_cpu(sb->s_first);
	last = be32_to_cpu(sb->s_maxlen);
	if (first + JBD2_MIN_JOURNAL_BLOCKS > last + 1) {
		printk(KERN_ERR "JBD2: Journal too short (blocks %llu-%llu).\n",
		       first, last);
		journal_fail_superblock(journal);
		return -EINVAL;
	}

	journal->j_first = first;
	journal->j_last = last;

	journal->j_head = first;
	journal->j_tail = first;
	journal->j_free = last - first;

	journal->j_tail_sequence = journal->j_transaction_sequence;
	journal->j_commit_sequence = journal->j_transaction_sequence - 1;
	journal->j_commit_request = journal->j_commit_sequence;

	journal->j_max_transaction_buffers = journal->j_maxlen / 4;

	/*
	 * As a special case, if the on-disk copy is already marked as needing
	 * no recovery (s_start == 0), then we can safely defer the superblock
	 * update until the next commit by setting JBD2_FLUSHED.  This avoids
	 * attempting a write to a potential-readonly device.
	 */
	if (sb->s_start == 0) {
		jbd_debug(1, "JBD2: Skipping superblock update on recovered sb "
			"(start %ld, seq %d, errno %d)\n",
			journal->j_tail, journal->j_tail_sequence,
			journal->j_errno);
		journal->j_flags |= JBD2_FLUSHED;
	} else {
		/* Lock here to make assertions happy... */
		mutex_lock/*_io*/(&journal->j_checkpoint_mutex);
		/*
		 * Update log tail information. We use REQ_FUA since new
		 * transaction will start reusing journal space and so we
		 * must make sure information about current log tail is on
		 * disk before that.
		 */
		jbd2_journal_update_sb_log_tail(journal,
						journal->j_tail_sequence,
						journal->j_tail,
						0/*REQ_SYNC | REQ_FUA*/);
		mutex_unlock(&journal->j_checkpoint_mutex);
	}
	return 0/*jbd2_journal_start_thread(journal)*/;
}

static int jbd2_write_superblock(journal_t *journal, int write_flags)
{
    UNREFERENCED_PARAMETER(write_flags);
	struct buffer_head *bh = journal->j_sb_buffer;
	journal_superblock_t *sb = journal->j_superblock;
	int ret;

	/*if (!(journal->j_flags & JBD2_BARRIER))
		write_flags &= ~(REQ_FUA | REQ_PREFLUSH);*/
	lock_buffer(bh);
	if (buffer_write_io_error(bh)) {
		/*
		 * Oh, dear.  A previous attempt to write the journal
		 * superblock failed.  This could happen because the
		 * USB device was yanked out.  Or it could happen to
		 * be a transient write error and maybe the block will
		 * be remapped.  Nothing we can do but to retry the
		 * write and hope for the best.
		 */
		printk(KERN_ERR "JBD2: previous I/O error detected "
		       "for journal superblock update for %s.\n",
		       journal->j_devname);
		clear_buffer_write_io_error(bh);
		set_buffer_uptodate(bh);
	}
	jbd2_superblock_csum_set(journal, sb);
	/* Windows: the journal superblock is written synchronously and
	   uncached (jbd2\commit.c); the cached copy in bh stays the master */
	ret = jbd2_win_write_superblock(journal);
	unlock_buffer(bh);
	if (ret) {
		printk(KERN_ERR "JBD2: Error %d detected when updating "
		       "journal superblock for %s.\n", ret,
		       journal->j_devname);
		jbd2_journal_abort(journal, ret);
	}

	return ret;
}

/* Windows: the checksummed superblock write, for callers outside this
   file that change superblock fields themselves (feature setup) */
int jbd2_journal_write_superblock(journal_t *journal)
{
	return jbd2_write_superblock(journal, 0);
}

/**
 * jbd2_journal_update_sb_log_tail() - Update log tail in journal sb on disk.
 * @journal: The journal to update.
 * @tail_tid: TID of the new transaction at the tail of the log
 * @tail_block: The first block of the transaction at the tail of the log
 * @write_op: With which operation should we write the journal sb
 *
 * Update a journal's superblock information about log tail and write it to
 * disk, waiting for the IO to complete.
 */
int jbd2_journal_update_sb_log_tail(journal_t *journal, tid_t tail_tid,
				     unsigned long tail_block, int write_op)
{
	journal_superblock_t *sb = journal->j_superblock;
	int ret;

	if (is_journal_aborted(journal))
		return -EIO;

	jbd_debug(1, "JBD2: updating superblock (start %lu, seq %u)\n",
		  tail_block, tail_tid);

	sb->s_sequence = cpu_to_be32(tail_tid);
	sb->s_start    = cpu_to_be32(tail_block);

	ret = jbd2_write_superblock(journal, write_op);
	if (ret)
		goto out;

	/* Log is no longer empty */
	journal->j_flags &= ~JBD2_FLUSHED;

out:
	return ret;
}

/**
 * jbd2_mark_journal_empty() - Mark on disk journal as empty.
 * @journal: The journal to update.
 * @write_op: With which operation should we write the journal sb
 *
 * Update a journal's dynamic superblock fields to show that journal is empty.
 * Write updated superblock to disk waiting for IO to complete.
 */
static void jbd2_mark_journal_empty(journal_t *journal, int write_op)
{
	journal_superblock_t *sb = journal->j_superblock;


	/* Is it already empty? */
	if (sb->s_start == 0) {
		return;
	}
	jbd_debug(1, "JBD2: Marking journal as empty (seq %d)\n",
		  journal->j_tail_sequence);

	sb->s_sequence = cpu_to_be32(journal->j_tail_sequence);
	sb->s_start    = cpu_to_be32(0);

	jbd2_write_superblock(journal, write_op);

	/* Log is no longer empty */
	journal->j_flags |= JBD2_FLUSHED;
}

/**
 * jbd2_journal_update_sb_errno() - Update error in the journal.
 * @journal: The journal to update.
 *
 * Update a journal's errno.  Write updated superblock to disk waiting for IO
 * to complete.
 */
void jbd2_journal_update_sb_errno(journal_t *journal)
{
	journal_superblock_t *sb = journal->j_superblock;
	int errcode;

	errcode = journal->j_errno;
	if (errcode == -ESHUTDOWN)
		errcode = 0;
	jbd_debug(1, "JBD2: updating superblock error (errno %d)\n", errcode);
	sb->s_errno    = cpu_to_be32(errcode);

	jbd2_write_superblock(journal, 0/*REQ_SYNC | REQ_FUA*/);
}

/*
 * Read the superblock for a given journal, performing initial
 * validation of the format.
 */
static int journal_get_superblock(journal_t *journal)
{
	struct buffer_head *bh;
	journal_superblock_t *sb;
	int err = -EIO;

	bh = journal->j_sb_buffer;

	J_ASSERT(bh != NULL);
	if (!buffer_uptodate(bh)) {
        ll_rw_block(READ, 1, &bh);
		wait_on_buffer(bh);
		if (!buffer_uptodate(bh)) {
			printk(KERN_ERR
				"JBD2: IO error reading journal superblock\n");
			goto out;
		}
	}

	if (buffer_verified(bh))
		return 0;

	sb = journal->j_superblock;

	err = -EINVAL;

	if (sb->s_header.h_magic != cpu_to_be32(JBD2_MAGIC_NUMBER) ||
	    sb->s_blocksize != cpu_to_be32(journal->j_blocksize)) {
		printk(KERN_WARNING "JBD2: no valid journal superblock found\n");
		goto out;
	}

	switch(be32_to_cpu(sb->s_header.h_blocktype)) {
	case JBD2_SUPERBLOCK_V1:
		journal->j_format_version = 1;
		break;
	case JBD2_SUPERBLOCK_V2:
		journal->j_format_version = 2;
		break;
	default:
		printk(KERN_WARNING "JBD2: unrecognised superblock format ID\n");
		goto out;
	}

	if (be32_to_cpu(sb->s_maxlen) < journal->j_maxlen)
		journal->j_maxlen = be32_to_cpu(sb->s_maxlen);
	else if (be32_to_cpu(sb->s_maxlen) > journal->j_maxlen) {
		printk(KERN_WARNING "JBD2: journal file too short\n");
		goto out;
	}

	if (be32_to_cpu(sb->s_first) == 0 ||
	    be32_to_cpu(sb->s_first) >= journal->j_maxlen) {
		printk(KERN_WARNING
			"JBD2: Invalid start block of journal: %u\n",
			be32_to_cpu(sb->s_first));
		goto out;
	}

	if (jbd2_has_feature_csum2(journal) &&
	    jbd2_has_feature_csum3(journal)) {
		/* Can't have checksum v2 and v3 at the same time! */
		printk(KERN_ERR "JBD2: Can't enable checksumming v2 and v3 "
		       "at the same time!\n");
		goto out;
	}

	if (jbd2_journal_has_csum_v2or3_feature(journal) &&
	    jbd2_has_feature_checksum(journal)) {
		/* Can't have checksum v1 and v2 on at the same time! */
		printk(KERN_ERR "JBD2: Can't enable checksumming v1 and v2/3 "
		       "at the same time!\n");
		goto out;
	}

	if (!jbd2_verify_csum_type(journal, sb)) {
		printk(KERN_ERR "JBD2: Unknown checksum type\n");
		goto out;
	}
	/* Windows: there is no crypto driver, crc32c is built in. The field
	   only records that checksums are in use (jbd2_journal_has_csum_v2or3). */
	if (jbd2_journal_has_csum_v2or3_feature(journal))
		journal->j_chksum_driver = (struct crypto_shash *)(ULONG_PTR)1;
	else
		journal->j_chksum_driver = NULL;
	/* Check superblock checksum */
	if (!jbd2_superblock_csum_verify(journal, sb)) {
		printk(KERN_ERR "JBD2: journal checksum error\n");
		DbgPrint("JBD2: journal checksum error\n");
		err = -EFSBADCRC;
		goto out;
	}

	/* Precompute checksum seed for all metadata */
	if (jbd2_journal_has_csum_v2or3(journal))
		journal->j_csum_seed = jbd2_chksum(journal, ~0U, sb->s_uuid,
						   sizeof(sb->s_uuid));

	set_buffer_verified(bh);

	return 0;

out:
	journal_fail_superblock(journal);
	return err;
}

/*
 * Load the on-disk journal superblock and read the key fields into the
 * journal_t.
 */

static int load_superblock(journal_t *journal)
{
	int err;
	journal_superblock_t *sb;

	err = journal_get_superblock(journal);
	if (err)
		return err;

	sb = journal->j_superblock;

	journal->j_tail_sequence = be32_to_cpu(sb->s_sequence);
	journal->j_tail = be32_to_cpu(sb->s_start);
	journal->j_first = be32_to_cpu(sb->s_first);
	journal->j_last = be32_to_cpu(sb->s_maxlen);
	journal->j_errno = be32_to_cpu(sb->s_errno);

	return 0;
}

/**
 * int jbd2_journal_load() - Read journal from disk.
 * @journal: Journal to act on.
 *
 * Given a journal_t structure which tells us which disk blocks contain
 * a journal, read the journal from disk to initialise the in-memory
 * structures.
 */
int jbd2_journal_load(journal_t *journal)
{
	int err;
	journal_superblock_t *sb;

	err = load_superblock(journal);
	if (err)
		return err;

	sb = journal->j_superblock;
	/* If this is a V2 superblock, then we have to check the
	 * features flags on it. */

	if (journal->j_format_version >= 2) {
		if ((sb->s_feature_ro_compat &
		     ~cpu_to_be32(JBD2_KNOWN_ROCOMPAT_FEATURES)) ||
		    (sb->s_feature_incompat &
		     ~cpu_to_be32(JBD2_KNOWN_INCOMPAT_FEATURES))) {
			printk(KERN_WARNING
				"JBD2: Unrecognised features on journal\n");
			return -EINVAL;
		}
	}

	/*
	 * Create a slab for this blocksize
	 */
	err = jbd2_journal_create_slab(be32_to_cpu(sb->s_blocksize));
	if (err)
		return err;

	/* Let the recovery code check whether it needs to recover any
	 * data from the journal. */
	if (jbd2_journal_recover(journal))
		goto recovery_error;

	if (journal->j_failed_commit) {
		printk(KERN_ERR "JBD2: journal transaction %u on %s "
		       "is corrupt.\n", journal->j_failed_commit,
		       journal->j_devname);
		return -EFSCORRUPTED;
	}

	/* OK, we've finished with the dynamic journal bits:
	 * reinitialise the dynamic contents of the superblock in memory
	 * and reset them on disk. */
	if (journal_reset(journal))
		goto recovery_error;

	journal->j_flags &= ~JBD2_ABORT;
	journal->j_flags |= JBD2_LOADED;
	return 0;

recovery_error:
	printk(KERN_WARNING "JBD2: recovery failed\n");
	return -EIO;
}

/**
 * void jbd2_journal_destroy() - Release a journal_t structure.
 * @journal: Journal to act on.
 *
 * Release a journal_t structure once it is no longer in use by the
 * journaled object.
 * Return <0 if we couldn't clean up the journal.
 */
int jbd2_journal_destroy(journal_t *journal)
{
	int err = 0;
	if (journal->j_sb_buffer) {
		if (!is_journal_aborted(journal)) {

			journal->j_tail_sequence =
				++journal->j_transaction_sequence;

			jbd2_mark_journal_empty(journal,
					0/*REQ_SYNC | REQ_PREFLUSH | REQ_FUA*/);
		} else
			err = -EIO;
		brelse(journal->j_sb_buffer);
	}

	if (journal->j_proc_entry)
		jbd2_stats_proc_exit(journal);
	if (journal->j_inode)
		iput(journal->j_inode);
	if (journal->j_revoke)
		jbd2_journal_destroy_revoke(journal);
	/*if (journal->j_chksum_driver)
		crypto_free_shash(journal->j_chksum_driver);*/
	kfree(journal->j_wbuf);
	kfree(journal);

	return err;
}

/**
 * int jbd2_journal_wipe() - Wipe journal contents
 * @journal: Journal to act on.
 * @write: flag (see below)
 *
 * Wipe out all of the contents of a journal, safely.  This will produce
 * a warning if the journal contains any valid recovery information.
 * Must be called between journal_init_*() and jbd2_journal_load().
 *
 * If 'write' is non-zero, then we wipe out the journal on disk; otherwise
 * we merely suppress recovery.
 */

int jbd2_journal_wipe(journal_t *journal, int write)
{
	int err = 0;


	J_ASSERT (!(journal->j_flags & JBD2_LOADED));

	err = load_superblock(journal);
	if (err)
		return err;

	if (!journal->j_tail) {
		goto no_recovery;
    }

	printk(KERN_WARNING "JBD2: %s recovery information on journal\n",
		write ? "Clearing" : "Ignoring");

	err = jbd2_journal_skip_recovery(journal);
	if (write) {
		/* Lock to make assertions happy... */
		mutex_lock(&journal->j_checkpoint_mutex);
		jbd2_mark_journal_empty(journal, 0/*REQ_SYNC | REQ_FUA*/);
		mutex_unlock(&journal->j_checkpoint_mutex);
	}

 no_recovery:
	return err;
}

/*
 * Journal abort has very specific semantics, which we describe
 * for journal abort.
 *
 * Two internal functions, which provide abort to the jbd layer
 * itself are here.
 */

/*
 * Quick version for internal journal use (doesn't lock the journal).
 * Aborts hard --- we mark the abort as occurred, but do _nothing_ else,
 * and don't attempt to make any other journal updates.
 */
void __jbd2_journal_abort_hard(journal_t *journal)
{
	transaction_t *transaction;

	if (journal->j_flags & JBD2_ABORT)
		return;

	printk(KERN_ERR "Aborting journal on device %s.\n",
	       journal->j_devname);

	journal->j_flags |= JBD2_ABORT;
	transaction = journal->j_running_transaction;
	if (transaction)
		__jbd2_log_start_commit(journal, transaction->t_tid);
}

/* Soft abort: record the abort error status in the journal superblock,
 * but don't do any other IO. */
static void __journal_abort_soft (journal_t *journal, int err)
{
	int old_errno;

	old_errno = journal->j_errno;
	if (!journal->j_errno || err == -ESHUTDOWN)
		journal->j_errno = err;

	if (journal->j_flags & JBD2_ABORT) {
		if (!old_errno && old_errno != -ESHUTDOWN &&
		    err == -ESHUTDOWN)
			jbd2_journal_update_sb_errno(journal);
		return;
	}

	__jbd2_journal_abort_hard(journal);

	if (err) {
		jbd2_journal_update_sb_errno(journal);
		journal->j_flags |= JBD2_REC_ERR;
	}
}

/**
 * void jbd2_journal_abort () - Shutdown the journal immediately.
 * @journal: the journal to shutdown.
 * @errno:   an error number to record in the journal indicating
 *           the reason for the shutdown.
 *
 * Perform a complete, immediate shutdown of the ENTIRE
 * journal (not of a single transaction).  This operation cannot be
 * undone without closing and reopening the journal.
 *
 * The jbd2_journal_abort function is intended to support higher level error
 * recovery mechanisms such as the ext2/ext3 remount-readonly error
 * mode.
 *
 * Journal abort has very specific semantics.  Any existing dirty,
 * unjournaled buffers in the main filesystem will still be written to
 * disk by bdflush, but the journaling mechanism will be suspended
 * immediately and no further transaction commits will be honoured.
 *
 * Any dirty, journaled buffers will be written back to disk without
 * hitting the journal.  Atomicity cannot be guaranteed on an aborted
 * filesystem, but we _do_ attempt to leave as much data as possible
 * behind for fsck to use for cleanup.
 *
 * Any attempt to get a new transaction handle on a journal which is in
 * ABORT state will just result in an -EROFS error return.  A
 * jbd2_journal_stop on an existing handle will return -EIO if we have
 * entered abort state during the update.
 *
 * Recursive transactions are not disturbed by journal abort until the
 * final jbd2_journal_stop, which will receive the -EIO error.
 *
 * Finally, the jbd2_journal_abort call allows the caller to supply an errno
 * which will be recorded (if possible) in the journal superblock.  This
 * allows a client to record failure conditions in the middle of a
 * transaction without having to complete the transaction to record the
 * failure to disk.  ext3_error, for example, now uses this
 * functionality.
 *
 * Errors which originate from within the journaling layer will NOT
 * supply an errno; a null errno implies that absolutely no further
 * writes are done to the journal (unless there are any already in
 * progress).
 *
 */

void jbd2_journal_abort(journal_t *journal, int err)
{
	__journal_abort_soft(journal, err);
}
