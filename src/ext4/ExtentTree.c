/**
 * ExtentTree.c - extent tree geometry, validation and path search.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "linux\ext4.h"
#include "extents_internal.h"


static int __ext4_ext_check(const char *function, unsigned int line,
		struct inode *inode,
		struct ext4_extent_header *eh, int depth,
		ext4_fsblk_t pblk);

#define ext4_ext_check(inode, eh, depth, pblk)			\
	__ext4_ext_check("", __LINE__, (inode), (eh), (depth), (pblk))

#define ext4_ext_show_path(inode, path)

static int
ext4_ext_max_entries(struct inode *inode, int depth)
{
	int max;

	if (depth == ext_depth(inode)) {
		if (depth == 0)
			max = ext4_ext_space_root(inode, 1);
		else
			max = ext4_ext_space_root_idx(inode, 1);
	} else {
		if (depth == 0)
			max = ext4_ext_space_block(inode, 1);
		else
			max = ext4_ext_space_block_idx(inode, 1);
	}

	return max;
}

/*
 * read_extent_tree_block:
 * Get a buffer_head by extents_bread, and read fresh data from the storage.
 */
struct buffer_head *
__read_extent_tree_block(const char *function, unsigned int line,
		struct inode *inode, ext4_fsblk_t pblk, int depth,
		int flags)
{
    UNREFERENCED_PARAMETER(flags);
	struct buffer_head		*bh;
	int				err;

	bh = extents_bread(inode->i_sb, pblk);
	if (!bh)
		return ERR_PTR(-ENOMEM);

	if (!buffer_uptodate(bh)) {
		err = -EIO;
		goto errout;
	}
	if (buffer_verified(bh))
		return bh;
	err = __ext4_ext_check(function, line, inode,
			ext_block_hdr(bh), depth, pblk);
	if (err)
		goto errout;
	set_buffer_verified(bh);
	return bh;
errout:
	extents_brelse(bh);
	return ERR_PTR(err);

}

int ext4_ext_check_inode(struct inode *inode)
{
	return ext4_ext_check(inode, ext_inode_hdr(inode), ext_depth(inode), 0);
}

/*
 * could return:
 *  - EROFS
 *  - ENOMEM
 */

int ext4_ext_get_access(void *icb, handle_t *handle, struct inode *inode,
		struct ext4_ext_path *path)
{
    UNREFERENCED_PARAMETER(inode);
	if (path->p_bh) {
		/* path points to block */

		return ext4_journal_get_write_access(icb, handle, path->p_bh);

	}
	/* path points to leaf/index in inode body */
	/* we use in-core data, no need to protect them */
	return 0;
}

ext4_fsblk_t ext4_ext_find_goal(struct inode *inode,
		struct ext4_ext_path *path,
		ext4_lblk_t block)
{
	if (path) {
		int depth = path->p_depth;
		struct ext4_extent *ex;

		/*
		 * Try to predict block placement assuming that we are
		 * filling in a file which will eventually be
		 * non-sparse --- i.e., in the case of libbfd writing
		 * an ELF object sections out-of-order but in a way
		 * the eventually results in a contiguous object or
		 * executable file, or some database extending a table
		 * space file.  However, this is actually somewhat
		 * non-ideal if we are writing a sparse file such as
		 * qemu or KVM writing a raw image file that is going
		 * to stay fairly sparse, since it will end up
		 * fragmenting the file system's free space.  Maybe we
		 * should have some hueristics or some way to allow
		 * userspace to pass a hint to file system,
		 * especially if the latter case turns out to be
		 * common.
		 */
		ex = path[depth].p_ext;
		if (ex) {
			ext4_fsblk_t ext_pblk = ext4_ext_pblock(ex);
			ext4_lblk_t ext_block = le32_to_cpu(ex->ee_block);

			if (block > ext_block)
				return ext_pblk + (block - ext_block);
			else
				return ext_pblk - (ext_block - block);
		}

		/* it looks like index is empty;
		 * try to find starting block from index itself */
		if (path[depth].p_bh)
			return path[depth].p_bh->b_blocknr;
	}

	/* OK. use inode's group */
	return ext4_inode_to_goal_block(inode);
}

