/**
 * JournalCheckpoint.c - checkpointing committed blocks home and the journal thread.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include <linux/jbd2.h>
#include "JournalInternal.h"

#define JNL_COMMIT_INTERVAL     ((LONGLONG)5 * 10 * 1000 * 1000)   /* 5 s   */
#define JNL_IDLE_CHECKPOINT     ((LONGLONG)30 * 10 * 1000 * 1000)  /* 30 s  */
#define JNL_THREAD_TICK         ((LONGLONG)1 * 10 * 1000 * 1000)   /* 1 s   */

typedef struct _JNL_CPENTRY {
    PEXT2_JREC  Rec;
    ULONGLONG   Block;
    PUCHAR      Data;
} JNL_CPENTRY, *PJNL_CPENTRY;

/* insertion sort by block number: batches are small (<= 129 entries) */
static VOID JnlSortBatch(PJNL_CPENTRY b, ULONG n)
{
    ULONG i, j;
    for (i = 1; i < n; i++) {
        JNL_CPENTRY key = b[i];
        for (j = i; j > 0 && b[j - 1].Block > key.Block; j--)
            b[j] = b[j - 1];
        b[j] = key;
    }
}

/*
 * Retire the oldest committed transaction: write its blocks home (only the
 * newest copy of each block, and only blocks nobody is touching), drop the
 * records, advance the tail. Returns STATUS_PENDING when the transaction
 * could not be fully retired because a newer, uncommitted transaction owns
 * some of its blocks or someone holds them. With Wait the routine retries
 * held buffers; with AllowCommit it also commits the running transaction
 * that owns pending blocks (never from inside a commit: not reentrant).
 */
