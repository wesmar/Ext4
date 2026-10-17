/**
 * ExtentFile.c - the extent trees of the driver's inodes: ExtentCore.c over
 * the volume's buffers, its allocator and its journal; and the mapping,
 * growth and truncation of extent-mapped files.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * A node is read from the volume cache and its checksum verified once
 * (the buffer is marked verified after ExtentCore.c's checks too); a changed
 * node gets its checksum and is marked dirty, which puts it in the running
 * journal transaction; a changed root marks the inode. New nodes come from
 * the allocator as journaled blocks; blocks given up go back through
 * Ext2ReleaseInodeBlocks, which keeps i_blocks.
 *
 * A block of a file is mapped (and allocated, when asked) as follows:
 *  - in a written extent: its physical block, and how many follow it there;
 *  - in an unwritten one: a write turns the blocks it covers into written
 *    ones; a preallocation, or a read, leaves them unwritten - read as
 *    zeros (block 0) - they have never been written;
 *  - in a hole: block 0 and the length of the hole; a write or a
 *    preallocation allocates for it (preallocations unwritten), at most up
 *    to the next mapped block, next to the extent before it.
 */

#include "ext4fs.h"
#include "ExtentCore.h"

typedef struct _EXT4_EXTENTS {
    EX_TREE             T;
    PEXT2_IRP_CONTEXT   IrpContext;
    struct inode       *Inode;
    BOOLEAN             InodeChanged;   /* the root or i_blocks: the inode to write */
} EXT4_EXTENTS, *PEXT4_EXTENTS;

static int EfGet(void *Context, unsigned long long Block, unsigned char **Data, void **Handle, int *Checked)
{
    PEXT4_EXTENTS       e = (PEXT4_EXTENTS)Context;
    struct buffer_head *bh = extents_bread(e->Inode->i_sb, Block);

    if (!bh)
        return EX_ENOMEM;
    if (!buffer_uptodate(bh)) {
        extents_brelse(bh);
        return EX_EIO;
    }
    /* a node that fails its checksum is refused, as Linux refuses it: changed
       and written back, its damage would be hidden under a new one */
    if (!buffer_verified(bh) && !ext4_extent_block_csum_verify(e->Inode, ext_block_hdr(bh))) {
        Ext2ReportCorruption((PEXT2_VCB)e->Inode->i_sb->s_priv, (ULONG)e->Inode->i_ino, Block, -EFSBADCRC);
        extents_brelse(bh);
        return EX_EBADCRC;
    }
    *Data = (unsigned char *)bh->b_data;
    *Handle = bh;
    *Checked = buffer_verified(bh) ? 1 : 0;
    return EX_OK;
}

static void EfChecked(void *Context, void *Handle)
{
    UNREFERENCED_PARAMETER(Context);
    set_buffer_verified((struct buffer_head *)Handle);
}

static void EfPut(void *Context, void *Handle)
{
    UNREFERENCED_PARAMETER(Context);
    extents_brelse((struct buffer_head *)Handle);
}

static int EfDirty(void *Context, void *Handle)
{
    PEXT4_EXTENTS       e = (PEXT4_EXTENTS)Context;
    struct buffer_head *bh = (struct buffer_head *)Handle;

    if (!bh) {
        e->InodeChanged = TRUE;     /* written once, when the change is done */
        return EX_OK;
    }
    ext4_extent_block_csum_set(e->Inode, ext_block_hdr(bh));
    extents_mark_buffer_dirty(bh);
    return EX_OK;
}

static int EfNew(void *Context, unsigned long long Goal, unsigned long long *Block, unsigned char **Data,
                 void **Handle)
{
    PEXT4_EXTENTS       e = (PEXT4_EXTENTS)Context;
    struct buffer_head *bh;
    ULONGLONG           block = 0;
    ULONG               count = 1;
    NTSTATUS            status;

    status = Ext2AllocateInodeBlocks(e->IrpContext, e->Inode, Goal, EXT2_ALLOC_JOURNALED, &block, &count);
    if (!NT_SUCCESS(status))
        return Ext2LinuxError(status);
    e->InodeChanged = TRUE;
    bh = extents_bwrite(e->Inode->i_sb, block);
    if (!bh) {
        (void)Ext2ReleaseInodeBlocks(e->IrpContext, e->Inode, block, 1);
        return EX_ENOMEM;
    }
    set_buffer_uptodate(bh);
    *Block = block;
    *Data = (unsigned char *)bh->b_data;
    *Handle = bh;
    return EX_OK;
}

