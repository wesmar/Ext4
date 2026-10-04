/**
 * ExtentRemove.c - extent removal, splitting and truncation.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "linux\ext4.h"
#include "extents_internal.h"


/*
 * used by extent splitting.
 */
#define EXT4_EXT_MAY_ZEROOUT	0x1  /* safe to zeroout if split fails \
					due to ENOSPC */

#define EXT4_EXT_DATA_VALID1	0x8  /* first half contains valid data */
#define EXT4_EXT_DATA_VALID2	0x10 /* second half contains valid data */

/*
 * returns 1 if current index have to be freed (even partial)
 */
static inline int
ext4_ext_more_to_rm(struct ext4_ext_path *path)
{
	BUG_ON(path->p_idx == NULL);

	if (path->p_idx < EXT_FIRST_INDEX(path->p_hdr))
		return 0;

	/*
	 * if truncate on deeper level happened it it wasn't partial
	 * so we have to consider current index for truncation
	 */
	if (le16_to_cpu(path->p_hdr->eh_entries) == path->p_block)
		return 0;
	return 1;
}

/* the allocation for the Linux extent code: *count blocks wanted (one if
   none given), fewer may come; the block, or 0 with *errp set */
static ext4_fsblk_t ext4_new_blocks(void *icb, struct inode *inode, ext4_fsblk_t goal,
		ULONG Flags, unsigned long *count, int *errp)
{
	ULONG		blockcnt = count ? (ULONG)*count : 1;
	ULONGLONG	block = 0;
	NTSTATUS	status;

	status = Ext2AllocateInodeBlocks((PEXT2_IRP_CONTEXT)icb, inode, goal, Flags,
					 &block, &blockcnt);
	if (!NT_SUCCESS(status)) {
		*errp = Ext2LinuxError(status);
		return 0;
	}
	if (count)
		*count = blockcnt;
	return block;
}

/* index blocks of the tree: journaled */
ext4_fsblk_t ext4_new_meta_blocks(void *icb, handle_t *handle, struct inode *inode,
		ext4_fsblk_t goal,
		unsigned int flags,
		unsigned long *count, int *errp)
{
	UNREFERENCED_PARAMETER(handle);
	UNREFERENCED_PARAMETER(flags);
	return ext4_new_blocks(icb, inode, goal, EXT2_ALLOC_JOURNALED, count, errp);
}

/* the blocks an extent maps: a directory's are journaled, a file's are
   written in place and wait for blocks freed by an uncommitted release */
ext4_fsblk_t ext4_new_data_blocks(void *icb, struct inode *inode, ext4_fsblk_t goal,
		unsigned long *count, int *errp)
{
	return ext4_new_blocks(icb, inode, goal,
			       S_ISDIR(inode->i_mode) ? EXT2_ALLOC_JOURNALED : 0, count, errp);
}

int ext4_free_blocks(void *icb, handle_t *handle, struct inode *inode, void *fake,
		ext4_fsblk_t block, int count, int flags)
{
	NTSTATUS status;

	UNREFERENCED_PARAMETER(fake);
	UNREFERENCED_PARAMETER(handle);
	UNREFERENCED_PARAMETER(flags);
	/* an extent removal does not commit when its blocks were not freed */
	status = Ext2ReleaseInodeBlocks((PEXT2_IRP_CONTEXT)icb, inode, block, (ULONG)count);
	return NT_SUCCESS(status) ? 0 : Ext2LinuxError(status);
}

/*
 * This function tries to merge the @ex extent to neighbours in the tree.
 * return 1 if merge left else 0.
 */
void ext4_ext_try_to_merge(void *icb, handle_t *handle,
		struct inode *inode,
		struct ext4_ext_path *path,
		struct ext4_extent *ex) {
	struct ext4_extent_header *eh;
	unsigned int depth;
	int merge_done = 0;

	depth = ext_depth(inode);
	BUG_ON(path[depth].p_hdr == NULL);
	eh = path[depth].p_hdr;

	if (ex > EXT_FIRST_EXTENT(eh))
		merge_done = ext4_ext_try_to_merge_right(inode, path, ex - 1);

	if (!merge_done)
		(void) ext4_ext_try_to_merge_right(inode, path, ex);

	ext4_ext_try_to_merge_up(icb, handle, inode, path);
}

