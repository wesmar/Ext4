/**
 * CacheReaper.c - background reapers for unused MCBs, buffer heads and FCBs.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Every reaper sleeps without a time-out while it knows of nothing to do.
 * When it holds idle objects that will expire, it sleeps until exactly the
 * earliest expiry - no periodic polling. It is woken early by:
 *   - Ext2ReaperKick, when an object becomes idle while the reaper has no
 *     deadline ("armed"), or when a cache has grown past its limit;
 *   - the system LowMemoryCondition event, while it holds idle objects:
 *     under memory pressure everything idle goes at once;
 *   - Ext2StopReaper.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "core_internal.h"

/* a released metadata block stays pinned this long for reuse: bitmaps,
   inode tables and directory blocks are touched again within moments */
#define EXT4_BH_KEEP            (10 * TICKSPERSEC)

/* an FCB nobody references keeps its inode and cached data this long */
#define EXT4_FCB_KEEP           (120 * TICKSPERSEC)

/* most names or FCBs freed per lock hold, so lookups keep flowing */
#define EXT4_REAP_BATCH         256

/* idle FCBs of deleted files that wake the FCB reaper at once: one scan of
   the FCB list per this many, instead of one per file or one per deadline */
#define EXT4_FCB_GONE_KICK      64

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2StartReaper)
#pragma alloc_text(PAGE, Ext2StopReaper)
#endif

/* ---------------------------------------------------------------- waiting */

static LONGLONG
Ext2Now(VOID)
{
    LARGE_INTEGER Now;

    KeQuerySystemTime(&Now);
    return Now.QuadPart;
}

static BOOLEAN
Ext2LowMemory(VOID)
{
    return Ext2Global->LowMemory != NULL && KeReadStateEvent(Ext2Global->LowMemory) != 0;
}

/* the earlier of two deadlines, 0 meaning "none" */
static LONGLONG
Ext2EarlierDeadline(LONGLONG a, LONGLONG b)
{
    if (a == 0) {
        return b;
    }
    if (b == 0) {
        return a;
    }
    return a < b ? a : b;
}

/*
 * Wake a reaper for new work. Without Now only a reaper with no deadline
 * ("armed") is woken: one that already waits for a deadline will see the
 * new object then, because objects expire in the order they became idle.
 */
VOID
Ext2ReaperKick(IN PEXT2_REAPER Reaper, IN BOOLEAN Now)
{
    if (Now || InterlockedExchange(&Reaper->Armed, FALSE)) {
        KeSetEvent(&Reaper->Wait, IO_NO_INCREMENT, FALSE);
    }
}

/*
 * The last reference to the Fcb of a deleted file is gone. It has nothing
 * left worth keeping, but it still holds its name, and a name held that way
 * cannot be reaped: under churn (create, delete, re-create) thousands of
 * them waited for the next FCB deadline, the name cache stayed above its
 * high water mark and the name reaper ran empty passes under the name
 * cache lock on every allocation - opens/s fell by 85% within a minute.
 * Batched: one scan per EXT4_FCB_GONE_KICK such Fcbs.
 */
VOID
Ext2FcbGone(IN PEXT2_VCB Vcb)
{
    if (InterlockedIncrement(&Vcb->FcbGone) >= EXT4_FCB_GONE_KICK) {
        Ext2ReaperKick(&Ext2Global->FcbReaper, TRUE);
    }
}

/*
 * A new name above the high water mark. The name reaper runs only if its
 * last sweep made room, or if the cache grew a batch past where that sweep
 * gave up: a sweep that finds nothing to free costs the same lock holds
 * as one that does, and repeating it per allocation starved
 * the opens it is meant to serve.
 */
VOID
Ext2McbReaperWake(VOID)
{
    ULONG Current = Ext2Global->PerfStat.Current.Mcb;

    if (Current > Ext2McbHighWater() &&
        Current > (ULONG)ReadNoFence(&Ext2Global->McbReapFloor)) {
        KeSetEvent(&Ext2Global->McbReaper.Wait, 0, FALSE);
    }
}

/*
 * Arm before scanning: an object that becomes idle during the scan kicks
 * the reaper and costs at most one extra pass, never a missed one.
 */
static VOID
Ext2ReaperArm(IN PEXT2_REAPER Reaper)
{
    InterlockedExchange(&Reaper->Armed, TRUE);
}

/*
 * Sleep until kicked, until Deadline (absolute system time; 0 = none) or,
 * while Holding idle objects, until memory runs low. A reaper that waits
 * for a deadline is disarmed: it needs no kick for objects expiring later.
 */