/*
 * Allocation for a meta data block
 */
ext4_fsblk_t
ext4_ext_new_meta_block(void *icb, handle_t *handle, struct inode *inode,
		struct ext4_ext_path *path,
		struct ext4_extent *ex, int *err, unsigned int flags)
{
	ext4_fsblk_t goal, newblock;

	goal = ext4_ext_find_goal(inode, path, le32_to_cpu(ex->ee_block));
	newblock = ext4_new_meta_blocks(icb, handle, inode, goal, flags,
			NULL, err);
	return newblock;
}

int __ext4_ext_dirty(const char *where, unsigned int line,
		void *icb, handle_t *handle,
		struct inode *inode,
		struct ext4_ext_path *path)
{
	int err;

	if (path->p_bh) {
		ext4_extent_block_csum_set(inode, ext_block_hdr(path->p_bh));
		/* path points to block */
		err = __ext4_handle_dirty_metadata(where, line, icb, handle, inode, path->p_bh);
	} else {
		/* path points to leaf/index in inode body */
		err = ext4_mark_inode_dirty(icb, handle, inode);
	}
	return err;
}

void ext4_ext_drop_refs(struct ext4_ext_path *path)
{
	int depth, i;

	if (!path)
		return;
	depth = path->p_depth;
	for (i = 0; i <= depth; i++, path++)
		if (path->p_bh) {
			extents_brelse(path->p_bh);
			path->p_bh = NULL;
		}
}

/*
 * The entries of a node, as Linux checks them (ext4_valid_extent_entries):
 * every block they name lies inside the file system, extents are not
 * empty, and the logical ranges go up without overlapping. A node that
 * points past the volume (an index naming block 0xFFFFFFFF) or loops back
 * on itself is corrupt, never followed. Nor may a block of the volume's own
 * metadata belong to a file (Linux: ext4_inode_block_valid and its system
 * zone): written as the file's data, it would destroy bitmaps or inodes.
 */
static int ext4_ext_block_valid(struct inode *inode, ext4_fsblk_t start, unsigned int count)
{
	struct ext4_super_block *es = EXT4_SB(inode->i_sb)->s_es;
	ext4_fsblk_t first = le32_to_cpu(es->s_first_data_block);
	ext4_fsblk_t total = ext3_blocks_count(es);

	return start >= first && count > 0 && start + count > start && start + count <= total &&
	       !Ext2MetadataOverlaps(inode->i_sb->s_priv, start, count);
}

static int ext4_valid_extent_entries(struct inode *inode,
				     struct ext4_extent_header *eh, int depth)
{
	unsigned short entries = le16_to_cpu(eh->eh_entries);

	if (entries == 0)
		return 1;
	if (depth == 0) {
		struct ext4_extent *ext = EXT_FIRST_EXTENT(eh);
		ext4_lblk_t next = 0;

		while (entries--) {
			ext4_lblk_t lblk = le32_to_cpu(ext->ee_block);
			unsigned int len = ext4_ext_get_actual_len(ext);

			if (!ext4_ext_block_valid(inode, ext4_ext_pblock(ext), len))
				return 0;
			if (lblk < next || lblk + len < lblk)
				return 0;
			next = lblk + len;
			ext++;
		}
	} else {
		struct ext4_extent_idx *idx = EXT_FIRST_INDEX(eh);
		ext4_lblk_t prev = 0;
		int first = 1;

		while (entries--) {
			ext4_lblk_t lblk = le32_to_cpu(idx->ei_block);

			if (!ext4_ext_block_valid(inode, ext4_idx_pblock(idx), 1))
				return 0;
			if (!first && lblk <= prev)
				return 0;
			prev = lblk;
			first = 0;
			idx++;
		}
	}
	return 1;
}

/*
 * Check that whether the basic information inside the extent header
 * is correct or not.
 */
