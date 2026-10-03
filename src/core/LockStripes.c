/**
 * LockStripes.c - lock striping: how many locks a shared structure gets, and the locks.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * A structure every thread of a volume uses - the name cache, the block
 * groups - is not guarded by one lock but by a set of them, each covering
 * the items whose index falls on it (index & mask). How many:
 *
 * The CPU count sets a sizing heuristic, not a contention guarantee:
 * threads may hold locks while waiting for I/O, and hot items need not be
 * distributed uniformly. With T concurrent holders and independent,
 * uniform choices among S stripes, the union bound is (T - 1) / S, capped
 * at one. Actual contention must be measured for the workload.
 *
 * EXT4_STRIPES_PER_CPU sets the CPU-scaled budget. A power-of-two count
 * lets the index select a stripe by a mask. The item count, rounded up to
 * a power of two, limits that budget; some stripes may remain unused.
 *
 * Each stripe is alone on its cache lines: stripes are taken by different
 * processors at once, and two sharing a line would bounce it between them
 * as a single lock does.
 */

#include "ext4fs.h"

ULONG
Ext2RoundUpPow2(IN ULONG Value)
{
    ULONG Pow2 = 1;

    while (Pow2 < Value && Pow2 < EXT4_POW2_MAX) {
        Pow2 <<= 1;
    }
    return Pow2;
}

/* stripes for Items items (Items >= 1) on this machine */
ULONG
Ext2StripeCount(IN ULONG Items)
{
    ULONG Processors = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    ULONG Stripes = Ext2RoundUpPow2(Processors * EXT4_STRIPES_PER_CPU);

    return min(Stripes, Ext2RoundUpPow2(max(Items, 1)));
}

/*
 * A set of Count stripes (a power of two), initialized. The pool aligns
 * to less than a cache line: the set is aligned by hand inside a slightly
 * larger block, kept in *Pool for Ext2FreeStripes.
 */
PEXT2_LOCK_STRIPE
Ext2AllocateStripes(IN ULONG Count, OUT PVOID *Pool)
{
    PEXT2_LOCK_STRIPE   Stripes;
    PUCHAR              Block;
    ULONG               i;

    Block = Ext2AllocatePool(NonPagedPool,
                             sizeof(EXT2_LOCK_STRIPE) * Count + SYSTEM_CACHE_ALIGNMENT_SIZE,
                             EXT4_STRIPE_TAG);
    *Pool = Block;
    if (Block == NULL) {
        return NULL;
    }
    Stripes = (PEXT2_LOCK_STRIPE)ALIGN_UP_POINTER_BY(Block, SYSTEM_CACHE_ALIGNMENT_SIZE);
    for (i = 0; i < Count; i++) {
        ExInitializeResourceLite(&Stripes[i].Lock);
    }
    return Stripes;
}

VOID
Ext2FreeStripes(IN PEXT2_LOCK_STRIPE Stripes, IN ULONG Count, IN PVOID Pool)
{
    ULONG i;

    if (Stripes == NULL) {
        return;
    }
    for (i = 0; i < Count; i++) {
        ExDeleteResourceLite(&Stripes[i].Lock);
    }
    Ext2FreePool(Pool, EXT4_STRIPE_TAG);
}

/* ---------------------------------------------------------------- block groups */

/*
 * Block group g falls on stripe g & GroupLockMask. Its block bitmap changes
 * under the stripe's BlockLock, its inode bitmap under the InodeLock: a
 * delete frees blocks and an inode of one group at the same moment, and
 * files gather in the group of their directory, so one lock for both made
 * them take turns. Inode before block when a thread needs both (the first
 * inode of a never-initialised group materialises its block bitmap).
 *
 * The descriptor both halves live in has one checksum. Every writer of a
 * descriptor field recomputes it afterwards (Ext2SaveGroup), computing and
 * storing it as one step under the DescLock push lock: the last checksum
 * stored is computed after every field written before it, so the two
 * halves cannot leave a stale one behind. A commit copies the block only
 * once every handle of its transaction - each writer's included - is done.
 *
 * Every other field belongs to one half, but bg_flags holds both halves'
 * flags: BLOCK_UNINIT is cleared under the BlockLock, INODE_UNINIT under
 * the InodeLock. A plain &= from each at once lost one of the two, and the
 * flag that came back made the next allocation build that bitmap afresh
 * over what had been allocated since - blocks and inodes handed out twice
 * (Ext2ClearGroupFlag).
 */
