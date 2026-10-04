/**
 * FreedBlocks.c - blocks freed by a transaction stay taken until it commits.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * File data does not go through the journal: a block allocated to a file is
 * written in place as soon as the cache manager gets to it. A block freed
 * by the running transaction is free only once that transaction commits;
 * reused before, a crash leaves the old owner - whose release was never
 * committed - pointing at the new owner's data, another file's contents in
 * it. Linux keeps such blocks out of the buddy cache until the commit
 * (ext4_mb_free_metadata); here they stay in an overlay on the group's
 * bitmap, which the allocator searches as "on-disk bitmap OR overlay".
 *
 * The bitmap on disk is changed at once, as before: the commit carries the
 * release, and the free totals count the blocks. Only reuse waits.
 *
 * Two slots per group are enough. Commits are serialised and a transaction
 * starts committing only after the previous one has committed, so at most
 * two are uncommitted at any time: the running one and the one being
 * written. A slot holds the blocks freed by one transaction (Tid); one
 * whose transaction has committed is emptied when the group's stripe is
 * next locked. Should a third transaction ever be seen with both slots
 * busy, its blocks go into the slot of the later transaction, and that
 * slot waits for the later of the two - never released too early.
 *
 * Everything here runs under the BlockLock of the group's stripe, which
 * the allocator and the release hold anyway.
 */

#include "ext4fs.h"
#include "linux\ext4.h"

#define EXT4_FREED_TAG          'DF4E'
#define EXT2_BITS_PER_WORD      64

typedef struct _EXT2_FREED_SLOT {
    ULONG       Tid;                /* the transaction that freed them */
    ULONG       Count;              /* bits set in Bits; 0: slot empty */
    PULONG64    Bits;               /* one bit per block of the group */
} EXT2_FREED_SLOT, *PEXT2_FREED_SLOT;

typedef struct _EXT2_FREED_GROUP {
    LIST_ENTRY      Link;           /* EXT2_GROUP_STRIPE::FreedGroups */
    ULONG           Group;
    EXT2_FREED_SLOT Slot[EXT2_FREED_SLOTS];
    ULONG64         Storage[ANYSIZE_ARRAY]; /* the slots' bits, one after the other */
} EXT2_FREED_GROUP, *PEXT2_FREED_GROUP;

static __inline PEXT2_GROUP_STRIPE
FreedStripe(IN PEXT2_VCB Vcb, IN ULONG Group)
{
    return &Vcb->GroupLocks[Group & Vcb->GroupLockMask];
}

/* words of one slot: a bit per block of a group, rounded up to whole words */
static __inline ULONG
FreedWords(IN PEXT2_VCB Vcb)
{
    return (BLOCKS_PER_GROUP + EXT2_BITS_PER_WORD - 1) / EXT2_BITS_PER_WORD;
}

/* tids wrap: a is later than b when the distance from b to a is positive */
static __inline BOOLEAN
FreedTidAfter(IN ULONG a, IN ULONG b)
{
    return (LONG)(a - b) > 0;
}

static VOID
FreedEmptySlot(IN PEXT2_VCB Vcb, IN OUT PEXT2_FREED_SLOT Slot)
{
    RtlZeroMemory(Slot->Bits, (SIZE_T)FreedWords(Vcb) * sizeof(ULONG64));
    InterlockedAdd64(&Vcb->FreedPending, -(LONG64)Slot->Count);
    Slot->Count = 0;
}