NTSTATUS JnlCheckpointOldest(PEXT2_JOURNAL J, BOOLEAN Wait, BOOLEAN AllowCommit)
{
    PEXT2_VCB            Vcb = J->Vcb;
    struct block_device *bdev = &Vcb->bd;
    PEXT2_JTXN           t;
    KIRQL                irql;
    NTSTATUS             Status = STATUS_SUCCESS;
    PJNL_CPENTRY         batch;
    ULONG                batchMax = J->GroupBlocks + 1;
    ULONGLONG            heldSince = 0;  /* 0: not waiting for held buffers */

    batch = Ext2AllocatePool(NonPagedPool, batchMax * sizeof(JNL_CPENTRY), JNL_TAG);
    if (!batch)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* a release of a journaled buffer from here on wakes the wait for held
       ones below (Ext2JournalBufferReleased); CommitLock makes this the
       only checkpoint */
    InterlockedIncrement(&J->HeldWaiters);

    for (;;) {

        LIST_ENTRY  done;
        PLIST_ENTRY e;
        ULONG       count = 0, held = 0, pending = 0, i;
        tid_t       pendingTid = 0;
        BOOLEAN     pendingLocked = FALSE;

        InitializeListHead(&done);

        /* cleared before the records are looked at: a holder letting go
           after this point is not missed */
        KeClearEvent(&J->HeldEvent);

        /* classify records under both locks; copy what can be written */
        ExAcquireResourceExclusiveLite(&bdev->bd_bh_lock, TRUE);
        JnlLock(J, irql);

        if (IsListEmpty(&J->Checkpoint)) {
            JnlUnlock(J, irql);
            ExReleaseResourceLite(&bdev->bd_bh_lock);
            break;
        }
        t = CONTAINING_RECORD(J->Checkpoint.Flink, EXT2_JTXN, Link);

        e = t->Records.Flink;
        while (e != &t->Records && count < batchMax) {

            PEXT2_JREC          r = CONTAINING_RECORD(e, EXT2_JREC, Link);
            struct buffer_head *bh = r->bh;
            e = e->Flink;

            if (r->Type == JREC_REVOKE) {
                JnlUnlinkRecord(J, t, r);
                InsertTailList(&done, &r->Link);
                continue;
            }

            if (bh->b_jctid != 0 && tid_gt(bh->b_jctid, t->Tid)) {
                /* a newer committed copy is in the log: this record is
                   superseded whatever the block does now (0 = never
                   committed; tids are compared modulo 2^32) */
                JnlUnlinkRecord(J, t, r);
                InsertTailList(&done, &r->Link);
                continue;
            }

            if (bh->b_jtid != t->Tid) {
                /* the only newer owner is an uncommitted transaction */
                pending++;
                if (!pendingTid || tid_gt(bh->b_jtid, pendingTid))
                    pendingTid = bh->b_jtid;
                if (J->Running->Tid == bh->b_jtid && J->Running->State != JTXN_RUNNING)
                    pendingLocked = TRUE;
                continue;
            }

            if (atomic_read(&bh->b_count) != bh->b_jrefs + bh->b_jpin) {
                held++;         /* someone may be modifying it */
                continue;
            }

            batch[count].Rec = r;
            batch[count].Block = r->Block;
            batch[count].Data = J->IoBuffer + ((ULONGLONG)count << J->BlockBits);
            count++;
        }

        JnlUnlock(J, irql);

        /* copy with only the bh lock held: nobody can obtain (and thus
           modify) these buffers until it is released, so the copies are
           exactly the committed content */
        for (i = 0; i < count; i++) {
            RtlCopyMemory(batch[i].Data, batch[i].Rec->bh->b_data, J->BlockSize);
        }

        ExReleaseResourceLite(&bdev->bd_bh_lock);

        JnlReleaseRecordList(J, &done);

        /* write the batch home, sorted for locality, coalescing runs */
        if (count) {
            JnlSortBatch(batch, count);
            i = 0;
            while (i < count && NT_SUCCESS(Status)) {
                ULONG run = 1;
                while (i + run < count && batch[i + run].Block == batch[i].Block + run)
                    run++;
                if (run == 1) {
                    Status = Ext2WriteDiskSync(Vcb, batch[i].Block << J->BlockBits,
                                               J->BlockSize, batch[i].Data, FALSE);
                } else {
                    PUCHAR gather = Ext2AllocatePool(NonPagedPool, (SIZE_T)run << J->BlockBits, JNL_TAG);
                    if (!gather) {
                        run = 1;
                        Status = Ext2WriteDiskSync(Vcb, batch[i].Block << J->BlockBits,
                                                   J->BlockSize, batch[i].Data, FALSE);
                    } else {
                        ULONG k;
                        for (k = 0; k < run; k++)
                            RtlCopyMemory(gather + ((ULONGLONG)k << J->BlockBits),
                                          batch[i + k].Data, J->BlockSize);
                        Status = Ext2WriteDiskSync(Vcb, batch[i].Block << J->BlockBits,
                                                   run << J->BlockBits, gather, FALSE);
                        Ext2FreePool(gather, JNL_TAG);
                    }
                }
                i += run;
            }

            if (!NT_SUCCESS(Status)) {
                DEBUG(DL_ERR, ("Ext2Jnl: checkpoint write failed: %xh\n", Status));
                JnlAbort(J, Status);
                break;
            }

            /* the copies are on disk: retire the records */
            InitializeListHead(&done);
            JnlLock(J, irql);
            for (i = 0; i < count; i++) {
                JnlUnlinkRecord(J, t, batch[i].Rec);
                InsertTailList(&done, &batch[i].Rec->Link);
            }
            JnlUnlock(J, irql);
            JnlReleaseRecordList(J, &done);
        }

        /* transaction fully retired? */
        JnlLock(J, irql);
        if (t->NumRecords == 0) {
            LONG updates = t->Updates;
            RemoveEntryList(&t->Link);
            JnlAdvanceTail(J);
            JnlUnlock(J, irql);
            DEBUG(DL_JNL, ("Ext2Jnl: retired tid %u\n", t->Tid));
            if (updates <= 0) {
                JnlFreeTxn(J, t);
            } else {
                /* a forced commit left handles behind: they still point
                   here, so the block is leaked rather than freed */
                DbgPrint("ext4: journal tid %u retired with %d handles open, leaked\n",
                         t->Tid, updates);
            }
            break;
        }
        JnlUnlock(J, irql);

        if (count) {
            heldSince = 0;
            continue;           /* more to write */
        }

        if (!Wait) {
            Status = STATUS_PENDING;
            break;
        }

        if (pending) {
            BOOLEAN commit = FALSE;
            /* the newer owner must commit first */
            if (!AllowCommit || pendingLocked) {
                Status = STATUS_PENDING;
                break;
            }
            JnlLock(J, irql);
            if (J->Running->Tid == pendingTid && J->Running->State == JTXN_RUNNING)
                commit = TRUE;
            JnlUnlock(J, irql);
            if (commit) {
                Status = JnlCommitTransaction(J);
                if (!NT_SUCCESS(Status))
                    break;
            }
            /* (else the owner committed meanwhile: classify again) */
            heldSince = 0;
            continue;
        }

        /* held buffers: wait until a holder lets one go, then look again;
           past JNL_HELD_STUCK without progress something is stuck */
        {
            ULONGLONG       now = KeQueryInterruptTime();
            LARGE_INTEGER   wait;

            if (heldSince == 0) {
                heldSince = now;
            }
            if (now - heldSince >= (ULONGLONG)JNL_HELD_STUCK) {
                DEBUG(DL_ERR, ("Ext2Jnl: checkpoint of tid %u stuck (held=%u pending=%u)\n",
                               t->Tid, held, pending));
                Status = STATUS_PENDING;
                break;
            }
            wait.QuadPart = -(LONGLONG)(heldSince + JNL_HELD_STUCK - now);
            KeWaitForSingleObject(&J->HeldEvent, Executive, KernelMode, FALSE, &wait);
        }
    }

    InterlockedDecrement(&J->HeldWaiters);
    Ext2FreePool(batch, JNL_TAG);
    return Status;
}

