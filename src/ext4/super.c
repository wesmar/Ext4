/**
 * super.c - superblock load, store and refresh.
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

BOOLEAN
Ext2SaveSuper(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb
)
{
    LONGLONG    offset;
    BOOLEAN     rc;

    ext4_superblock_csum_set(&Vcb->sb);
    offset = (LONGLONG) SUPER_BLOCK_OFFSET;
    rc = Ext2SaveBuffer( IrpContext,
                         Vcb,
                         offset,
                         sizeof(EXT2_SUPER_BLOCK),
                         Vcb->SuperBlock
                       );


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

        /* initializeroot node */
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
 * Carry a change of the per-group free counters into the superblock.
 *
 * The superblock totals are the sum of the group descriptors. Summing them
 * again after every allocation cost O(groups) per block or inode - 8192
 * descriptors twice over on a 1 TiB volume, for every write that allocates.
 * The allocators know the exact change they made to one descriptor, so the
 * total moves by that difference instead (Linux keeps the same totals in
 * per-cpu counters for the same reason). Mount sums the descriptors once
 * (Ext2LoadGroup), which is where a stale superblock gets corrected.
 *
 * BlockDelta is serialized by Vcb->MetaBlock, InodeDelta by Vcb->MetaInode;
 * the two touch disjoint fields, so neither needs the other's lock.
 */
VOID
Ext2AdjustVcbStat(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN LONGLONG             BlockDelta,
    IN LONGLONG             InodeDelta
)
{
    if (BlockDelta != 0) {
        LONGLONG Free = (LONGLONG)ext3_free_blocks_count(SUPER_BLOCK) + BlockDelta;
        /* clamp: a descriptor that was wrong before we touched it must not
           drive the total below zero or past the volume */
        Free = max(Free, 0);
        Free = min(Free, (LONGLONG)ext3_blocks_count(SUPER_BLOCK));
        ext3_free_blocks_count_set(SUPER_BLOCK, (ext4_fsblk_t)Free);
    }
    if (InodeDelta != 0) {
        LONGLONG Free = (LONGLONG)le32_to_cpu(SUPER_BLOCK->s_free_inodes_count) + InodeDelta;
        Free = max(Free, 0);
        Free = min(Free, (LONGLONG)le32_to_cpu(SUPER_BLOCK->s_inodes_count));
        SUPER_BLOCK->s_free_inodes_count = cpu_to_le32((__u32)Free);
    }
    if (BlockDelta != 0 || InodeDelta != 0) {
        Ext2SaveSuper(IrpContext, Vcb);
    }
}