static VOID
Ext2ReaperSleep(IN PEXT2_REAPER Reaper, IN LONGLONG Deadline, IN BOOLEAN Holding)
{
    PVOID           Objects[2];
    ULONG           Count = 0;
    LARGE_INTEGER   Due;

    Objects[Count++] = &Reaper->Wait;
    if (Holding && Ext2Global->LowMemory != NULL && !Ext2LowMemory()) {
        /* while it stays signaled it must not be waited on again, or the
           reaper would spin; a pass under low memory holds nothing idle */
        Objects[Count++] = Ext2Global->LowMemory;
    }

    if (Deadline != 0) {
        InterlockedExchange(&Reaper->Armed, FALSE);
    }

    Due.QuadPart = Deadline;
    KeWaitForMultipleObjects(Count, Objects, WaitAny, Executive, KernelMode, FALSE,
                             Deadline != 0 ? &Due : NULL, NULL);
}

/* ---------------------------------------------------------------- names */

/*
 * The name cache grows until Ext2McbHighWater(), where Ext2AllocateMcb
 * kicks the reaper, and is trimmed back to Ext2McbLowWater() in bounded
 * batches, a fair share from every volume. Names are cheap; they are not
 * kept on a timer and need no deadline.
 */
VOID
Ext2McbReaperThread(
    PVOID   Context
)
{
    PEXT2_REAPER    Reaper = Context;
    PLIST_ENTRY     List = NULL;
    PEXT2_VCB       Vcb = NULL;
    PEXT2_MCB       Mcb  = NULL;
    BOOLEAN         GlobalAcquired = FALSE;

    __try {

        /* wake up DriverEntry */
        KeSetEvent(&Reaper->Engine, 0, FALSE);

        while (TRUE) {

            Ext2ReaperArm(Reaper);
            Ext2ReaperSleep(Reaper, 0, FALSE);
            if (IsFlagOn(Reaper->Flags, EXT2_REAPER_FLAG_STOP)) {
                break;
            }

            if (Ext2Global->PerfStat.Current.Mcb <= Ext2McbLowWater()) {
                continue;
            }

            ExAcquireResourceSharedLite(&Ext2Global->Resource, TRUE);
            GlobalAcquired = TRUE;

            for (List = Ext2Global->VcbList.Flink;
                 List != &(Ext2Global->VcbList) &&
                 Ext2Global->PerfStat.Current.Mcb > Ext2McbLowWater();
                 List = List->Flink) {

                Vcb = CONTAINING_RECORD(List, EXT2_VCB, Next);

                while (Ext2Global->PerfStat.Current.Mcb > Ext2McbLowWater()) {

                    LIST_ENTRY  Reaped;
                    ULONG       Want, Got;

                    Want = Ext2Global->PerfStat.Current.Mcb - Ext2McbLowWater();
                    if (Want > EXT4_REAP_BATCH) {
                        Want = EXT4_REAP_BATCH;
                    }

                    InitializeListHead(&Reaped);
                    Got = Ext2FirstUnusedMcb(Vcb, TRUE, Want, &Reaped);

                    while (!IsListEmpty(&Reaped)) {
                        Mcb = CONTAINING_RECORD(RemoveHeadList(&Reaped), EXT2_MCB, Link);
                        DEBUG(DL_RES, ( "Ext2McbReaperThread: releasing Mcb (%p): %wZ"
                                        " Total: %xh\n", Mcb, &Mcb->FullName,
                                        Ext2Global->PerfStat.Current.Mcb));
                        Ext2FreeMcb(Vcb, Mcb);
                    }

                    if (Got < Want) {
                        break;      /* nothing more to reap here for now */
                    }
                }
            }

            ExReleaseResourceLite(&Ext2Global->Resource);
            GlobalAcquired = FALSE;

            /* under the low water mark: no floor. Otherwise what is left is
               in use (open files, idle FCBs, children of cached
               directories), and the next sweep waits until a batch more
               names exist or the FCB reaper has released some. */
            {
                ULONG Current = Ext2Global->PerfStat.Current.Mcb;
                InterlockedExchange(&Ext2Global->McbReapFloor,
                                    Current > Ext2McbLowWater() ?
                                    (LONG)(Current + EXT4_REAP_BATCH) : 0);
            }
        }

    } __finally {

        if (GlobalAcquired) {
            ExReleaseResourceLite(&Ext2Global->Resource);
        }

        KeSetEvent(&Reaper->Engine, 0, FALSE);
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ---------------------------------------------------------------- buffer heads */

/*
 * Move the released buffer heads of a volume that have expired onto Head
 * (all of them when Everything). Released buffers sit on bd_bh_free in the
 * order they were released, so the first one still young gives the next
 * deadline of this volume, which is returned (0: none held).
 */
static LONGLONG
Ext2QueryUnusedBH(PEXT2_VCB Vcb, PLIST_ENTRY Head, LONGLONG Now, BOOLEAN Everything)
{
    struct buffer_head *bh;
    PLIST_ENTRY         Next;
    LONGLONG            Deadline = 0;

    if (IsFlagOn(Vcb->Flags, VCB_BEING_DROPPED)) {
        Everything = TRUE;
    }

    ExAcquireResourceExclusiveLite(&Vcb->bd.bd_bh_lock, TRUE);

    while (!IsListEmpty(&Vcb->bd.bd_bh_free)) {

        Next = RemoveHeadList(&Vcb->bd.bd_bh_free);
        bh = CONTAINING_RECORD(Next, struct buffer_head, b_link);
        if (atomic_read(&bh->b_count)) {
            /* taken again since its release: off the list, brelse puts it
               back when it is released next time */
            InitializeListHead(&bh->b_link);
            continue;
        }

        if (!Everything && bh->b_ts_drop.QuadPart + EXT4_BH_KEEP > Now) {
            InsertHeadList(&Vcb->bd.bd_bh_free, &bh->b_link);
            Deadline = bh->b_ts_drop.QuadPart + EXT4_BH_KEEP;
            break;
        }

        InsertTailList(Head, &bh->b_link);
        buffer_head_remove(&Vcb->bd, bh);
        /* out of the tree but still pinning its Bcb until the reaper
           frees it; Ext2DrainBH waits for this count (under the lock,
           so the Vcb outlives the reaper's last touch of it) */
        Vcb->bd.bd_bh_reaping++;
        KeClearEvent(&Vcb->bd.bd_bh_notify);
    }

    ExReleaseResourceLite(&Vcb->bd.bd_bh_lock);

    return Deadline;
}

/* the reaper is done with one bh of this volume: unpinned and freed */
static VOID
Ext2ReapedBH(struct block_device *bdev)
{
    ExAcquireResourceExclusiveLite(&bdev->bd_bh_lock, TRUE);
    ASSERT(bdev->bd_bh_reaping > 0);
    if (--bdev->bd_bh_reaping == 0) {
        KeSetEvent(&bdev->bd_bh_notify, 0, FALSE);
    }
    ExReleaseResourceLite(&bdev->bd_bh_lock);
}

/* Reaper thread to release unused buffer heads */
VOID
Ext2bhReaperThread(
    PVOID   Context
)
{
    PEXT2_REAPER    Reaper = Context;
    PEXT2_VCB       Vcb = NULL;
    LIST_ENTRY      List, *Link;
    LONGLONG        Deadline = 0;
    BOOLEAN         GlobalAcquired = FALSE;

    __try {

        /* wake up DriverEntry */
        KeSetEvent(&Reaper->Engine, 0, FALSE);

        while (TRUE) {

            Ext2ReaperSleep(Reaper, Deadline, Deadline != 0);
            if (IsFlagOn(Reaper->Flags, EXT2_REAPER_FLAG_STOP)) {
                break;
            }

            {
                LONGLONG    Now = Ext2Now();
                BOOLEAN     Everything = Ext2LowMemory();

                Ext2ReaperArm(Reaper);
                Deadline = 0;
                InitializeListHead(&List);

                ExAcquireResourceSharedLite(&Ext2Global->Resource, TRUE);
                GlobalAcquired = TRUE;
                for (Link = Ext2Global->VcbList.Flink;
                     Link != &(Ext2Global->VcbList);
                     Link = Link->Flink ) {

                    Vcb = CONTAINING_RECORD(Link, EXT2_VCB, Next);
                    Deadline = Ext2EarlierDeadline(Deadline,
                                   Ext2QueryUnusedBH(Vcb, &List, Now, Everything));
                }
                ExReleaseResourceLite(&Ext2Global->Resource);
                GlobalAcquired = FALSE;
            }

            while (!IsListEmpty(&List)) {
                struct buffer_head *bh;
                struct block_device *bdev;
                Link = RemoveHeadList(&List);
                bh = CONTAINING_RECORD(Link, struct buffer_head, b_link);
                ASSERT(0 == atomic_read(&bh->b_count));
                bdev = bh->b_bdev;
                free_buffer_head(bh);
                Ext2ReapedBH(bdev);
            }
        }

    } __finally {

        if (GlobalAcquired) {
            ExReleaseResourceLite(&Ext2Global->Resource);
        }

        KeSetEvent(&Reaper->Engine, 0, FALSE);
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ---------------------------------------------------------------- FCBs */

/*
 * Move the FCBs of a volume that nobody references and that have expired
 * onto List: all of them when Everything (a dismounted volume, low
 * memory), at most EXT4_REAP_BATCH per call otherwise. Returns the next
 * deadline of this volume (0: none, Now: more expired ones left over).
 */
static LONGLONG
Ext2QueryUnusedFcb(PEXT2_VCB Vcb, PLIST_ENTRY List, LONGLONG Now, BOOLEAN Everything)
{
    PEXT2_FCB       Fcb;
    PLIST_ENTRY     Next, Link;
    LONGLONG        Deadline = 0;
    ULONG           Count = 0;

    BOOLEAN         Over;

    ExAcquireResourceExclusiveLite(&Vcb->FcbLock, TRUE);

    /* this scan takes every idle Fcb of a deleted file there is */
    InterlockedExchange(&Vcb->FcbGone, 0);

    /* past the high water mark the oldest idle Fcbs go whatever their age,
       down to the low water mark */
    Over = (ULONG)Vcb->FcbCount > Ext2FcbHighWater();

    for (Link = Vcb->FcbList.Flink; Link != &Vcb->FcbList; Link = Next) {

        Next = Link->Flink;
        Fcb = CONTAINING_RECORD(Link, EXT2_FCB, Next);

        if (Fcb->ReferenceCount > 0) {
            continue;
        }

        if (!Everything && Count >= EXT4_REAP_BATCH) {
            Deadline = Now;
            break;
        }

        /* TsDrop 0: deleted or delete pending, nothing worth keeping. The
           idle Fcbs follow in the order they became idle (Ext2ReleaseFcb
           puts those of deleted files first and the others last), so the
           first one still young ends the scan: every later one is younger.
           The scan went on to the end instead, under the exclusive FcbLock,
           and past the high water mark every close kicked it - creating
           files in a cache of 16 000 took a full scan per file. */
        if (!Everything && Fcb->TsDrop.QuadPart != 0 &&
            Fcb->TsDrop.QuadPart + EXT4_FCB_KEEP > Now &&
            !(Over && (ULONG)Vcb->FcbCount > Ext2FcbLowWater())) {
            Deadline = Ext2EarlierDeadline(Deadline, Fcb->TsDrop.QuadPart + EXT4_FCB_KEEP);
            break;
        }

        RemoveEntryList(&Fcb->Next);
        Ext2UnlinkFcb(Fcb);
        Ext2DerefXcb(&Vcb->FcbCount);
        InsertTailList(List, &Fcb->Next);
        Count++;
    }

    ExReleaseResourceLite(&Vcb->FcbLock);

    return Deadline;
}

/*
 * Free every Fcb of the volume that nobody references, right now. Used on
 * the way to a dismount: what the reaper would trim over minutes has to be
 * gone before the volume can go. The Vcb is kept alive across the frees;
 * the caller decides about the dismount itself (Ext2CheckDismount).
 */
VOID
Ext2DropIdleFcbs(PEXT2_VCB Vcb)
{
    LIST_ENTRY      idle, busy;
    PLIST_ENTRY     next;
    PEXT2_FCB       Fcb;

    InitializeListHead(&idle);
    InitializeListHead(&busy);

    ExAcquireResourceExclusiveLite(&Vcb->FcbLock, TRUE);
    while (!IsListEmpty(&Vcb->FcbList)) {
        next = RemoveHeadList(&Vcb->FcbList);
        Fcb = CONTAINING_RECORD(next, EXT2_FCB, Next);
        if (Fcb->ReferenceCount > 0) {
            InsertTailList(&busy, &Fcb->Next);
            continue;
        }
        Ext2UnlinkFcb(Fcb);
        Ext2DerefXcb(&Vcb->FcbCount);
        InsertTailList(&idle, &Fcb->Next);
    }
    while (!IsListEmpty(&busy)) {
        next = RemoveHeadList(&busy);
        InsertTailList(&Vcb->FcbList, next);
    }
    ExReleaseResourceLite(&Vcb->FcbLock);

    Ext2ReferXcb(&Vcb->ReferenceCount);
    while (!IsListEmpty(&idle)) {
        next = RemoveHeadList(&idle);
        Ext2FreeFcb(CONTAINING_RECORD(next, EXT2_FCB, Next));
    }
    Ext2DerefXcb(&Vcb->ReferenceCount);
}

/* Reaper thread to release Fcb */
VOID
Ext2FcbReaperThread(
    PVOID   Context
)
{
    PEXT2_REAPER    Reaper = Context;
    PEXT2_VCB       Vcb = NULL;
    LIST_ENTRY      List, *Link;
    LONGLONG        Deadline = 0;
    BOOLEAN         GlobalAcquired = FALSE;

    __try {

        /* wake up DriverEntry */
        KeSetEvent(&Reaper->Engine, 0, FALSE);

        while (TRUE) {

            Ext2ReaperSleep(Reaper, Deadline, Deadline != 0);
            if (IsFlagOn(Reaper->Flags, EXT2_REAPER_FLAG_STOP)) {
                break;
            }

            {
                LONGLONG    Now = Ext2Now();
                BOOLEAN     LowMemory = Ext2LowMemory();

                Ext2ReaperArm(Reaper);
                Deadline = 0;
                InitializeListHead(&List);

                ExAcquireResourceSharedLite(&Ext2Global->Resource, TRUE);
                GlobalAcquired = TRUE;

                /* a volume that lost its mount (forced dismount with file
                   objects still around) is drained like a dismounted one */
                for (Link  = Ext2Global->VcbList.Flink;
                     Link != &(Ext2Global->VcbList);
                     Link  = Link->Flink ) {

                    Vcb = CONTAINING_RECORD(Link, EXT2_VCB, Next);
                    Deadline = Ext2EarlierDeadline(Deadline,
                                   Ext2QueryUnusedFcb(Vcb, &List, Now,
                                                      LowMemory || !IsMounted(Vcb)));
                }

                /* dismounted volumes: drain their caches right away so the
                   last Fcb can take the Vcb (and its device object) with it */
                for (Link  = Ext2Global->DismountingVcbList.Flink;
                     Link != &(Ext2Global->DismountingVcbList);
                     Link  = Link->Flink ) {

                    Vcb = CONTAINING_RECORD(Link, EXT2_VCB, Next);
                    Ext2QueryUnusedFcb(Vcb, &List, Now, TRUE);
                }

                ExReleaseResourceLite(&Ext2Global->Resource);
                GlobalAcquired = FALSE;
            }

            if (!IsListEmpty(&List)) {
                while (!IsListEmpty(&List)) {
                    PEXT2_FCB  Fcb;
                    Link = RemoveHeadList(&List);
                    Fcb = CONTAINING_RECORD(Link, EXT2_FCB, Next);
                    ASSERT(0 == Fcb->ReferenceCount);
                    Ext2FreeFcb(Fcb);
                }
                /* the freed Fcbs held names: the name reaper has work again */
                InterlockedExchange(&Ext2Global->McbReapFloor, 0);
                Ext2McbReaperWake();
            }
        }

    } __finally {

        if (GlobalAcquired) {
            ExReleaseResourceLite(&Ext2Global->Resource);
        }

        KeSetEvent(&Reaper->Engine, 0, FALSE);
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ---------------------------------------------------------------- threads */

NTSTATUS
Ext2StartReaper(PEXT2_REAPER Reaper, EXT2_REAPER_RELEASE Free)
{
    NTSTATUS            status;
    OBJECT_ATTRIBUTES   oa;
    HANDLE              handle = 0;

    Reaper->Free = Free;
    Reaper->Armed = FALSE;

    KeInitializeEvent(&Reaper->Wait, SynchronizationEvent, FALSE);
    KeInitializeEvent(&Reaper->Engine, SynchronizationEvent, FALSE);

    InitializeObjectAttributes(&oa, NULL, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    status = PsCreateSystemThread(&handle, 0, &oa, NULL, NULL, Free, (PVOID)Reaper);
    if (NT_SUCCESS(status)) {

        /* keep a reference on the thread object: DriverUnload waits for
           the thread itself, not for an event it sets before it is gone */
        ObReferenceObjectByHandle(handle, THREAD_ALL_ACCESS, *PsThreadType,
                                  KernelMode, (PVOID *)&Reaper->Thread, NULL);
        ZwClose(handle);

        /* the thread signals Engine as its first action */
        KeWaitForSingleObject(&Reaper->Engine, Executive, KernelMode, FALSE, NULL);
    }

    return status;
}

VOID
Ext2StopReaper(PEXT2_REAPER Reaper)
{
    SetLongFlag(Reaper->Flags, EXT2_REAPER_FLAG_STOP);
    KeSetEvent(&Reaper->Wait, 0, FALSE);

    /* wait for the thread object: it is signaled only once the thread has
       left the driver for good, which is what unmapping the image needs */
    if (Reaper->Thread) {
        KeWaitForSingleObject(Reaper->Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Reaper->Thread);
        Reaper->Thread = NULL;
    }
}
