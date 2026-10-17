/**
 * DirectoryEntries.c - the names of a directory: found, added, removed, and
 * whether a directory has any left.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * The blocks are read and changed by DirBlock.c (its rules L1-L4); an
 * indexed directory goes through HtreeDirectory.c first. Names match as
 * Windows matches them - ASCII case ignored - and in a casefolded directory
 * as Linux matches them there, folded ("Łódź" is "łódź"); a name that is
 * not valid UTF-8 matches as it is there, as in Linux.
 *
 *  A lookup in an indexed directory takes the index (the spelling on disk,
 *  O(log n)); a miss there is final in a casefolded directory, and
 *  elsewhere only when the directory's set of caseless names says so - any
 *  other spelling is found by the scan of every block. A damaged index
 *  falls back to that scan too, as in Linux.
 *  An add to a plain directory takes the first block with room; a full
 *  directory of one block becomes indexed (Linux make_indexed_dir), a
 *  longer one gets a block more. A damaged index is dropped for the plain
 *  layout only without metadata checksums - with them the add fails, as in
 *  Linux, rather than lose the index's protection.
 *  A directory is empty when it has '.' and '..' and no other name; a
 *  block or an entry that cannot be read leaves it not empty (Linux
 *  ext4_empty_dir): an rmdir never goes ahead on a guess.
 */

#include "ext4fs.h"
#include "DirectoryInternal.h"

int ext4_match(struct inode *dir, int len, const char *name, struct ext3_dir_entry_2 *de)
{
    if (ext4_casefolded(dir) && de->inode &&
        (unsigned)EXT3_DIR_REC_LEN(de->name_len) <= ext3_rec_len_from_disk(de->rec_len)) {
        int rc;

        if (len == de->name_len && memcmp(name, de->name, len) == 0)
            return 1;
        rc = Ext4Utf8CasefoldCompare(name, len, de->name, de->name_len);
        if (rc >= 0)
            return rc == 0;
    }
    return ext3_match(len, name, de);
}

/* the file type an entry carries, from the inode's mode (S_IFMT >> 12) */
unsigned char ext3_type_by_mode(umode_t mode)
{
    static const unsigned char types[16] = {
        EXT3_FT_UNKNOWN, EXT3_FT_FIFO, EXT3_FT_CHRDEV, EXT3_FT_UNKNOWN,
        EXT3_FT_DIR, EXT3_FT_UNKNOWN, EXT3_FT_BLKDEV, EXT3_FT_UNKNOWN,
        EXT3_FT_REG_FILE, EXT3_FT_UNKNOWN, EXT3_FT_SYMLINK, EXT3_FT_UNKNOWN,
        EXT3_FT_SOCK, EXT3_FT_UNKNOWN, EXT3_FT_UNKNOWN, EXT3_FT_UNKNOWN,
    };
    return types[(mode >> 12) & 15];
}

/* the name into leaf bh if it has room: -ENOSPC keeps bh, anything else
   releases it */
static int AddToLeaf(struct ext2_icb *icb, struct dentry *dentry, struct inode *inode,
                     struct buffer_head *bh)
{
    struct inode   *dir = dentry->d_parent->d_inode;
    DB_GEOMETRY     g;
    EXT4_DIR_NAME   name;
    unsigned int    at;
    int             rc;

    Ext4DirGeometry(dir, &g);
    name.Dir = dir;
    name.Name = dentry->d_name.name;
    name.Length = dentry->d_name.len;
    rc = DbFindSpace(&g, (const unsigned char *)bh->b_data, DbLeafSize(&g), (unsigned int)name.Length,
                     Ext4DirMatch, &name, &at);
    if (rc == DB_ENOSPC)
        return -ENOSPC;
    if (rc) {
        if (rc == DB_ECORRUPT)
            Ext4DirCorrupt(dir, bh);
        brelse(bh);
        return rc == DB_ECORRUPT ? -EIO : rc;
    }
    DbInsert(&g, (unsigned char *)bh->b_data, at, inode ? (unsigned int)inode->i_ino : 0,
             inode ? ext3_type_by_mode(inode->i_mode) : 0,
             (const unsigned char *)name.Name, (unsigned int)name.Length);
    ext4_dirent_csum_set(dir, (struct ext4_dir_entry *)bh->b_data);
    mark_buffer_dirty(bh);
    brelse(bh);
    return Ext4DirTouched(icb, dir);
}

