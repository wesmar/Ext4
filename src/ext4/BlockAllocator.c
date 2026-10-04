/**
 * BlockAllocator.c - block allocation and release, one group at a time.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * A group is searched and changed under the BlockLock of its stripe
 * (core\LockStripes.c), joined to the journal first (Ext2JournalJoin).
 *
 * What the search sees as taken: the bitmap on disk, and the blocks freed by
 * transactions not yet committed (FreedBlocks.c), which must not be reused
 * before the commit. The view is scanned a 64-bit word at a time with a bit
 * scan, never bit by bit: O(words + runs) per group.
 *
 * What may never be handed out or given back: metadata - superblock and
 * descriptor copies, bitmaps, inode tables, of any group, wherever flex_bg
 * put them (MetadataMap.c). A bitmap that claims otherwise is corrupt.
 *
 * A release is checked whole before anything changes - every block taken,
 * none of it metadata - and only then revoked in the journal and cleared:
 * a revoke of a block still in use would make a replay drop its logged
 * contents.
 */

#include "ext4fs.h"
#include "linux\ext4.h"
#include "BlockSearch.h"

/* ---------------------------------------------------------------- geometry */

static __inline ULONGLONG
Ext2GroupFirst(IN PEXT2_VCB Vcb, IN ULONG Group)
{
    return EXT2_FIRST_DATA_BLOCK + (ULONGLONG)Group * BLOCKS_PER_GROUP;
}

/* blocks in Group: a full group, or what the volume leaves of the last one */
static __inline ULONG
Ext2GroupBlocks(IN PEXT2_VCB Vcb, IN ULONG Group)
{
    return (ULONG)min((ULONGLONG)BLOCKS_PER_GROUP, TOTAL_BLOCKS - Ext2GroupFirst(Vcb, Group));
}

/* ---------------------------------------------------------------- one group */

