/**
 * DirectoryEntries.c - directory entry add, delete, find and emptiness test.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "htree_internal.h"

/*
 * The entry de carries name. Elsewhere names match without regard to ASCII
 * case (Windows semantics, ext3_match); in a casefolded directory they
 * match as Linux matches them there, folded (so "Łódź" is "łódź"). A name
 * that is not valid UTF-8 falls back to ext3_match, as Linux falls back to
 * a plain comparison.
 */
int ext4_match(struct inode *dir, int len, const char *name, struct ext3_dir_entry_2 *de)
{
    if (ext4_casefolded(dir) && de->inode &&
        (unsigned)EXT3_DIR_REC_LEN(de->name_len) <= ext3_rec_len_from_disk(de->rec_len)) {
        int rc;

        if (len == de->name_len && memcmp(name, de->name, len) == 0) {
            return 1;
        }
        rc = Ext4Utf8CasefoldCompare(name, len, de->name, de->name_len);
        if (rc >= 0) {
            return rc == 0;
        }
    }
    return ext3_match(len, name, de);
}
/*
 * Returns 0 if not found, -1 on failure, and 1 on success
 */
static inline int search_dirblock(struct buffer_head * bh,
                                  struct inode *dir,
                                  struct dentry *dentry,
                                  unsigned long offset,
                                  struct ext3_dir_entry_2 ** res_dir)
{
    struct ext3_dir_entry_2 * de;
    char * dlimit;
    int de_len;
    const char *name = dentry->d_name.name;
    int namelen = dentry->d_name.len;

    de = (struct ext3_dir_entry_2 *) bh->b_data;
    dlimit = bh->b_data + dir->i_sb->s_blocksize;
    while ((char *) de < dlimit) {
        /* this code is executed quadratically often */
        /* do minimal checking `by hand' */

        if ((char *) de + EXT3_DIR_REC_LEN(de->name_len) <= dlimit &&
                ext4_match (dir, namelen, name, de)) {
            /* found a match - just to be sure, do a full check */
            if (!ext3_check_dir_entry("ext3_find_entry",
                                      dir, de, bh, offset))
                return -1;
            *res_dir = de;
            return 1;
        }
        /* prevent looping on a bad block */
        de_len = ext3_rec_len_from_disk(de->rec_len);

        if (de_len <= 0)
            return -1;
        offset += de_len;
        de = (struct ext3_dir_entry_2 *) ((char *) de + de_len);
    }
    return 0;
}

/*
 * define how far ahead to read directories while searching them.
 */
#define NAMEI_RA_CHUNKS  2
#define NAMEI_RA_BLOCKS  4
#define NAMEI_RA_SIZE	     (NAMEI_RA_CHUNKS * NAMEI_RA_BLOCKS)

unsigned char ext3_type_by_mode(umode_t mode)
{
    unsigned char type = 0;

    switch (mode & S_IFMT) {
    case S_IFREG:
        type = EXT3_FT_REG_FILE;
        break;
    case S_IFDIR:
        type = EXT3_FT_DIR;
        break;
    case S_IFCHR:
        type =  EXT3_FT_CHRDEV;
        break;
    case S_IFBLK:
        type = EXT3_FT_BLKDEV;
        break;
    case S_IFIFO:
        type = EXT3_FT_FIFO;
        break;
    case S_IFSOCK:
        type = EXT3_FT_SOCK;
        break;
    case S_IFLNK:
        type = EXT3_FT_SYMLINK;
    }

    return type;
};

/*
 *	ext3_add_entry()
 *
 * adds a file entry to the specified directory, using the same
 * semantics as ext3_find_entry(). It returns NULL if it failed.
 *
 * NOTE!! The inode part of 'de' is left at 0 - which means you
 * may not sleep between calling this and putting something into
 * the entry, as someone else might have used it while you slept.
 */