/* Zeroing a split-off part on disk (Linux falls back to it when a split runs out of space) is not implemented: returning success here would convert blocks that still hold stale data. The error makes ext4_split_extent_at restore the extent instead. */
static int ext4_ext_zeroout(struct inode *inode, struct ext4_extent *ex)
{
	UNREFERENCED_PARAMETER(inode);
	UNREFERENCED_PARAMETER(ex);

	return -EIO;
}

static int ext4_remove_blocks(void *icb, handle_t *handle, struct inode *inode,
		struct ext4_extent *ex,
		unsigned long from, unsigned long to)
{

	if (from >= le32_to_cpu(ex->ee_block)
			&& to == le32_to_cpu(ex->ee_block) + ext4_ext_get_actual_len(ex) - 1) {
		/* tail removal */
		unsigned long num;
		num = le32_to_cpu(ex->ee_block) + ext4_ext_get_actual_len(ex) - from;
		ext4_fsblk_t start = ext4_ext_pblock(ex) + ext4_ext_get_actual_len(ex) - num;
        return ext4_free_blocks(icb, handle, inode, NULL, start, num, 0);
	} else if (from == le32_to_cpu(ex->ee_block)
			&& to <= le32_to_cpu(ex->ee_block) + ext4_ext_get_actual_len(ex) - 1) {
	} else {
	}
	return 0;
}

/*
 * routine removes index from the index block
 * it's used in truncate case only. thus all requests are for
 * last index in the block only
 */
int ext4_ext_rm_idx(void *icb, handle_t *handle, struct inode *inode,
		struct ext4_ext_path *path)
{
	int err;
	ext4_fsblk_t leaf;

	/* free index block */
	path--;
	leaf = ext4_idx_pblock(path->p_idx);
	BUG_ON(path->p_hdr->eh_entries == 0);
	if ((err = ext4_ext_get_access(icb, handle, inode, path)))
		return err;
	path->p_hdr->eh_entries = cpu_to_le16(le16_to_cpu(path->p_hdr->eh_entries)-1);
	if ((err = ext4_ext_dirty(icb, handle, inode, path)))
		return err;
    return ext4_free_blocks(icb, handle, inode, NULL, leaf, 1, 0);
}

static int
ext4_ext_rm_leaf(void *icb, handle_t *handle, struct inode *inode,
		struct ext4_ext_path *path, unsigned long start)
{
	int err = 0, correct_index = 0;
	int depth = ext_depth(inode), credits;
	struct ext4_extent_header *eh;
	unsigned a, b, block, num;
	unsigned long ex_ee_block;
	unsigned short ex_ee_len;
	struct ext4_extent *ex;

	/* the header must be checked already in ext4_ext_remove_space() */
	if (!path[depth].p_hdr)
		path[depth].p_hdr = ext_block_hdr(path[depth].p_bh);
	eh = path[depth].p_hdr;
	BUG_ON(eh == NULL);

	/* find where to start removing */
	ex = EXT_LAST_EXTENT(eh);

	ex_ee_block = le32_to_cpu(ex->ee_block);
	ex_ee_len = ext4_ext_get_actual_len(ex);