static int EfFree(void *Context, unsigned long long Block, unsigned int Count, int Node)
{
    PEXT4_EXTENTS e = (PEXT4_EXTENTS)Context;
    NTSTATUS      status;

    UNREFERENCED_PARAMETER(Node);
    /* (a release that fails has stopped the journal: the change that
       dropped the blocks never commits) */
    status = Ext2ReleaseInodeBlocks(e->IrpContext, e->Inode, Block, Count);
    e->InodeChanged = TRUE;
    return NT_SUCCESS(status) ? EX_OK : Ext2LinuxError(status);
}

/* inside the volume, and none of its own metadata (Linux: the system zone):
   written as a file's data, such a block would destroy bitmaps or inodes */
static int EfValid(void *Context, unsigned long long Block, unsigned int Count)
{
    PEXT4_EXTENTS            e = (PEXT4_EXTENTS)Context;
    struct ext4_super_block *es = EXT4_SB(e->Inode->i_sb)->s_es;
    ULONGLONG                first = le32_to_cpu(es->s_first_data_block), total = ext3_blocks_count(es);

    return Block >= first && Count > 0 && Block + Count > Block && Block + Count <= total &&
           !Ext2MetadataOverlaps(e->Inode->i_sb->s_priv, Block, Count);
}

static void EfCorrupt(void *Context, unsigned long long Block)
{
    PEXT4_EXTENTS e = (PEXT4_EXTENTS)Context;

    Ext2ReportCorruption((PEXT2_VCB)e->Inode->i_sb->s_priv, (ULONG)e->Inode->i_ino, Block, -EFSCORRUPTED);
}

static VOID EfSetup(PEXT4_EXTENTS e, PEXT2_IRP_CONTEXT IrpContext, struct inode *Inode)
{
    RtlZeroMemory(e, sizeof(*e));
    e->IrpContext = IrpContext;
    e->Inode = Inode;
    e->T.Root = (unsigned char *)Inode->i_block;
    e->T.BlockSize = (unsigned int)Inode->i_sb->s_blocksize;
    e->T.Goal = ext4_inode_to_goal_block(Inode);
    e->T.Io.Context = e;
    e->T.Io.Get = EfGet;
    e->T.Io.Checked = EfChecked;
    e->T.Io.Put = EfPut;
    e->T.Io.Dirty = EfDirty;
    e->T.Io.New = EfNew;
    e->T.Io.Free = EfFree;
    e->T.Io.Valid = EfValid;
    e->T.Io.Corrupt = EfCorrupt;
}

/* the inode written when the tree's root or its block count changed */
static int EfDone(PEXT4_EXTENTS e, int Err)
{
    int written = 0;

    if (e->InodeChanged)
        written = ext3_mark_inode_dirty(e->IrpContext, e->Inode);
    return Err ? Err : written;
}

int ext4_ext_tree_init(void *icb, handle_t *handle, struct inode *inode)
{
    EXT4_EXTENTS e;

    UNREFERENCED_PARAMETER(handle);
    EfSetup(&e, (PEXT2_IRP_CONTEXT)icb, inode);
    ExInit(&e.T);
    e.InodeChanged = TRUE;
    return EfDone(&e, 0);
}

int ext4_ext_truncate(void *icb, struct inode *inode, unsigned long start)
{
    EXT4_EXTENTS e;

    EfSetup(&e, (PEXT2_IRP_CONTEXT)icb, inode);
    e.InodeChanged = TRUE;      /* i_blocks, whatever went */
    return EfDone(&e, ExTruncate(&e.T, (unsigned int)start));
}

/*
 * Block iblock of inode and the blocks after it, up to max_blocks: the
 * count (bh_result->b_blocknr the first physical block, 0 for a hole or
 * unwritten blocks; BH_New for blocks allocated written), or an error.
 * create allocates for a hole and writes unwritten blocks; with
 * EXT4_GET_BLOCKS_PRE_IO the allocation is unwritten and unwritten blocks
 * stay so.
 */
