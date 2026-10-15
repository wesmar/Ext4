/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _EXT4_JBD2_H
#define _EXT4_JBD2_H

int __ext4_journal_get_write_access(const char *where, unsigned int line,
				    void *icb, handle_t *handle, struct buffer_head *bh);

int __ext4_journal_get_create_access(const char *where, unsigned int line,
				void *icb, handle_t *handle, struct buffer_head *bh);

int __ext4_handle_dirty_metadata(const char *where, unsigned int line,
				 void *icb, handle_t *handle, struct inode *inode,
				 struct buffer_head *bh);

#define ext4_journal_get_write_access(handle, icb, bh) \
	__ext4_journal_get_write_access("", __LINE__, (icb), (handle), (bh))
#define ext4_journal_get_create_access(handle, icb, bh) \
	__ext4_journal_get_create_access("", __LINE__, (icb), (handle), (bh))
#define ext4_handle_dirty_metadata(handle, icb, inode, bh) \
	__ext4_handle_dirty_metadata("", __LINE__, (icb), (handle), (inode), \
				     (bh))

int __ext4_journal_stop(const char *where, unsigned int line, void *icb, handle_t *handle);

#define ext4_journal_stop(icb, handle) \
	__ext4_journal_stop("", __LINE__, (icb), (handle))

static inline int ext4_journal_extend(void *icb, handle_t *handle, int nblocks)
{
    UNREFERENCED_PARAMETER(handle);
    UNREFERENCED_PARAMETER(icb);
    UNREFERENCED_PARAMETER(nblocks);
	return 0;
}

#endif