int ext3_add_entry(struct ext2_icb *icb, struct dentry *dentry, struct inode *inode)
{
    struct inode *dir = dentry->d_parent->d_inode;
    struct buffer_head *bh;
    struct ext3_dir_entry_2 *de;
    struct super_block *sb;
    int	retval;
    int	dx_fallback=0;
    unsigned blocksize;
    ext3_lblk_t block, blocks;
    struct ext4_dir_entry_tail *t;
    int	csum_size = 0;

    if (ext4_has_metadata_csum(inode->i_sb))
        csum_size = sizeof(struct ext4_dir_entry_tail);

    sb = dir->i_sb;
    blocksize = sb->s_blocksize;
    if (!dentry->d_name.len)
        return -EINVAL;

    /* a casefolded directory in strict mode takes valid UTF-8 names only,
       as Linux refuses the others there */
    if (ext4_casefolded(dir) &&
        (le16_to_cpu(EXT3_SB(sb)->s_es->s_encoding_flags) & EXT4_ENC_STRICT_MODE_FL) &&
        !Ext4Utf8Valid(dentry->d_name.name, dentry->d_name.len))
        return -EINVAL;

    if (is_dx(dir)) {
        retval = ext3_dx_add_entry(icb, dentry, inode);
        if (!retval || (retval != ERR_BAD_DX_DIR))
            return retval;
        EXT3_I(dir)->i_flags &= ~EXT3_INDEX_FL;
        dx_fallback++;
        retval = ext3_mark_inode_dirty(icb, dir);
        if (retval)
            return retval;
    }

    blocks = (ext3_lblk_t)(dir->i_size >> sb->s_blocksize_bits);
    for (block = 0; block < blocks; block++) {
        bh = ext3_bread(icb, dir, block, &retval);
        if (!bh)
            return retval;
        retval = add_dirent_to_buf(icb, dentry, inode, NULL, bh);
        if (retval != -ENOSPC)
            return retval;

        if (blocks == 1 && !dx_fallback &&
                EXT3_HAS_COMPAT_FEATURE(sb, EXT3_FEATURE_COMPAT_DIR_INDEX))
            return make_indexed_dir(icb, dentry, inode, bh);

        brelse(bh);
    }
    bh = ext3_append(icb, dir, &block, &retval);
    if (!bh)
        return retval;
    de = (struct ext3_dir_entry_2 *) bh->b_data;
    de->inode = 0;
    de->rec_len = ext3_rec_len_to_disk(blocksize - csum_size);

    if (csum_size) {
        t = EXT4_DIRENT_TAIL(bh->b_data, blocksize);
        initialize_dirent_tail(t, blocksize);
    }

    return add_dirent_to_buf(icb, dentry, inode, de, bh);
}

/*
 * ext3_delete_entry deletes a directory entry by merging it with the
 * previous entry
 */
int ext3_delete_entry(struct ext2_icb *icb, struct inode *dir,
                      struct ext3_dir_entry_2 *de_del,
                      struct buffer_head *bh)
{
    UNREFERENCED_PARAMETER(icb);
    struct ext3_dir_entry_2 *de, *pde = NULL;
    size_t i = 0;
    int	csum_size = 0;

    if (ext4_has_metadata_csum(dir->i_sb))
        csum_size = sizeof(struct ext4_dir_entry_tail);

    de = (struct ext3_dir_entry_2 *) bh->b_data;
    while (i < bh->b_size - csum_size) {
        if (!ext3_check_dir_entry("ext3_delete_entry", dir, de, bh, (unsigned long)i))
            return -EIO;
        if (de == de_del)  {
            if (pde)
                pde->rec_len = ext3_rec_len_to_disk(
                                   ext3_rec_len_from_disk(pde->rec_len) +
                                   ext3_rec_len_from_disk(de->rec_len));
            else
                de->inode = 0;
            dir->i_version++;
            /* ext3_journal_dirty_metadata(handle, bh); */
            ext4_dirent_csum_set(dir, (struct ext4_dir_entry *)bh->b_data);
            mark_buffer_dirty(bh);
            return 0;
        }
        i += ext3_rec_len_from_disk(de->rec_len);
        pde = de;
        de = ext3_next_entry(de);
    }
    return -ENOENT;
}