static int __ext4_ext_check(const char *function, unsigned int line,
		struct inode *inode,
		struct ext4_extent_header *eh, int depth,
		ext4_fsblk_t pblk)
{
    UNREFERENCED_PARAMETER(function);
    UNREFERENCED_PARAMETER(line);
	const char *error_msg;

	if (eh->eh_magic != EXT4_EXT_MAGIC) {
		error_msg = "invalid magic";
		goto corrupted;
	}
	if (le16_to_cpu(eh->eh_depth) != depth) {
		error_msg = "unexpected eh_depth";
		goto corrupted;
	}
	if (eh->eh_max == 0 ||
	    le16_to_cpu(eh->eh_max) > ext4_ext_max_entries(inode, depth)) {
		error_msg = "invalid eh_max";
		goto corrupted;
	}
	if (le16_to_cpu(eh->eh_entries) > le16_to_cpu(eh->eh_max)) {
		error_msg = "invalid eh_entries";
		goto corrupted;
	}
	if (eh->eh_entries == 0 && depth != 0) {
		error_msg = "an empty index";
		goto corrupted;
	}
	if (!ext4_valid_extent_entries(inode, eh, depth)) {
		error_msg = "invalid extent entries";
		goto corrupted;
	}

	/* A node whose checksum does not match is refused, as Linux refuses it
	   (EFSBADCRC): followed and changed, it would be written back under a
	   fresh checksum, its damage hidden. The root is in the inode, under
	   the inode's checksum: it has no tail. */
	if (depth != ext_depth(inode) && !ext4_extent_block_csum_verify(inode, eh)) {
		error_msg = "checksum does not match";
		goto corrupted;
	}

	return 0;

corrupted:
	DbgPrint("ext4: inode %u: extent node (block %I64u, depth %d): %s\n",
		 (ULONG)inode->i_ino, (ULONGLONG)pblk, depth, error_msg);
	return -EIO;
}

/*
 * ext4_ext_binsearch_idx:
 * binary search for the closest index of the given block
 * the header must be checked before calling this
 */
static void
ext4_ext_binsearch_idx(struct inode *inode,
		struct ext4_ext_path *path, ext4_lblk_t block)
{
    UNREFERENCED_PARAMETER(inode);
	struct ext4_extent_header *eh = path->p_hdr;
	struct ext4_extent_idx *r, *l, *m;

	ext_debug("binsearch for %u(idx):  ", block);

	l = EXT_FIRST_INDEX(eh) + 1;
	r = EXT_LAST_INDEX(eh);
	while (l <= r) {
		m = l + (r - l) / 2;
		if (block < (m->ei_block))
			r = m - 1;
		else
			l = m + 1;
		ext_debug("%p(%u):%p(%u):%p(%u) ", l, (l->ei_block),
				m, (m->ei_block),
				r, (r->ei_block));
	}

	path->p_idx = l - 1;
	ext_debug("  -> %u->%lld ", (path->p_idx->ei_block),
			ext4_idx_pblock(path->p_idx));


}

/*
 * ext4_ext_binsearch:
 * binary search for closest extent of the given block
 * the header must be checked before calling this
 */
static void
ext4_ext_binsearch(struct inode *inode,
		struct ext4_ext_path *path, ext4_lblk_t block)
{
    UNREFERENCED_PARAMETER(inode);
	struct ext4_extent_header *eh = path->p_hdr;
	struct ext4_extent *r, *l, *m;

	if (eh->eh_entries == 0) {
		/*
		 * this leaf is empty:
		 * we get such a leaf in split/add case
		 */
		return;
	}

	ext_debug("binsearch for %u:  ", block);

	l = EXT_FIRST_EXTENT(eh) + 1;
	r = EXT_LAST_EXTENT(eh);

	while (l <= r) {
		m = l + (r - l) / 2;
		if (block < m->ee_block)
			r = m - 1;
		else
			l = m + 1;
		ext_debug("%p(%u):%p(%u):%p(%u) ", l, l->ee_block,
				m, (m->ee_block),
				r, (r->ee_block));
	}

