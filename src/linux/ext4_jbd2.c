/**
 * ext4_jbd2.c - ext4 journal hooks used by the extent code.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "linux\ext4.h"

static handle_t no_journal;

handle_t *__ext4_journal_start_sb(void *icb, struct super_block *sb, unsigned int line,
				  int type, int blocks, int rsv_blocks)
{
    UNREFERENCED_PARAMETER(icb);
    UNREFERENCED_PARAMETER(line);
    UNREFERENCED_PARAMETER(sb);
    UNREFERENCED_PARAMETER(blocks);
    UNREFERENCED_PARAMETER(rsv_blocks);
    UNREFERENCED_PARAMETER(type);
	return &no_journal;
}

int __ext4_journal_stop(const char *where, unsigned int line, void *icb, handle_t *handle)
{
    UNREFERENCED_PARAMETER(handle);
    UNREFERENCED_PARAMETER(icb);
    UNREFERENCED_PARAMETER(line);
    UNREFERENCED_PARAMETER(where);
	return 0;
}

void ext4_journal_abort_handle(const char *caller, unsigned int line,
			       const char *err_fn, struct buffer_head *bh,
			       handle_t *handle, int err)
{
    UNREFERENCED_PARAMETER(caller);
    UNREFERENCED_PARAMETER(line);
    UNREFERENCED_PARAMETER(bh);
    UNREFERENCED_PARAMETER(err_fn);
    UNREFERENCED_PARAMETER(err);
    UNREFERENCED_PARAMETER(handle);
}

int __ext4_journal_get_write_access(const char *where, unsigned int line,
				    void *icb, handle_t *handle, struct buffer_head *bh)
{
    UNREFERENCED_PARAMETER(line);
    UNREFERENCED_PARAMETER(where);
    UNREFERENCED_PARAMETER(bh);
    UNREFERENCED_PARAMETER(handle);
    UNREFERENCED_PARAMETER(icb);
	int err = 0;
	return err;
}

/*
 * The ext4 forget function must perform a revoke if we are freeing data
 * which has been journaled.  Metadata (eg. indirect blocks) must be
 * revoked in all cases.
 *
 * "bh" may be NULL: a metadata block may have been freed from memory
 * but there may still be a record of it in the journal, and that record
 * still needs to be revoked.
 *
 * If the handle isn't valid we're not journaling, but we still need to
 * call into ext4_journal_revoke() to put the buffer head.
 */
int __ext4_forget(const char *where, unsigned int line, void *icb, handle_t *handle,
		  int is_metadata, struct inode *inode,
		  struct buffer_head *bh, ext4_fsblk_t blocknr)
{
    UNREFERENCED_PARAMETER(handle);
    UNREFERENCED_PARAMETER(icb);
    UNREFERENCED_PARAMETER(line);
    UNREFERENCED_PARAMETER(where);
    UNREFERENCED_PARAMETER(inode);
    UNREFERENCED_PARAMETER(is_metadata);
    UNREFERENCED_PARAMETER(bh);
    UNREFERENCED_PARAMETER(blocknr);
	int err = 0;
	return err;
}

int __ext4_journal_get_create_access(const char *where, unsigned int line,
				void *icb, handle_t *handle, struct buffer_head *bh)
{
    UNREFERENCED_PARAMETER(line);
    UNREFERENCED_PARAMETER(where);
    UNREFERENCED_PARAMETER(bh);
    UNREFERENCED_PARAMETER(handle);
    UNREFERENCED_PARAMETER(icb);
	int err = 0;
	return err;
}

int __ext4_handle_dirty_metadata(const char *where, unsigned int line,
				 void *icb, handle_t *handle, struct inode *inode,
				 struct buffer_head *bh)
{
    UNREFERENCED_PARAMETER(line);
    UNREFERENCED_PARAMETER(where);
    UNREFERENCED_PARAMETER(handle);
    UNREFERENCED_PARAMETER(icb);
    UNREFERENCED_PARAMETER(inode);
	int err = 0;

	extents_mark_buffer_dirty(bh);
	return err;
}

int __ext4_handle_dirty_super(const char *where, unsigned int line,
			      handle_t *handle, struct super_block *sb)
{
    UNREFERENCED_PARAMETER(line);
    UNREFERENCED_PARAMETER(where);
    UNREFERENCED_PARAMETER(handle);
    UNREFERENCED_PARAMETER(sb);
    return 0;
}
