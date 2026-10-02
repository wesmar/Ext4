/**
 * xattr.c - extended attributes: hashing, in-memory item list, loading.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include <linux/module.h>
#include <linux/ext4_xattr.h>
#include "xattr_internal.h"

#define BLOCK_HASH_SHIFT 16

/*
 * ext4_xattr_rehash()
 *
 * Re-compute the block hash from the hashes of its entries (each entry's
 * own hash is computed as it is written).
 */
void ext4_xattr_rehash(struct ext4_xattr_header *header)
{
	struct ext4_xattr_entry *here;
	__u32 hash = 0;

	here = EXT4_XATTR_ENTRY(header + 1);
	while (!EXT4_XATTR_IS_LAST_ENTRY(here)) {
		if (!here->e_hash) {
			/* Block is not shared if an entry's hash value == 0 */
			hash = 0;
			break;
		}
		hash = (hash << BLOCK_HASH_SHIFT) ^
		       (hash >> (8 * sizeof(hash) - BLOCK_HASH_SHIFT)) ^
		       le32_to_cpu(here->e_hash);
		here = EXT4_XATTR_NEXT(here);
	}
	header->h_hash = cpu_to_le32(hash);
}

static int ext4_xattr_item_cmp(struct rb_node *_a,
			       struct rb_node *_b)
{
	int result;
	struct ext4_xattr_item *a, *b;
	a = container_of(_a, struct ext4_xattr_item, node);
	a = container_of(_a, struct ext4_xattr_item, node);
	b = container_of(_b, struct ext4_xattr_item, node);

	if (a->is_data && !b->is_data)
		return -1;
	
	if (!a->is_data && b->is_data)
		return 1;

	result = a->name_index - b->name_index;
	if (result)
		return result;

	if (a->name_len < b->name_len)
		return -1;

	if (a->name_len > b->name_len)
		return 1;

	return memcmp(a->name, b->name, a->name_len);
}

/* Red-black tree insert routine. */

static struct ext4_xattr_item *
ext4_xattr_item_search(struct ext4_xattr_ref *xattr_ref,
		       struct ext4_xattr_item *name)
{
	struct rb_node *new = xattr_ref->root.rb_node;

	while (new) {
		struct ext4_xattr_item *node =
			container_of(new, struct ext4_xattr_item, node);
		int result = ext4_xattr_item_cmp(&name->node, new);

		if (result < 0)
			new = new->rb_left;
		else if (result > 0)
			new = new->rb_right;
		else
			return node;

	}

	return NULL;
}

void ext4_xattr_item_insert(struct ext4_xattr_ref *xattr_ref,
				   struct ext4_xattr_item *item)
{
	rb_insert(&xattr_ref->root, &item->node,
	      ext4_xattr_item_cmp);
	list_add_tail(&item->list_node, &xattr_ref->ordered_list);
}

void ext4_xattr_item_remove(struct ext4_xattr_ref *xattr_ref,
				   struct ext4_xattr_item *item)
{
	rb_erase(&item->node, &xattr_ref->root);
	list_del_init(&item->list_node);
}

struct ext4_xattr_item *
ext4_xattr_item_alloc(__u8 name_index, const char *name, size_t name_len)
{
	struct ext4_xattr_item *item;
	item = kzalloc(sizeof(struct ext4_xattr_item) + name_len, GFP_NOFS);
	if (!item)
		return NULL;

	item->name_index = name_index;
	item->name = (char *)(item + 1);
	item->name_len = name_len;
	item->data = NULL;
	item->data_size = 0;
	INIT_LIST_HEAD(&item->list_node);

	memcpy(item->name, name, name_len);

	if (name_index == EXT4_XATTR_INDEX_SYSTEM &&
	    name_len == 4 &&
	    !memcmp(name, "data", 4))
		item->is_data = TRUE;
	else
		item->is_data = FALSE;

	return item;
}

int ext4_xattr_item_alloc_data(struct ext4_xattr_item *item,
				      const void *orig_data, size_t data_size)
{
	void *data = NULL;
	ASSERT(!item->data);
	/* an empty value is legal (system.data of a small inline file is
	   one); a zero-byte pool request is not */
	data = kmalloc(data_size ? data_size : 1, GFP_NOFS);
	if (!data)
		return -ENOMEM;

	if (orig_data)
		memcpy(data, orig_data, data_size);

	item->data = data;
	item->data_size = data_size;
	return 0;
}