int ext3_add_entry(struct ext2_icb *icb, struct dentry *dentry, struct inode *inode)
{
    struct inode       *dir = dentry->d_parent->d_inode;
    struct super_block *sb = dir->i_sb;
    struct buffer_head *bh;
    DB_GEOMETRY         g;
    ext3_lblk_t         block, blocks;
    BOOLEAN             fallback = FALSE;
    int                 rc;

    /* (the name was refused before, at the open or rename, when its form on
       disk is over 255 bytes: an entry's name length is one byte) */
    if (!dentry->d_name.len || dentry->d_name.len > EXT3_NAME_LEN)
        return -EINVAL;

    /* a casefolded directory in strict mode takes valid UTF-8 names only,
       as Linux refuses the others there */
    if (ext4_casefolded(dir) &&
        (le16_to_cpu(EXT3_SB(sb)->s_es->s_encoding_flags) & EXT4_ENC_STRICT_MODE_FL) &&
        !Ext4Utf8Valid(dentry->d_name.name, dentry->d_name.len))
        return -EINVAL;

    if (is_dx(dir)) {
        rc = ext3_dx_add_entry(icb, dentry, inode);
        if (rc != ERR_BAD_DX_DIR)
            return rc;
        if (ext4_has_metadata_csum(sb)) {
            Ext4DirCorrupt(dir, NULL);
            return -EFSCORRUPTED;
        }
        EXT3_I(dir)->i_flags &= ~EXT3_INDEX_FL;
        fallback = TRUE;
        rc = ext3_mark_inode_dirty(icb, dir);
        if (rc)
            return rc;
    }

    Ext4DirGeometry(dir, &g);
    blocks = (ext3_lblk_t)(dir->i_size >> sb->s_blocksize_bits);
    for (block = 0; block < blocks; block++) {
        bh = ext3_bread(icb, dir, block, &rc);
        if (!bh)
            return rc;
        rc = AddToLeaf(icb, dentry, inode, bh);
        if (rc != -ENOSPC)
            return rc;
        if (blocks == 1 && !fallback &&
            EXT3_HAS_COMPAT_FEATURE(sb, EXT3_FEATURE_COMPAT_DIR_INDEX) &&
            DxCanIndex(&g, (const unsigned char *)bh->b_data) == DB_OK)
            return make_indexed_dir(icb, dentry, inode, bh);
        brelse(bh);
    }

    /* a block more */
    bh = ext3_append(icb, dir, &block, &rc);
    if (!bh)
        return rc;
    DbInitLeaf(&g, (unsigned char *)bh->b_data);
    return AddToLeaf(icb, dentry, inode, bh);
}

/* the entry de_del of block bh goes: its room joins the entry before it */
int ext3_delete_entry(struct ext2_icb *icb, struct inode *dir,
                      struct ext3_dir_entry_2 *de_del, struct buffer_head *bh)
{
    DB_GEOMETRY g;
    int         rc;

    UNREFERENCED_PARAMETER(icb);
    Ext4DirGeometry(dir, &g);
    rc = DbDelete(&g, (unsigned char *)bh->b_data, DbLeafSize(&g),
                  (unsigned int)((char *)de_del - bh->b_data));
    if (rc == DB_ECORRUPT) {
        Ext4DirCorrupt(dir, bh);
        return -EIO;
    }
    if (rc)
        return -ENOENT;
    dir->i_version++;
    ext4_dirent_csum_set(dir, (struct ext4_dir_entry *)bh->b_data);
    mark_buffer_dirty(bh);
    return 0;
}

