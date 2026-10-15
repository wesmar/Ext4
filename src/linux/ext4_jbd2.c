/**
 * ext4_jbd2.c - ext4 journal hooks used by the extent code.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"

int __ext4_journal_stop(const char *where, unsigned int line, void *icb, handle_t *handle)
{
    UNREFERENCED_PARAMETER(handle);
    UNREFERENCED_PARAMETER(icb);
    UNREFERENCED_PARAMETER(line);
    UNREFERENCED_PARAMETER(where);
	return 0;
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
