/**
 * Superblock.c - superblock load, store and refresh.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "linux\ext4.h"

NTSTATUS
Ext2LoadSuper(IN PEXT2_VCB      Vcb,
              IN BOOLEAN        bVerify,
              OUT PEXT2_SUPER_BLOCK * Sb)
{
    NTSTATUS          Status;
    PEXT2_SUPER_BLOCK Ext2Sb = NULL;

    Ext2Sb = (PEXT2_SUPER_BLOCK)
             Ext2AllocatePool(
                 PagedPool,
                 sizeof(EXT2_SUPER_BLOCK),
                 EXT2_SB_MAGIC
             );
    if (!Ext2Sb) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto errorout;
    }

    Status = Ext2ReadDisk(
                 Vcb,
                 (ULONGLONG) SUPER_BLOCK_OFFSET,
                 sizeof(EXT2_SUPER_BLOCK),
                 (PVOID) Ext2Sb,
                 bVerify );

    if (!NT_SUCCESS(Status)) {
        Ext2FreePool(Ext2Sb, EXT2_SB_MAGIC);
        Ext2Sb = NULL;
    }

errorout:

    *Sb = Ext2Sb;
    return Status;
}

/*
 * The superblock in memory changes under SuperLock, and is written under
 * it: the checksum covers every field, so a change made by one thread while
 * another computes it would leave a superblock that Linux and e2fsck reject.
 * Taken inside a group lock (the allocators adjust the totals), never
 * around one; recursive, so a holder may call Ext2SaveSuper. The journal
 * is joined first: a commit writes the superblock under this lock too
 * (JnlMarkSuper), so a holder must never wait for one.
 */
VOID
Ext2LockSuper(IN PEXT2_VCB Vcb)
{
    Ext2JournalJoin(Vcb);
    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&Vcb->SuperLock, TRUE);
}

VOID
Ext2UnlockSuper(IN PEXT2_VCB Vcb)
{
    ExReleaseResourceLite(&Vcb->SuperLock);
    KeLeaveCriticalRegion();
}

/* set ro_compat feature Feature, once, and write the superblock */
VOID
Ext2SetSuperRoCompat(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN ULONG Feature)
{
    Ext2LockSuper(Vcb);
    if (!IsFlagOn(Vcb->SuperBlock->s_feature_ro_compat, Feature)) {
        SetFlag(Vcb->SuperBlock->s_feature_ro_compat, Feature);
        Ext2SaveSuper(IrpContext, Vcb);
    }
    Ext2UnlockSuper(Vcb);
}

BOOLEAN
Ext2SaveSuper(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb
)
{
    LONGLONG    offset;
    BOOLEAN     rc;

    Ext2LockSuper(Vcb);
    ext4_superblock_csum_set(&Vcb->sb);
    offset = (LONGLONG) SUPER_BLOCK_OFFSET;
    rc = Ext2SaveBuffer( IrpContext,
                         Vcb,
                         offset,
                         sizeof(EXT2_SUPER_BLOCK),
                         Vcb->SuperBlock
                       );
    Ext2UnlockSuper(Vcb);

    return rc;
}

