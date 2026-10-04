/**
 * AttributeApi.c - extended attributes: public get/set/remove/iterate and name prefixes.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include <linux/module.h>
#include <linux/ext4_xattr.h>
#include "xattr_internal.h"

struct xattr_prefix {
	const char *prefix;
	__u8 name_index;
};

static const struct xattr_prefix prefix_tbl[] = {
    {"user.", EXT4_XATTR_INDEX_USER},
    {"system.posix_acl_access", EXT4_XATTR_INDEX_POSIX_ACL_ACCESS},
    {"system.posix_acl_default", EXT4_XATTR_INDEX_POSIX_ACL_DEFAULT},
    {"trusted.", EXT4_XATTR_INDEX_TRUSTED},
    {"security.", EXT4_XATTR_INDEX_SECURITY},
    {"system.", EXT4_XATTR_INDEX_SYSTEM},
    {"system.richacl", EXT4_XATTR_INDEX_RICHACL},
    {NULL, 0},
};

void ext4_fs_xattr_iterate(struct ext4_xattr_ref *ref,
			   int (*iter)(struct ext4_xattr_ref *ref,
				     struct ext4_xattr_item *item,
					 BOOL is_last))
{
	struct ext4_xattr_item *item;
	if (!ref->iter_from) {
		struct list_head *first_node;
		first_node = ref->ordered_list.next;
		if (first_node && first_node != &ref->ordered_list) {
			ref->iter_from =
				list_entry(first_node,
					     struct ext4_xattr_item,
					     list_node);
		}
	}

	item = ref->iter_from;
	while (item) {
		struct list_head *next_node;
		struct ext4_xattr_item *next_item;
		int ret = EXT4_XATTR_ITERATE_CONT;
		next_node = item->list_node.next;
		if (next_node && next_node != &ref->ordered_list)
			next_item = list_entry(next_node, struct ext4_xattr_item,
						 list_node);
		else
			next_item = NULL;
		if (iter)
			ret = iter(ref, item, !next_item);

		if (ret != EXT4_XATTR_ITERATE_CONT) {
			if (ret == EXT4_XATTR_ITERATE_STOP)
				ref->iter_from = NULL;

			break;
		}
		item = next_item;
	}
}

void ext4_fs_xattr_iterate_reset(struct ext4_xattr_ref *ref)
{
	ref->iter_from = NULL;
}

int ext4_fs_set_xattr(struct ext4_xattr_ref *ref, __u8 name_index,
		      const char *name, size_t name_len, const void *data,
		      size_t data_size, BOOL replace)
{
	int ret = 0;
	struct ext4_xattr_item *item =
	    ext4_xattr_lookup_item(ref, name_index, name, name_len);
	if (replace) {
		if (!item) {
			ret = -ENODATA;
			goto Finish;
		}
		/* in place when it stays a value in the body or the block */
		if (!item->ea &&
		    !(ext4_has_feature_ea_inode(&ref->fs->sb) &&
		      EXT4_XATTR_SIZE(data_size) > EXT4_XATTR_MIN_LARGE_EA_SIZE(ref->fs->BlockSize))) {
			if (item->data_size != data_size)
				ret = ext4_xattr_resize_item(ref, item, data_size);
			if (ret == 0) {
				memcpy(item->data, data, data_size);
				ref->dirty = TRUE;
				goto Finish;
			}
			if (ret != -ENOSPC || !ext4_has_feature_ea_inode(&ref->fs->sb))
				goto Finish;
		}
		/* else afresh: into or out of a value inode */
		ret = ext4_xattr_replace_item(ref, item, data, data_size);
	} else {
		if (item) {
			ret = -EEXIST;
			goto Finish;
		}
		item = ext4_xattr_insert_item(ref, name_index, name, name_len,
					      data, data_size, &ret);
	}
Finish:
	return ret;
}

int ext4_fs_set_xattr_ordered(struct ext4_xattr_ref *ref, __u8 name_index,
	const char *name, size_t name_len, const void *data,
	size_t data_size)
{
	int ret = 0;
	struct ext4_xattr_item *item =
		ext4_xattr_lookup_item(ref, name_index, name, name_len);
	if (item) {
		ret = -EEXIST;
		goto Finish;
	}
	item = ext4_xattr_insert_item_ordered(ref, name_index, name, name_len,
		data, data_size, &ret);
Finish:
	return ret;
}