/* 1: only '.' and '..' are left; 0: a name is, or the directory cannot be read */
int ext3_is_dir_empty(struct ext2_icb *icb, struct inode *inode)
{
    struct super_block *sb = inode->i_sb;
    struct buffer_head *bh;
    DB_GEOMETRY         g;
    unsigned char      *data;
    loff_t              size = Ext4DirSize(inode), offset;
    unsigned int        at;
    int                 err = 0;

    Ext4DirGeometry(inode, &g);
    if (size < (loff_t)(EXT3_DIR_REC_LEN(1) + EXT3_DIR_REC_LEN(2)))
        return 0;
    bh = ext3_bread(icb, inode, 0, &err);
    if (!bh)
        return 0;
    data = (unsigned char *)bh->b_data;

    /* '.' naming the directory itself, '..' naming an inode */
    if (DbCheck(&g, data, g.BlockSize, 0) || DbGet32(data) != inode->i_ino ||
        data[DB_E_NAME_LEN] != 1 || data[DB_E_NAME] != '.') {
        brelse(bh);
        return 0;
    }
    at = DbRecLen(&g, data);
    if (DbCheck(&g, data, g.BlockSize, at) || DbGet32(data + at) == 0 ||
        data[at + DB_E_NAME_LEN] != 2 || data[at + DB_E_NAME] != '.' || data[at + DB_E_NAME + 1] != '.') {
        brelse(bh);
        return 0;
    }
    offset = at + DbRecLen(&g, data + at);

    while (offset < size) {
        at = (unsigned int)(offset & (g.BlockSize - 1));
        if (at == 0) {
            brelse(bh);
            bh = ext3_bread(icb, inode, (unsigned long)(offset >> sb->s_blocksize_bits), &err);
            if (!bh)
                return 0;
            data = (unsigned char *)bh->b_data;
        }
        if (DbCheck(&g, data, g.BlockSize, at) || DbGet32(data + at) != 0) {
            brelse(bh);
            return 0;
        }
        offset += DbRecLen(&g, data + at);
    }
    brelse(bh);
    return 1;
}

/*
 * The entry of a name: its block, held, and *res_dir in it; NULL when there
 * is none, or it cannot be read.
 */
struct buffer_head *ext3_find_entry(struct ext2_icb *icb, struct dentry *dentry,
                                    struct ext3_dir_entry_2 **res_dir)
{
    struct inode       *dir = dentry->d_parent->d_inode;
    struct super_block *sb = dir->i_sb;
    struct buffer_head *bh;
    DB_GEOMETRY         g;
    EXT4_DIR_NAME       name;
    ext3_lblk_t         block, blocks, done = 0;
    unsigned int        at;
    int                 err = 0, rc;
    BOOLEAN             dots;

    *res_dir = NULL;
    if (dentry->d_name.len > EXT3_NAME_LEN)
        return NULL;
    name.Dir = dir;
    name.Name = dentry->d_name.name;
    name.Length = dentry->d_name.len;
    Ext4DirGeometry(dir, &g);
    blocks = (ext3_lblk_t)(Ext4DirSize(dir) >> sb->s_blocksize_bits);

    /* '.' and '..' are in the first block only */
    dots = name.Length <= 2 && name.Name[0] == '.' && (name.Length == 1 || name.Name[1] == '.');
    if (dots) {
        blocks = min(blocks, 1);
    } else {
        if (is_dx(dir)) {
            bh = ext3_dx_find_entry(icb, dentry, res_dir, &err);
            if (bh || (err != ERR_BAD_DX_DIR && err != -ENOENT))
                return bh;
            /* a miss needs no scan: in a casefolded directory the index
               hashes the folded name every spelling shares (a UTF-8 one) */
            if (ext4_casefolded(dir) && err == -ENOENT && Ext4Utf8Valid(name.Name, name.Length))
                return NULL;
        }
        /* elsewhere the set of caseless names says whether any spelling exists */
        if (!ext4_casefolded(dir) && !Ext4DirNameMayExist(icb, dir, name.Name, name.Length))
            return NULL;
    }

    /* every block; again over the ones the directory grew by meanwhile */
    for (;;) {
        for (block = done; block < blocks; block++) {
            bh = ext3_bread(icb, dir, block, &err);
            if (!bh)
                continue;       /* reported by the read: the next one */
            rc = DbSearch(&g, (const unsigned char *)bh->b_data, g.BlockSize, Ext4DirMatch, &name, &at);
            if (rc == DB_OK) {
                *res_dir = (struct ext3_dir_entry_2 *)(bh->b_data + at);
                return bh;
            }
            if (rc == DB_ECORRUPT)
                Ext4DirCorrupt(dir, bh);    /* the search stops there, as in Linux */
            brelse(bh);
            if (rc == DB_ECORRUPT)
                return NULL;
        }
        done = blocks;
        blocks = (ext3_lblk_t)(Ext4DirSize(dir) >> sb->s_blocksize_bits);
        if (dots || blocks <= done)
            return NULL;
    }
}
