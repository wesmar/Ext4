/**
 * MetadataMap.c - every block of metadata of the volume, as sorted ranges.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The allocator must never hand out, and a free must never return, a block
 * that holds metadata: the superblock and descriptor table copies at the
 * start of a group, the block and inode bitmaps and the inode tables. A
 * group's own descriptor tells only where its own bitmaps and table are;
 * with flex_bg those of sixteen (or more) groups are packed into the first
 * group of the flex group, where they are invisible to a check that looks
 * at one descriptor. So the whole volume's metadata is collected once at
 * mount - it does not move while the volume is mounted - into disjoint
 * ranges sorted by their start. One question, "does [Block, Block + Count)
 * touch metadata?", is then a binary search: O(log R) for R ranges.
 *
 * R stays small: flex_bg packs each kind of metadata of a flex group into
 * one run, so a volume has about four ranges per flex group (with the
 * reserved prefixes of the groups that carry superblock copies); without
 * flex_bg the bitmaps and table of a group lie together, one range per
 * group. Every kind is collected in group order, merging each block with
 * the run it extends, then all runs are sorted and merged once.
 */

#include "ext4fs.h"
#include "linux\ext4.h"

#define EXT4_METAMAP_TAG        'MM4E'
#define EXT4_METAMAP_FIRST_ROOM 256         /* ranges a collector starts with, then doubles */

typedef struct _EXT2_META_RANGE {
    ULONGLONG   Start;                      /* first block */
    ULONGLONG   End;                        /* one past the last block */
} EXT2_META_RANGE, *PEXT2_META_RANGE;

typedef struct _EXT2_META_MAP {
    ULONG           Count;
    EXT2_META_RANGE Range[ANYSIZE_ARRAY];   /* sorted, disjoint, not touching */
} EXT2_META_MAP, *PEXT2_META_MAP;

/* the ranges of one kind of metadata, in the order they are found */
typedef struct _EXT2_META_RUNS {
    PEXT2_META_RANGE    Range;
    ULONG               Count;
    ULONG               Room;
} EXT2_META_RUNS, *PEXT2_META_RUNS;

/* [Start, Start + Count) appended, or merged into the last run it touches */
static BOOLEAN
MetaAppend(IN OUT PEXT2_META_RUNS Runs, IN ULONGLONG Start, IN ULONGLONG Count)
{
    PEXT2_META_RANGE Last = Runs->Count ? &Runs->Range[Runs->Count - 1] : NULL;

    if (Count == 0) {
        return TRUE;
    }
    if (Last != NULL && Last->Start <= Start && Start <= Last->End) {
        Last->End = max(Last->End, Start + Count);
        return TRUE;
    }
    if (Runs->Count == Runs->Room) {
        ULONG               Room = Runs->Room ? Runs->Room * 2 : EXT4_METAMAP_FIRST_ROOM;
        PEXT2_META_RANGE    New;

        if (Room <= Runs->Room) {
            return FALSE;                   /* the count would wrap */
        }
        New = Ext2AllocatePool(PagedPool, (SIZE_T)Room * sizeof(EXT2_META_RANGE), EXT4_METAMAP_TAG);
        if (New == NULL) {
            return FALSE;
        }
        if (Runs->Range != NULL) {
            RtlCopyMemory(New, Runs->Range, (SIZE_T)Runs->Count * sizeof(EXT2_META_RANGE));
            Ext2FreePool(Runs->Range, EXT4_METAMAP_TAG);
        }
        Runs->Range = New;
        Runs->Room = Room;
    }
    Runs->Range[Runs->Count].Start = Start;
    Runs->Range[Runs->Count].End = Start + Count;
    Runs->Count++;
    return TRUE;
}

static VOID
MetaFreeRuns(IN OUT PEXT2_META_RUNS Runs)
{
    if (Runs->Range != NULL) {
        Ext2FreePool(Runs->Range, EXT4_METAMAP_TAG);
    }
    RtlZeroMemory(Runs, sizeof(*Runs));
}

/* in place, by Start: heapsort - no recursion, no scratch memory, O(n log n) */
static VOID
MetaSiftDown(IN OUT PEXT2_META_RANGE Range, IN ULONG Root, IN ULONG Count)
{
    for (;;) {
        ULONG           Child = 2 * Root + 1;
        EXT2_META_RANGE Swap;

        if (Child >= Count) {
            return;
        }
        if (Child + 1 < Count && Range[Child + 1].Start > Range[Child].Start) {
            Child++;
        }
        if (Range[Root].Start >= Range[Child].Start) {
            return;
        }
        Swap = Range[Root];
        Range[Root] = Range[Child];
        Range[Child] = Swap;
        Root = Child;
    }
}

static VOID
MetaSort(IN OUT PEXT2_META_RANGE Range, IN ULONG Count)
{
    ULONG i;

    for (i = Count / 2; i-- > 0; ) {
        MetaSiftDown(Range, i, Count);
    }
    for (i = Count; i-- > 1; ) {
        EXT2_META_RANGE Swap = Range[0];
        Range[0] = Range[i];
        Range[i] = Swap;
        MetaSiftDown(Range, 0, i);
    }
}