int ext4_fs_remove_xattr(struct ext4_xattr_ref *ref, __u8 name_index,
			 const char *name, size_t name_len)
{
	return ext4_xattr_remove_item(ref, name_index, name, name_len);
}

int ext4_fs_get_xattr(struct ext4_xattr_ref *ref, __u8 name_index,
		      const char *name, size_t name_len, void *buf,
		      size_t buf_size, size_t *data_size)
{
	int ret = 0;
	size_t item_size = 0;
	struct ext4_xattr_item *item =
	    ext4_xattr_lookup_item(ref, name_index, name, name_len);

	if (!item) {
		ret = -ENODATA;
		goto Finish;
	}
	item_size = item->data_size;
	if (buf_size > item_size)
		buf_size = item_size;

	if (buf)
		memcpy(buf, item->data, buf_size);

Finish:
	if (data_size)
		*data_size = item_size;

	return ret;
}

int ext4_fs_get_xattr_ref(PEXT2_IRP_CONTEXT IrpContext, PEXT2_VCB fs, PEXT2_MCB inode_ref,
			  struct ext4_xattr_ref *ref)
{
	int rc;
	ext4_fsblk_t xattr_block;
	xattr_block = inode_ref->Inode->i_file_acl;
	memset(&ref->root, 0, sizeof(struct rb_root));
	ref->ea_size = 0;
	ref->iter_from = NULL;
	ref->ea_drop = NULL;
	ref->ea_drop_count = ref->ea_drop_max = 0;
	ref->written = FALSE;
	/* value inodes are read through it while the set is loaded */
	ref->IrpContext = IrpContext;
	if (xattr_block) {
		ref->block_bh = extents_bread(&fs->sb, xattr_block);
		if (!ref->block_bh)
			return -EIO;

		ref->block_loaded = TRUE;
	} else
		ref->block_loaded = FALSE;

	ref->inode_ref = inode_ref;
	ref->fs = fs;
	INIT_LIST_HEAD(&ref->ordered_list);

	ref->OnDiskInode = Ext2AllocateInode(fs);
	if (!ref->OnDiskInode) {
		if (xattr_block) {
			extents_brelse(ref->block_bh);
			ref->block_bh = NULL;
		}
		return -ENOMEM;
	}
	if (!Ext2LoadInodeXattr(fs, inode_ref->Inode, ref->OnDiskInode)) {
		if (xattr_block) {
			extents_brelse(ref->block_bh);
			ref->block_bh = NULL;
		}

		Ext2DestroyInode(fs, ref->OnDiskInode);
		return -EIO;
	}
	ref->IsOnDiskInodeDirty = FALSE;

	ref->inode_size_rem = ext4_xattr_inode_room(ref);

	ref->block_size_rem =
		ext4_xattr_block_space(ref) -
		sizeof(struct ext4_xattr_header) -
		sizeof(__u32);

	rc = ext4_xattr_fetch(ref);
	if (rc != 0) {
		ext4_xattr_purge_items(ref);
		if (xattr_block) {
			extents_brelse(ref->block_bh);
			ref->block_bh = NULL;
		}

		Ext2DestroyInode(fs, ref->OnDiskInode);
		return rc;
	}
	ref->IrpContext = IrpContext;
	return 0;
}