BOOLEAN
Ext2RefreshSuper (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb
)
{
    UNREFERENCED_PARAMETER(IrpContext);
    LONGLONG        offset;
    IO_STATUS_BLOCK iosb;

    offset = (LONGLONG) SUPER_BLOCK_OFFSET;
    if (!CcCopyRead(
                Vcb->Volume,
                (PLARGE_INTEGER)&offset,
                sizeof(EXT2_SUPER_BLOCK),
                TRUE,
                (PVOID)Vcb->SuperBlock,
                &iosb )) {
        return FALSE;
    }

    if (!NT_SUCCESS(iosb.Status)) {
        return FALSE;
    }

    /* reload root inode */
    if (Vcb->McbTree) {

        if (!Ext2LoadInode(Vcb, Vcb->McbTree->Inode))
            return FALSE;

        /* initialize root node */
        Vcb->McbTree->Icb->LastAccessTime = Ext2GetInodeTime(Vcb->McbTree->Inode->i_atime, Vcb->McbTree->Inode->i_atime_extra);
        Vcb->McbTree->Icb->LastWriteTime = Ext2GetInodeTime(Vcb->McbTree->Inode->i_mtime, Vcb->McbTree->Inode->i_mtime_extra);
        Vcb->McbTree->Icb->ChangeTime = Ext2GetInodeTime(Vcb->McbTree->Inode->i_ctime, Vcb->McbTree->Inode->i_ctime_extra);
        if (Vcb->McbTree->Inode->i_crtime)
            Vcb->McbTree->Icb->CreationTime = Ext2GetInodeTime(Vcb->McbTree->Inode->i_crtime, Vcb->McbTree->Inode->i_crtime_extra);
        else
            Vcb->McbTree->Icb->CreationTime = Ext2GetInodeTime(Vcb->McbTree->Inode->i_ctime, Vcb->McbTree->Inode->i_ctime_extra);
    }

    return TRUE;
}

/*
 * The free block and inode totals of the volume.
 *
 * They are the sum of the group descriptors. Summing those after every
 * allocation cost O(groups); writing the superblock after every one made
 * all allocations of the volume take turns on it - one checksum and one
 * journaled block per block or inode allocated or freed. So the totals
 * live in the Vcb, moved by each allocator by the exact change it made to
 * its descriptor with one atomic add, and reach the superblock when the
 * volume is flushed: on flush, shutdown and dismount (Ext2SyncSuperTotals).
 * Linux keeps them the same way, in per-cpu counters written at sync. After
 * a crash the superblock may be behind; mount sums the descriptors again
 * (as Linux does, and e2fsck accepts).
 */
VOID
Ext2AdjustVcbStat(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN LONGLONG             BlockDelta,
    IN LONGLONG             InodeDelta
)
{
    UNREFERENCED_PARAMETER(IrpContext);

    if (BlockDelta != 0) {
        InterlockedAdd64(&Vcb->FreeBlocks, BlockDelta);
    }
    if (InodeDelta != 0) {
        InterlockedAdd64(&Vcb->FreeInodes, InodeDelta);
    }
}

/* the totals, clamped: a descriptor that was wrong before we touched it
   must not drive them below zero or past the volume */
ULONGLONG
Ext2FreeBlocks(IN PEXT2_VCB Vcb)
{
    LONGLONG Free = ReadNoFence64(&Vcb->FreeBlocks);

    Free = max(Free, 0);
    return (ULONGLONG)min(Free, (LONGLONG)ext3_blocks_count(SUPER_BLOCK));
}

ULONG
Ext2FreeInodes(IN PEXT2_VCB Vcb)
{
    LONGLONG Free = ReadNoFence64(&Vcb->FreeInodes);

    Free = max(Free, 0);
    return (ULONG)min(Free, (LONGLONG)le32_to_cpu(SUPER_BLOCK->s_inodes_count));
}

/* the totals into the superblock, written when they changed */
VOID
Ext2SyncSuperTotals(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb)
{
    ULONGLONG   Blocks;
    ULONG       Inodes;

    if (IsVcbReadOnly(Vcb)) {
        return;
    }

    Ext2LockSuper(Vcb);
    Blocks = Ext2FreeBlocks(Vcb);
    Inodes = Ext2FreeInodes(Vcb);
    if (ext3_free_blocks_count(SUPER_BLOCK) != Blocks ||
        le32_to_cpu(SUPER_BLOCK->s_free_inodes_count) != Inodes) {
        ext3_free_blocks_count_set(SUPER_BLOCK, (ext4_fsblk_t)Blocks);
        SUPER_BLOCK->s_free_inodes_count = cpu_to_le32(Inodes);
        Ext2SaveSuper(IrpContext, Vcb);
    }
    Ext2UnlockSuper(Vcb);
}