/* empty the slots of committed transactions on the stripe; drop groups left empty */
static VOID
FreedReclaimStripe(IN PEXT2_VCB Vcb, IN PEXT2_GROUP_STRIPE Stripe)
{
    PLIST_ENTRY Entry = Stripe->FreedGroups.Flink;

    while (Entry != &Stripe->FreedGroups) {
        PEXT2_FREED_GROUP   Freed = CONTAINING_RECORD(Entry, EXT2_FREED_GROUP, Link);
        BOOLEAN             Busy = FALSE;
        ULONG               s;

        Entry = Entry->Flink;
        for (s = 0; s < EXT2_FREED_SLOTS; s++) {
            if (Freed->Slot[s].Count != 0 && Ext2JournalCommitted(Vcb, Freed->Slot[s].Tid)) {
                FreedEmptySlot(Vcb, &Freed->Slot[s]);
            }
            Busy |= Freed->Slot[s].Count != 0;
        }
        if (!Busy) {
            RemoveEntryList(&Freed->Link);
            Ext2FreePool(Freed, EXT4_FREED_TAG);
        }
    }
}

static PEXT2_FREED_GROUP
FreedFind(IN PEXT2_GROUP_STRIPE Stripe, IN ULONG Group)
{
    PLIST_ENTRY Entry;

    for (Entry = Stripe->FreedGroups.Flink; Entry != &Stripe->FreedGroups; Entry = Entry->Flink) {
        PEXT2_FREED_GROUP Freed = CONTAINING_RECORD(Entry, EXT2_FREED_GROUP, Link);
        if (Freed->Group == Group) {
            return Freed;
        }
    }
    return NULL;
}

VOID
Ext2FreedBlocksOf(IN PEXT2_VCB Vcb, IN ULONG Group, OUT const ULONG64 *Busy[EXT2_FREED_SLOTS])
{
    PEXT2_GROUP_STRIPE  Stripe = FreedStripe(Vcb, Group);
    PEXT2_FREED_GROUP   Freed;
    ULONG               s;

    FreedReclaimStripe(Vcb, Stripe);
    Freed = FreedFind(Stripe, Group);
    for (s = 0; s < EXT2_FREED_SLOTS; s++) {
        Busy[s] = (Freed != NULL && Freed->Slot[s].Count != 0) ? Freed->Slot[s].Bits : NULL;
    }
}

/* the slot that takes blocks freed by Tid */
static PEXT2_FREED_SLOT
FreedSlotFor(IN PEXT2_VCB Vcb, IN PEXT2_FREED_GROUP Freed, IN ULONG Tid)
{
    PEXT2_FREED_SLOT    Later = &Freed->Slot[0];
    ULONG               s;

    for (s = 0; s < EXT2_FREED_SLOTS; s++) {
        if (Freed->Slot[s].Count != 0 && Freed->Slot[s].Tid == Tid) {
            return &Freed->Slot[s];
        }
    }
    for (s = 0; s < EXT2_FREED_SLOTS; s++) {
        PEXT2_FREED_SLOT Slot = &Freed->Slot[s];
        if (Slot->Count != 0 && Ext2JournalCommitted(Vcb, Slot->Tid)) {
            FreedEmptySlot(Vcb, Slot);
        }
        if (Slot->Count == 0) {
            Slot->Tid = Tid;
            return Slot;
        }
        if (FreedTidAfter(Slot->Tid, Later->Tid)) {
            Later = Slot;
        }
    }
    /* a third uncommitted transaction: wait for the later of the two */
    if (FreedTidAfter(Tid, Later->Tid)) {
        Later->Tid = Tid;
    }
    return Later;
}