	while (ex >= EXT_FIRST_EXTENT(eh) &&
			ex_ee_block + ex_ee_len > start) {
		path[depth].p_ext = ex;

		a = ex_ee_block > start ? ex_ee_block : start;
		b = (unsigned long long)ex_ee_block + ex_ee_len - 1 < 
			EXT_MAX_BLOCKS ? ex_ee_block + ex_ee_len - 1 : EXT_MAX_BLOCKS;


		if (a != ex_ee_block && b != ex_ee_block + ex_ee_len - 1) {
			/* a cut inside one extent: removal runs from start to the
			   end of the file, so only a corrupt tree asks for it */
			err = -EFSCORRUPTED;
			goto out;
		} else if (a != ex_ee_block) {
			/* remove tail of the extent */
			block = ex_ee_block;
			num = a - block;
		} else if (b != ex_ee_block + ex_ee_len - 1) {
			/* remove head of the extent */
			block = a;
			num = b - a;
			/* removal runs to the end of the file: a head cut means the
			   extent runs past the last logical block (corrupt tree) */
			err = -EFSCORRUPTED;
			goto out;
		} else {
			/* remove whole extent: excellent! */
			block = ex_ee_block;
			num = 0;
			BUG_ON(a != ex_ee_block);
			BUG_ON(b != ex_ee_block + ex_ee_len - 1);
		}

		/* at present, extent can't cross block group */
		/* leaf + bitmap + group desc + sb + inode */
		credits = 5;
		if (ex == EXT_FIRST_EXTENT(eh)) {
			correct_index = 1;
			credits += (ext_depth(inode)) + 1;
		}

		/*handle = ext4_ext_journal_restart(icb, handle, credits);*/
		/*if (IS_ERR(icb, handle)) {*/
		/*err = PTR_ERR(icb, handle);*/
		/*goto out;*/
		/*}*/

		err = ext4_ext_get_access(icb, handle, inode, path + depth);
		if (err)
			goto out;

		err = ext4_remove_blocks(icb, handle, inode, ex, a, b);
		if (err)
			goto out;

		if (num == 0) {
			/* this extent is removed entirely mark slot unused */
			ext4_ext_store_pblock(ex, 0);
			eh->eh_entries = cpu_to_le16(le16_to_cpu(eh->eh_entries)-1);
		}

		ex->ee_block = cpu_to_le32(block);
		ex->ee_len = cpu_to_le16(num);

		err = ext4_ext_dirty(icb, handle, inode, path + depth);
		if (err)
			goto out;

		ex--;
		ex_ee_block = le32_to_cpu(ex->ee_block);
		ex_ee_len = ext4_ext_get_actual_len(ex);
	}

	if (correct_index && eh->eh_entries)
		err = ext4_ext_correct_indexes(icb, handle, inode, path);

	/* if this leaf is free, then we should
	 * remove it from index block above */
	if (err == 0 && eh->eh_entries == 0 && path[depth].p_bh != NULL)
		err = ext4_ext_rm_idx(icb, handle, inode, path + depth);

out:
	return err;
}

/*
 * ext4_split_extent_at() splits an extent at given block.
 *
 * @handle: the journal handle
 * @inode: the file inode
 * @path: the path to the extent
 * @split: the logical block where the extent is splitted.
 * @split_flags: indicates if the extent could be zeroout if split fails, and
 *		 the states(init or unwritten) of new extents.
 * @flags: flags used to insert new extent to extent tree.
 *
 *
 * Splits extent [a, b] into two extents [a, @split) and [@split, b], states
 * of which are deterimined by split_flag.
 *
 * There are two cases:
 *  a> the extent are splitted into two extent.
 *  b> split is not needed, and just mark the extent.
 *
 * return 0 on success.
 */