static void ext4_xattr_item_free_data(struct ext4_xattr_item *item)
{
	ASSERT(item->data);
	kfree(item->data);
	item->data = NULL;
	item->data_size = 0;
}

int ext4_xattr_item_resize_data(struct ext4_xattr_item *item,
				       size_t new_data_size)
{
	if (new_data_size != item->data_size) {
		void *new_data;
		new_data = kmalloc(new_data_size ? new_data_size : 1, GFP_NOFS);
		if (!new_data)
			return -ENOMEM;

		/* what still fits: a smaller value must not be overrun by the
		   old one (the caller copies the new bytes in afterwards) */
		memcpy(new_data, item->data, min(item->data_size, new_data_size));
		kfree(item->data);

		item->data = new_data;
		item->data_size = new_data_size;
	}
	return 0;
}

void ext4_xattr_item_free(struct ext4_xattr_item *item)
{
	if (item->data)
		ext4_xattr_item_free_data(item);

	kfree(item);
}

static void *ext4_xattr_entry_data(struct ext4_xattr_ref *xattr_ref,
				   struct ext4_xattr_entry *entry,
				   BOOL in_inode)
{
	char *ret;
	int block_size;
	if (in_inode) {
		struct ext4_xattr_ibody_header *header;
		struct ext4_xattr_entry *first_entry;
		int inode_size = xattr_ref->fs->InodeSize;
		header = EXT4_XATTR_IHDR(xattr_ref->OnDiskInode);
		first_entry = EXT4_XATTR_IFIRST(header);

		ret = ((char *)first_entry + le16_to_cpu(entry->e_value_offs));
		if (ret + EXT4_XATTR_SIZE(le32_to_cpu(entry->e_value_size)) -
			(char *)xattr_ref->OnDiskInode > inode_size)
			ret = NULL;

		return ret;

	}
	block_size = xattr_ref->fs->BlockSize;
	ret = ((char *)xattr_ref->block_bh->b_data + le16_to_cpu(entry->e_value_offs));
	if (ret + EXT4_XATTR_SIZE(le32_to_cpu(entry->e_value_size)) -
			(char *)xattr_ref->block_bh->b_data > block_size)
		ret = NULL;
	return ret;
}

/*
 * One entry into an item: the value from where the entry says it is - the
 * inode body or the block (data), or a value inode (e_value_inum).
 */
static int ext4_xattr_entry_item(struct ext4_xattr_ref *xattr_ref,
				 struct ext4_xattr_entry *entry, BOOL in_inode)
{
	struct ext4_xattr_item *item;
	__u32 inum = le32_to_cpu(entry->e_value_block);
	size_t size = le32_to_cpu(entry->e_value_size);
	void *data = NULL;
	int ret;

	if (inum) {
		/* only with the feature, and no larger than Linux writes them */
		if (!ext4_has_feature_ea_inode(&xattr_ref->fs->sb) ||
		    size > EXT4_XATTR_SIZE_MAX)
			return -EFSCORRUPTED;
	} else {
		data = ext4_xattr_entry_data(xattr_ref, entry, in_inode);
		if (!data)
			return -EIO;
	}

	item = ext4_xattr_item_alloc(entry->e_name_index, EXT4_XATTR_NAME(entry),
				     (size_t)entry->e_name_len);
	if (!item)
		return -ENOMEM;
	if (ext4_xattr_item_alloc_data(item, data, size) != 0) {
		ext4_xattr_item_free(item);
		return -ENOMEM;
	}
	if (inum) {
		ret = ext4_xattr_inode_read(xattr_ref, inum, item->data, size,
					    &item->ea_hash);
		if (ret) {
			ext4_xattr_item_free(item);
			return ret;
		}
		item->ea = TRUE;
		item->ea_ino = inum;
	}
	item->in_inode = in_inode;
	ext4_xattr_item_insert(xattr_ref, item);
	if (in_inode)
		xattr_ref->inode_size_rem -= ext4_xattr_item_space(item);
	else
		xattr_ref->block_size_rem -= ext4_xattr_item_space(item);
	xattr_ref->ea_size += ext4_xattr_item_space(item);
	return 0;
}

/*
 * The entries of a table that ends at end: each one whole inside it, the
 * list closed by four zero bytes inside it too (Linux's check_xattrs). A
 * table that runs past its end is corrupt, not read on into memory that
 * is not the table.
 */
