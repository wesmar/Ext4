/**
 * htree_internal.h - definitions shared by the files split from htree.c.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _EXT4_EXT4_HTREE_INTERNAL_H_
#define _EXT4_EXT4_HTREE_INTERNAL_H_

#define dxtrace(command)

/*
 * Future: use high four bits of block for coalesce-on-delete flags
 * Mask them off for now.
 */

static inline unsigned dx_get_block (struct dx_entry *entry)
{
    return le32_to_cpu(entry->block) & 0x00ffffff;
}

static inline void dx_set_block (struct dx_entry *entry, unsigned value)
{
    entry->block = cpu_to_le32(value);
}

static inline unsigned dx_get_hash (struct dx_entry *entry)
{
    return le32_to_cpu(entry->hash);
}

static inline unsigned dx_get_count (struct dx_entry *entries)
{
    return le16_to_cpu(((struct dx_countlimit *) entries)->count);
}

static inline unsigned dx_get_limit (struct dx_entry *entries)
{
    return le16_to_cpu(((struct dx_countlimit *) entries)->limit);
}

static inline unsigned dx_root_limit (struct inode *dir, unsigned infosize)
{
    unsigned entry_space = dir->i_sb->s_blocksize - EXT4_DIR_REC_LEN(1) -
        EXT4_DIR_REC_LEN(2) - infosize;

    if (ext4_has_metadata_csum(dir->i_sb))
        entry_space -= sizeof(struct dx_tail);
    return entry_space / sizeof(struct dx_entry);
}

static inline unsigned dx_node_limit (struct inode *dir)
{
    unsigned entry_space = dir->i_sb->s_blocksize - EXT4_DIR_REC_LEN(0);

    if (ext4_has_metadata_csum(dir->i_sb))
        entry_space -= sizeof(struct dx_tail);
    return entry_space / sizeof(struct dx_entry);
}

unsigned char ext3_type_by_mode(umode_t mode);

int ext3_dirhash(const char *name, int len, struct dx_hash_info *hinfo);

/* the hash of a name in dir: folded first in a casefolded directory */
int ext4_dir_hash(struct inode *dir, const char *name, int len, struct dx_hash_info *hinfo);

/* a directory entry has this name: compared folded in a casefolded directory */
int ext4_match(struct inode *dir, int len, const char *name, struct ext3_dir_entry_2 *de);

int ext3_save_inode ( struct ext2_icb *icb, struct inode *in);

struct dx_frame *
            dx_probe(struct ext2_icb *icb, struct dentry *dentry, struct inode *dir,
                     struct dx_hash_info *hinfo, struct dx_frame *frame_in, int *err);

void dx_release (struct dx_frame *frames);

int ext3_dx_add_entry(struct ext2_icb *icb, struct dentry *dentry,
                      struct inode *inode);

int make_indexed_dir(struct ext2_icb *icb, struct dentry *dentry,
                     struct inode *inode, struct buffer_head *bh);

#endif /* _EXT4_EXT4_HTREE_INTERNAL_H_ */
