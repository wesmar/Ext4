/**
 * inode.c - on-disk inode load/store, size limits and block counts.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "linux\ext4.h"

#define MAX_LFS_FILESIZE 	0x7fffffffffffffff

BOOLEAN
Ext2GetInodeLba (
    IN PEXT2_VCB    Vcb,
    IN  ULONG       inode,
    OUT PLONGLONG   offset
)
{
    PEXT2_GROUP_DESC gd;
    struct buffer_head *bh = NULL;
    ext4_fsblk_t loc;
    int group;

    if (inode < 1 || inode > INODES_COUNT)  {
        DEBUG(DL_ERR, ( "Ext2GetInodeLba: Inode value %xh is invalid.\n",inode));
        *offset = 0;
        return FALSE;
    }

    group = (inode - 1) / INODES_PER_GROUP ;
    gd = ext4_get_group_desc(&Vcb->sb, group, &bh);
    if (!bh) {
        *offset = 0;
        return FALSE;
    }
    loc = (LONGLONG)ext4_inode_table(&Vcb->sb, gd);
    loc = loc << BLOCK_BITS;
    loc = loc + ((inode - 1) % INODES_PER_GROUP) * Vcb->InodeSize;

    *offset = loc;
    __brelse(bh);

    return TRUE;
}

/*
 * The i_extra_isize of an on-disk inode, if it is one Linux accepts (it
 * fits the inode, is a multiple of four); 0 without the feature or room.
 * Linux refuses an inode with a bad one (EFSCORRUPTED): its extended time
 * fields would lie past the end of the inode.
 */
static BOOLEAN Ext2ExtraIsize(struct super_block *sb, struct ext4_inode *raw, ULONG *extra)
{
    ULONG e = le16_to_cpu(raw->i_extra_isize);

    *extra = 0;
    if (EXT4_INODE_SIZE(sb) <= EXT4_GOOD_OLD_INODE_SIZE ||
        !EXT4_HAS_RO_COMPAT_FEATURE(sb, EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE)) {
        return TRUE;
    }
    if ((ULONG)EXT4_GOOD_OLD_INODE_SIZE + e > (ULONG)EXT4_INODE_SIZE(sb) || (e & 3)) {
        return FALSE;
    }
    *extra = e;
    return TRUE;
}

void Ext2DecodeInode(struct inode *dst, struct ext4_inode *src)
{
    ULONG extra;

    if (!Ext2ExtraIsize(dst->i_sb, src, &extra)) {
        extra = 0;
    }
    dst->i_mode = src->i_mode;
    dst->i_flags = src->i_flags;
    dst->i_uid = src->i_uid;
    dst->i_gid = src->i_gid;
    dst->i_nlink = src->i_links_count;
    dst->i_generation = src->i_generation;
    dst->i_size = src->i_size_lo;
    /* the high half: of files, and of directories with large_dir (which
       may pass 4 GiB); elsewhere it was the obsolete i_dir_acl */
    if (S_ISREG(src->i_mode) ||
        (S_ISDIR(src->i_mode) && EXT3_HAS_INCOMPAT_FEATURE(dst->i_sb, EXT4_FEATURE_INCOMPAT_LARGEDIR))) {
        dst->i_size |= (loff_t)src->i_size_high << 32;
    }
    dst->i_file_acl = src->i_file_acl_lo;
    dst->i_file_acl |= (ext4_fsblk_t)src->osd2.linux2.l_i_file_acl_high << 32;
    dst->i_atime = src->i_atime;
    dst->i_ctime = src->i_ctime;
    dst->i_mtime = src->i_mtime;
    dst->i_dtime = src->i_dtime;
    dst->i_blocks = ext3_inode_blocks(src, dst);
    memcpy(&dst->i_block[0], &src->i_block[0], sizeof(__u32) * 15);
    dst->i_extra_isize = (__u16)extra;
    if (extra &&
        offsetof(struct ext4_inode, i_atime_extra) + sizeof(src->i_atime_extra) <= (ULONGLONG)EXT4_GOOD_OLD_INODE_SIZE + extra) {
        dst->i_atime_extra = src->i_atime_extra;
        dst->i_ctime_extra = src->i_ctime_extra;
        dst->i_mtime_extra = src->i_mtime_extra;
    }
    if (extra &&
        offsetof(struct ext4_inode, i_crtime_extra) + sizeof(src->i_crtime_extra) <= (ULONGLONG)EXT4_GOOD_OLD_INODE_SIZE + extra) {
        dst->i_crtime = src->i_crtime;
        dst->i_crtime_extra = src->i_crtime_extra;
    }
}