/*
 * routine to check that the specified directory is empty (for rmdir)
 */
int ext3_is_dir_empty(struct ext2_icb *icb, struct inode *inode)
{
    unsigned int offset;
    struct buffer_head *bh;
    struct ext3_dir_entry_2 *de, *de1;
    struct super_block *sb;
    int err = 0;

    sb = inode->i_sb;
    if (Ext4DirSize(inode) < EXT3_DIR_REC_LEN(1) + EXT3_DIR_REC_LEN(2) ||
            !(bh = ext3_bread(icb, inode, 0, &err))) {
        if (err)
            ext3_error(inode->i_sb, __FUNCTION__,
                       "error %d reading directory #%lu offset 0",
                       err, inode->i_ino);
        else
            ext3_warning(inode->i_sb, __FUNCTION__,
                         "bad directory (dir #%lu) - no data block",
                         inode->i_ino);
        return 1;
    }
    de = (struct ext3_dir_entry_2 *) bh->b_data;
    de1 = ext3_next_entry(de);
    if (le32_to_cpu(de->inode) != inode->i_ino ||
            !le32_to_cpu(de1->inode) ||
            strcmp(".", de->name) ||
            strcmp("..", de1->name)) {
        ext3_warning(inode->i_sb, "empty_dir",
                     "bad directory (dir #%lu) - no `.' or `..'",
                     inode->i_ino);
        brelse(bh);
        return 1;
    }
    offset = ext3_rec_len_from_disk(de->rec_len) +
             ext3_rec_len_from_disk(de1->rec_len);
    de = ext3_next_entry(de1);
    while (offset < Ext4DirSize(inode)) {
        if (!bh ||
                (void *) de >= (void *) (bh->b_data+sb->s_blocksize)) {
            err = 0;
            brelse(bh);
            bh = ext3_bread(icb, inode, offset >> EXT3_BLOCK_SIZE_BITS(sb), &err);
            if (!bh) {
                if (err)
                    ext3_error(sb, __FUNCTION__, "error %d reading directory"
                               " #%lu offset %u", err, inode->i_ino, offset);
                offset += sb->s_blocksize;
                continue;
            }
            de = (struct ext3_dir_entry_2 *) bh->b_data;
        }
        if (!ext3_check_dir_entry("empty_dir", inode, de, bh, offset)) {
            de = (struct ext3_dir_entry_2 *)(bh->b_data +
                                             sb->s_blocksize);
            offset = (offset | (sb->s_blocksize - 1)) + 1;
            continue;
        }
        if (le32_to_cpu(de->inode)) {
            brelse(bh);
            return 0;
        }
        offset += ext3_rec_len_from_disk(de->rec_len);
        de = ext3_next_entry(de);
    }
    brelse(bh);
    return 1;
}

/*
 *	ext4_find_entry()
 *
 * finds an entry in the specified directory with the wanted name. It
 * returns the cache buffer in which the entry was found, and the entry
 * itself (as a parameter - res_dir). It does NOT read the inode of the
 * entry - you'll have to do that yourself if you want to.
 *
 * The returned buffer_head has ->b_count elevated.  The caller is expected
 * to brelse() it when appropriate.
 */
struct buffer_head * ext3_find_entry (struct ext2_icb *icb,
                                                  struct dentry *dentry,
                                                  struct ext3_dir_entry_2 ** res_dir)
{
    struct inode *dir = dentry->d_parent->d_inode;
    struct super_block *sb = dir->i_sb;
    struct buffer_head *bh_use[NAMEI_RA_SIZE];
    struct buffer_head *bh, *ret = NULL;
    ext3_lblk_t start, block, b;
    int ra_max = 0;		/* Number of bh's in the readahead
				   buffer, bh_use[] */
    int ra_ptr = 0;		/* Current index into readahead
				   buffer */
    int num = 0;
    ext3_lblk_t  nblocks;
    int i, err = 0;
    int namelen = dentry->d_name.len;