	path->p_ext = l - 1;
	ext_debug("  -> %d:%llu:[%d]%d ",
			(path->p_ext->ee_block),
			ext4_ext_pblock(path->p_ext),
			ext4_ext_is_unwritten(path->p_ext),
			ext4_ext_get_actual_len(path->p_ext));


}

struct ext4_ext_path *
ext4_find_extent(struct inode *inode, ext4_lblk_t block,
		struct ext4_ext_path **orig_path, int flags)
{
	struct ext4_extent_header *eh;
	struct buffer_head *bh;
	struct ext4_ext_path *path = orig_path ? *orig_path : NULL;
	short int depth, i, ppos = 0;
	int ret;

	eh = ext_inode_hdr(inode);
	depth = ext_depth(inode);

	/* the root in the inode, checked as every block below it is: a
	   corrupt one (a depth of thousands, entries past i_block) is never
	   walked */
	if (depth < 0 || depth > EXT4_MAX_EXTENT_DEPTH || ext4_ext_check_inode(inode))
		return ERR_PTR(-EFSCORRUPTED);

	if (path) {
		ext4_ext_drop_refs(path);
		if (depth > path[0].p_maxdepth) {
			kfree(path);
			*orig_path = path = NULL;
		}
	}
	if (!path) {
		/* account possible depth increase */
		path = kzalloc(sizeof(struct ext4_ext_path) * (depth + 2),
				GFP_NOFS);
		if (unlikely(!path))
			return ERR_PTR(-ENOMEM);
		path[0].p_maxdepth = depth + 1;
	}
	path[0].p_hdr = eh;
	path[0].p_bh = NULL;

	i = depth;
	/* walk through the tree */
	while (i) {
		ext_debug("depth %d: num %d, max %d\n",
				ppos, le16_to_cpu(eh->eh_entries), le16_to_cpu(eh->eh_max));

		ext4_ext_binsearch_idx(inode, path + ppos, block);
		path[ppos].p_block = ext4_idx_pblock(path[ppos].p_idx);
		path[ppos].p_depth = i;
		path[ppos].p_ext = NULL;

		bh = read_extent_tree_block(inode, path[ppos].p_block, --i,
				flags);
		if (unlikely(IS_ERR(bh))) {
			ret = PTR_ERR(bh);
			goto err;
		}

		eh = ext_block_hdr(bh);
		ppos++;
		if (unlikely(ppos > depth)) {
			extents_brelse(bh);
			EXT4_ERROR_INODE(inode,
					"ppos %d > depth %d", ppos, depth);
			ret = -EIO;
			goto err;
		}
		path[ppos].p_bh = bh;
		path[ppos].p_hdr = eh;
	}

	path[ppos].p_depth = i;
	path[ppos].p_ext = NULL;
	path[ppos].p_idx = NULL;

	/* find extent */
	ext4_ext_binsearch(inode, path + ppos, block);
	/* if not an empty leaf */
	if (path[ppos].p_ext)
		path[ppos].p_block = ext4_ext_pblock(path[ppos].p_ext);

	ext4_ext_show_path(inode, path);

	return path;

err:
	ext4_ext_drop_refs(path);
	if (path) {
		kfree(path);
		if (orig_path)
			*orig_path = NULL;
	}
	return ERR_PTR(ret);
}

int ext4_ext_tree_init(void *icb, handle_t *handle, struct inode *inode)
{
    UNREFERENCED_PARAMETER(handle);
	struct ext4_extent_header *eh;

	eh = ext_inode_hdr(inode);
	eh->eh_depth = 0;
	eh->eh_entries = 0;
	eh->eh_magic = cpu_to_le16(EXT4_EXT_MAGIC);
	eh->eh_max = cpu_to_le16(ext4_ext_space_root(inode, 0));
	return ext4_mark_inode_dirty(icb, handle, inode);
}

/*
 * called at mount time
 */
void ext4_ext_init(struct super_block *sb)
{
    UNREFERENCED_PARAMETER(sb);
	/*
	 * possible initialization would be here
	 */
}
