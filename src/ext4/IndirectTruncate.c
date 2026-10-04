/**
 * IndirectTruncate.c - ext2/ext3 indirect block maps: truncation.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"

/*
 * The map is a tree per i_block slot: 12 data blocks, then one, two and
 * three levels of indirect blocks above the data. Everything mapped from
 * the new end on goes, data and the indirect blocks left with nothing
 * under them; an indirect block that keeps something gets its tail zeroed.
 * One walk, reading each indirect block once, as Linux does
 * (ext2_truncate_blocks). A pointer is zeroed in the step that frees what
 * it points at, so a failure leaves a map and bitmaps that agree, short of
 * a freeing that failed after its pointer went: the journal stops there
 * (Ext2ReleaseInodeBlocks).
 */

/* data blocks are given back a contiguous run at a time: a file written in
   one go lies in long runs, and a run is one bitmap update per group */
typedef struct _EXT2_FREE_RUN {
    ULONGLONG   Block;
    ULONG       Count;
} EXT2_FREE_RUN, *PEXT2_FREE_RUN;

static NTSTATUS
IndFlush(PEXT2_IRP_CONTEXT IrpContext, struct inode *Inode, PEXT2_FREE_RUN Run)
{
    NTSTATUS Status = STATUS_SUCCESS;

    if (Run->Count != 0) {
        Status = Ext2ReleaseInodeBlocks(IrpContext, Inode, Run->Block, Run->Count);
        Run->Count = 0;
    }
    return Status;
}

static NTSTATUS
IndRelease(PEXT2_IRP_CONTEXT IrpContext, struct inode *Inode, PEXT2_FREE_RUN Run,
           ULONGLONG Block)
{
    NTSTATUS Status;

    if (Run->Count != 0 && Run->Count < MAXULONG && Run->Block + Run->Count == Block) {
        Run->Count++;
        return STATUS_SUCCESS;
    }
    Status = IndFlush(IrpContext, Inode, Run);
    Run->Block = Block;
    Run->Count = 1;
    return Status;
}

static BOOLEAN
IndIsEmpty(const __u32 *Slots, ULONG Count)
{
    ULONG i;

    for (i = 0; i < Count; i++) {
        if (Slots[i] != 0) {
            return FALSE;
        }
    }
    return TRUE;
}

/*
 * What the subtree under *Ptr maps from logical block From on goes: Depth
 * levels of indirect blocks above the data (0: *Ptr is a data block), the
 * subtree mapping logical blocks from Base on. *Ptr is zeroed once nothing
 * under it is left; the caller writes what holds it (an indirect block, or
 * the inode).
 */
