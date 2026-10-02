/**
 * dir_block.c - directory block helpers: read/append blocks, link counts, entry insertion.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "htree_internal.h"

__u32 ext3_current_time(struct inode *in)
{
    LARGE_INTEGER SysTime;
    __u32 hi, lo;

    KeQuerySystemTime(&SysTime);
    Ext2SetInodeTime(&SysTime, &lo, &hi);
    in->i_ctime_extra = in->i_mtime_extra = hi;
    in->i_ctime = in->i_mtime = lo;
    return lo;
}

void ext3_warning (struct super_block * sb, const char * function,
                   char * fmt, ...)
{
    UNREFERENCED_PARAMETER(function);
    UNREFERENCED_PARAMETER(sb);
    UNREFERENCED_PARAMETER(fmt);
}

/* ext3_bread is safe for meta-data blocks. it's not safe to read file data,
   since file data is managed by file cache, not volume cache */
static struct buffer_head *ext3_bread_mapped(struct ext2_icb *icb, struct inode *inode,
                                             PEXT2_MCB Mcb, unsigned long block, int *err);

struct buffer_head *ext3_bread(struct ext2_icb *icb, struct inode *inode,
                                           unsigned long block, int *err)
{
    PEXT2_MCB           Mcb = (PEXT2_MCB) inode->i_priv;    /* a name of this inode */
    PEXT2_MCB           Target;
    struct buffer_head *bh;

    /* an inline directory has no block: its view, built in memory */
    if (S_ISDIR(inode->i_mode) && Ext4IsInline(inode))
        return Ext4InlineDirBlock(icb, inode, block, err);

    if (NULL == Mcb) {
        *err = -EINVAL;
        return NULL;
    }

    /* A directory listed through a symlink to it is read through the
       link's entry: the blocks are the target's. The target is held while
       they are mapped (a demotion of the link may drop it meanwhile). */
    if (IsMcbSymLink(Mcb)) {
        Target = Ext2ReferLinkTarget((PEXT2_VCB)inode->i_sb->s_priv, Mcb);
        if (Target == NULL) {
            *err = -ENOENT;
            return NULL;
        }
        bh = ext3_bread_mapped(icb, inode, Target, block, err);
        Ext2DerefMcb(Target);
        return bh;
    }

    return ext3_bread_mapped(icb, inode, Mcb, block, err);
}

/* block of the file Mcb names, read into the volume cache */
static struct buffer_head *ext3_bread_mapped(struct ext2_icb *icb, struct inode *inode,
                                             PEXT2_MCB Mcb, unsigned long block, int *err)
{
    struct buffer_head * bh = NULL;
    NTSTATUS    status = STATUS_SUCCESS;
    ULONG       lbn = 0, num = 0;

    /* mapping file offset to ext2 block */
    if (INODE_HAS_EXTENT(Mcb->Inode)) {
        status = Ext2MapExtent(icb, inode->i_sb->s_priv,
                               Mcb, block, FALSE,
                               &lbn, &num);
    } else {
        status = Ext2MapIndirect(icb, inode->i_sb->s_priv,
                                 Mcb, block, FALSE,
                                 &lbn, &num);
    }

    if (!NT_SUCCESS(status)) {
        *err = Ext2LinuxError(status);
        return bh;
    }

    /* A hole: no block behind this part of the directory. Block 0 is what
       the mapping reports for one, and it is the superblock's - a directory
       block read from it and written back destroyed the volume. Linux
       refuses a hole in a directory the same way (ext4_read_dirblock). */
    if (lbn == 0) {
        *err = -EIO;
        return NULL;
    }

    bh = sb_getblk(inode->i_sb, lbn);
    if (!bh) {
        *err = -ENOMEM;
        return bh;
    }
    if (buffer_uptodate(bh))
        return bh;

    *err = bh_submit_read(bh);
    if (*err) {
	    __brelse(bh);
	    return NULL;
    }
    return bh;
}