int ext4_ext_get_blocks(void *icb, handle_t *handle, struct inode *inode, ext4_lblk_t iblock,
                        unsigned long max_blocks, struct buffer_head *bh_result, int create, int flags)
{
    EXT4_EXTENTS        e;
    EX_EXTENT           x;
    unsigned long long  next, goal;
    unsigned long       count;
    int                 err, prealloc = (flags & EXT4_GET_BLOCKS_PRE_IO) != 0;

    UNREFERENCED_PARAMETER(handle);
    clear_buffer_new(bh_result);
    if (max_blocks == 0)
        max_blocks = 1;
    EfSetup(&e, (PEXT2_IRP_CONTEXT)icb, inode);

    err = ExMap(&e.T, iblock, &x, &next, &goal);
    if (err == EX_OK) {
        count = (unsigned long)min((unsigned long long)max_blocks, x.Block + (unsigned long long)x.Length - iblock);
        if (x.Unwritten && create && !prealloc) {
            /* written now: only the blocks of this write */
            err = ExConvert(&e.T, iblock, (unsigned int)count);
            if (err)
                return EfDone(&e, err);
            x.Unwritten = 0;
        }
        bh_result->b_blocknr = x.Unwritten ? 0 : x.Start + (iblock - x.Block);
        set_buffer_mapped(bh_result);
        bh_result->b_bdev = inode->i_sb->s_bdev;
        err = EfDone(&e, 0);
        return err ? err : (int)count;
    }
    if (err != EX_ENOENT)
        return err;

    /* a hole, as long as it runs */
    count = (unsigned long)min((unsigned long long)max_blocks, next - iblock);
    if (!create) {
        bh_result->b_blocknr = 0;
        return (int)count;
    }

    /* blocks for it: no more than one extent of its kind holds */
    {
        ULONGLONG start = 0;
        ULONG     got = (ULONG)min(count, (unsigned long)(prealloc ? EX_UNWRITTEN_MAX : EX_INIT_MAX));
        NTSTATUS  status;

        status = Ext2AllocateInodeBlocks((PEXT2_IRP_CONTEXT)icb, inode, goal,
                                         S_ISDIR(inode->i_mode) ? EXT2_ALLOC_JOURNALED : 0, &start, &got);
        if (!NT_SUCCESS(status))
            return Ext2LinuxError(status);
        if (got == 0)
            return -ENOSPC;
        e.InodeChanged = TRUE;
        x.Block = iblock;
        x.Length = got;
        x.Start = start;
        x.Unwritten = prealloc;
        err = ExInsert(&e.T, &x, 1);
        if (err) {
            /* exactly this allocation goes back (a release that fails stops
               the journal) */
            (void)Ext2ReleaseInodeBlocks((PEXT2_IRP_CONTEXT)icb, inode, start, got);
            return EfDone(&e, err);
        }
        count = got;
    }
    err = EfDone(&e, 0);
    if (err)
        return err;
    bh_result->b_blocknr = prealloc ? 0 : x.Start;
    if (!prealloc)
        set_buffer_new(bh_result);
    set_buffer_mapped(bh_result);
    bh_result->b_bdev = inode->i_sb->s_bdev;
    return (int)count;
}

/* ---- the files ---- */

/* how a request maps: writes, directories and the journal's own file write
   their blocks (the unwritten ones they touch become written); any other
   allocation - a file made larger - preallocates */
static int EfFlags(PEXT2_IRP_CONTEXT IrpContext, PEXT2_MCB Mcb, BOOLEAN Alloc)
{
    if (IsMcbDirectory(Mcb) || IrpContext == NULL || IrpContext->MajorFunction == IRP_MJ_WRITE || !Alloc)
        return EXT4_GET_BLOCKS_IO_CONVERT_EXT;
    return EXT4_GET_BLOCKS_IO_CREATE_EXT;
}