void Ext2EncodeInode(struct ext4_inode *dst,  struct inode *src)
{
    dst->i_mode = src->i_mode;
    dst->i_flags = src->i_flags;
    dst->i_uid = src->i_uid;
    dst->i_gid = src->i_gid;
    dst->i_links_count = src->i_nlink;
    dst->i_generation = src->i_generation;
    dst->i_size_lo = (__u32)src->i_size;
    if (S_ISREG(src->i_mode) ||
        (S_ISDIR(src->i_mode) && EXT3_HAS_INCOMPAT_FEATURE(src->i_sb, EXT4_FEATURE_INCOMPAT_LARGEDIR))) {
        dst->i_size_high = (__u32)(src->i_size >> 32);
    }
    dst->i_file_acl_lo = (__u32)src->i_file_acl;
    /* assigned, not OR-ed: a block that moved lower, or went away, must
       not leave its old high bits behind in the 48-bit number */
    dst->osd2.linux2.l_i_file_acl_high = (__u16)(src->i_file_acl >> 32);
    dst->i_atime = src->i_atime;
    dst->i_ctime = src->i_ctime;
    dst->i_mtime = src->i_mtime;
    dst->i_dtime = src->i_dtime;
    ASSERT(src->i_sb);
    ext3_inode_blocks_set(dst, src);
    memcpy(&dst->i_block[0], &src->i_block[0], sizeof(__u32) * 15);
    /* the extended part only where the inode has one: a 128-byte inode
       ends where i_extra_isize would begin */
    if (EXT4_INODE_SIZE(src->i_sb) <= EXT4_GOOD_OLD_INODE_SIZE)
        return;
    if (EXT4_HAS_RO_COMPAT_FEATURE(src->i_sb,
                                   EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE))
        dst->i_extra_isize = src->i_extra_isize;
    if ((ULONG)EXT4_GOOD_OLD_INODE_SIZE + dst->i_extra_isize > (ULONG)EXT4_INODE_SIZE(src->i_sb))
        return;
    if (EXT4_INODE_SIZE(src->i_sb) > EXT4_GOOD_OLD_INODE_SIZE &&
        offsetof(struct ext4_inode, i_atime_extra) + sizeof(dst->i_atime_extra) <= (ULONGLONG)EXT4_GOOD_OLD_INODE_SIZE + dst->i_extra_isize) {
        dst->i_atime_extra = src->i_atime_extra;
        dst->i_ctime_extra = src->i_ctime_extra;
        dst->i_mtime_extra = src->i_mtime_extra;
    }
    if (EXT4_INODE_SIZE(src->i_sb) > EXT4_GOOD_OLD_INODE_SIZE &&
        offsetof(struct ext4_inode, i_crtime_extra) + sizeof(dst->i_crtime_extra) <= (ULONGLONG)EXT4_GOOD_OLD_INODE_SIZE + dst->i_extra_isize) {
        dst->i_crtime = src->i_crtime;
        dst->i_crtime_extra = src->i_crtime_extra;
    }
}

BOOLEAN
Ext2LoadInode (IN PEXT2_VCB Vcb,
               IN struct inode *Inode)
{
    struct ext4_inode*      ext4i;
    struct ext4_inode_info  unused = {0};
    LONGLONG                offset;

    if (!Ext2GetInodeLba(Vcb, Inode->i_ino, &offset))  {
        DEBUG(DL_ERR, ("Ext2LoadInode: failed inode %u.\n", Inode->i_ino));
        return FALSE;
    }

    ext4i = (struct ext4_inode*) Ext2AllocatePool(NonPagedPool, EXT4_INODE_SIZE(Inode->i_sb), EXT2_INODE_MAGIC);
    if (!ext4i) {
        return FALSE;
    }

    if (!Ext2LoadBuffer(NULL, Vcb, offset, EXT4_INODE_SIZE(Inode->i_sb), ext4i)) {
        Ext2FreePool(ext4i, EXT2_INODE_MAGIC);
        return FALSE;
    }

    /* an inode whose extended part does not fit itself is corrupt (Linux
       refuses it the same way) */
    {
        ULONG extra;

        if (!Ext2ExtraIsize(Inode->i_sb, ext4i, &extra)) {
            DbgPrint("ext4: inode %u: bad i_extra_isize %u\n", Inode->i_ino,
                     (ULONG)le16_to_cpu(ext4i->i_extra_isize));
            Ext2FreePool(ext4i, EXT2_INODE_MAGIC);
            return FALSE;
        }
    }

    Ext2DecodeInode(Inode, ext4i);

    ext4_inode_csum_verify(Inode, ext4i, &unused);

    Ext2FreePool(ext4i, EXT2_INODE_MAGIC);

    return TRUE;
}