NTSTATUS
Ext2InitializeGroupLocks(IN PEXT2_VCB Vcb)
{
    ULONG   Count = Ext2StripeCount(Vcb->sbi.s_groups_count);
    PUCHAR  Block;
    ULONG   i;

    Block = Ext2AllocatePool(NonPagedPool,
                             sizeof(EXT2_GROUP_STRIPE) * Count + SYSTEM_CACHE_ALIGNMENT_SIZE,
                             EXT4_STRIPE_TAG);
    if (Block == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Vcb->GroupLockPool = Block;
    Vcb->GroupLocks = (PEXT2_GROUP_STRIPE)ALIGN_UP_POINTER_BY(Block, SYSTEM_CACHE_ALIGNMENT_SIZE);
    for (i = 0; i < Count; i++) {
        ExInitializeResourceLite(&Vcb->GroupLocks[i].BlockLock);
        ExInitializeResourceLite(&Vcb->GroupLocks[i].InodeLock);
        ExInitializePushLock(&Vcb->GroupLocks[i].DescLock);
    }
    Vcb->GroupLockMask = Count - 1;
    return STATUS_SUCCESS;
}

VOID
Ext2DestroyGroupLocks(IN PEXT2_VCB Vcb)
{
    ULONG i;

    if (Vcb->GroupLocks == NULL) {
        return;
    }
    for (i = 0; i <= Vcb->GroupLockMask; i++) {
        ExDeleteResourceLite(&Vcb->GroupLocks[i].BlockLock);
        ExDeleteResourceLite(&Vcb->GroupLocks[i].InodeLock);
    }
    Ext2FreePool(Vcb->GroupLockPool, EXT4_STRIPE_TAG);
    Vcb->GroupLocks = NULL;
    Vcb->GroupLockPool = NULL;
}

static PERESOURCE
Ext2AcquireGroupLock(IN PERESOURCE Lock)
{
    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(Lock, TRUE);
    return Lock;
}

PERESOURCE
Ext2LockGroupBlocks(IN PEXT2_VCB Vcb, IN ULONG Group)
{
    return Ext2AcquireGroupLock(&Vcb->GroupLocks[Group & Vcb->GroupLockMask].BlockLock);
}

PERESOURCE
Ext2LockGroupInodes(IN PEXT2_VCB Vcb, IN ULONG Group)
{
    return Ext2AcquireGroupLock(&Vcb->GroupLocks[Group & Vcb->GroupLockMask].InodeLock);
}

/* the checksum of group Group's descriptor, computed and stored as one step */
VOID
Ext2SetGroupDescCsum(IN PEXT2_VCB Vcb, IN ULONG Group, IN struct ext4_group_desc *Desc)
{
    PEX_PUSH_LOCK Lock = &Vcb->GroupLocks[Group & Vcb->GroupLockMask].DescLock;

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(Lock);
    ext4_group_desc_csum_set(&Vcb->sb, Group, Desc);
    ExReleasePushLockExclusive(Lock);
    KeLeaveCriticalRegion();
}

VOID
Ext2ClearGroupFlag(IN struct ext4_group_desc *Desc, IN USHORT Flag)
{
    InterlockedAnd16((SHORT volatile *)&Desc->bg_flags, (SHORT)~cpu_to_le16(Flag));
}

VOID
Ext2UnlockGroup(IN PERESOURCE Lock)
{
    ExReleaseResourceLite(Lock);
    KeLeaveCriticalRegion();
}