/*
 * Retire transactions while the log or the pinned-page budget is under
 * pressure, and everything when idle.
 */
static VOID JnlCheckpointPolicy(PEXT2_JOURNAL J)
{
    journal_t *jbd = J->jbd;
    KIRQL      irql;
    ULONG      guard = 0;

    for (;;) {
        BOOLEAN   pressure, idle, empty;
        LONGLONG  now = JnlNow();
        NTSTATUS  Status;

        JnlLock(J, irql);
        empty    = IsListEmpty(&J->Checkpoint);
        pressure = (jbd->j_free < jbd->j_maxlen / 2) ||
                   (J->CheckpointRecords > J->MaxTxnBlocks * 2);
        idle     = (J->Running->NumRecords == 0) &&
                   (now - J->LastCommitTime.QuadPart > JNL_IDLE_CHECKPOINT);
        JnlUnlock(J, irql);

        if (empty || (!pressure && !idle) || guard++ > 64)
            break;

        Status = JnlCheckpointOldest(J, pressure, TRUE);
        if (Status != STATUS_SUCCESS)
            break;
    }
}

VOID JnlThread(PVOID Context)
{
    PEXT2_JOURNAL   J = Context;
    LARGE_INTEGER   tick;
    KIRQL           irql;

    tick.QuadPart = -JNL_THREAD_TICK;

    for (;;) {

        BOOLEAN  commit, stop;
        LONGLONG now;

        KeWaitForSingleObject(&J->WakeEvent, Executive, KernelMode, FALSE, &tick);

        now = JnlNow();

        JnlLock(J, irql);
        stop = (J->Flags & JF_STOPPING) != 0;
        commit = (J->Running->NumRecords > 0) &&
                 (tid_geq(J->CommitRequest, J->Running->Tid) ||
                  now - J->Running->StartTime.QuadPart >= JNL_COMMIT_INTERVAL);
        if (J->Flags & JF_ABORTED)
            commit = FALSE;
        JnlUnlock(J, irql);

        if (stop)
            break;

        KeWaitForSingleObject(&J->CommitLock, Executive, KernelMode, FALSE, NULL);
        /* a commit that fails aborts the journal itself */
        if (commit && !(J->Flags & JF_ABORTED))
            (void)JnlCommitTransaction(J);
        if (!(J->Flags & JF_ABORTED))
            JnlCheckpointPolicy(J);
        KeReleaseMutex(&J->CommitLock, FALSE);
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}