BOOLEAN
Ext2ClearInode (
    IN PEXT2_IRP_CONTEXT IrpContext,
    IN PEXT2_VCB Vcb,
    IN ULONG Inode)
{
    LONGLONG            Offset = 0;
    BOOLEAN             rc;

    rc = Ext2GetInodeLba(Vcb, Inode, &Offset);
    if (!rc)  {
        DEBUG(DL_ERR, ( "Ext2SaveInode: failed inode %u.\n", Inode));
        goto errorout;
    }

    rc = Ext2ZeroBuffer(IrpContext, Vcb, Offset, Vcb->InodeSize);

errorout:

    return rc;
}

BOOLEAN
Ext2SaveInode ( IN PEXT2_IRP_CONTEXT IrpContext,
                IN PEXT2_VCB Vcb,
                IN struct inode *Inode)
{
    struct ext4_inode*      ext4i;
    struct ext4_inode_info  unused = {0};
    LONGLONG                offset;
    BOOLEAN                 rc = 0;

    DEBUG(DL_INF, ( "Ext2SaveInode: Saving Inode %xh: Mode=%xh Size=%xh\n",
                    Inode->i_ino, Inode->i_mode, Inode->i_size));
    rc = Ext2GetInodeLba(Vcb,  Inode->i_ino, &offset);
    if (!rc)  {
        DEBUG(DL_ERR, ( "Ext2SaveInode: failed inode %u.\n", Inode->i_ino));
        goto errorout;
    }

    ext4i = (struct ext4_inode*) Ext2AllocatePool(NonPagedPool, EXT4_INODE_SIZE(Inode->i_sb), EXT2_INODE_MAGIC);
    if (!ext4i) {
        rc = FALSE;
        goto errorout;
    }

    rc = Ext2LoadBuffer(NULL, Vcb, offset, EXT4_INODE_SIZE(Inode->i_sb), ext4i);
    if (!rc) {
        DEBUG(DL_ERR, ( "Ext2SaveInode: failed reading inode %u.\n", Inode->i_ino));
        Ext2FreePool(ext4i, EXT2_INODE_MAGIC);
        goto errorout;
    }

    Ext2EncodeInode(ext4i, Inode);

    ext4_inode_csum_set(Inode, ext4i, &unused);

    rc = Ext2SaveBuffer(IrpContext, Vcb, offset, EXT4_INODE_SIZE(Inode->i_sb), ext4i);


    Ext2FreePool(ext4i, EXT2_INODE_MAGIC);

errorout:
    return rc;
}

BOOLEAN
Ext2LoadInodeXattr(IN PEXT2_VCB Vcb,
	IN struct inode *Inode,
	IN PEXT2_INODE InodeXattr)
{
	IO_STATUS_BLOCK     IoStatus;
	LONGLONG            Offset;

	if (!Ext2GetInodeLba(Vcb, Inode->i_ino, &Offset)) {
		DEBUG(DL_ERR, ("Ext2LoadRawInode: error get inode(%xh)'s addr.\n", Inode->i_ino));
		return FALSE;
	}

	if (!CcCopyRead(
		Vcb->Volume,
		(PLARGE_INTEGER)&Offset,
		Vcb->InodeSize,
		TRUE,
		(PVOID)InodeXattr,
		&IoStatus)) {
		return FALSE;
	}

	if (!NT_SUCCESS(IoStatus.Status)) {
		return FALSE;
	}

	Ext2EncodeInode(InodeXattr, Inode);
	return TRUE;
}