static NTSTATUS
IndTruncateTree(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN struct inode        *Inode,
    IN PEXT2_FREE_RUN       Run,
    IN OUT __u32           *Ptr,
    IN ULONG                Depth,
    IN ULONGLONG            Base,
    IN ULONGLONG            From
)
{
    struct buffer_head *bh;
    __u32      *Slots;
    ULONG       PerBlock = BLOCK_SIZE / sizeof(__u32);
    ULONGLONG   Span;       /* logical blocks under one slot */
    ULONG       i;
    BOOLEAN     Changed = FALSE;
    BOOLEAN     Gone;
    NTSTATUS    Status = STATUS_SUCCESS;

    if (*Ptr == 0) {
        return STATUS_SUCCESS;
    }
    /* a block outside the volume, or an indirect block among the volume's
       own metadata, is no part of this file: what it seems to point at
       belongs to others */
    if (*Ptr >= TOTAL_BLOCKS || (Depth > 0 && Ext2MetadataOverlaps(Vcb, *Ptr, 1))) {
        DbgPrint("ext4: inode %u maps block %u at depth %u: corrupt\n",
                 Inode->i_ino, *Ptr, Depth);
        return STATUS_DISK_CORRUPT_ERROR;
    }

    if (Depth == 0) {
        Status = IndRelease(IrpContext, Inode, Run, *Ptr);
        *Ptr = 0;
        return Status;
    }

    bh = sb_getblk(&Vcb->sb, (sector_t)*Ptr);
    if (bh && !buffer_uptodate(bh) && bh_submit_read(bh) < 0) {
        fini_bh(&bh);
    }
    if (!bh) {
        return STATUS_UNEXPECTED_IO_ERROR;
    }

    Slots = (__u32 *)bh->b_data;
    Span = 1ULL << ((BLOCK_BITS - 2) * (Depth - 1));
    for (i = From > Base ? (ULONG)((From - Base) / Span) : 0; i < PerBlock; i++) {
        ULONGLONG SlotBase = Base + (ULONGLONG)i * Span;

        if (Slots[i] == 0) {
            continue;
        }
        Status = IndTruncateTree(IrpContext, Vcb, Inode, Run, &Slots[i], Depth - 1,
                                 SlotBase, max(From, SlotBase));
        Changed = TRUE;
        if (!NT_SUCCESS(Status)) {
            break;
        }
    }

    /* A block that goes is not written: nothing reads it any more, and the
       freeing revokes it. One that stays is journaled with its new tail. */
    Gone = NT_SUCCESS(Status) && (From <= Base || IndIsEmpty(Slots, PerBlock));
    if (Changed && !Gone) {
        mark_buffer_dirty(bh);
    }
    fini_bh(&bh);

    if (Gone) {
        Status = IndRelease(IrpContext, Inode, Run, *Ptr);
        *Ptr = 0;
    }
    return Status;
}

NTSTATUS
Ext2TruncateIndirect(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB         Vcb,
    PEXT2_MCB         Mcb,
    PLARGE_INTEGER    Size
)
{
    struct inode   *Inode = Mcb->Inode;
    EXT2_FREE_RUN   Run = {0, 0};
    ULONGLONG       From = ((ULONGLONG)Size->QuadPart + BLOCK_SIZE - 1) >> BLOCK_BITS;
    ULONGLONG       Base = 0;
    ULONGLONG       Span;
    ULONG           Depth;
    NTSTATUS        Status = STATUS_SUCCESS;
    NTSTATUS        Flushed;

    /* the direct blocks are trees of depth 0, one block each */
    for (Depth = 0; Depth < EXT2_BLOCK_TYPES && NT_SUCCESS(Status); Depth++) {
        ULONG Slot, Slots = Depth == 0 ? EXT2_NDIR_BLOCKS : 1;

        Span = 1ULL << ((BLOCK_BITS - 2) * Depth);
        for (Slot = 0; Slot < Slots && NT_SUCCESS(Status); Slot++, Base += Span) {
            if (From < Base + Span) {
                Status = IndTruncateTree(IrpContext, Vcb, Inode, &Run,
                                         &Inode->i_block[Depth == 0 ? Slot : EXT2_NDIR_BLOCKS + Depth - 1],
                                         Depth, Base, max(From, Base));
            }
        }
    }

    /* the run gathered so far lost its pointers: it goes even after a failure */
    Flushed = IndFlush(IrpContext, Inode, &Run);
    if (NT_SUCCESS(Status)) {
        Status = Flushed;
    }

    /* The cached runs past the end go; after a failure no one knows which
       blocks went, so they all do. On a failure Size stays as asked: an
       allocation reported short only makes a later extension find blocks
       that are already there. */
    if (!NT_SUCCESS(Status) || From >= MAXULONG ||
        !Ext2RemoveBlockExtent(Vcb, Mcb, From, (ULONG)(MAXULONG - From))) {
        Ext2InvalidateZone(Mcb);
    }

    if (Inode->i_size > (loff_t)(Size->QuadPart))
        Inode->i_size = (loff_t)(Size->QuadPart);
    if (!Ext2SaveInode(IrpContext, Vcb, Inode) && NT_SUCCESS(Status))
        Status = STATUS_UNEXPECTED_IO_ERROR;

    return Status;
}
