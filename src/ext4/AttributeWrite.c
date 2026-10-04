/**
 * AttributeWrite.c - extended attributes: item insertion, resizing and writing to disk.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include <linux/module.h>
#include <linux/ext4_xattr.h>
#include "xattr_internal.h"


/* the xattr block of an inode: journaled metadata */
static int ext4_xattr_new_block(PEXT2_IRP_CONTEXT IrpContext, struct inode *inode,
	ext4_fsblk_t *block)
{
	ULONG		count = 1;
	ULONGLONG	found = 0;
	NTSTATUS	status;

	status = Ext2AllocateInodeBlocks(IrpContext, inode, ext4_inode_to_goal_block(inode),
					 EXT2_ALLOC_JOURNALED, &found, &count);
	*block = found;
	return NT_SUCCESS(status) ? 0 : Ext2LinuxError(status);
}

static int ext4_xattr_free_block(PEXT2_IRP_CONTEXT IrpContext, struct inode *inode,
	ext4_fsblk_t block)
{
	NTSTATUS status = Ext2ReleaseInodeBlocks(IrpContext, inode, block, 1);
	return NT_SUCCESS(status) ? 0 : Ext2LinuxError(status);
}

/*
 * Where a new value goes: the inode body, else the block; with ea_inode a
 * value too large for either goes to a value inode and only its entry to
 * the body or the block - at once when it is larger than a block entry
 * can be, as on Linux, else when it fits nowhere. -ENOSPC: nowhere at all.
 */
static int ext4_xattr_place(struct ext4_xattr_ref *xattr_ref, size_t name_len,
			    size_t data_size, BOOL block_only,
			    BOOL *in_inode, BOOL *ea)
{
	BOOL has_ea = ext4_has_feature_ea_inode(&xattr_ref->fs->sb);
	size_t need;

	*ea = has_ea && EXT4_XATTR_SIZE(data_size) >
		EXT4_XATTR_MIN_LARGE_EA_SIZE(xattr_ref->fs->BlockSize);
	for (;;) {
		if (*ea && data_size > EXT4_XATTR_SIZE_MAX)
			return -ENOSPC;
		need = ext4_xattr_space(name_len, data_size, *ea);
		if (!block_only && xattr_ref->inode_size_rem >= need) {
			*in_inode = TRUE;
			return 0;
		}
		if (xattr_ref->block_size_rem >= need) {
			*in_inode = FALSE;
			return 0;
		}
		if (*ea || !has_ea)
			return -ENOSPC;
		*ea = TRUE;
	}
}

/* an item into the set, its space taken where it was placed */
static void ext4_xattr_attach(struct ext4_xattr_ref *xattr_ref,
			      struct ext4_xattr_item *item)
{
	ext4_xattr_item_insert(xattr_ref, item);
	xattr_ref->ea_size += ext4_xattr_item_space(item);
	if (item->in_inode)
		xattr_ref->inode_size_rem -= ext4_xattr_item_space(item);
	else
		xattr_ref->block_size_rem -= ext4_xattr_item_space(item);
	xattr_ref->dirty = TRUE;
}

/* the reverse: out of the set, its space given back, not freed */
static void ext4_xattr_detach(struct ext4_xattr_ref *xattr_ref,
			      struct ext4_xattr_item *item)
{
	if (item == xattr_ref->iter_from) {
		struct rb_node *next_node = rb_next(&item->node);

		xattr_ref->iter_from = next_node ?
			container_of(next_node, struct ext4_xattr_item, node) : NULL;
	}
	xattr_ref->ea_size -= ext4_xattr_item_space(item);
	if (item->in_inode)
		xattr_ref->inode_size_rem += ext4_xattr_item_space(item);
	else
		xattr_ref->block_size_rem += ext4_xattr_item_space(item);
	ext4_xattr_item_remove(xattr_ref, item);
	xattr_ref->dirty = TRUE;
}

static struct ext4_xattr_item *
ext4_xattr_add_item(struct ext4_xattr_ref *xattr_ref, __u8 name_index,
		    const char *name, size_t name_len, const void *data,
		    size_t data_size, BOOL block_only, int *err)
{
	struct ext4_xattr_item *item;
	BOOL in_inode, ea;
	int ret;

	ret = ext4_xattr_place(xattr_ref, name_len, data_size, block_only,
			       &in_inode, &ea);
	if (ret) {
		if (err)
			*err = ret;
		return NULL;
	}

	item = ext4_xattr_item_alloc(name_index, name, name_len);
	if (!item || ext4_xattr_item_alloc_data(item, data, data_size) != 0) {
		if (item)
			ext4_xattr_item_free(item);
		if (err)
			*err = -ENOMEM;
		return NULL;
	}
	item->in_inode = in_inode;
	item->ea = ea;
	ext4_xattr_attach(xattr_ref, item);
	if (err)
		*err = 0;
	return item;
}