struct buffer_head *ext3_append(struct ext2_icb *icb, struct inode *inode,
                                            ext3_lblk_t *block, int *err)
{
    PEXT2_MCB       mcb = (PEXT2_MCB) inode->i_priv;    /* a name of this inode */
    PEXT2_FCB       dcb = mcb->Icb->Fcb;
    LARGE_INTEGER   Size;
    NTSTATUS        status;

    ASSERT(dcb);
    ASSERT(inode == dcb->Inode);

    /* One block after the last one, and nothing changes unless it is
       there. The size used to be raised before the expansion and kept when
       it failed (a full volume): the directory then ended in a hole, its
       index pointed past its blocks, and the block was read anyway - from
       the hole, i.e. block 0 - and written as a directory block over the
       superblock. */
    *block = (ext3_lblk_t)(inode->i_size >> inode->i_sb->s_blocksize_bits);
    Size.QuadPart = ((LONGLONG)*block + 1) << inode->i_sb->s_blocksize_bits;
    status = Ext2ExpandFile(icb, dcb->Vcb, mcb, &Size);
    if (!NT_SUCCESS(status)) {
        *err = Ext2LinuxError(status);
        return NULL;
    }
    if (Size.QuadPart < (((LONGLONG)*block + 1) << inode->i_sb->s_blocksize_bits)) {
        *err = -ENOSPC;
        return NULL;
    }

    dcb->Header.AllocationSize = Size;
    dcb->Header.ValidDataLength = dcb->Header.FileSize = Size;
    mcb->Inode->i_size = Size.QuadPart;
    Ext2SaveInode(icb, dcb->Vcb, inode);

    return ext3_bread(icb, inode, *block, err);
}

/*
 * Set directory link count to 1 if nlinks > EXT4_LINK_MAX, or if nlinks == 2
 * since this indicates that nlinks count was previously 1 to avoid overflowing
 * the 16-bit i_links_count field on disk.  Directories with i_nlink == 1 mean
 * that subdirectory link counts are not being maintained accurately.
 *
 * The caller has already checked for i_nlink overflow in case the DIR_LINK
 * feature is not enabled and returned -EMLINK.  The is_dx() check is a proxy
 * for checking S_ISDIR(inode) (since the INODE_INDEX feature will not be set
 * on regular files) and to avoid creating huge/slow non-HTREE directories.
 */
void ext3_inc_count(struct inode *inode)
{
    inode->i_nlink++;
    if (is_dx(inode) &&
        (inode->i_nlink > EXT4_LINK_MAX || inode->i_nlink == 2))
        inode->i_nlink = 1;
}

/*
 * If a directory had nlink == 1, then we should let it be 1. This indicates
 * directory has >EXT4_LINK_MAX subdirs.
 */
void ext3_dec_count(struct inode *inode)
{
    if (!S_ISDIR(inode->i_mode) || inode->i_nlink > 2)
        inode->i_nlink--;
}

void ext3_set_de_type(struct super_block *sb,
                      struct ext3_dir_entry_2 *de,
                      umode_t mode)
{
    if (EXT3_HAS_INCOMPAT_FEATURE(sb, EXT3_FEATURE_INCOMPAT_FILETYPE))
        de->file_type = ext3_type_by_mode(mode);
}

/*
 * ext3_mark_inode_dirty is somewhat expensive, so unlike ext2 we
 * do not perform it in these functions.  We perform it at the call site,
 * if it is needed.
 */
int ext3_mark_inode_dirty(struct ext2_icb *icb, struct inode *in)
{
    if (Ext2SaveInode(icb, in->i_sb->s_priv, in))
        return 0;

    return -ENOMEM;
}

void ext3_update_dx_flag(struct inode *inode)
{
    if (!EXT3_HAS_COMPAT_FEATURE(inode->i_sb,
                                 EXT3_FEATURE_COMPAT_DIR_INDEX))
        EXT3_I(inode)->i_flags &= ~EXT3_INDEX_FL;
}