NTSTATUS
Ext2MapExtent(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_MCB            Mcb,
    IN ULONG                Index,
    IN BOOLEAN              Alloc,
    OUT PULONGLONG          Block,
    OUT PULONG              Number
)
{
    struct buffer_head  bh = { 0 };
    unsigned long       max;
    int                 flags = EfFlags(IrpContext, Mcb, Alloc), rc;

    /* i_block holds data, not a tree: an allocation that reached here
       missed the conversion (InlineData.c) */
    if (Alloc && Ext4IsInline(Mcb->Inode)) {
        DbgPrint("ext4: allocation for inline inode %u refused\n", Mcb->Inode->i_ino);
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (get_ext4_header(Mcb->Inode)->eh_magic != EXT4_EXT_MAGIC) {
        if (!Alloc) {
            /* no tree yet: nothing is mapped */
            LONGLONG size = Mcb->Icb->Fcb ? Mcb->Icb->Fcb->Header.AllocationSize.QuadPart : Mcb->Inode->i_size;
            if (Block)
                *Block = 0;
            if (Number)
                *Number = (ULONG)((size + BLOCK_SIZE - 1) >> BLOCK_BITS);
            return STATUS_SUCCESS;
        }
        rc = ext4_ext_tree_init(IrpContext, NULL, Mcb->Inode);
        if (rc)
            return Ext2WinntError(rc);
    }

    /* an allocation covers the run asked for, never more: a block made
       written past the i/o would show what the disk held before */
    max = flags == EXT4_GET_BLOCKS_IO_CONVERT_EXT ? EX_INIT_MAX : EX_UNWRITTEN_MAX;
    if (Alloc)
        max = Number && *Number ? min(max, *Number) : 1;

    rc = ext4_ext_get_blocks(IrpContext, NULL, Mcb->Inode, Index, max, &bh, Alloc, flags);
    if (rc < 0) {
        DEBUG(DL_ERR, ("Ext2MapExtent: inode %u, block %u: error %d\n", Mcb->Inode->i_ino, Index, rc));
        return Ext2WinntError(rc);
    }
    if (Alloc && !Ext2SaveInode(IrpContext, Vcb, Mcb->Inode))
        return STATUS_UNEXPECTED_IO_ERROR;
    if (Number)
        *Number = rc ? rc : 1;
    if (Block)
        *Block = bh.b_blocknr;
    return STATUS_SUCCESS;
}

/* blocks Index.. of a file made larger: Number of them asked, how many came */
static NTSTATUS
Ext2ExtendExtent(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN PEXT2_MCB Mcb, IN ULONG Index,
                 IN OUT PULONGLONG Block, IN OUT PULONG Number)
{
    struct buffer_head  bh = { 0 };
    int                 rc;

    if (Ext4IsInline(Mcb->Inode)) {
        DbgPrint("ext4: allocation for inline inode %u refused\n", Mcb->Inode->i_ino);
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (get_ext4_header(Mcb->Inode)->eh_magic != EXT4_EXT_MAGIC) {
        rc = ext4_ext_tree_init(IrpContext, NULL, Mcb->Inode);
        if (rc)
            return Ext2WinntError(rc);
    }
    rc = ext4_ext_get_blocks(IrpContext, NULL, Mcb->Inode, Index, *Number, &bh, 1,
                             EfFlags(IrpContext, Mcb, TRUE));
    if (rc < 0)
        return Ext2WinntError(rc);
    *Number = rc ? rc : 1;
    *Block = bh.b_blocknr;
    return Ext2SaveInode(IrpContext, Vcb, Mcb->Inode) ? STATUS_SUCCESS : STATUS_UNEXPECTED_IO_ERROR;
}

NTSTATUS
Ext2ExpandExtent(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB         Vcb,
    PEXT2_MCB         Mcb,
    ULONG             Start,
    ULONG             End,
    PLARGE_INTEGER    Size
    )
{
    NTSTATUS  Status = STATUS_SUCCESS;
    ULONG     Count = 0;

    while (End > Start + Count) {
        ULONGLONG Block = 0;
        ULONG     Number = End - Start - Count;

        /* a full volume is STATUS_DISK_FULL (ENOSPC), as applications
           report it, not a lack of resources */
        Status = Ext2ExtendExtent(IrpContext, Vcb, Mcb, Start + Count, &Block, &Number);
        if (!NT_SUCCESS(Status))
            break;
        if (Number == 0) {
            Status = STATUS_DISK_FULL;
            break;
        }
        if (Block && IsZoneInited(Mcb) && !Ext2AddBlockExtent(Vcb, Mcb, Start + Count, Block, Number)) {
            ClearLongFlag(Mcb->Icb->Flags, ICB_ZONE_INITED);
            Ext2ClearAllExtents(&Mcb->Icb->Extents);
        }
        Count += Number;
    }

    Size->QuadPart = ((LONGLONG)(Start + Count)) << BLOCK_BITS;
    /* the inode whatever came of it */
    if (!Ext2SaveInode(IrpContext, Vcb, Mcb->Inode) && NT_SUCCESS(Status))
        Status = STATUS_UNEXPECTED_IO_ERROR;
    return Status;
}

NTSTATUS
Ext2TruncateExtent(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB         Vcb,
    PEXT2_MCB         Mcb,
    PLARGE_INTEGER    Size
    )
{
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG    Wanted = (ULONG)((Size->QuadPart + BLOCK_SIZE - 1) >> BLOCK_BITS);    /* the first block that goes */
    int      err;

    /* Failing part way, the tree has lost some of the blocks past Wanted and
       the cached runs cannot tell which: they all go, not only those past
       it - one left behind would map a freed block. The size stays as asked:
       an allocation reported short only makes a later extension find blocks
       that are there already. */
    err = ext4_ext_truncate(IrpContext, Mcb->Inode, Wanted);
    if (err)
        Status = Ext2WinntError(err);
    if (err || !Ext2RemoveBlockExtent(Vcb, Mcb, Wanted, MAXULONG - Wanted)) {
        ClearLongFlag(Mcb->Icb->Flags, ICB_ZONE_INITED);
        Ext2ClearAllExtents(&Mcb->Icb->Extents);
    }

    if (Mcb->Inode->i_size > (loff_t)Size->QuadPart)
        Mcb->Inode->i_size = (loff_t)Size->QuadPart;
    /* i_blocks and i_size */
    if (!Ext2SaveInode(IrpContext, Vcb, Mcb->Inode) && NT_SUCCESS(Status))
        Status = STATUS_UNEXPECTED_IO_ERROR;
    return Status;
}