struct ext4_xattr_item *
ext4_xattr_insert_item(struct ext4_xattr_ref *xattr_ref, __u8 name_index,
		       const char *name, size_t name_len, const void *data,
		       size_t data_size,
		       int *err)
{
	return ext4_xattr_add_item(xattr_ref, name_index, name, name_len,
				   data, data_size, FALSE, err);
}

/* after an item already in the block, the rest go to the block too: the
   order they came in is the order they are listed in */
struct ext4_xattr_item *
ext4_xattr_insert_item_ordered(struct ext4_xattr_ref *xattr_ref, __u8 name_index,
	const char *name, size_t name_len, const void *data,
	size_t data_size,
	int *err)
{
	struct ext4_xattr_item *last_item = NULL;

	if (!list_empty(&xattr_ref->ordered_list))
		last_item = list_entry(xattr_ref->ordered_list.prev,
					struct ext4_xattr_item,
					list_node);

	return ext4_xattr_add_item(xattr_ref, name_index, name, name_len,
				   data, data_size,
				   last_item && !last_item->in_inode, err);
}

int ext4_xattr_drop_ea(struct ext4_xattr_ref *ref, struct ext4_xattr_item *item)
{
	if (!item->ea || !item->ea_ino)
		return 0;

	if (ref->ea_drop_count == ref->ea_drop_max) {
		size_t max = ref->ea_drop_max ? ref->ea_drop_max * 2 : 8;
		struct ext4_xattr_drop *drop = kmalloc(max * sizeof(*drop), GFP_NOFS);

		if (!drop)
			return -ENOMEM;
		if (ref->ea_drop) {
			memcpy(drop, ref->ea_drop, ref->ea_drop_count * sizeof(*drop));
			kfree(ref->ea_drop);
		}
		ref->ea_drop = drop;
		ref->ea_drop_max = max;
	}
	ref->ea_drop[ref->ea_drop_count].ino = item->ea_ino;
	ref->ea_drop[ref->ea_drop_count].size = item->data_size;
	ref->ea_drop_count++;
	item->ea_ino = 0;
	item->ea_new = FALSE;
	return 0;
}

int ext4_xattr_remove_item(struct ext4_xattr_ref *xattr_ref,
				  __u8 name_index, const char *name,
				  size_t name_len)
{
	int ret;
	struct ext4_xattr_item *item =
	    ext4_xattr_lookup_item(xattr_ref, name_index, name, name_len);

	if (!item)
		return -ENOENT;
	ret = ext4_xattr_drop_ea(xattr_ref, item);
	if (ret)
		return ret;
	ext4_xattr_detach(xattr_ref, item);
	ext4_xattr_item_free(item);
	return 0;
}

/*
 * A new value for an existing item, placed afresh: into or out of a value
 * inode, between the body and the block. The old item comes back if the
 * new value fits nowhere; its value inode is dropped once it is replaced.
 */
int ext4_xattr_replace_item(struct ext4_xattr_ref *xattr_ref,
			    struct ext4_xattr_item *item,
			    const void *data, size_t data_size)
{
	struct ext4_xattr_item *fresh;
	int ret;

	ext4_xattr_detach(xattr_ref, item);
	fresh = ext4_xattr_add_item(xattr_ref, item->name_index, item->name,
				    item->name_len, data, data_size, FALSE, &ret);
	if (!fresh) {
		ext4_xattr_attach(xattr_ref, item);
		return ret;
	}
	ret = ext4_xattr_drop_ea(xattr_ref, item);
	if (ret) {
		ext4_xattr_detach(xattr_ref, fresh);
		ext4_xattr_item_free(fresh);
		ext4_xattr_attach(xattr_ref, item);
		return ret;
	}
	ext4_xattr_item_free(item);
	return 0;
}

