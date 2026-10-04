/**
 * extents_internal.h - definitions shared by the files split from ext4_extents.c.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _EXT4_EXT4_EXTENTS_INTERNAL_H_
#define _EXT4_EXT4_EXTENTS_INTERNAL_H_

#define EXT4_EXT_MARK_UNWRIT1	0x2  /* mark first half unwritten */
#define EXT4_EXT_MARK_UNWRIT2	0x4  /* mark second half unwritten */

#define ext4_mark_inode_dirty(icb, handle, n) ext3_mark_inode_dirty(icb, n)


/* an inconsistency found in an inode's extent tree: goes to the debug log */
#define EXT4_ERROR_INODE(inode, ...) do {                               \
            DbgPrint("ext4: inode %lu: ", (ULONG)(inode)->i_ino);   \
            DbgPrint(__VA_ARGS__);                                  \
            DbgPrint("\n");                                        \
        } while (0)

#define assert ASSERT

/*
 * Return the right sibling of a tree node(either leaf or indexes node)
 */

#define EXT_MAX_BLOCKS 0xffffffff

static inline int ext4_ext_space_block(struct inode *inode, int check)
{
    UNREFERENCED_PARAMETER(check);
	int size;

	size = (inode->i_sb->s_blocksize - sizeof(struct ext4_extent_header))
		/ sizeof(struct ext4_extent);
	return size;
}

static inline int ext4_ext_space_block_idx(struct inode *inode, int check)
{
    UNREFERENCED_PARAMETER(check);
	int size;

	size = (inode->i_sb->s_blocksize - sizeof(struct ext4_extent_header))
		/ sizeof(struct ext4_extent_idx);
	return size;
}

static inline int ext4_ext_space_root(struct inode *inode, int check)
{
    UNREFERENCED_PARAMETER(check);
	int size;

	size = sizeof(EXT4_I(inode)->i_block);
	size -= sizeof(struct ext4_extent_header);
	size /= sizeof(struct ext4_extent);
	return size;
}

static inline int ext4_ext_space_root_idx(struct inode *inode, int check)
{
    UNREFERENCED_PARAMETER(check);
	int size;

	size = sizeof(EXT4_I(inode)->i_block);
	size -= sizeof(struct ext4_extent_header);
	size /= sizeof(struct ext4_extent_idx);
	return size;
}

#define read_extent_tree_block(inode, pblk, depth, flags)		\
	__read_extent_tree_block("", __LINE__, (inode), (pblk),   \
			(depth), (flags))

int ext4_split_extent_at(void *icb,
			     handle_t *handle,
			     struct inode *inode,
			     struct ext4_ext_path **ppath,
			     ext4_lblk_t split,
			     int split_flag,
			     int flags);

#define ext4_ext_show_leaf(inode, path)

ext4_fsblk_t ext4_new_meta_blocks(void *icb, handle_t *handle, struct inode *inode,
		ext4_fsblk_t goal,
		unsigned int flags,
		unsigned long *count, int *errp);

ext4_fsblk_t ext4_new_data_blocks(void *icb, struct inode *inode, ext4_fsblk_t goal,
		unsigned long *count, int *errp);

int ext4_free_blocks(void *icb, handle_t *handle, struct inode *inode, void *fake,
		ext4_fsblk_t block, int count, int flags);

struct buffer_head *
__read_extent_tree_block(const char *function, unsigned int line,
		struct inode *inode, ext4_fsblk_t pblk, int depth,
		int flags);

int ext4_ext_get_access(void *icb, handle_t *handle, struct inode *inode,
		struct ext4_ext_path *path);

ext4_fsblk_t ext4_ext_find_goal(struct inode *inode,
		struct ext4_ext_path *path,
		ext4_lblk_t block);

ext4_fsblk_t
ext4_ext_new_meta_block(void *icb, handle_t *handle, struct inode *inode,
		struct ext4_ext_path *path,
		struct ext4_extent *ex, int *err, unsigned int flags);

int __ext4_ext_dirty(const char *where, unsigned int line,
		void *icb, handle_t *handle,
		struct inode *inode,
		struct ext4_ext_path *path);

int ext4_ext_create_new_leaf(void *icb, handle_t *handle, struct inode *inode,
		unsigned int mb_flags,
		unsigned int gb_flags,
		struct ext4_ext_path **ppath,
		struct ext4_extent *newext);

int ext4_ext_correct_indexes(void *icb, handle_t *handle, struct inode *inode,
		struct ext4_ext_path *path);

int ext4_ext_try_to_merge_right(struct inode *inode,
		struct ext4_ext_path *path,
		struct ext4_extent *ex);

void ext4_ext_try_to_merge_up(void *icb, handle_t *handle,
		struct inode *inode,
		struct ext4_ext_path *path);

void ext4_ext_try_to_merge(void *icb, handle_t *handle,
		struct inode *inode,
		struct ext4_ext_path *path,
		struct ext4_extent *ex);

int ext4_ext_insert_extent(void *icb, handle_t *handle, struct inode *inode,
		struct ext4_ext_path **ppath,
		struct ext4_extent *newext,
		int gb_flags);

#endif /* _EXT4_EXT4_EXTENTS_INTERNAL_H_ */