static int ext4_xattr_fetch_entries(struct ext4_xattr_ref *xattr_ref,
				    struct ext4_xattr_entry *entry, char *end,
				    BOOL in_inode)
{
	int ret;

	for (;;) {
		if ((char *)entry + sizeof(__u32) > end)
			return -EFSCORRUPTED;
		if (EXT4_XATTR_IS_LAST_ENTRY(entry))
			return 0;
		if ((char *)entry + sizeof(struct ext4_xattr_entry) > end ||
		    (char *)EXT4_XATTR_NEXT(entry) > end)
			return -EFSCORRUPTED;
		ret = ext4_xattr_entry_item(xattr_ref, entry, in_inode);
		if (ret)
			return ret;
		entry = EXT4_XATTR_NEXT(entry);
	}
}

static int ext4_xattr_block_fetch(struct ext4_xattr_ref *xattr_ref)
{
	ASSERT(xattr_ref->block_bh->b_data);
	/* not an xattr block (Linux refuses it the same way) */
	if (EXT4_XATTR_BHDR(xattr_ref->block_bh)->h_magic != cpu_to_le32(EXT4_XATTR_MAGIC) ||
	    EXT4_XATTR_BHDR(xattr_ref->block_bh)->h_blocks != cpu_to_le32(1))
		return -EFSCORRUPTED;
	return ext4_xattr_fetch_entries(xattr_ref, EXT4_XATTR_BFIRST(xattr_ref->block_bh),
					xattr_ref->block_bh->b_data + xattr_ref->fs->BlockSize,
					FALSE);
}

static int ext4_xattr_inode_fetch(struct ext4_xattr_ref *xattr_ref)
{
	struct ext4_xattr_ibody_header *header = NULL;

	/* no room for the table, or no table: nothing in the body */
	if (ext4_xattr_inode_space(xattr_ref) <
	    (__s32)(sizeof(struct ext4_xattr_ibody_header) + sizeof(__u32)))
		return 0;
	header = EXT4_XATTR_IHDR(xattr_ref->OnDiskInode);
	if (header->h_magic != cpu_to_le32(EXT4_XATTR_MAGIC))
		return 0;
	return ext4_xattr_fetch_entries(xattr_ref, EXT4_XATTR_IFIRST(header),
					(char *)xattr_ref->OnDiskInode + xattr_ref->fs->InodeSize,
					TRUE);
}

__s32 ext4_xattr_inode_space(struct ext4_xattr_ref *xattr_ref)
{
	int inode_size = xattr_ref->fs->InodeSize;
	int size_rem;

	/* a 128-byte inode has no body for attributes - and ends where
	   i_extra_isize would be */
	if (inode_size <= EXT4_GOOD_OLD_INODE_SIZE)
		return 0;
	size_rem = inode_size - EXT4_GOOD_OLD_INODE_SIZE -
		   xattr_ref->OnDiskInode->i_extra_isize;
	return size_rem > 0 ? size_rem : 0;
}

/* what the entries and values may take in the inode body: the table less
   its header and the four zero bytes that end the entry list */
size_t ext4_xattr_inode_room(struct ext4_xattr_ref *xattr_ref)
{
	__s32 space = ext4_xattr_inode_space(xattr_ref);

	if (space < (__s32)(sizeof(struct ext4_xattr_ibody_header) + sizeof(__u32)))
		return 0;
	return space - sizeof(struct ext4_xattr_ibody_header) - sizeof(__u32);
}

__s32 ext4_xattr_block_space(struct ext4_xattr_ref *xattr_ref)
{
	return xattr_ref->fs->BlockSize;
}

int ext4_xattr_fetch(struct ext4_xattr_ref *xattr_ref)
{
	int ret = 0;
	int inode_size = xattr_ref->fs->InodeSize;
	if (inode_size > EXT4_GOOD_OLD_INODE_SIZE) {
		ret = ext4_xattr_inode_fetch(xattr_ref);
		if (ret != 0)
			return ret;
	}

	if (xattr_ref->block_loaded)
		ret = ext4_xattr_block_fetch(xattr_ref);

	xattr_ref->dirty = FALSE;
	return ret;
}

struct ext4_xattr_item *
ext4_xattr_lookup_item(struct ext4_xattr_ref *xattr_ref, __u8 name_index,
		       const char *name, size_t name_len)
{
	struct ext4_xattr_item tmp = {
		FALSE,
		FALSE,
		name_index,
		(char *)name, /*won't touch this string*/
		name_len,
	};
	if (name_index == EXT4_XATTR_INDEX_SYSTEM &&
	    name_len == 4 &&
	    !memcmp(name, "data", 4))
		tmp.is_data = TRUE;

	return ext4_xattr_item_search(xattr_ref, &tmp);
}