int ext4_xattr_resize_item(struct ext4_xattr_ref *xattr_ref,
				  struct ext4_xattr_item *item,
				  size_t new_data_size)
{
	int ret = 0;
	BOOL to_inode = FALSE, to_block = FALSE;
	size_t old_data_size = item->data_size;
	size_t orig_room_size = item->in_inode ?
		xattr_ref->inode_size_rem :
		xattr_ref->block_size_rem;

	/*
	 * Check if we can hold this entry in both in-inode and
	 * on-block form
	 *
	 * More complicated case: we do not allow entries stucking in
	 * the middle between in-inode space and on-block space, so
	 * the entry has to stay in either inode space or block space.
	 */
	if (item->in_inode) {
		if (xattr_ref->inode_size_rem +
			       EXT4_XATTR_SIZE(old_data_size) <
			       EXT4_XATTR_SIZE(new_data_size)) {
			if (xattr_ref->block_size_rem <
				       EXT4_XATTR_SIZE(new_data_size) +
				       EXT4_XATTR_LEN(item->name_len))
				return -ENOSPC;

			to_block = TRUE;
		}
	} else {
		if (xattr_ref->block_size_rem +
				EXT4_XATTR_SIZE(old_data_size) <
				EXT4_XATTR_SIZE(new_data_size)) {
			if (xattr_ref->inode_size_rem <
					EXT4_XATTR_SIZE(new_data_size) +
					EXT4_XATTR_LEN(item->name_len))
				return -ENOSPC;

			to_inode = TRUE;
		}
	}
	ret = ext4_xattr_item_resize_data(item, new_data_size);
	if (ret)
		return ret;

	xattr_ref->ea_size =
	    xattr_ref->ea_size -
	    EXT4_XATTR_SIZE(old_data_size) +
	    EXT4_XATTR_SIZE(new_data_size);

	/*
	 * This entry may originally lie in inode space or block space,
	 * and it is going to be transferred to another place.
	 */
	if (to_block) {
		xattr_ref->inode_size_rem +=
			EXT4_XATTR_SIZE(old_data_size) +
			EXT4_XATTR_LEN(item->name_len);
		xattr_ref->block_size_rem -=
			EXT4_XATTR_SIZE(new_data_size) +
			EXT4_XATTR_LEN(item->name_len);
		item->in_inode = FALSE;
	} else if (to_inode) {
		xattr_ref->block_size_rem +=
			EXT4_XATTR_SIZE(old_data_size) +
			EXT4_XATTR_LEN(item->name_len);
		xattr_ref->inode_size_rem -=
			EXT4_XATTR_SIZE(new_data_size) +
			EXT4_XATTR_LEN(item->name_len);
		item->in_inode = TRUE;
	} else {
		/*
		 * No need to transfer as there is enough space for the entry
		 * to stay in inode space or block space it used to be.
		 */
		orig_room_size +=
			EXT4_XATTR_SIZE(old_data_size);
		orig_room_size -=
			EXT4_XATTR_SIZE(new_data_size);
		if (item->in_inode)
			xattr_ref->inode_size_rem = orig_room_size;
		else
			xattr_ref->block_size_rem = orig_room_size;

	}
	xattr_ref->dirty = TRUE;
	return ret;
}

void ext4_xattr_purge_items(struct ext4_xattr_ref *xattr_ref)
{
	struct rb_node *first_node;
	struct ext4_xattr_item *item = NULL;
	first_node = rb_first(&xattr_ref->root);
	if (first_node)
		item = container_of(first_node, struct ext4_xattr_item,
				    node);

	while (item) {
		struct rb_node *next_node;
		struct ext4_xattr_item *next_item = NULL;
		next_node = rb_next(&item->node);
		if (next_node)
			next_item = container_of(next_node, struct ext4_xattr_item,
						 node);
		else
			next_item = NULL;

		ext4_xattr_item_remove(xattr_ref, item);
		ext4_xattr_item_free(item);

		item = next_item;
	}
	xattr_ref->ea_size = 0;
	xattr_ref->iter_from = NULL;
	xattr_ref->inode_size_rem = ext4_xattr_inode_room(xattr_ref);
	xattr_ref->block_size_rem =
		ext4_xattr_block_space(xattr_ref) -
		sizeof(struct ext4_xattr_header) -
		sizeof(__u32);
}

void ext4_xattr_remove_all(struct ext4_xattr_ref *xattr_ref)
{
	struct ext4_xattr_item *item;

	list_for_each_entry(item, &xattr_ref->ordered_list, struct ext4_xattr_item, list_node) {
		if (ext4_xattr_drop_ea(xattr_ref, item))
			DbgPrint("ext4: inode %u: no memory to release xattr value inode %u\n",
				 xattr_ref->inode_ref->Inode->i_ino, item->ea_ino);
	}
	ext4_xattr_purge_items(xattr_ref);
	xattr_ref->dirty = TRUE;
}