/* the block bitmap of Group; one lazy mkfs never wrote is built and published first */
static NTSTATUS
Ext2LoadBlockBitmap(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN ULONG Group,
                    IN struct ext4_group_desc *gd, OUT struct buffer_head **Bh)
{
    struct super_block *sb = &Vcb->sb;
    ext4_fsblk_t        Where = ext4_block_bitmap(sb, gd);

    if (gd->bg_flags & cpu_to_le16(EXT4_BG_BLOCK_UNINIT)) {
        *Bh = sb_getblk_zero(sb, Where);
        if (*Bh == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        /* built from the group layout and journaled at once: clearing
           BLOCK_UNINIT publishes the block as the truth, whether or not an
           allocation ends up in this group */
        ext4_init_block_bitmap(sb, *Bh, Group, gd);
        set_buffer_uptodate(*Bh);
        Ext2ClearGroupFlag(gd, EXT4_BG_BLOCK_UNINIT);
        ext4_block_bitmap_csum_set(sb, Group, gd, *Bh);
        mark_buffer_dirty(*Bh);
        return Ext2SaveGroup(IrpContext, Vcb, Group) ? STATUS_SUCCESS : STATUS_UNEXPECTED_IO_ERROR;
    }

    *Bh = sb_getblk(sb, Where);
    if (*Bh == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (!buffer_uptodate(*Bh)) {
        int err = bh_submit_read(*Bh);
        if (err < 0) {
            return Ext2WinntError(err);
        }
    }
    return STATUS_SUCCESS;
}

/*
 * The descriptor's free count from the bitmap after it changed, its
 * checksums, and the volume total moved by the exact difference. Counted,
 * not adjusted by the change: a count that had drifted is right again.
 */
static NTSTATUS
Ext2RecountGroupBlocks(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN ULONG Group,
                       IN struct ext4_group_desc *gd, IN struct buffer_head *bh)
{
    struct super_block *sb = &Vcb->sb;
    RTL_BITMAP          Bitmap;
    LONGLONG            Before = ext4_free_blks_count(sb, gd);
    LONGLONG            After;

    RtlInitializeBitMap(&Bitmap, (PULONG)bh->b_data, Ext2GroupBlocks(Vcb, Group));
    After = RtlNumberOfClearBits(&Bitmap);
    ext4_free_blks_set(sb, gd, (__u32)After);
    ext4_block_bitmap_csum_set(sb, Group, gd, bh);
    if (!Ext2SaveGroup(IrpContext, Vcb, Group)) {
        return STATUS_UNEXPECTED_IO_ERROR;
    }
    Ext2AdjustVcbStat(IrpContext, Vcb, After - Before, 0);
    return STATUS_SUCCESS;
}

/*
 * Allocate in Group, its stripe's BlockLock held: STATUS_SUCCESS with
 * *Block / *Number, STATUS_DISK_FULL when the group has nothing to give
 * (the next one is tried), anything else stops the search.
 */
static NTSTATUS
Ext2AllocateInLockedGroup(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN ULONG Group,
                          IN ULONG Hint, IN ULONGLONG Limit, IN ULONG Flags, OUT PULONGLONG Block,
                          IN OUT PULONG Number, OUT struct buffer_head **Gb,
                          OUT struct buffer_head **Bh)
{
    struct super_block     *sb = &Vcb->sb;
    struct ext4_group_desc *gd;
    EXT2_BLOCK_VIEW         View;
    RTL_BITMAP              Bitmap;
    ULONGLONG               First = Ext2GroupFirst(Vcb, Group);
    ULONG                   GroupBits = Ext2GroupBlocks(Vcb, Group);
    ULONG                   Index;
    NTSTATUS                Status;

    gd = ext4_get_group_desc(sb, Group, Gb);
    if (gd == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (ext4_free_blks_count(sb, gd) == 0) {
        return STATUS_DISK_FULL;
    }
    Status = Ext2LoadBlockBitmap(IrpContext, Vcb, Group, gd, Bh);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    /* the caller's pointer width may leave only a prefix of the group */
    View.Disk = (const ULONG64 *)(*Bh)->b_data;
    View.Bits = (ULONG)min((ULONGLONG)GroupBits, Limit - First);
    Ext2FreedBlocksOf(Vcb, Group, View.Freed);
    if (Flags & EXT2_ALLOC_JOURNALED) {
        ULONG s;
        for (s = 0; s < EXT2_FREED_SLOTS; s++) {
            View.Freed[s] = NULL;   /* journaled contents may take them now */
        }
    }

    if (!Ext2FindClearRun(&View, Hint, Number, &Index)) {
        /* nothing free in the whole group although the descriptor said so,
           with nothing waiting for a commit either: believe the bitmap */
        if (View.Bits == GroupBits && View.Freed[0] == NULL && View.Freed[1] == NULL) {
            Status = Ext2RecountGroupBlocks(IrpContext, Vcb, Group, gd, *Bh);
            if (!NT_SUCCESS(Status)) {
                return Status;
            }
        }
        return STATUS_DISK_FULL;
    }

    /* a bitmap that marks metadata free is corrupt: nothing is handed out */
    ASSERT(Index < View.Bits && *Number <= View.Bits - Index);
    if (Ext2MetadataOverlaps(Vcb, First + Index, *Number)) {
        return STATUS_DISK_CORRUPT_ERROR;
    }

    RtlInitializeBitMap(&Bitmap, (PULONG)(*Bh)->b_data, GroupBits);
    RtlSetBits(&Bitmap, Index, *Number);
    mark_buffer_dirty(*Bh);
    Status = Ext2RecountGroupBlocks(IrpContext, Vcb, Group, gd, *Bh);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    /* A dirty range left from the blocks' previous life must not be
       written by the lazy writer into their new owner. */
    *Block = First + Index;
    if (!Ext2RemoveBlockExtent(Vcb, NULL, *Block, *Number)) {
        Ext2JournalAbandon(Vcb, STATUS_INSUFFICIENT_RESOURCES);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
Ext2AllocateInGroup(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN ULONG Group,
                    IN ULONG Hint, IN ULONGLONG Limit, IN ULONG Flags, OUT PULONGLONG Block,
                    IN OUT PULONG Number)
{
    PERESOURCE          Held = Ext2LockGroupBlocks(Vcb, Group);
    struct buffer_head *gb = NULL, *bh = NULL;
    NTSTATUS            Status;

    Status = Ext2AllocateInLockedGroup(IrpContext, Vcb, Group, Hint, Limit, Flags, Block, Number, &gb, &bh);
    if (bh != NULL) {
        fini_bh(&bh);
    }
    if (gb != NULL) {
        fini_bh(&gb);
    }
    Ext2UnlockGroup(Held);
    return Status;
}

/*
 * Check (Apply FALSE) or release (Apply TRUE) blocks [Index, Index + Count)
 * of Group: every one must be taken on disk. A group lazy mkfs never
 * initialised has nothing allocated: a release there is corrupt.
 */
static NTSTATUS
Ext2ReleaseInGroup(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN ULONG Group,
                   IN ULONG Index, IN ULONG Count, IN BOOLEAN Apply)
{
    struct super_block     *sb = &Vcb->sb;
    PERESOURCE              Held = Ext2LockGroupBlocks(Vcb, Group);
    struct buffer_head     *gb = NULL, *bh = NULL;
    struct ext4_group_desc *gd;
    RTL_BITMAP              Bitmap;
    NTSTATUS                Status;

    gd = ext4_get_group_desc(sb, Group, &gb);
    if (gd == NULL) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
    } else if (gd->bg_flags & cpu_to_le16(EXT4_BG_BLOCK_UNINIT)) {
        Status = STATUS_DISK_CORRUPT_ERROR;
    } else {
        Status = Ext2LoadBlockBitmap(IrpContext, Vcb, Group, gd, &bh);
    }

    if (NT_SUCCESS(Status)) {
        RtlInitializeBitMap(&Bitmap, (PULONG)bh->b_data, Ext2GroupBlocks(Vcb, Group));
        if (!RtlAreBitsSet(&Bitmap, Index, Count)) {
            Status = STATUS_DISK_CORRUPT_ERROR;     /* freed already: a double free */
        }
    }

    if (NT_SUCCESS(Status) && Apply) {
        RtlClearBits(&Bitmap, Index, Count);
        mark_buffer_dirty(bh);
        Status = Ext2RecountGroupBlocks(IrpContext, Vcb, Group, gd, bh);
        if (NT_SUCCESS(Status)) {
            /* filed in the journal: reusable once the release commits;
               without memory for that, at once, as before */
            if (!Ext2DeferFreedBlocks(Vcb, Group, Index, Count)) {
                DEBUG(DL_ERR, ("Ext2ReleaseInGroup: no memory to defer reuse of %u blocks\n", Count));
            }
            if (!Ext2RemoveBlockExtent(Vcb, NULL, Ext2GroupFirst(Vcb, Group) + Index, Count)) {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                Ext2JournalAbandon(Vcb, Status);
            }
        }
    }

    if (bh != NULL) {
        fini_bh(&bh);
    }
    if (gb != NULL) {
        fini_bh(&gb);
    }
    Ext2UnlockGroup(Held);
    return Status;
}

/* [Block, Block + Number) group by group: checked (Apply FALSE) or released */
static NTSTATUS
Ext2ReleaseRange(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN ULONGLONG Block,
                 IN ULONG Number, IN BOOLEAN Apply)
{
    while (Number != 0) {
        ULONG       Group = (ULONG)((Block - EXT2_FIRST_DATA_BLOCK) / BLOCKS_PER_GROUP);
        ULONG       Index = (ULONG)((Block - EXT2_FIRST_DATA_BLOCK) % BLOCKS_PER_GROUP);
        ULONG       Part = min(Number, Ext2GroupBlocks(Vcb, Group) - Index);
        NTSTATUS    Status = Ext2ReleaseInGroup(IrpContext, Vcb, Group, Index, Part, Apply);

        if (!NT_SUCCESS(Status)) {
            return Status;
        }
        Block += Part;
        Number -= Part;
    }
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- the interface */

NTSTATUS
Ext2NewBlock(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN ULONG                GroupHint,
    IN ULONGLONG            BlockHint,
    OUT PULONGLONG          Block,
    IN OUT PULONG           Number,
    IN ULONGLONG            BlockLimit,
    IN ULONG                Flags
)
{
    ULONGLONG   Limit = min(TOTAL_BLOCKS, BlockLimit);
    ULONG       SearchGroups, Group, Tried, Hint = 0;
    NTSTATUS    Status;

    *Block = 0;
    if (*Number == 0 || Limit <= EXT2_FIRST_DATA_BLOCK) {
        return STATUS_INVALID_PARAMETER;
    }
    /* The caller's on-disk pointer width bounds the search, not the volume. */
    SearchGroups = (ULONG)((Limit - EXT2_FIRST_DATA_BLOCK - 1) / BLOCKS_PER_GROUP + 1);

    Ext2JournalJoin(Vcb);       /* before the lock: see Ext2JournalJoin */
    if (IsVcbReadOnly(Vcb)) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }

    /* The goal is the block after a file's last extent. Outside the
       searchable volume (an extent ending at its last block) it says
       nothing: the search starts at the hint group instead of failing. */
    if (GroupHint >= SearchGroups) {
        GroupHint = SearchGroups - 1;
    }
    if (BlockHint >= EXT2_FIRST_DATA_BLOCK && BlockHint < Limit) {
        GroupHint = (ULONG)((BlockHint - EXT2_FIRST_DATA_BLOCK) / BLOCKS_PER_GROUP);
        Hint = (ULONG)((BlockHint - EXT2_FIRST_DATA_BLOCK) % BLOCKS_PER_GROUP);
    }

    /* every group once, from the goal's on, wrapping */
    for (Tried = 0, Group = GroupHint; Tried < SearchGroups; Tried++) {
        Status = Ext2AllocateInGroup(IrpContext, Vcb, Group, Hint, Limit, Flags, Block, Number);
        if (Status != STATUS_DISK_FULL) {
            if (NT_SUCCESS(Status)) {
                DEBUG(DL_INF, ("Ext2NewBlock: blocks %I64xh - %I64xh allocated\n",
                               *Block, *Block + *Number));
            }
            return Status;
        }
        Hint = 0;
        Group = (Group + 1 == SearchGroups) ? 0 : Group + 1;
    }

    /* what is left waits for a commit: ask for it now, not at the interval */
    if (Vcb->FreedPending > 0) {
        Ext2JournalCommit(Vcb, FALSE);
    }
    return STATUS_DISK_FULL;
}

NTSTATUS
Ext2FreeBlock(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN ULONGLONG            Block,
    IN ULONG                Number
)
{
    NTSTATUS Status;

    Ext2JournalJoin(Vcb);       /* before the lock: see Ext2JournalJoin */

    DEBUG(DL_INF, ("Ext2FreeBlock: blocks %I64xh - %I64xh to be freed\n",
                   Block, Block + Number));

    if (Number == 0 || Block < EXT2_FIRST_DATA_BLOCK || Block >= TOTAL_BLOCKS ||
        (ULONGLONG)Number > TOTAL_BLOCKS - Block) {
        return STATUS_INVALID_PARAMETER;
    }
    if (IsVcbReadOnly(Vcb)) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }

    /* 1. all of it taken, none of it metadata - checked before anything
          changes, so that a corrupt pointer frees nothing and revokes nothing */
    if (Ext2MetadataOverlaps(Vcb, Block, Number)) {
        return STATUS_DISK_CORRUPT_ERROR;
    }
    Status = Ext2ReleaseRange(IrpContext, Vcb, Block, Number, FALSE);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    /* 2. any logged copy of these blocks is revoked before they can be
          reused (invariant I3) */
    if (!Ext2JournalRevokeBlocks(Vcb, Block, Number)) {
        return STATUS_UNEXPECTED_IO_ERROR;
    }

    /* 3. released group by group. Each group is checked again under its
          lock; a failure now, after part of the range is gone, leaves the
          owner's view and the bitmaps apart: the volume stops writing. */
    Status = Ext2ReleaseRange(IrpContext, Vcb, Block, Number, TRUE);
    if (!NT_SUCCESS(Status)) {
        Ext2JournalAbandon(Vcb, Status);
    }
    return Status;
}

/*
 * The block bitmap of Group made real if lazy mkfs left it uninitialised:
 * the first inode of such a group does that (as Linux ext4_new_inode), so
 * that the flag is gone and the block on disk is the truth. Taken under the
 * group's BlockLock; the caller may hold its InodeLock (inode before block).
 */
NTSTATUS
Ext2MaterializeBlockBitmap(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN ULONG Group)
{
    PERESOURCE              Held = Ext2LockGroupBlocks(Vcb, Group);
    struct buffer_head     *gb = NULL, *bh = NULL;
    struct ext4_group_desc *gd = ext4_get_group_desc(&Vcb->sb, Group, &gb);
    NTSTATUS                Status = STATUS_SUCCESS;

    if (gd == NULL) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
    } else if (gd->bg_flags & cpu_to_le16(EXT4_BG_BLOCK_UNINIT)) {
        Status = Ext2LoadBlockBitmap(IrpContext, Vcb, Group, gd, &bh);
        if (NT_SUCCESS(Status)) {
            Status = Ext2RecountGroupBlocks(IrpContext, Vcb, Group, gd, bh);
        }
    }
    if (bh != NULL) {
        fini_bh(&bh);
    }
    if (gb != NULL) {
        fini_bh(&gb);
    }
    Ext2UnlockGroup(Held);
    return Status;
}

/* ---------------------------------------------------------------- for an inode */

/* in memory i_blocks counts 512-byte units, whatever the device's sector
   size; EXT4_HUGE_FILE_FL scaling happens only on disk (InodeStorage.c) */
static __inline ULONGLONG
Ext2BlocksToSectors(IN PEXT2_VCB Vcb, IN ULONG Count)
{
    return (ULONGLONG)Count << (BLOCK_BITS - EXT4_SECTOR_SHIFT);
}

NTSTATUS
Ext2AllocateInodeBlocks(IN PEXT2_IRP_CONTEXT IrpContext, IN struct inode *Inode,
                        IN ULONGLONG Goal, IN ULONG Flags, OUT PULONGLONG Block,
                        IN OUT PULONG Count)
{
    PEXT2_VCB   Vcb = Inode->i_sb->s_priv;
    NTSTATUS    Status;

    Status = Ext2NewBlock(IrpContext, Vcb, 0, Goal, Block, Count, EXT2_EXTENT_BLOCK_LIMIT, Flags);
    if (NT_SUCCESS(Status)) {
        Inode->i_blocks += Ext2BlocksToSectors(Vcb, *Count);
    }
    return Status;
}

/*
 * Blocks of Inode given back. A release that fails leaves the owner's
 * pointer removed and the blocks still taken (or worse): the change that
 * removed the pointer must not commit, so the journal stops.
 */
NTSTATUS
Ext2ReleaseInodeBlocks(IN PEXT2_IRP_CONTEXT IrpContext, IN struct inode *Inode,
                       IN ULONGLONG Block, IN ULONG Count)
{
    PEXT2_VCB   Vcb = Inode->i_sb->s_priv;
    NTSTATUS    Status;

    Status = Ext2FreeBlock(IrpContext, Vcb, Block, Count);
    if (!NT_SUCCESS(Status)) {
        Ext2JournalAbandon(Vcb, Status);
        return Status;
    }
    Inode->i_blocks -= Ext2BlocksToSectors(Vcb, Count);
    return STATUS_SUCCESS;
}