int ext4_split_extent_at(void *icb, handle_t *handle,
		struct inode *inode,
		struct ext4_ext_path **ppath,
		ext4_lblk_t split,
		int split_flag,
		int flags)
{
	struct ext4_ext_path *path = *ppath;
	ext4_fsblk_t newblock;
	ext4_lblk_t ee_block;
	struct ext4_extent *ex, newex, orig_ex, zero_ex;
	struct ext4_extent *ex2 = NULL;
	unsigned int ee_len, depth;
	int err = 0;

	ext4_ext_show_leaf(inode, path);

	depth = ext_depth(inode);
	ex = path[depth].p_ext;
	ee_block = le32_to_cpu(ex->ee_block);
	ee_len = ext4_ext_get_actual_len(ex);
	newblock = split - ee_block + ext4_ext_pblock(ex);

	/* BUG_ON compiles to nothing: a split outside the extent would
	 * wrap ee_len and write a corrupt tree, so refuse it */
	if (unlikely(split < ee_block || split >= ee_block + ee_len)) {
		EXT4_ERROR_INODE(inode, "split %u outside extent %u+%u",
						 split, ee_block, ee_len);
		return -EIO;
	}

	err = ext4_ext_get_access(icb, handle, inode, path + depth);
	if (err)
		goto out;

	if (split == ee_block) {
		/*
		 * case b: block @split is the block that the extent begins with
		 * then we just change the state of the extent, and splitting
		 * is not needed.
		 */
		if (split_flag & EXT4_EXT_MARK_UNWRIT2)
			ext4_ext_mark_unwritten(ex);
		else
			ext4_ext_mark_initialized(ex);

		if (!(flags & EXT4_GET_BLOCKS_PRE_IO))
			ext4_ext_try_to_merge(icb, handle, inode, path, ex);

		err = ext4_ext_dirty(icb, handle, inode, path + path->p_depth);
		goto out;
	}

	/* case a */
	memcpy(&orig_ex, ex, sizeof(orig_ex));
	ex->ee_len = cpu_to_le16(split - ee_block);
	if (split_flag & EXT4_EXT_MARK_UNWRIT1)
		ext4_ext_mark_unwritten(ex);

	/*
	 * path may lead to new leaf, not to original leaf any more
	 * after ext4_ext_insert_extent() returns,
	 */
	err = ext4_ext_dirty(icb, handle, inode, path + depth);
	if (err)
		goto fix_extent_len;

	ex2 = &newex;
	ex2->ee_block = cpu_to_le32(split);
	ex2->ee_len   = cpu_to_le16(ee_len - (split - ee_block));
	ext4_ext_store_pblock(ex2, newblock);
	if (split_flag & EXT4_EXT_MARK_UNWRIT2)
		ext4_ext_mark_unwritten(ex2);

	err = ext4_ext_insert_extent(icb, handle, inode, ppath, &newex, flags);
	if (err == -ENOSPC && (EXT4_EXT_MAY_ZEROOUT & split_flag)) {
		if (split_flag & (EXT4_EXT_DATA_VALID1|EXT4_EXT_DATA_VALID2)) {
			if (split_flag & EXT4_EXT_DATA_VALID1) {
				err = ext4_ext_zeroout(inode, ex2);
				zero_ex.ee_block = ex2->ee_block;
				zero_ex.ee_len = cpu_to_le16(
						ext4_ext_get_actual_len(ex2));
				ext4_ext_store_pblock(&zero_ex,
						ext4_ext_pblock(ex2));
			} else {
				err = ext4_ext_zeroout(inode, ex);
				zero_ex.ee_block = ex->ee_block;
				zero_ex.ee_len = cpu_to_le16(
						ext4_ext_get_actual_len(ex));
				ext4_ext_store_pblock(&zero_ex,
						ext4_ext_pblock(ex));
			}
		} else {
			err = ext4_ext_zeroout(inode, &orig_ex);
			zero_ex.ee_block = orig_ex.ee_block;
			zero_ex.ee_len = cpu_to_le16(
					ext4_ext_get_actual_len(&orig_ex));
			ext4_ext_store_pblock(&zero_ex,
					ext4_ext_pblock(&orig_ex));
		}

		if (err)
			goto fix_extent_len;
		/* update the extent length and mark as initialized */
		ex->ee_len = cpu_to_le16(ee_len);
		ext4_ext_try_to_merge(icb, handle, inode, path, ex);
		err = ext4_ext_dirty(icb, handle, inode, path + path->p_depth);
		if (err)
			goto fix_extent_len;

		goto out;
	} else if (err)
		goto fix_extent_len;

out:
	ext4_ext_show_leaf(inode, path);
	return err;

fix_extent_len:
	ex->ee_len = orig_ex.ee_len;
	ext4_ext_dirty(icb, handle, inode, path + path->p_depth);
	return err;
}