BOOLEAN
Ext2DeferFreedBlocks(IN PEXT2_VCB Vcb, IN ULONG Group, IN ULONG Index, IN ULONG Count)
{
    PEXT2_GROUP_STRIPE  Stripe = FreedStripe(Vcb, Group);
    PEXT2_FREED_GROUP   Freed;
    PEXT2_FREED_SLOT    Slot;
    RTL_BITMAP          Bitmap;
    ULONG               Tid, s;

    if (!Ext2JournalFreeingTid(Vcb, &Tid)) {
        return TRUE;                /* nothing logs: the release is final now */
    }

    Freed = FreedFind(Stripe, Group);
    if (Freed == NULL) {
        SIZE_T Words = FreedWords(Vcb);

        Freed = Ext2AllocatePool(PagedPool,
                                 FIELD_OFFSET(EXT2_FREED_GROUP, Storage) +
                                 EXT2_FREED_SLOTS * Words * sizeof(ULONG64),
                                 EXT4_FREED_TAG);
        if (Freed == NULL) {
            return FALSE;
        }
        RtlZeroMemory(Freed, FIELD_OFFSET(EXT2_FREED_GROUP, Storage) +
                             EXT2_FREED_SLOTS * Words * sizeof(ULONG64));
        Freed->Group = Group;
        for (s = 0; s < EXT2_FREED_SLOTS; s++) {
            Freed->Slot[s].Bits = &Freed->Storage[s * Words];
        }
        InsertHeadList(&Stripe->FreedGroups, &Freed->Link);
    }

    Slot = FreedSlotFor(Vcb, Freed, Tid);
    RtlInitializeBitMap(&Bitmap, (PULONG)Slot->Bits, BLOCKS_PER_GROUP);
    RtlSetBits(&Bitmap, Index, Count);
    Slot->Count += Count;
    InterlockedAdd64(&Vcb->FreedPending, Count);

    /* once per transaction, when a fair share of the free space waits for
       it, ask for the commit now rather than at the next interval */
    if (Vcb->FreedPending * EXT2_FREED_COMMIT_SHARE > (LONG64)Ext2FreeBlocks(Vcb) &&
        InterlockedExchange(&Vcb->FreedCommitAsked, (LONG)Tid) != (LONG)Tid) {
        Ext2JournalCommit(Vcb, FALSE);
    }
    return TRUE;
}

/*
 * Before a request that will allocate Needed blocks of file data takes any
 * lock: when the free space that is not waiting for a commit cannot cover
 * it, commit now and let the blocks go. Linux retries an allocation the
 * same way after a forced commit, outside every handle
 * (ext4_should_retry_alloc).
 *
 * Only here, at the top of a request, may the thread wait for a commit: no
 * journal handle of its own is active yet, it holds no buffer a checkpoint
 * could wait for, and no file lock that a handle holder could wait for -
 * inside the allocator any of these would make the commit wait for this
 * very thread.
 */
VOID
Ext2WaitForFreedBlocks(IN PEXT2_VCB Vcb, IN LONGLONG Allocated, IN LONGLONG Wanted)
{
    ULONGLONG   Needed;
    ULONG       i;

    if (Wanted <= Allocated) {
        return;
    }
    Needed = ((ULONGLONG)(Wanted - Allocated) + BLOCK_SIZE - 1) >> BLOCK_BITS;
    if (Vcb->FreedPending <= 0 ||
        (LONGLONG)Ext2FreeBlocks(Vcb) - Vcb->FreedPending >= (LONGLONG)Needed) {
        return;
    }
    if (!NT_SUCCESS(Ext2JournalCommit(Vcb, TRUE))) {
        return;                     /* the allocation reports what is left */
    }
    for (i = 0; i <= Vcb->GroupLockMask; i++) {
        PEXT2_GROUP_STRIPE Stripe = &Vcb->GroupLocks[i];

        KeEnterCriticalRegion();
        ExAcquireResourceExclusiveLite(&Stripe->BlockLock, TRUE);
        FreedReclaimStripe(Vcb, Stripe);
        ExReleaseResourceLite(&Stripe->BlockLock);
        KeLeaveCriticalRegion();
    }
}

VOID
Ext2DestroyFreedBlocks(IN PEXT2_VCB Vcb)
{
    ULONG i;

    for (i = 0; i <= Vcb->GroupLockMask; i++) {
        PLIST_ENTRY Head = &Vcb->GroupLocks[i].FreedGroups;
        while (!IsListEmpty(Head)) {
            PEXT2_FREED_GROUP Freed = CONTAINING_RECORD(RemoveHeadList(Head), EXT2_FREED_GROUP, Link);
            Ext2FreePool(Freed, EXT4_FREED_TAG);
        }
    }
    Vcb->FreedPending = 0;
}
