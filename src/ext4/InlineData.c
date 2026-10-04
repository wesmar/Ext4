/**
 * InlineData.c - inline_data: files and directories kept inside their inode.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include <linux/module.h>
#include <linux/ext4_ext.h>
#include <linux/ext4_xattr.h>

/*
 * The on-disk layout, as Linux keeps it (fs/ext4/inline.c): an inode with
 * EXT4_INLINE_DATA_FL holds its first 60 bytes in i_block and the rest in
 * the value of the "system.data" attribute in the inode body.
 *  - a file: bytes 0..59 in i_block, the rest in system.data; i_size is the
 *    file's size;
 *  - a directory: i_block starts with the parent's inode number (4 bytes),
 *    then directory entries up to byte 60 (the last one's rec_len ends
 *    there), then more entries in system.data (the last one ends with the
 *    value). There is no "." nor "..". i_size is 60 + the value's length.
 *
 * Read here as they are. Anything that changes such an inode converts it
 * to blocks first, as Linux does when inline data no longer fits:
 * Ext4UninlineFile, Ext4UninlineDir.
 */

#define INLINE_IBLOCK       (sizeof(__le32) * EXT4_N_BLOCKS)   /* 60 */
#define INLINE_DOTDOT       sizeof(__le32)
#define INLINE_NAME         "data"
#define INLINE_NAME_LEN     4
#define DIRENT_HEAD         8   /* inode, rec_len, name_len, file_type */

BOOLEAN Ext4IsInline(struct inode *inode)
{
    return (inode->i_flags & EXT4_INLINE_DATA_FL) != 0;
}

/* the whole inline content: i_block, then the value of system.data */
static int ext4_inline_get(PEXT2_IRP_CONTEXT IrpContext, PEXT2_VCB Vcb,
                           PEXT2_MCB Mcb, char **out, size_t *len)
{
    struct ext4_xattr_ref ref;
    size_t xlen = 0;
    char  *buf;
    int    ret;

    ret = ext4_fs_get_xattr_ref(IrpContext, Vcb, Mcb, &ref);
    if (ret)
        return ret;
    ret = ext4_fs_get_xattr(&ref, EXT4_XATTR_INDEX_SYSTEM, INLINE_NAME,
                            INLINE_NAME_LEN, NULL, 0, &xlen);
    if (ret == -ENODATA) {
        xlen = 0;
        ret = 0;
    }
    if (!ret) {
        buf = kmalloc(INLINE_IBLOCK + xlen, GFP_NOFS);
        if (!buf) {
            ret = -ENOMEM;
        } else {
            memcpy(buf, Mcb->Inode->i_block, INLINE_IBLOCK);
            if (xlen)
                ret = ext4_fs_get_xattr(&ref, EXT4_XATTR_INDEX_SYSTEM, INLINE_NAME,
                                        INLINE_NAME_LEN, buf + INLINE_IBLOCK, xlen, NULL);
            if (ret) {
                kfree(buf);     /* its tail never filled: never handed out */
            } else {
                *out = buf;
                *len = INLINE_IBLOCK + xlen;
            }
        }
    }
    ref.dirty = FALSE;
    (void)ext4_fs_put_xattr_ref(&ref);      /* read only: nothing to write */
    return ret;
}

/*
 * An inline directory as the block it would be: ".", "..", then the live
 * entries, the last one stretched to end. Every entry is checked to lie
 * whole inside its region (i_block or the attribute value).
 */
