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
 *
 * Where blocks go is BlockPlacement.c's: this file gives it each group's
 * bound - the longest free run of its bitmap, kept with every change to it
 * (GroupLongestRun), or before the bitmap is first read the descriptor's
 * free count - and the taking of blocks in a group, around the windows of
 * other files (BlockWindows.c, one table for the volume under WindowLock,
 * taken inside the group's BlockLock). A window is added only by a take in
 * its group, so the windows a take sees cannot grow while it searches; one
 * dropped meanwhile only kept a few blocks out of that search.
 */

#include "ext4fs.h"
#include "BlockSearch.h"
#include "BlockPlacement.h"

#define EXT2_BLOCK_RUNS_TAG     'RB4E'
#define EXT2_RUN_UNSEEN         ((LONG)-1)

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

/* the whole group as its bitmap on disk has it, freed blocks counted free */
static __inline VOID
Ext2DiskView(IN PEXT2_VCB Vcb, IN ULONG Group, IN struct buffer_head *Bh, OUT PEXT2_BLOCK_VIEW View)
{
    RtlZeroMemory(View, sizeof(*View));
    View->Disk = (const ULONG64 *)Bh->b_data;
    View->Bits = Ext2GroupBlocks(Vcb, Group);
}

/* ---------------------------------------------------------------- the runs */

/* every group unseen, no window: built with the groups at mount, freed with them */
NTSTATUS
Ext2InitializeBlockRuns(IN PEXT2_VCB Vcb)
{
    ULONG g;

    RtlZeroMemory(&Vcb->BlockWindows, sizeof(Vcb->BlockWindows));
    ExInitializePushLock(&Vcb->WindowLock);
    Vcb->GroupLongestRun = Ext2AllocatePool(PagedPool, (SIZE_T)Vcb->sbi.s_groups_count * sizeof(LONG),
                                            EXT2_BLOCK_RUNS_TAG);
    if (Vcb->GroupLongestRun == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    for (g = 0; g < Vcb->sbi.s_groups_count; g++) {
        Vcb->GroupLongestRun[g] = EXT2_RUN_UNSEEN;
    }
    return STATUS_SUCCESS;
}

VOID
Ext2FreeBlockRuns(IN PEXT2_VCB Vcb)
{
    if (Vcb->GroupLongestRun != NULL) {
        Ext2FreePool((PVOID)Vcb->GroupLongestRun, EXT2_BLOCK_RUNS_TAG);
        Vcb->GroupLongestRun = NULL;
    }
}

/* Group's bitmap as it now is, its BlockLock held */
static __inline VOID
Ext2NoteLongestRun(IN PEXT2_VCB Vcb, IN ULONG Group, IN struct buffer_head *Bh)
{
    EXT2_BLOCK_VIEW View;

    Ext2DiskView(Vcb, Group, Bh, &View);
    WriteNoFence(&Vcb->GroupLongestRun[Group], (LONG)Ext2ViewLongestRun(&View));
}

/* ---------------------------------------------------------------- the windows */

static __inline VOID
Ext2LockWindows(IN PEXT2_VCB Vcb)
{
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&Vcb->WindowLock);
}

static __inline VOID
Ext2UnlockWindows(IN PEXT2_VCB Vcb)
{
    ExReleasePushLockExclusive(&Vcb->WindowLock);
    KeLeaveCriticalRegion();
}

VOID
Ext2DropBlockWindow(IN PEXT2_VCB Vcb, IN ULONG Owner)
{
    EXT2_BLOCK_CUT None = { 0, 0 };

    Ext2LockWindows(Vcb);
    Ext2WindowSet(&Vcb->BlockWindows, Owner, 0, None);
    Ext2UnlockWindows(Vcb);
}

/* ---------------------------------------------------------------- one group */

/* [Block, Block + Count) lies in the group the bitmap maps, all of it set */
static BOOLEAN
Ext2BlocksMarked(IN PRTL_BITMAP Bitmap, IN ULONGLONG First, IN ULONGLONG Block, IN ULONG Count)
{
    return Block >= First && Block - First + Count <= Bitmap->SizeOfBitMap &&
           RtlAreBitsSet(Bitmap, (ULONG)(Block - First), Count);
}

/*
 * A block bitmap from disk, checked once (Linux: ext4_validate_block_bitmap):
 * its checksum, and - without flex_bg, where a group's bitmaps and inode
 * table lie inside it - that it marks them in use. One that fails takes
 * its group out of use for this mount: nothing is allocated there, nothing
 * given back, it is reported once.
 */
static NTSTATUS
Ext2CheckBlockBitmap(IN PEXT2_VCB Vcb, IN ULONG Group, IN struct ext4_group_desc *gd,
                     IN struct buffer_head *bh)
{
    struct super_block *sb = &Vcb->sb;
    RTL_BITMAP          Bitmap;
    ULONGLONG           First = Ext2GroupFirst(Vcb, Group);
    int                 Err = 0;

    if (buffer_verified(bh)) {
        return STATUS_SUCCESS;
    }
    if (!ext4_block_bitmap_csum_verify(sb, Group, gd, bh)) {
        Err = -EFSBADCRC;
    } else if (!EXT3_HAS_INCOMPAT_FEATURE(sb, EXT4_FEATURE_INCOMPAT_FLEX_BG)) {
        RtlInitializeBitMap(&Bitmap, (PULONG)bh->b_data, Ext2GroupBlocks(Vcb, Group));
        if (!Ext2BlocksMarked(&Bitmap, First, ext4_block_bitmap(sb, gd), 1) ||
            !Ext2BlocksMarked(&Bitmap, First, ext4_inode_bitmap(sb, gd), 1) ||
            !Ext2BlocksMarked(&Bitmap, First, ext4_inode_table(sb, gd), Vcb->sbi.s_itb_per_group)) {
            Err = -EFSCORRUPTED;
        }
    }
    if (Err != 0) {
        if (Ext2MarkGroupCorrupt(Vcb, Group, EXT2_GROUP_BLOCK_BITMAP)) {
            Ext2ReportCorruption(Vcb, 0, bh->b_blocknr, Err);
        }
        return STATUS_DISK_CORRUPT_ERROR;
    }
    set_buffer_verified(bh);
    return STATUS_SUCCESS;
}

/* the block bitmap of Group; one lazy mkfs never wrote is built and published first */
static NTSTATUS
Ext2LoadBlockBitmap(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN ULONG Group,
                    IN struct ext4_group_desc *gd, OUT struct buffer_head **Bh)
{
    struct super_block *sb = &Vcb->sb;
    ext4_fsblk_t        Where = ext4_block_bitmap(sb, gd);

    if (Ext2GroupCorrupt(Vcb, Group, EXT2_GROUP_BLOCK_BITMAP)) {
        return STATUS_DISK_CORRUPT_ERROR;
    }

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
        set_buffer_verified(*Bh);
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
    return Ext2CheckBlockBitmap(Vcb, Group, gd, *Bh);
}

/*
 * The descriptor's free count from the bitmap after it changed, its
 * checksums, the group's longest run, and the volume total moved by the
 * exact difference. Counted, not adjusted by the change: a count that had
 * drifted is right again.
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
    Ext2NoteLongestRun(Vcb, Group, bh);
    ext4_free_blks_set(sb, gd, (__u32)After);
    ext4_block_bitmap_csum_set(sb, Group, gd, bh);
    if (!Ext2SaveGroup(IrpContext, Vcb, Group)) {
        return STATUS_UNEXPECTED_IO_ERROR;
    }
    Ext2AdjustVcbStat(IrpContext, Vcb, After - Before, 0);
    return STATUS_SUCCESS;
}

/* one Ext2NewBlock: what its placement's calls work with */
typedef struct _EXT2_PLACE_STATE {
    PEXT2_IRP_CONTEXT   IrpContext;
    PEXT2_VCB           Vcb;
    ULONG               Owner;      /* the inode the blocks are for: its window */
    ULONGLONG           Limit;      /* the reach of the owner's block pointers */
    ULONG               Flags;
    ULONGLONG           Block;      /* the first block taken */
    NTSTATUS            Status;     /* why a Take ended the search */
} EXT2_PLACE_STATE, *PEXT2_PLACE_STATE;

/*
 * Take blocks in a group, its stripe's BlockLock held, as EXT2_PLACE_IO.Take
 * asks: STATUS_SUCCESS with *Index / *Count, STATUS_DISK_FULL when the group
 * has nothing to give or is out of use (the search goes on), anything else
 * ends the search.
 */
static NTSTATUS
Ext2TakeInLockedGroup(IN PEXT2_PLACE_STATE P, IN const EXT2_PLACE_TAKE *Ask, OUT PULONG Index,
                      OUT PULONG Count, OUT struct buffer_head **Gb, OUT struct buffer_head **Bh)
{
    PEXT2_VCB               Vcb = P->Vcb;
    struct super_block     *sb = &Vcb->sb;
    struct ext4_group_desc *gd;
    EXT2_BLOCK_VIEW         View;
    EXT2_BLOCK_CUT          Cuts[EXT2_WINDOW_SLOTS], After;
    RTL_BITMAP              Bitmap;
    ULONG                   Group = Ask->Group;
    ULONGLONG               First = Ext2GroupFirst(Vcb, Group);
    ULONG                   GroupBits = Ext2GroupBlocks(Vcb, Group);
    ULONG                   CutCount, At = 0, Length = 0;
    NTSTATUS                Status;

    /* a pass that may go into other files' windows has nothing to find
       where there is none: the pass before saw the same */
    Ext2LockWindows(Vcb);
    CutCount = Ext2WindowCuts(&Vcb->BlockWindows, Group, P->Owner, Cuts);
    Ext2UnlockWindows(Vcb);
    if (Ask->Any && CutCount == 0) {
        return STATUS_DISK_FULL;
    }

    gd = ext4_get_group_desc(sb, Group, Gb);
    if (gd == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (Ext2GroupCorrupt(Vcb, Group, EXT2_GROUP_BLOCK_BITMAP)) {
        return STATUS_DISK_FULL;
    }
    if (ext4_free_blks_count(sb, gd) == 0) {
        WriteNoFence(&Vcb->GroupLongestRun[Group], 0);     /* known without the bitmap */
        return STATUS_DISK_FULL;
    }
    Status = Ext2LoadBlockBitmap(P->IrpContext, Vcb, Group, gd, Bh);
    if (!NT_SUCCESS(Status)) {
        /* a bitmap found corrupt just now: reported, and skipped like a full group */
        return Status == STATUS_DISK_CORRUPT_ERROR ? STATUS_DISK_FULL : Status;
    }
    if (ReadNoFence(&Vcb->GroupLongestRun[Group]) == EXT2_RUN_UNSEEN) {
        Ext2NoteLongestRun(Vcb, Group, *Bh);
        /* nothing free in the whole group although the descriptor said so:
           believe the bitmap */
        if (ReadNoFence(&Vcb->GroupLongestRun[Group]) == 0) {
            Status = Ext2RecountGroupBlocks(P->IrpContext, Vcb, Group, gd, *Bh);
            return NT_SUCCESS(Status) ? STATUS_DISK_FULL : Status;
        }
    }

    /* the caller's pointer width may leave only a prefix of the group */
    View.Disk = (const ULONG64 *)(*Bh)->b_data;
    View.Bits = (ULONG)min((ULONGLONG)GroupBits, P->Limit - First);
    View.Cuts = Ask->Any ? NULL : Cuts;
    View.CutCount = Ask->Any ? 0 : CutCount;
    Ext2FreedBlocksOf(Vcb, Group, View.Freed);
    if (P->Flags & EXT2_ALLOC_JOURNALED) {
        ULONG s;
        for (s = 0; s < EXT2_FREED_SLOTS; s++) {
            View.Freed[s] = NULL;   /* journaled contents may take them now */
        }
    }

    if (Ask->Want == 0) {
        At = Ask->Hint;
        Length = Ext2ViewRunAt(&View, At);
    } else if (!Ext2ViewBestRun(&View, Ask->Hint, Ask->Want, &At, &Length)) {
        Length = 0;
    }
    if (Length == 0) {
        return STATUS_DISK_FULL;
    }
    *Count = min(Ask->Need, Length);
    *Index = Ask->Any ? At + Length - *Count : At;

    /* a bitmap that marks metadata free is corrupt: nothing is handed out
       from it, and the search goes on in the other groups */
    ASSERT(*Index < View.Bits && *Count <= View.Bits - *Index);
    if (Ext2MetadataOverlaps(Vcb, First + *Index, *Count)) {
        if (Ext2MarkGroupCorrupt(Vcb, Group, EXT2_GROUP_BLOCK_BITMAP)) {
            Ext2ReportCorruption(Vcb, 0, First + *Index, -EFSCORRUPTED);
        }
        return STATUS_DISK_FULL;
    }

    RtlInitializeBitMap(&Bitmap, (PULONG)(*Bh)->b_data, GroupBits);
    RtlSetBits(&Bitmap, *Index, *Count);
    mark_buffer_dirty(*Bh);
    Status = Ext2RecountGroupBlocks(P->IrpContext, Vcb, Group, gd, *Bh);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    /* windows the blocks fell in shrink; an owner growing at its end
       keeps the free run after them */
    After.Start = *Index + *Count;
    After.Count = Ask->Room != 0 ? min(Ext2ViewRunAt(&View, After.Start), Ask->Room) : 0;
    Ext2LockWindows(Vcb);
    Ext2WindowTaken(&Vcb->BlockWindows, Group, *Index, *Count);
    if (Ask->Room != 0) {
        Ext2WindowSet(&Vcb->BlockWindows, P->Owner, Group, After);
    }
    Ext2UnlockWindows(Vcb);

    /* A dirty range left from the blocks' previous life must not be
       written by the lazy writer into their new owner. */
    P->Block = First + *Index;
    if (!Ext2RemoveBlockExtent(Vcb, NULL, P->Block, *Count)) {
        Ext2JournalAbandon(Vcb, STATUS_INSUFFICIENT_RESOURCES);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    return STATUS_SUCCESS;
}

/* EXT2_PLACE_IO.Bound: no free run of Group is longer */
static unsigned int
Ext2GroupBound(void *Context, unsigned int Group)
{
    PEXT2_VCB               Vcb = ((PEXT2_PLACE_STATE)Context)->Vcb;
    LONG                    Longest = ReadNoFence(&Vcb->GroupLongestRun[Group]);
    struct ext4_group_desc *gd;

    if (Ext2GroupCorrupt(Vcb, Group, EXT2_GROUP_BLOCK_BITMAP)) {
        return 0;
    }
    if (Longest != EXT2_RUN_UNSEEN) {
        return (unsigned int)Longest;
    }
    gd = ext4_get_group_desc(&Vcb->sb, Group, NULL);
    return gd != NULL ? (unsigned int)ext4_free_blks_count(&Vcb->sb, gd) : 0;
}

/* EXT2_PLACE_IO.Take */
static int
Ext2GroupTake(void *Context, const EXT2_PLACE_TAKE *Ask, unsigned int *Index, unsigned int *Count)
{
    PEXT2_PLACE_STATE   P = (PEXT2_PLACE_STATE)Context;
    PERESOURCE          Held = Ext2LockGroupBlocks(P->Vcb, Ask->Group);
    struct buffer_head *gb = NULL, *bh = NULL;
    NTSTATUS            Status;

    Status = Ext2TakeInLockedGroup(P, Ask, (PULONG)Index, (PULONG)Count, &gb, &bh);
    if (bh != NULL) {
        fini_bh(&bh);
    }
    if (gb != NULL) {
        fini_bh(&gb);
    }
    Ext2UnlockGroup(Held);
    if (Status == STATUS_DISK_FULL) {
        return EXT2_PLACE_NONE;
    }
    if (!NT_SUCCESS(Status)) {
        P->Status = Status;
        return -1;
    }
    return 0;
}

/*
 * Check (Apply FALSE) or release (Apply TRUE) blocks [Index, Index + Count)
 * of Group: every one must be taken on disk. A group lazy mkfs never
 * initialised has nothing allocated: a release there is corrupt. What the
 * check finds is reported; the release finds it only when the bitmap
 * changed in between.
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
        Status = STATUS_DISK_CORRUPT_ERROR;         /* the owner's pointer is wrong */
        if (!Apply) {
            Ext2ReportCorruption(Vcb, 0, Ext2GroupFirst(Vcb, Group) + Index, -EFSCORRUPTED);
        }
    } else {
        Status = Ext2LoadBlockBitmap(IrpContext, Vcb, Group, gd, &bh);
    }

    if (NT_SUCCESS(Status)) {
        RtlInitializeBitMap(&Bitmap, (PULONG)bh->b_data, Ext2GroupBlocks(Vcb, Group));
        if (!RtlAreBitsSet(&Bitmap, Index, Count)) {
            /* freed already: a double free, or a bitmap that lost them
               (Linux takes the bitmap for corrupt, as here) */
            Status = STATUS_DISK_CORRUPT_ERROR;
            if (!Apply && Ext2MarkGroupCorrupt(Vcb, Group, EXT2_GROUP_BLOCK_BITMAP)) {
                Ext2ReportCorruption(Vcb, 0, Ext2GroupFirst(Vcb, Group) + Index, -EFSCORRUPTED);
            }
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

/*
 * Up to *Number blocks for inode Owner, placed by Ext2PlaceBlocks: BlockHint
 * is where they belong (with EXT2_ALLOC_AT_GOAL their place in the owner's
 * layout), Room how much the owner may grow after them - kept as its window
 * when it is more than nothing; without a hint the search starts at
 * GroupHint. *Block and *Number the run taken, or STATUS_DISK_FULL.
 */
NTSTATUS
Ext2NewBlock(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN ULONG                Owner,
    IN ULONG                GroupHint,
    IN ULONGLONG            BlockHint,
    IN ULONG                Room,
    OUT PULONGLONG          Block,
    IN OUT PULONG           Number,
    IN ULONGLONG            BlockLimit,
    IN ULONG                Flags
)
{
    EXT2_PLACE_STATE    State;
    EXT2_PLACE_IO       Io;
    EXT2_PLACE          Request;
    unsigned int        Group, Index, Count;
    int                 Err;

    *Block = 0;
    RtlZeroMemory(&State, sizeof(State));
    State.Limit = min(TOTAL_BLOCKS, BlockLimit);
    if (*Number == 0 || State.Limit <= EXT2_FIRST_DATA_BLOCK) {
        return STATUS_INVALID_PARAMETER;
    }
    State.IrpContext = IrpContext;
    State.Vcb = Vcb;
    State.Owner = Owner;
    State.Flags = Flags;

    /* The caller's on-disk pointer width bounds the search, not the volume. */
    Io.Context = &State;
    Io.Groups = (ULONG)((State.Limit - EXT2_FIRST_DATA_BLOCK - 1) / BLOCKS_PER_GROUP + 1);
    Io.GroupBlocks = BLOCKS_PER_GROUP;
    Io.Bound = Ext2GroupBound;
    Io.Take = Ext2GroupTake;

    Ext2JournalJoin(Vcb);       /* before the lock: see Ext2JournalJoin */
    if (IsVcbReadOnly(Vcb)) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }

    /* A hint outside the searchable volume (an extent ending at its last
       block) says nothing: the search starts at the hint group instead. */
    Request.Group = min(GroupHint, Io.Groups - 1);
    Request.Hint = 0;
    Request.Need = *Number;
    Request.Room = Room;
    Request.AtGoal = FALSE;
    if (BlockHint >= EXT2_FIRST_DATA_BLOCK && BlockHint < State.Limit) {
        Request.Group = (ULONG)((BlockHint - EXT2_FIRST_DATA_BLOCK) / BLOCKS_PER_GROUP);
        Request.Hint = (ULONG)((BlockHint - EXT2_FIRST_DATA_BLOCK) % BLOCKS_PER_GROUP);
        Request.AtGoal = (Flags & EXT2_ALLOC_AT_GOAL) != 0;
    }

    Err = Ext2PlaceBlocks(&Io, &Request, &Group, &Index, &Count);
    if (Err == 0) {
        *Block = State.Block;
        *Number = Count;
        DEBUG(DL_INF, ("Ext2NewBlock: blocks %I64xh - %I64xh allocated\n", *Block, *Block + *Number));
        return STATUS_SUCCESS;
    }
    if (Err != EXT2_PLACE_NONE) {
        return State.Status;
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
        Ext2ReportCorruption(Vcb, 0, Block, -EFSCORRUPTED);
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
                        IN ULONGLONG Goal, IN ULONG Room, IN ULONG Flags, OUT PULONGLONG Block,
                        IN OUT PULONG Count)
{
    PEXT2_VCB   Vcb = Inode->i_sb->s_priv;
    NTSTATUS    Status;

    Status = Ext2NewBlock(IrpContext, Vcb, Inode->i_ino, 0, Goal, Room, Block, Count, EXT2_EXTENT_BLOCK_LIMIT, Flags);
    if (NT_SUCCESS(Status)) {
        Inode->i_blocks += Ext2BlocksToSectors(Vcb, *Count);
    }
    return Status;
}

/*
 * Blocks of Inode given back. Ones found corrupt before anything changed
 * (free already, metadata, a group out of use) are not the inode's to
 * give back: reported, and let go of all the same, as Linux does - a leak
 * at worst, which e2fsck reclaims. A release that fails otherwise leaves
 * the owner's pointer removed and the blocks still taken: the change that
 * removed the pointer must not commit, so the journal stops (a release
 * that failed half way has stopped it already).
 */
NTSTATUS
Ext2ReleaseInodeBlocks(IN PEXT2_IRP_CONTEXT IrpContext, IN struct inode *Inode,
                       IN ULONGLONG Block, IN ULONG Count)
{
    PEXT2_VCB   Vcb = Inode->i_sb->s_priv;
    ULONGLONG   Sectors = Ext2BlocksToSectors(Vcb, Count);
    NTSTATUS    Status;

    Status = Ext2FreeBlock(IrpContext, Vcb, Block, Count);
    if (!NT_SUCCESS(Status) && Status != STATUS_DISK_CORRUPT_ERROR) {
        Ext2JournalAbandon(Vcb, Status);
        return Status;
    }
    Inode->i_blocks -= min(Inode->i_blocks, Sectors);
    return STATUS_SUCCESS;
}