static int ext4_xattr_try_free_block(struct ext4_xattr_ref *xattr_ref)
{
	ext4_fsblk_t xattr_block;
	xattr_block = xattr_ref->inode_ref->Inode->i_file_acl;
    int ret = ext4_xattr_free_block(xattr_ref->IrpContext, xattr_ref->inode_ref->Inode,
                                    xattr_block);
    if (ret)
        return ret;
    xattr_ref->inode_ref->Inode->i_file_acl = 0;
	extents_brelse(xattr_ref->block_bh);
	xattr_ref->block_bh = NULL;
	xattr_ref->IsOnDiskInodeDirty = TRUE;
    xattr_ref->block_loaded = FALSE;
    return 0;
}

static void ext4_xattr_set_block_header(struct ext4_xattr_ref *xattr_ref)
{
	struct ext4_xattr_header *block_header = NULL;
	block_header = EXT4_XATTR_BHDR(xattr_ref->block_bh);

	memset(block_header, 0, sizeof(struct ext4_xattr_header));
	block_header->h_magic = EXT4_XATTR_MAGIC;
	block_header->h_refcount = cpu_to_le32(1);
	block_header->h_blocks = cpu_to_le32(1);
}

/*
 * One entry: the name, and the value where the item keeps it - in the
 * table's value area (data moves down from the end), or in a value inode
 * (the entry names it, offset 0). The hash is Linux's: of the value for a
 * block entry, of the value inode's hash for a value inode, none for a
 * value in the inode body.
 */
static void ext4_xattr_set_entry(struct ext4_xattr_item *item,
				 struct ext4_xattr_entry *entry,
				 char **data_end, char *base, BOOL in_block)
{
	entry->e_name_len = (__u8)item->name_len;
	entry->e_name_index = item->name_index;
	entry->e_value_size = cpu_to_le32(item->data_size);
	memcpy(EXT4_XATTR_NAME(entry), item->name, item->name_len);
	if (item->ea) {
		entry->e_value_offs = 0;
		entry->e_value_block = cpu_to_le32(item->ea_ino);
		ext4_xattr_compute_hash_ea(entry, item->ea_hash);
		return;
	}
	*data_end -= EXT4_XATTR_SIZE(item->data_size);
	entry->e_value_offs = cpu_to_le16((__u16)(*data_end - base));
	entry->e_value_block = 0;
	memcpy(*data_end, item->data, item->data_size);
	if (in_block)
		ext4_xattr_compute_hash((struct ext4_xattr_header *)base, entry);
	else
		entry->e_hash = 0;
}