/*
 * Add a new entry into a directory (leaf) block.  If de is non-NULL,
 * it points to a directory entry which is guaranteed to be large
 * enough for new directory entry.  If de is NULL, then
 * add_dirent_to_buf will attempt search the directory block for
 * space.  It will return -ENOSPC if no space is available, and -EIO
 * and -EEXIST if directory entry already exists.
 *
 * NOTE!  bh is NOT released in the case where ENOSPC is returned.  In
 * all other cases bh is released.
 */
int add_dirent_to_buf(struct ext2_icb *icb, struct dentry *dentry,
                      struct inode *inode, struct ext3_dir_entry_2 *de,
                      struct buffer_head *bh)
{
    struct inode *dir = dentry->d_parent->d_inode;
    const char	*name = dentry->d_name.name;
    int		namelen = dentry->d_name.len;
    unsigned int	offset = 0;
    unsigned short	reclen;
    int nlen, rlen;
    char		*top;
    int		csum_size = 0;

    if (ext4_has_metadata_csum(inode->i_sb))
        csum_size = sizeof(struct ext4_dir_entry_tail);

    reclen = EXT3_DIR_REC_LEN(namelen);
    if (!de) {
        de = (struct ext3_dir_entry_2 *)bh->b_data;
        top = bh->b_data + dir->i_sb->s_blocksize - reclen - csum_size;
        while ((char *) de <= top) {
            if (!ext3_check_dir_entry("ext3_add_entry", dir, de,
                                      bh, offset)) {
                __brelse(bh);
                return -EIO;
            }
            if (ext4_match(dir, namelen, name, de)) {
                __brelse(bh);
                return -EEXIST;
            }
            nlen = EXT3_DIR_REC_LEN(de->name_len);
            rlen = ext3_rec_len_from_disk(de->rec_len);
            if ((de->inode? rlen - nlen: rlen) >= reclen)
                break;
            de = (struct ext3_dir_entry_2 *)((char *)de + rlen);
            offset += rlen;
        }
        if ((char *) de > top)
            return -ENOSPC;
    }

    /* By now the buffer is marked for journaling */
    nlen = EXT3_DIR_REC_LEN(de->name_len);
    rlen = ext3_rec_len_from_disk(de->rec_len);
    if (de->inode) {
        struct ext3_dir_entry_2 *de1 = (struct ext3_dir_entry_2 *)((char *)de + nlen);
        de1->rec_len = ext3_rec_len_to_disk(rlen - nlen);
        de->rec_len = ext3_rec_len_to_disk(nlen);
        de = de1;
    }
    de->file_type = EXT3_FT_UNKNOWN;
    if (inode) {
        de->inode = cpu_to_le32(inode->i_ino);
        ext3_set_de_type(dir->i_sb, de, inode->i_mode);
    } else
        de->inode = 0;
    de->name_len = (__u8)namelen;
    memcpy(de->name, name, namelen);

    /*
     * XXX shouldn't update any times until successful
     * completion of syscall, but too many callers depend
     * on this.
     *
     * XXX similarly, too many callers depend on
     * ext4_new_inode() setting the times, but error
     * recovery deletes the inode, so the worst that can
     * happen is that the times are slightly out of date
     * and/or different from the directory change time.
     */
    dir->i_mtime = dir->i_ctime = ext3_current_time(dir);
    ext3_update_dx_flag(dir);
    dir->i_version++;
    ext4_dirent_csum_set(dir, (struct ext4_dir_entry *)bh->b_data);
    ext3_mark_inode_dirty(icb, dir);
    mark_buffer_dirty(bh);
    __brelse(bh);
    return 0;
}

/*
 * Debug
 */

int ext3_save_inode ( struct ext2_icb *icb, struct inode *in)
{
    return Ext2SaveInode(icb, in->i_sb->s_priv, in);
}
