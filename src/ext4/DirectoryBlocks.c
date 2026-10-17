/**
 * DirectoryBlocks.c - directory block helpers: read/append blocks, link counts, entry types.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "DirectoryInternal.h"

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

/*
 * The checks a directory block passes once, as Linux's ext4_read_dirblock
 * makes them. A block asked for as an index node must look like one; with
 * metadata_csum its dx tail checksum, or a leaf's tail checksum, must match.
 * A block that may be either - an index root, a node whose one entry spans
 * it - is checked only when the caller knows it reads an index node: an
 * empty leaf looks the same. A view of an inline directory has no tail.
 */
static int ext3_check_dirblock(struct inode *inode, struct buffer_head *bh,
                               unsigned long block, BOOLEAN index)
{
    struct ext4_dir_entry *de = (struct ext4_dir_entry *)bh->b_data;
    BOOLEAN dx_block = FALSE;
    int err;

    if (buffer_virtual(bh))
        return 0;
    if (is_dx(inode))
        dx_block = block == 0 ||
                   ext3_rec_len_from_disk(de->rec_len) == inode->i_sb->s_blocksize;
    if (index && !dx_block) {
        err = -EFSCORRUPTED;        /* a leaf where an index node should be */
    } else if (buffer_verified(bh) || !ext4_has_metadata_csum(inode->i_sb)) {
        return 0;
    } else if (index) {
        err = ext4_dx_csum_verify(inode, de) ? 0 : -EFSBADCRC;
    } else if (!dx_block) {
        err = ext4_dirent_csum_verify(inode, de) ? 0 : -EFSBADCRC;
    } else {
        return 0;                   /* either kind: nothing known to check */
    }
    if (err) {
        Ext2ReportCorruption((PEXT2_VCB)inode->i_sb->s_priv, (ULONG)inode->i_ino,
                             (ULONGLONG)bh->b_blocknr, err);
        return err;
    }
    set_buffer_verified(bh);
    return 0;
}

static struct buffer_head *ext3_bread_block(struct ext2_icb *icb, struct inode *inode,
                                            unsigned long block, int *err);

/* a block of directory inode, checked as a leaf when it can only be one */
struct buffer_head *ext3_bread(struct ext2_icb *icb, struct inode *inode,
                                           unsigned long block, int *err)
{
    struct buffer_head *bh = ext3_bread_block(icb, inode, block, err);

    if (bh && (*err = ext3_check_dirblock(inode, bh, block, FALSE)) != 0) {
        __brelse(bh);
        return NULL;
    }
    return bh;
}

/* a block of an htree index: the root, or a node below it */
struct buffer_head *ext3_bread_index(struct ext2_icb *icb, struct inode *inode,
                                     unsigned long block, int *err)
{
    struct buffer_head *bh = ext3_bread_block(icb, inode, block, err);

    if (bh && (*err = ext3_check_dirblock(inode, bh, block, TRUE)) != 0) {
        __brelse(bh);
        return NULL;
    }
    return bh;
}

/* a block of directory inode as it is: a new one is filled by its caller */
static struct buffer_head *ext3_bread_block(struct ext2_icb *icb, struct inode *inode,
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
    ULONGLONG   lbn = 0;
    ULONG       num = 0;

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
       refuses a hole in a directory the same way, as corruption
       (ext4_read_dirblock: "Directory hole found"); readdir passes it by. */
    if (lbn == 0) {
        *err = -EFSCORRUPTED;
        Ext2ReportCorruption((PEXT2_VCB)inode->i_sb->s_priv, (ULONG)inode->i_ino, 0, *err);
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
    if (!Ext2SaveInode(icb, dcb->Vcb, inode)) {
        *err = -EIO;
        return NULL;
    }

    /* not checked: what is there now is the block's previous life */
    return ext3_bread_block(icb, inode, *block, err);
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
/* the inode written: 0, or -EIO when it could not be (the volume stopped writing) */
int ext3_mark_inode_dirty(struct ext2_icb *icb, struct inode *in)
{
    return Ext2SaveInode(icb, in->i_sb->s_priv, in) ? 0 : -EIO;
}

void ext3_update_dx_flag(struct inode *inode)
{
    if (!EXT3_HAS_COMPAT_FEATURE(inode->i_sb,
                                 EXT3_FEATURE_COMPAT_DIR_INDEX))
        EXT3_I(inode)->i_flags &= ~EXT3_INDEX_FL;
}