int ext4_xattr_write_to_disk(struct ext4_xattr_ref *xattr_ref)
{
	int ret = 0;
	BOOL need_block = FALSE, shared = FALSE;
	struct inode *inode = xattr_ref->inode_ref->Inode;
	struct buffer_head *new_bh = NULL;
	ext4_fsblk_t new_block = 0;
	struct ext4_xattr_item *item = NULL;
	struct rb_node *node;
	__s32 ibody_space;

	if (!xattr_ref->dirty)
		goto Finish;

	/* values that go to value inodes first: the entries name them */
	list_for_each_entry(item, &xattr_ref->ordered_list, struct ext4_xattr_item, list_node) {
		if (item->ea && !item->ea_ino) {
			ret = ext4_xattr_inode_create(xattr_ref, item->data,
						      item->data_size,
						      &item->ea_ino, &item->ea_hash);
			if (ret)
				goto Finish;
			item->ea_new = TRUE;
		}
		if (!item->in_inode)
			need_block = TRUE;
	}

	/*
	 * The block. One shared with other inodes (Linux shares identical
	 * ones, h_refcount counts them) is theirs as much as ours: this inode
	 * lets go of it and takes a block of its own if it still needs one.
	 * Everything that can fail comes before anything is given up.
	 */
	if (xattr_ref->block_loaded)
		shared = le32_to_cpu(EXT4_XATTR_BHDR(xattr_ref->block_bh)->h_refcount) > 1;
	if (need_block && (!xattr_ref->block_loaded || shared)) {
		ret = ext4_xattr_new_block(xattr_ref->IrpContext, inode, &new_block);
		if (ret)
			goto Finish;
		new_bh = extents_bwrite(&xattr_ref->fs->sb, new_block);
		if (!new_bh) {
			/* a failed release stops the journal by itself (Ext2ReleaseInodeBlocks);
			   the error reported is the one that came first */
			(void)ext4_xattr_free_block(xattr_ref->IrpContext, inode, new_block);
			ret = -ENOMEM;
			goto Finish;
		}
	}
	if (xattr_ref->block_loaded && (shared || !need_block)) {
		if (shared) {
			struct ext4_xattr_header *old = EXT4_XATTR_BHDR(xattr_ref->block_bh);

			le32_add_cpu(&old->h_refcount, -1);
			/* the block stays, but no longer counts for this inode */
			inode->i_blocks -= min(inode->i_blocks,
					       (__u64)(xattr_ref->fs->BlockSize >> 9));
			ext4_xattr_block_csum_set(inode, xattr_ref->block_bh);
			extents_mark_buffer_dirty(xattr_ref->block_bh);
			extents_brelse(xattr_ref->block_bh);
			xattr_ref->block_bh = NULL;
			xattr_ref->block_loaded = FALSE;
			inode->i_file_acl = 0;
			xattr_ref->IsOnDiskInodeDirty = TRUE;
		} else {
            ret = ext4_xattr_try_free_block(xattr_ref);
            if (ret)
                goto Finish;
		}
	}
	if (new_bh) {
		xattr_ref->block_bh = new_bh;
		xattr_ref->block_loaded = TRUE;
		inode->i_file_acl = new_block;
		xattr_ref->IsOnDiskInodeDirty = TRUE;
	}

	/* the inode body: rewritten whole when it has room for a table */
	ibody_space = ext4_xattr_inode_space(xattr_ref);
	if (ibody_space > (__s32)sizeof(struct ext4_xattr_ibody_header)) {
		struct ext4_xattr_ibody_header *ibody_header =
			EXT4_XATTR_IHDR(xattr_ref->OnDiskInode);
		struct ext4_xattr_entry *entry = EXT4_XATTR_IFIRST(ibody_header);
		char *base = (char *)entry;
		char *data_end = (char *)ibody_header + ibody_space;

		memset(ibody_header, 0, ibody_space);
		ibody_header->h_magic = EXT4_XATTR_MAGIC;
		list_for_each_entry(item, &xattr_ref->ordered_list, struct ext4_xattr_item, list_node) {
			if (!item->in_inode)
				continue;
			ext4_xattr_set_entry(item, entry, &data_end, base, FALSE);
			entry = EXT4_XATTR_NEXT(entry);
		}
		xattr_ref->IsOnDiskInodeDirty = TRUE;
	}

	/* the block: entries sorted as Linux looks them up (it stops at the
	   first entry past the name it wants) - the order of the item tree */
	if (need_block) {
		struct ext4_xattr_header *block_header = EXT4_XATTR_BHDR(xattr_ref->block_bh);
		struct ext4_xattr_entry *entry = EXT4_XATTR_BFIRST(xattr_ref->block_bh);
		char *data_end = (char *)block_header + ext4_xattr_block_space(xattr_ref);

		memset(xattr_ref->block_bh->b_data, 0, xattr_ref->fs->BlockSize);
		ext4_xattr_set_block_header(xattr_ref);
		for (node = rb_first(&xattr_ref->root); node; node = rb_next(node)) {
			item = container_of(node, struct ext4_xattr_item, node);
			if (item->in_inode)
				continue;
			ext4_xattr_set_entry(item, entry, &data_end,
					     (char *)block_header, TRUE);
			entry = EXT4_XATTR_NEXT(entry);
		}
		ext4_xattr_rehash(block_header);
		/* the checksum covers the whole block and its number: last */
		ext4_xattr_block_csum_set(inode, xattr_ref->block_bh);
		extents_mark_buffer_dirty(xattr_ref->block_bh);
	}

	/*
	 * Linux charges a value inode's size, rounded up to whole blocks, to
	 * every inode with an entry that names it (quota and i_blocks alike,
	 * ext4_xattr_inode_alloc_quota), and gives it back when the entry
	 * goes; e2fsck counts i_blocks that way.
	 */
	{
		__u64 add = 0, sub = 0;
		size_t i;

		list_for_each_entry(item, &xattr_ref->ordered_list, struct ext4_xattr_item, list_node) {
			if (item->ea_new)
				add += ext4_xattr_ea_charge(xattr_ref, item->data_size);
		}
		for (i = 0; i < xattr_ref->ea_drop_count; i++)
			sub += ext4_xattr_ea_charge(xattr_ref, xattr_ref->ea_drop[i].size);
		if (add || sub) {
			inode->i_blocks += add;
			inode->i_blocks = inode->i_blocks > sub ? inode->i_blocks - sub : 0;
			xattr_ref->IsOnDiskInodeDirty = TRUE;
		}
	}

	xattr_ref->dirty = FALSE;
	xattr_ref->written = TRUE;

Finish:
	return ret;
}