int ext4_fs_put_xattr_ref(struct ext4_xattr_ref *ref)
{
	int ret;
	sector_t orig_file_acl = ref->inode_ref->Inode->i_file_acl;
	ret = ext4_xattr_write_to_disk(ref);
	if (ref->IsOnDiskInodeDirty) {
		/* As we may do block allocation in ext4_xattr_write_to_disk */
		if (ret)
			ref->inode_ref->Inode->i_file_acl = orig_file_acl;

		/* The in-inode xattr area first, the inode itself last: the
		 * inode checksum covers the whole on-disk inode, xattr area
		 * included, and Ext2SaveInode computes it over what is in the
		 * buffer at that moment. The other order left every inode with
		 * in-inode xattrs changed from Windows with a stale checksum -
		 * Linux refused its xattrs (EBADMSG) and e2fsck the inode.
		 * An inode of 128 bytes has no such area: only the inode
		 * changed (i_file_acl). Its save used to be asked for anyway
		 * and to fail, so every removal of an xattr block there
		 * reported EIO, and a delete kept the inode. */
		if (!ret && ref->fs->InodeSize > EXT4_GOOD_OLD_INODE_SIZE) {
			ret = Ext2SaveInodeXattr(ref->IrpContext,
					ref->fs,
					ref->inode_ref->Inode,
					ref->OnDiskInode)
				? 0 : -EIO;
		}
		if (!ret) {
			ret = Ext2SaveInode(ref->IrpContext, ref->fs, ref->inode_ref->Inode)
				? 0 : -EIO;
		}
		ref->IsOnDiskInodeDirty = FALSE;
	}
	if (ref->block_loaded) {
		if (!ret)
			extents_brelse(ref->block_bh);
		else
			extents_bforget(ref->block_bh);

		ref->block_bh = NULL;
		ref->block_loaded = FALSE;
	}

	/* value inodes: the replaced and removed ones lose this reference
	   once the new set is on disk; the ones made for a set that did not
	   get there go again */
	if (!ret && ref->written) {
		size_t i;

		for (i = 0; i < ref->ea_drop_count; i++) {
			int rc = ext4_xattr_inode_dec_ref(ref, ref->ea_drop[i].ino);

			if (rc)
				DbgPrint("ext4: inode %u: xattr value inode %u not released (%d)\n",
					 ref->inode_ref->Inode->i_ino, ref->ea_drop[i].ino, rc);
		}
	} else {
		struct ext4_xattr_item *item;

		list_for_each_entry(item, &ref->ordered_list, struct ext4_xattr_item, list_node) {
			/* one that stays is only unattached, which e2fsck
			   collects: nothing points at it */
			if (item->ea_new && item->ea_ino)
				(void)ext4_xattr_inode_dec_ref(ref, item->ea_ino);
		}
	}
	if (ref->ea_drop) {
		kfree(ref->ea_drop);
		ref->ea_drop = NULL;
	}
	ref->ea_drop_count = ref->ea_drop_max = 0;
	ref->written = FALSE;

	ext4_xattr_purge_items(ref);
	Ext2DestroyInode(ref->fs, ref->OnDiskInode);
	ref->OnDiskInode = NULL;
	ref->inode_ref = NULL;
	ref->fs = NULL;
	return ret;
}

const char *ext4_extract_xattr_name(const char *full_name, size_t full_name_len,
			      __u8 *name_index, size_t *name_len,
			      BOOL *found)
{
	int i;
	ASSERT(name_index);
	ASSERT(found);

	*found = FALSE;

	if (!full_name_len) {
		if (name_len)
			*name_len = 0;

		return NULL;
	}

	for (i = 0; prefix_tbl[i].prefix; i++) {
		size_t prefix_len = strlen(prefix_tbl[i].prefix);
		if (full_name_len >= prefix_len &&
		    !memcmp(full_name, prefix_tbl[i].prefix, prefix_len)) {
			BOOL require_name =
				prefix_tbl[i].prefix[prefix_len - 1] == '.';
			*name_index = prefix_tbl[i].name_index;
			if (name_len)
				*name_len = full_name_len - prefix_len;

			if (!(full_name_len - prefix_len) && require_name)
				return NULL;

			*found = TRUE;
			if (require_name)
				return full_name + prefix_len;

			return NULL;
		}
	}
	if (name_len)
		*name_len = 0;

	return NULL;
}

const char *ext4_get_xattr_name_prefix(__u8 name_index,
				       size_t *ret_prefix_len)
{
	int i;

	for (i = 0; prefix_tbl[i].prefix; i++) {
		size_t prefix_len = strlen(prefix_tbl[i].prefix);
		if (prefix_tbl[i].name_index == name_index) {
			if (ret_prefix_len)
				*ret_prefix_len = prefix_len;

			return prefix_tbl[i].prefix;
		}
	}
	if (ret_prefix_len)
		*ret_prefix_len = 0;

	return NULL;
}