/* the four kinds collected, group by group */
typedef enum { META_PREFIX, META_BLOCK_BITMAP, META_INODE_BITMAP, META_INODE_TABLE, META_KINDS } META_KIND;

static NTSTATUS
MetaCollect(IN PEXT2_VCB Vcb, IN OUT EXT2_META_RUNS Runs[META_KINDS])
{
    struct super_block *sb = &Vcb->sb;
    ULONG               Group;

    /* the boot block before the first group (block 0 of a 1 KiB volume) */
    if (!MetaAppend(&Runs[META_PREFIX], 0, EXT2_FIRST_DATA_BLOCK)) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    for (Group = 0; Group < Vcb->sbi.s_groups_count; Group++) {
        struct buffer_head     *gb = NULL;
        struct ext4_group_desc *gd = ext4_get_group_desc(sb, Group, &gb);
        ULONGLONG               First = EXT2_FIRST_DATA_BLOCK + (ULONGLONG)Group * BLOCKS_PER_GROUP;
        BOOLEAN                 Ok;

        if (gd == NULL) {
            return STATUS_DISK_CORRUPT_ERROR;
        }
        Ok = MetaAppend(&Runs[META_PREFIX], First, Ext2GroupReservedBlocks(sb, Group)) &&
             MetaAppend(&Runs[META_BLOCK_BITMAP], ext4_block_bitmap(sb, gd), 1) &&
             MetaAppend(&Runs[META_INODE_BITMAP], ext4_inode_bitmap(sb, gd), 1) &&
             MetaAppend(&Runs[META_INODE_TABLE], ext4_inode_table(sb, gd), Vcb->sbi.s_itb_per_group);
        fini_bh(&gb);
        if (!Ok) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS
Ext2BuildMetadataMap(IN PEXT2_VCB Vcb)
{
    EXT2_META_RUNS  Runs[META_KINDS];
    EXT2_META_RUNS  All;
    PEXT2_META_MAP  Map = NULL;
    ULONG           Kind, i, Out;
    NTSTATUS        Status;

    RtlZeroMemory(Runs, sizeof(Runs));
    RtlZeroMemory(&All, sizeof(All));

    Status = MetaCollect(Vcb, Runs);

    /* all kinds in one array, sorted by start, merged where they touch */
    for (Kind = 0; Kind < META_KINDS && NT_SUCCESS(Status); Kind++) {
        for (i = 0; i < Runs[Kind].Count; i++) {
            if (!MetaAppend(&All, Runs[Kind].Range[i].Start,
                            Runs[Kind].Range[i].End - Runs[Kind].Range[i].Start)) {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
        }
    }
    if (NT_SUCCESS(Status)) {
        MetaSort(All.Range, All.Count);
        for (i = 0, Out = 0; i < All.Count; i++) {
            if (Out > 0 && All.Range[i].Start <= All.Range[Out - 1].End) {
                All.Range[Out - 1].End = max(All.Range[Out - 1].End, All.Range[i].End);
            } else {
                All.Range[Out++] = All.Range[i];
            }
        }
        Map = Ext2AllocatePool(PagedPool,
                               FIELD_OFFSET(EXT2_META_MAP, Range) + (SIZE_T)max(Out, 1) * sizeof(EXT2_META_RANGE),
                               EXT4_METAMAP_TAG);
        if (Map == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
        } else {
            Map->Count = Out;
            RtlCopyMemory(Map->Range, All.Range, (SIZE_T)Out * sizeof(EXT2_META_RANGE));
            Vcb->MetaMap = Map;
        }
    }

    for (Kind = 0; Kind < META_KINDS; Kind++) {
        MetaFreeRuns(&Runs[Kind]);
    }
    MetaFreeRuns(&All);
    return Status;
}

VOID
Ext2FreeMetadataMap(IN PEXT2_VCB Vcb)
{
    if (Vcb->MetaMap != NULL) {
        Ext2FreePool(Vcb->MetaMap, EXT4_METAMAP_TAG);
        Vcb->MetaMap = NULL;
    }
}

/*
 * Does [Block, Block + Count) touch metadata? The ranges are disjoint and
 * sorted, so their ends are sorted too: the first range ending after Block
 * is the only one that can begin inside the run. Every mount builds the
 * map with the group descriptors and frees it with them (Ext2PutGroup), so
 * the allocator never runs without one.
 */
BOOLEAN
Ext2MetadataOverlaps(IN PEXT2_VCB Vcb, IN ULONGLONG Block, IN ULONGLONG Count)
{
    PEXT2_META_MAP      Map = Vcb->MetaMap;
    PEXT2_META_RANGE    Range;
    ULONG               Low = 0, High;

    if (Map == NULL || Count == 0) {
        return FALSE;
    }
    High = Map->Count;
    while (Low < High) {
        ULONG Mid = Low + (High - Low) / 2;
        if (Map->Range[Mid].End > Block) {
            High = Mid;
        } else {
            Low = Mid + 1;
        }
    }
    if (Low == Map->Count) {
        return FALSE;
    }
    Range = &Map->Range[Low];
    return Range->Start <= Block || Range->Start - Block < Count;
}