static int ext4_inline_dir_build(struct inode *dir, const char *raw, size_t rawlen,
                                 char *blk, unsigned end)
{
    struct super_block      *sb = dir->i_sb;
    struct ext3_dir_entry_2 *de, *last;
    unsigned                 off;
    size_t                   pos;

    if (rawlen < INLINE_IBLOCK)
        return -EFSCORRUPTED;

    de = (struct ext3_dir_entry_2 *)blk;
    de->inode = cpu_to_le32(dir->i_ino);
    de->name_len = 1;
    de->rec_len = ext3_rec_len_to_disk(EXT3_DIR_REC_LEN(1));
    de->name[0] = '.';
    ext3_set_de_type(sb, de, S_IFDIR);
    off = EXT3_DIR_REC_LEN(1);

    de = (struct ext3_dir_entry_2 *)(blk + off);
    de->inode = *(const __le32 *)raw;
    de->name_len = 2;
    de->rec_len = ext3_rec_len_to_disk(EXT3_DIR_REC_LEN(2));
    de->name[0] = de->name[1] = '.';
    ext3_set_de_type(sb, de, S_IFDIR);
    last = de;
    off += EXT3_DIR_REC_LEN(2);

    for (pos = INLINE_DOTDOT; pos < rawlen; ) {
        const struct ext3_dir_entry_2 *src = (const struct ext3_dir_entry_2 *)(raw + pos);
        size_t   region_end = pos < INLINE_IBLOCK ? INLINE_IBLOCK : rawlen;
        unsigned rec;

        if (region_end - pos < DIRENT_HEAD)
            return -EFSCORRUPTED;
        rec = ext3_rec_len_from_disk(src->rec_len);
        if (rec < DIRENT_HEAD || (rec & 3) || pos + rec > region_end ||
            (unsigned)src->name_len + DIRENT_HEAD > rec)
            return -EFSCORRUPTED;
        if (src->inode) {
            unsigned need = EXT3_DIR_REC_LEN(src->name_len);

            if (off + need > end)
                return -EFSCORRUPTED;
            de = (struct ext3_dir_entry_2 *)(blk + off);
            memcpy(de, src, DIRENT_HEAD + src->name_len);
            de->rec_len = ext3_rec_len_to_disk(need);
            last = de;
            off += need;
        }
        pos += rec;
    }
    last->rec_len = ext3_rec_len_to_disk(end - (unsigned)((char *)last - blk));
    return 0;
}

/* the size a directory has for the code that walks its blocks */
loff_t Ext4DirSize(struct inode *dir)
{
    return Ext4IsInline(dir) ? (loff_t)dir->i_sb->s_blocksize : dir->i_size;
}

/* ext3_bread of block 0 of an inline directory: its view, read only */
struct buffer_head *Ext4InlineDirBlock(struct ext2_icb *icb, struct inode *dir,
                                       unsigned long block, int *err)
{
    PEXT2_MCB           Mcb = (PEXT2_MCB)dir->i_priv;
    PEXT2_VCB           Vcb = dir->i_sb->s_priv;
    struct buffer_head *bh;
    char               *raw = NULL;
    size_t              len = 0;

    *err = 0;
    if (block != 0 || Mcb == NULL) {
        *err = -EIO;
        return NULL;
    }
    *err = ext4_inline_get(icb, Vcb, Mcb, &raw, &len);
    if (*err)
        return NULL;
    bh = alloc_virtual_bh(dir->i_sb->s_bdev, dir->i_sb->s_blocksize);
    if (!bh) {
        kfree(raw);
        *err = -ENOMEM;
        return NULL;
    }
    *err = ext4_inline_dir_build(dir, raw, len, bh->b_data, (unsigned)dir->i_sb->s_blocksize);
    kfree(raw);
    if (*err) {
        DbgPrint("ext4: inline directory %u is corrupt\n", dir->i_ino);
        __brelse(bh);
        return NULL;
    }
    return bh;
}

/*
 * Ext2ReadInode of an inline inode: a file's bytes, or a directory's view
 * (what the enumeration walks). Past the end: zeros.
 */