int ext4_ext_remove_space(void *icb, struct inode *inode, unsigned long start)
{
	int depth = ext_depth(inode);
	struct ext4_ext_path *path;
	handle_t *handle = NULL;
	int i = 0, err = 0;

	/* probably first extent we're gonna free will be last in block */
	/*handle = ext4_journal_start(inode, depth + 1);*/
	/*if (IS_ERR(icb, handle))*/
	/*return PTR_ERR(icb, handle);*/

	/*
	 * we start scanning from right side freeing all the blocks
	 * after i_size and walking into the deep
	 */
	path = kmalloc(sizeof(struct ext4_ext_path) * (depth + 1), GFP_KERNEL);
	if (path == NULL) {
		ext4_journal_stop(icb, handle);
		return -ENOMEM;
	}
	memset(path, 0, sizeof(struct ext4_ext_path) * (depth + 1));
	path[0].p_hdr = ext_inode_hdr(inode);
	if (ext4_ext_check_inode(inode)) {
		err = -EIO;
		goto out;
	}
	path[0].p_depth = depth;

	while (i >= 0 && err == 0) {
		if (i == depth) {
			/* this is leaf block */
			err = ext4_ext_rm_leaf(icb, handle, inode, path, start);
			/* root level have p_bh == NULL, extents_brelse() eats this */
			extents_brelse(path[i].p_bh);
			path[i].p_bh = NULL;
			i--;
			continue;
		}

		/* this is index block */
		if (!path[i].p_hdr) {
			path[i].p_hdr = ext_block_hdr(path[i].p_bh);
		}

		if (!path[i].p_idx) {
			/* this level hasn't touched yet */
			path[i].p_idx = EXT_LAST_INDEX(path[i].p_hdr);
			path[i].p_block = le16_to_cpu(path[i].p_hdr->eh_entries)+1;
		} else {
			/* we've already was here, see at next index */
			path[i].p_idx--;
		}

		if (ext4_ext_more_to_rm(path + i)) {
			struct buffer_head *bh;
			/* go to the next level */
			memset(path + i + 1, 0, sizeof(*path));
			bh = read_extent_tree_block(inode, ext4_idx_pblock(path[i].p_idx), path[0].p_depth - (i + 1), 0);
			if (IS_ERR(bh)) {
				/* should we reset i_size? */
				err = -EIO;
				break;
			}
			path[i+1].p_bh = bh;

			/* put actual number of indexes to know is this
			 * number got changed at the next iteration */
			path[i].p_block = le16_to_cpu(path[i].p_hdr->eh_entries);
			i++;
		} else {
			/* we finish processing this index, go up */
			if (path[i].p_hdr->eh_entries == 0 && i > 0) {
				/* index is empty, remove it
				 * handle must be already prepared by the
				 * truncatei_leaf() */
				err = ext4_ext_rm_idx(icb, handle, inode, path + i);
			}
			/* root level have p_bh == NULL, extents_brelse() eats this */
			extents_brelse(path[i].p_bh);
			path[i].p_bh = NULL;
			i--;
		}
	}

	/* a tree emptied by the truncation collapses back into the inode: depth 0, root-sized node */
	if (path->p_hdr->eh_entries == 0) {
		/*
		 * truncate to zero freed all the tree
		 * so, we need to correct eh_depth
		 */
		err = ext4_ext_get_access(icb, handle, inode, path);
		if (err == 0) {
			ext_inode_hdr(inode)->eh_depth = 0;
			ext_inode_hdr(inode)->eh_max =
				cpu_to_le16(ext4_ext_space_root(inode, 0));
			err = ext4_ext_dirty(icb, handle, inode, path);
		}
	}
out:
	if (path) {
		ext4_ext_drop_refs(path);
		kfree(path);
	}
	ext4_journal_stop(icb, handle);

	return err;
}

int ext4_ext_truncate(void *icb, struct inode *inode, unsigned long start)
{
    int ret = ext4_ext_remove_space(icb, inode, start);

	/* Save modifications on i_blocks field of the inode. */
	if (!ret)
		ret = ext4_mark_inode_dirty(icb, NULL, inode);

	return ret;
}