    *res_dir = NULL;
    if (namelen > EXT3_NAME_LEN)
        return NULL;

    if (is_dx(dir)) {
        bh = ext3_dx_find_entry(icb, dentry, res_dir, &err);
        /*
         * A hit is final. A miss is not: the index hashes the exact
         * spelling, while names are matched case-insensitively (Windows
         * semantics), so a differently-cased entry can only be found by
         * the linear scan below. That scan is also the fallback for a
         * damaged index, as in Linux. Trying the index first makes every
         * lookup with the on-disk spelling - which is what applications
         * do nearly all the time - O(log n) instead of O(n), for opens
         * as well as directory queries.
         */
        if (bh || (err != ERR_BAD_DX_DIR && err != -ENOENT))
            return bh;
        dxtrace(printk("ext4_find_entry: dx failed, "
                       "falling back\n"));
    }

    /* A miss that needs no scan. A casefolded directory's index hashes
       the folded name, which every spelling shares: its miss is final
       (for a name that is UTF-8; any other is hashed and compared as it
       is). Elsewhere the directory's set of caseless names says whether
       any spelling exists - before it, every create read the whole
       directory. */
    if (ext4_casefolded(dir)) {
        if (is_dx(dir) && err == -ENOENT &&
            Ext4Utf8Valid(dentry->d_name.name, namelen))
            return NULL;
    } else if (!Ext4DirNameMayExist(icb, dir, dentry->d_name.name, namelen)) {
        return NULL;
    }

    nblocks = (ext3_lblk_t)(Ext4DirSize(dir) >> EXT3_BLOCK_SIZE_BITS(sb));
    start = 0;
    block = start;
restart:
    do {
        /*
         * We deal with the read-ahead logic here.
         */
        if (ra_ptr >= ra_max) {
            /* Refill the readahead buffer */
            ra_ptr = 0;
            b = block;
            for (ra_max = 0; ra_max < NAMEI_RA_SIZE; ra_max++) {
                /*
                 * Terminate if we reach the end of the
                 * directory and must wrap, or if our
                 * search has finished at this block.
                 */
                if (b >= nblocks || (num && block == start)) {
                    bh_use[ra_max] = NULL;
                    break;
                }
                num++;
                bh = ext3_bread(icb, dir, b++, &err);
                bh_use[ra_max] = bh;
            }
        }
        if ((bh = bh_use[ra_ptr++]) == NULL)
            goto next;
        wait_on_buffer(bh);
        if (!buffer_uptodate(bh)) {
            /* read error, skip block & hope for the best */
            ext3_error(sb, __FUNCTION__, "reading directory #%lu "
                       "offset %lu", dir->i_ino,
                       (unsigned long)block);
            brelse(bh);
            goto next;
        }
        i = search_dirblock(bh, dir, dentry,
                            block << EXT3_BLOCK_SIZE_BITS(sb), res_dir);
        if (i == 1) {
            ret = bh;
            goto cleanup_and_exit;
        } else {
            brelse(bh);
            if (i < 0)
                goto cleanup_and_exit;
        }
next:
        if (++block >= nblocks)
            block = 0;
    } while (block != start);

    /*
     * If the directory has grown while we were searching, then
     * search the last part of the directory before giving up.
     */
    block = nblocks;
    nblocks = (ext3_lblk_t)(Ext4DirSize(dir) >> EXT3_BLOCK_SIZE_BITS(sb));
    if (block < nblocks) {
        start = 0;
        goto restart;
    }

cleanup_and_exit:
    /* Clean up the read-ahead blocks */
    for (; ra_ptr < ra_max; ra_ptr++)
        brelse(bh_use[ra_ptr]);
    return ret;
}