NTSTATUS Ext4InlineRead(PEXT2_IRP_CONTEXT IrpContext, PEXT2_VCB Vcb, PEXT2_MCB Mcb,
                        ULONGLONG Offset, PVOID Buffer, ULONG Size, PULONG BytesRead)
{
    struct inode *inode = Mcb->Inode;
    char         *raw = NULL, *view = NULL;
    const char   *data;
    size_t        len = 0, avail;
    int           err;

    err = ext4_inline_get(IrpContext, Vcb, Mcb, &raw, &len);
    if (err)
        return Ext2WinntError(err);

    if (S_ISDIR(inode->i_mode)) {
        view = kmalloc(BLOCK_SIZE, GFP_NOFS);
        if (!view) {
            kfree(raw);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(view, BLOCK_SIZE);
        err = ext4_inline_dir_build(inode, raw, len, view, BLOCK_SIZE);
        if (err) {
            kfree(raw);
            kfree(view);
            return STATUS_FILE_CORRUPT_ERROR;
        }
        data = view;
        avail = BLOCK_SIZE;
    } else {
        data = raw;
        avail = (size_t)min((ULONGLONG)len, (ULONGLONG)inode->i_size);
    }

    if (Offset < avail) {
        size_t n = (size_t)min((ULONGLONG)Size, avail - Offset);

        RtlCopyMemory(Buffer, data + Offset, n);
        if (n < Size)
            RtlZeroMemory((PCHAR)Buffer + n, Size - n);
    } else {
        RtlZeroMemory(Buffer, Size);
    }
    if (BytesRead)
        *BytesRead = Size;

    kfree(raw);
    if (view)
        kfree(view);
    return STATUS_SUCCESS;
}

/* the fields of an inline inode that leaving inline rewrites */
struct ext4_inline_body {
    unsigned long   flags;
    __u64           blocks;
    __u32           block[EXT4_N_BLOCKS];
};

static void ext4_inline_keep(const struct inode *inode, struct ext4_inline_body *body)
{
    body->flags = inode->i_flags;
    body->blocks = inode->i_blocks;
    RtlCopyMemory(body->block, inode->i_block, sizeof(body->block));
}

/*
 * Leaving inline failed part way: the inode is inline again, as the disk
 * still has it once the caller drops its attribute set unwritten. The empty
 * tree was already written with the inode (ext4_ext_tree_init), so the
 * inline body is written back too, or the data in i_block were lost. A
 * block taken for the data goes back first; one that cannot leaves the
 * transaction half made, and it must not commit.
 */
static void ext4_inline_undo(PEXT2_IRP_CONTEXT IrpContext, struct inode *inode,
                             const struct ext4_inline_body *body)
{
    PEXT2_VCB Vcb = inode->i_sb->s_priv;

    if (inode->i_flags == body->flags &&
        RtlEqualMemory(inode->i_block, body->block, sizeof(body->block))) {
        return;     /* nothing was changed yet */
    }
    if (inode->i_blocks != body->blocks && ext4_ext_truncate(IrpContext, inode, 0) != 0) {
        Ext2JournalAbandon(Vcb, STATUS_UNEXPECTED_IO_ERROR);
    }
    inode->i_flags = body->flags;
    inode->i_blocks = body->blocks;
    RtlCopyMemory(inode->i_block, body->block, sizeof(inode->i_block));
    (void)Ext2SaveInode(IrpContext, Vcb, inode);    /* fails only once the volume stopped writing */
}

/*
 * The inode leaves inline: system.data goes, the flag goes, an empty
 * extent tree takes i_block. Block 0 is allocated (initialized, not
 * unwritten) when want_block. The attribute set is written back with the
 * inode by the caller's ext4_fs_put_xattr_ref; on a failure, here or after,
 * the caller puts the inode back with ext4_inline_undo.
 */
static int ext4_inline_drop(PEXT2_IRP_CONTEXT IrpContext, struct ext4_xattr_ref *ref,
                            struct inode *inode, BOOLEAN want_block, ext4_fsblk_t *pblk)
{
    struct buffer_head bh_got = {0};
    int ret;

    ret = ext4_fs_remove_xattr(ref, EXT4_XATTR_INDEX_SYSTEM, INLINE_NAME, INLINE_NAME_LEN);
    if (ret && ret != -ENOENT)
        return ret;
    ref->dirty = TRUE;

    inode->i_flags &= ~EXT4_INLINE_DATA_FL;
    memset(inode->i_block, 0, sizeof(inode->i_block));
    if (EXT3_HAS_INCOMPAT_FEATURE(inode->i_sb, EXT4_FEATURE_INCOMPAT_EXTENTS)) {
        inode->i_flags |= EXT4_EXTENTS_FL;
        ret = ext4_ext_tree_init(IrpContext, NULL, inode);
        if (ret)
            return ret;
    }
    if (!want_block)
        return 0;
    if (!(inode->i_flags & EXT4_EXTENTS_FL))
        return -EOPNOTSUPP;     /* inline_data comes with extents in practice */
    ret = ext4_ext_get_blocks(IrpContext, NULL, inode, 0, 1, &bh_got, 1, 0);
    if (ret <= 0)
        return ret ? ret : -ENOSPC;
    *pblk = bh_got.b_blocknr;
    return 0;
}

/*
 * The inode out of inline for good: the attribute set without system.data,
 * then the inode (the set writes it only when a change landed in the inode
 * body). Past this point there is nothing to go back to: a failure leaves
 * the transaction half made, and it must not commit.
 */
static NTSTATUS ext4_inline_commit(PEXT2_IRP_CONTEXT IrpContext, PEXT2_VCB Vcb,
                                   struct ext4_xattr_ref *ref, struct inode *inode)
{
    NTSTATUS Status;

    Status = Ext2WinntError(ext4_fs_put_xattr_ref(ref));
    if (NT_SUCCESS(Status) && !Ext2SaveInode(IrpContext, Vcb, inode))
        Status = STATUS_UNEXPECTED_IO_ERROR;
    if (!NT_SUCCESS(Status))
        Ext2JournalAbandon(Vcb, Status);
    return Status;
}

/*
 * A file leaves inline before anything changes its data or its size. The
 * bytes go straight to their new block, written through, before the inode
 * that points there is journaled (data before metadata, as Linux orders
 * it); with KeepData FALSE (truncation to nothing) no block is taken.
 */
NTSTATUS Ext4UninlineFile(PEXT2_IRP_CONTEXT IrpContext, PEXT2_VCB Vcb,
                          PEXT2_MCB Mcb, BOOLEAN KeepData)
{
    struct inode         *inode = Mcb->Inode;
    struct ext4_xattr_ref ref;
    struct ext4_inline_body body;
    char                 *raw = NULL, *blk = NULL;
    size_t                len = 0;
    ext4_fsblk_t          pblk = 0;
    NTSTATUS              Status = STATUS_SUCCESS;
    int                   ret;

    if (!Ext4IsInline(inode))
        return STATUS_SUCCESS;
    if (IsVcbReadOnly(Vcb))
        return STATUS_MEDIA_WRITE_PROTECTED;

    KeepData = KeepData && inode->i_size > 0;
    if (KeepData) {
        ret = ext4_inline_get(IrpContext, Vcb, Mcb, &raw, &len);
        if (ret)
            return Ext2WinntError(ret);
        len = (size_t)min((ULONGLONG)len, (ULONGLONG)inode->i_size);
        blk = kmalloc(BLOCK_SIZE, GFP_NOFS);
        if (!blk) {
            kfree(raw);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(blk, BLOCK_SIZE);
        memcpy(blk, raw, len);
        kfree(raw);
    }

    ret = ext4_fs_get_xattr_ref(IrpContext, Vcb, Mcb, &ref);
    if (ret) {
        Status = Ext2WinntError(ret);
        goto out;
    }
    ext4_inline_keep(inode, &body);
    ret = ext4_inline_drop(IrpContext, &ref, inode, KeepData, &pblk);
    if (!ret && KeepData) {
        Status = Ext2WriteDiskSync(Vcb, (ULONGLONG)pblk << BLOCK_BITS, BLOCK_SIZE, blk, TRUE);
        if (!NT_SUCCESS(Status))
            ret = -EIO;
    }
    if (ret) {
        ref.dirty = FALSE;
        (void)ext4_fs_put_xattr_ref(&ref);      /* dropped unwritten */
        ext4_inline_undo(IrpContext, inode, &body);
        if (NT_SUCCESS(Status))
            Status = Ext2WinntError(ret);
        goto out;
    }
    if (!KeepData)
        inode->i_size = 0;
    Status = ext4_inline_commit(IrpContext, Vcb, &ref, inode);

out:
    if (blk)
        kfree(blk);
    return Status;
}

/*
 * A directory leaves inline before an entry is added, removed or changed:
 * its view becomes block 0, with the checksum tail when metadata_csum, and
 * goes through the journal like any directory block.
 */
NTSTATUS Ext4UninlineDir(PEXT2_IRP_CONTEXT IrpContext, PEXT2_VCB Vcb, PEXT2_MCB Mcb)
{
    struct inode         *dir = Mcb->Inode;
    struct super_block   *sb = dir->i_sb;
    struct ext4_xattr_ref ref;
    struct ext4_inline_body body;
    NTSTATUS              Status;
    struct buffer_head   *bh;
    char                 *raw = NULL;
    size_t                len = 0;
    ext4_fsblk_t          pblk = 0;
    unsigned              blocksize = (unsigned)sb->s_blocksize;
    unsigned              csum = ext4_has_metadata_csum(sb) ? sizeof(struct ext4_dir_entry_tail) : 0;
    int                   ret;

    if (!Ext4IsInline(dir))
        return STATUS_SUCCESS;
    if (IsVcbReadOnly(Vcb))
        return STATUS_MEDIA_WRITE_PROTECTED;

    ret = ext4_inline_get(IrpContext, Vcb, Mcb, &raw, &len);
    if (ret)
        return Ext2WinntError(ret);

    ret = ext4_fs_get_xattr_ref(IrpContext, Vcb, Mcb, &ref);
    if (ret) {
        kfree(raw);
        return Ext2WinntError(ret);
    }
    ext4_inline_keep(dir, &body);
    ret = ext4_inline_drop(IrpContext, &ref, dir, TRUE, &pblk);
    if (!ret) {
        bh = extents_bwrite(&Vcb->sb, pblk);
        if (!bh) {
            ret = -ENOMEM;
        } else {
            ret = ext4_inline_dir_build(dir, raw, len, bh->b_data, blocksize - csum);
            if (!ret) {
                if (csum)
                    initialize_dirent_tail(EXT4_DIRENT_TAIL(bh->b_data, blocksize), blocksize);
                ext4_dirent_csum_set(dir, (struct ext4_dir_entry *)bh->b_data);
                mark_buffer_dirty(bh);
            }
            extents_brelse(bh);
        }
    }
    kfree(raw);
    if (ret) {
        ref.dirty = FALSE;
        (void)ext4_fs_put_xattr_ref(&ref);      /* dropped unwritten */
        ext4_inline_undo(IrpContext, dir, &body);
        return Ext2WinntError(ret);
    }

    dir->i_size = blocksize;
    Status = ext4_inline_commit(IrpContext, Vcb, &ref, dir);
    if (!NT_SUCCESS(Status))
        return Status;

    /* the open directory's sizes follow */
    if (Mcb->Icb && Mcb->Icb->Fcb) {
        PEXT2_FCB Dcb = Mcb->Icb->Fcb;

        Dcb->Header.AllocationSize.QuadPart =
        Dcb->Header.FileSize.QuadPart =
        Dcb->Header.ValidDataLength.QuadPart = blocksize;
    }
    return STATUS_SUCCESS;
}