BOOLEAN
Ext2SaveInodeXattr(IN PEXT2_IRP_CONTEXT IrpContext,
	IN PEXT2_VCB Vcb,
	IN struct inode *Inode,
	IN PEXT2_INODE InodeXattr)
{
	LONGLONG            Offset = 0;
	ULONG               InodeSize = Vcb->InodeSize;
	BOOLEAN             rc = 0;

	/* There is no way to put EA information in such a small inode */
	if (InodeSize == EXT2_GOOD_OLD_INODE_SIZE)
		return FALSE;

	DEBUG(DL_INF, ("Ext2SaveInodeXattr: Saving Inode %xh: Mode=%xh Size=%xh\n",
		Inode->i_ino, Inode->i_mode, Inode->i_size));
	rc = Ext2GetInodeLba(Vcb, Inode->i_ino, &Offset);
	if (!rc) {
		DEBUG(DL_ERR, ("Ext2SaveInodeXattr: error get inode(%xh)'s addr.\n", Inode->i_ino));
		goto errorout;
	}

	rc = Ext2SaveBuffer(IrpContext,
									Vcb,
									Offset + EXT2_GOOD_OLD_INODE_SIZE + Inode->i_extra_isize,
									InodeSize - EXT2_GOOD_OLD_INODE_SIZE - Inode->i_extra_isize,
									(char *)InodeXattr + EXT2_GOOD_OLD_INODE_SIZE + Inode->i_extra_isize);


errorout:
	return rc;
}

/*
 * Maximal extent format file size.
 * Resulting logical blkno at s_maxbytes must fit in our on-disk
 * extent format containers, within a sector_t, and within i_blocks
 * in the vfs.  ext4 inode has 48 bits of i_block in fsblock units,
 * so that won't be a limiting factor.
 *
 * Note, this does *not* consider any metadata overhead for vfs i_blocks.
 */
static loff_t ext4_max_size(int blkbits, int has_huge_files)
{
    loff_t res;
    loff_t upper_limit = MAX_LFS_FILESIZE;

    /* small i_blocks in vfs inode? */
    if (!has_huge_files || sizeof(blkcnt_t) < sizeof(u64)) {
        /*
         * CONFIG_LBD is not enabled implies the inode
         * i_block represent total blocks in 512 bytes
         * 32 == size of vfs inode i_blocks * 8
         */
        upper_limit = (1LL << 32) - 1;

        /* total blocks in file system block size */
        upper_limit >>= (blkbits - 9);
        upper_limit <<= blkbits;
    }

    /* 32-bit extent-start container, ee_block */
    res = 1LL << 32;
    res <<= blkbits;
    res -= 1;

    /* Sanity check against vm- & vfs- imposed limits */
    if (res > upper_limit)
        res = upper_limit;

    return res;
}

/*
 * Maximal extent format file size.
 * Resulting logical blkno at s_maxbytes must fit in our on-disk
 * extent format containers, within a sector_t, and within i_blocks
 * in the vfs.  ext4 inode has 48 bits of i_block in fsblock units,
 * so that won't be a limiting factor.
 *
 * Note, this does *not* consider any metadata overhead for vfs i_blocks.
 */
loff_t ext3_max_size(int blkbits, int has_huge_files)
{
    loff_t res;
    loff_t upper_limit = MAX_LFS_FILESIZE;

    /* small i_blocks in vfs inode? */
    if (!has_huge_files) {
        /*
         * CONFIG_LBD is not enabled implies the inode
         * i_block represent total blocks in 512 bytes
         * 32 == size of vfs inode i_blocks * 8
         */
        upper_limit = ((loff_t)1 << 32) - 1;

        /* total blocks in file system block size */
        upper_limit >>= (blkbits - 9);
        upper_limit <<= blkbits;
    }

    /* 32-bit extent-start container, ee_block */
    res = (loff_t)1 << 32;
    res <<= blkbits;
    res -= 1;

    /* Sanity check against vm- & vfs- imposed limits */
    if (res > upper_limit)
        res = upper_limit;

    return res;
}

/*
 * Maximal bitmap file size.  There is a direct, and {,double-,triple-}indirect
 * block limit, and also a limit of (2^48 - 1) 512-byte sectors in i_blocks.
 * We need to be 1 filesystem block less than the 2^48 sector limit.
 */
loff_t ext3_max_bitmap_size(int bits, int has_huge_files)
{
    loff_t res = EXT3_NDIR_BLOCKS;
    int meta_blocks;
    loff_t upper_limit;
    /* This is calculated to be the largest file size for a
     * dense, bitmapped file such that the total number of
     * sectors in the file, including data and all indirect blocks,
     * does not exceed 2^48 -1
     * __u32 i_blocks_lo and _u16 i_blocks_high representing the
     * total number of  512 bytes blocks of the file
     */

    if (!has_huge_files) {
        /*
         * !has_huge_files or CONFIG_LBD is not enabled
         * implies the inode i_block represent total blocks in
         * 512 bytes 32 == size of vfs inode i_blocks * 8
         */
        upper_limit = ((loff_t)1 << 32) - 1;

        /* total blocks in file system block size */
        upper_limit >>= (bits - 9);

    } else {
        /*
         * We use 48 bit ext4_inode i_blocks
         * With EXT4_HUGE_FILE_FL set the i_blocks
         * represent total number of blocks in
         * file system block size
         */
        upper_limit = ((loff_t)1 << 48) - 1;

    }

    /* indirect blocks */
    meta_blocks = 1;
    /* double indirect blocks */
    meta_blocks += 1 + ((loff_t)1 << (bits-2));
    /* tripple indirect blocks */
    meta_blocks += 1 + ((loff_t)1 << (bits-2)) + ((loff_t)1 << (2*(bits-2)));

    upper_limit -= meta_blocks;
    upper_limit <<= bits;

    res += (loff_t)1 << (bits-2);
    res += (loff_t)1 << (2*(bits-2));
    res += (loff_t)1 << (3*(bits-2));
    res <<= bits;
    if (res > upper_limit)
        res = upper_limit;

    if (res > MAX_LFS_FILESIZE)
        res = MAX_LFS_FILESIZE;

    return res;
}

blkcnt_t ext3_inode_blocks(struct ext4_inode *raw_inode,
                           struct inode *inode)
{
    blkcnt_t i_blocks ;
    struct super_block *sb = inode->i_sb;
    PEXT2_VCB Vcb = (PEXT2_VCB)sb->s_priv;

    if (EXT4_HAS_RO_COMPAT_FEATURE(sb,
                                   EXT4_FEATURE_RO_COMPAT_HUGE_FILE)) {
        /* we are using combined 48 bit field */
        i_blocks = ((u64)le16_to_cpu(raw_inode->i_blocks_high)) << 32 |
                   le32_to_cpu(raw_inode->i_blocks_lo);
        if (inode->i_flags & EXT4_HUGE_FILE_FL) {
            /* i_blocks represent file system block size */
            return i_blocks  << (BLOCK_BITS - 9);
        } else {
            return i_blocks;
        }
    } else {
        return le32_to_cpu(raw_inode->i_blocks_lo);
    }
}

int ext3_inode_blocks_set(struct ext4_inode *raw_inode,
                          struct inode * inode)
{
    u64 i_blocks = inode->i_blocks;
    struct super_block *sb = inode->i_sb;
    PEXT2_VCB Vcb = (PEXT2_VCB)sb->s_priv;

    if (i_blocks < 0x100000000) {
        /*
         * i_blocks can be represnted in a 32 bit variable
         * as multiple of 512 bytes
         */
        raw_inode->i_blocks_lo = cpu_to_le32(i_blocks);
        raw_inode->i_blocks_high = 0;
        inode->i_flags &= ~EXT4_HUGE_FILE_FL;
        return 0;
    }

    if (!EXT4_HAS_RO_COMPAT_FEATURE(sb, EXT4_FEATURE_RO_COMPAT_HUGE_FILE)) {
        Ext2SetSuperRoCompat(NULL, Vcb, EXT4_FEATURE_RO_COMPAT_HUGE_FILE);
    }

    if (i_blocks <= 0xffffffffffff) {
        /*
         * i_blocks can be represented in a 48 bit variable
         * as multiple of 512 bytes
         */
        raw_inode->i_blocks_lo = (__u32)cpu_to_le32(i_blocks);
        raw_inode->i_blocks_high = (__u16)cpu_to_le16(i_blocks >> 32);
        inode->i_flags &= ~EXT4_HUGE_FILE_FL;
    } else {
        inode->i_flags |= EXT4_HUGE_FILE_FL;
        /* i_block is stored in file system block size */
        i_blocks = i_blocks >> (BLOCK_BITS - 9);
        raw_inode->i_blocks_lo  = (__u32)cpu_to_le32(i_blocks);
        raw_inode->i_blocks_high = (__u16)cpu_to_le16(i_blocks >> 32);
    }
    return 0;
}
