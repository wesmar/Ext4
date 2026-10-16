/**
 * JournalSession.c - journal session life cycle: start, commit, flush, clean stop.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "JournalInternal.h"

#define JNL_MAX_GROUP_BLOCKS    128     /* data blocks per descriptor group */

VOID
Ext2JournalGlobalInit(VOID)
{
    ULONG i;

    for (i = 0; i < JNL_SCOPE_BUCKETS; i++)
        InitializeListHead(&JnlScopeBuckets[i]);
    KeInitializeSpinLock(&JnlScopeLock);
    ExInitializeNPagedLookasideList(&JnlScopeLookaside, NULL, NULL, 0,
                                    sizeof(EXT2_JSCOPE), JNL_TAG, 0);
    JnlScopeInited = TRUE;
}

VOID
Ext2JournalGlobalExit(VOID)
{
    if (JnlScopeInited) {
        JnlScopeInited = FALSE;
        ExDeleteNPagedLookasideList(&JnlScopeLookaside);
    }
}

/*
 * Bring the journal features in line with the fs (Linux ext4_load_journal):
 * 64-bit tags when the fs is 64-bit, checksum v3 when metadata_csum is on,
 * revoke records always. The journal is empty at this point so this is safe.
 */
static NTSTATUS JnlSetupFeatures(PEXT2_JOURNAL J)
{
    PEXT2_JLOG  L = J->Log;
    ULONG       incompat = L->Incompat | JF_INCOMPAT_REVOKE;
    ULONG       compat = L->Compat & ~JF_COMPAT_CHECKSUM;

    if (ext4_has_feature_64bit(&J->Vcb->sb))
        incompat |= JF_INCOMPAT_64BIT;
    incompat &= ~(JF_INCOMPAT_CSUM_V2 | JF_INCOMPAT_CSUM_V3);
    if (ext4_has_metadata_csum(&J->Vcb->sb))
        incompat |= JF_INCOMPAT_CSUM_V3;

    /* a version 1 superblock keeps its (no) features */
    return NT_SUCCESS(JlogSetFeatures(L, compat, incompat)) ? STATUS_SUCCESS
                                                            : STATUS_UNEXPECTED_IO_ERROR;
}

/*
 * The per-volume engine block: allocated once, reused by every session.
 */
static PEXT2_JOURNAL JnlCreate(PEXT2_VCB Vcb)
{
    PEXT2_JOURNAL J = Ext2AllocatePool(NonPagedPool, sizeof(EXT2_JOURNAL), JNL_TAG);
    if (!J)
        return NULL;
    RtlZeroMemory(J, sizeof(EXT2_JOURNAL));

    J->Vcb = Vcb;
    KeInitializeSpinLock(&J->Lock);
    KeInitializeMutex(&J->CommitLock, 0);
    KeInitializeEvent(&J->WakeEvent, SynchronizationEvent, FALSE);
    KeInitializeEvent(&J->UnlockedEvent, NotificationEvent, TRUE);
    KeInitializeEvent(&J->UpdatesEvent, SynchronizationEvent, FALSE);
    KeInitializeEvent(&J->HeldEvent, NotificationEvent, FALSE);
    InitializeListHead(&J->Checkpoint);
    ExInitializeNPagedLookasideList(&J->RecLookaside, NULL, NULL, 0, sizeof(EXT2_JREC), JNL_TAG, 0);
    ExInitializeNPagedLookasideList(&J->TxnLookaside, NULL, NULL, 0, sizeof(EXT2_JTXN), JNL_TAG, 0);
    return J;
}

/*
 * Take over a loaded (recovered or wiped) journal and start a session.
 * On failure the caller keeps ownership of journal and Jcb.
 */
NTSTATUS
Ext2JournalStart(IN PEXT2_VCB Vcb, IN PEXT2_JLOG Log, IN PEXT2_MCB Jcb)
{
    PEXT2_JOURNAL     J = Vcb->Journal;
    NTSTATUS          Status;
    HANDLE            hThread = NULL;
    OBJECT_ATTRIBUTES oa;
    ULONG             tag_bytes, csum_size, tags;

    if (J && (J->Flags & JF_ACTIVE))
        return STATUS_ALREADY_INITIALIZED;

    if (Log->Flags & JLOG_ABORTED)
        return STATUS_INVALID_PARAMETER;

    if (!J) {
        J = JnlCreate(Vcb);
        if (!J)
            return STATUS_INSUFFICIENT_RESOURCES;
        Vcb->Journal = J;
    }

    ASSERT(J->Log == NULL && J->Running == NULL && IsListEmpty(&J->Checkpoint));

    J->Flags = 0;
    J->Log = Log;
    J->Jcb = Jcb;
    J->BlockSize = Log->BlockSize;
    J->BlockBits = Ext2Log2(J->BlockSize);
    J->MountState = Vcb->SuperBlock->s_state;
    J->CheckpointRecords = 0;
    J->NoScopeReports = 0;
    J->ThreadBlocked = FALSE;
    KeSetEvent(&J->UnlockedEvent, 0, FALSE);
    KeClearEvent(&J->WakeEvent);
    KeClearEvent(&J->UpdatesEvent);

    Status = JnlSetupFeatures(J);
    if (!NT_SUCCESS(Status)) {
        DbgPrint("ext4: journal start: feature setup failed %xh\n", Status);
        goto errorout;
    }

    /* descriptor capacity (Linux packs tags while there is room for a
       tag plus the 16-byte uuid); we also cap the group for the buffer */
    tag_bytes = JfTagBytes(Log->Incompat);
    csum_size = JfHasCsumV2V3(Log->Incompat) ? JF_TAIL_SIZE : 0;
    {
        LONG space = (LONG)(J->BlockSize - JF_HEADER_SIZE);
        tags = 0;
        while (space >= (LONG)(tag_bytes + 16 + csum_size) && tags < JNL_MAX_GROUP_BLOCKS) {
            space -= tag_bytes;
            if (tags == 0)
                space -= 16;
            tags++;
        }
    }
    if (tags == 0) {
        DbgPrint("ext4: journal start: no room for tags (blocksize %u tag %u csum %u)\n",
                 J->BlockSize, tag_bytes, csum_size);
        Status = STATUS_INVALID_PARAMETER;
        goto errorout;
    }
    J->GroupBlocks = tags;
    J->RevokesPerBlock = (J->BlockSize - JF_REVOKE_HEADER_SIZE - csum_size) /
                         ((Log->Incompat & JF_INCOMPAT_64BIT) ? 8 : 4);
    J->IoBufferSize = (tags + 1) << J->BlockBits;
    J->IoBuffer = Ext2AllocatePool(NonPagedPool, J->IoBufferSize, JNL_TAG);
    if (!J->IoBuffer) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto errorout;
    }

    J->MaxTxnBlocks = Log->MaxLen / 8;
    if (J->MaxTxnBlocks < 64)
        J->MaxTxnBlocks = 64;
    if (J->MaxTxnBlocks > Log->MaxLen / 4)
        J->MaxTxnBlocks = Log->MaxLen / 4;

    /* what the on-disk superblock says right now */
    J->DiskTail = JfGet32(Log->Super + JF_S_START);
    J->DiskTailSeq = JfGet32(Log->Super + JF_S_SEQUENCE);

    J->Running = JnlAllocTxn(J, Log->TransactionSequence);
    if (!J->Running) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto errorout;
    }
    J->CommitSequence = Log->TransactionSequence - 1;
    J->CommitRequest = J->CommitSequence;
    Log->CommitSequence = J->CommitSequence;
    KeQuerySystemTime(&J->LastCommitTime);

    /* tell the world the fs is in use before anything is logged */
    if (!JnlMarkSuper(J, TRUE)) {
        DbgPrint("ext4: journal start: superblock marker write failed\n");
        Status = STATUS_UNEXPECTED_IO_ERROR;
        goto errorout;
    }

    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = PsCreateSystemThread(&hThread, THREAD_ALL_ACCESS, &oa, NULL, NULL, JnlThread, J);
    if (!NT_SUCCESS(Status)) {
        DbgPrint("ext4: journal start: thread creation failed %xh\n", Status);
        goto errorout;
    }
    ObReferenceObjectByHandle(hThread, THREAD_ALL_ACCESS, NULL, KernelMode, (PVOID *)&J->Thread, NULL);
    ZwClose(hThread);

    SetFlag(J->Flags, JF_ACTIVE);

    DEBUG(DL_JNL, ("Ext2Jnl: started: blocks %u first %lu last %lu head %lu free %lu tid %u\n",
                   Log->MaxLen, Log->First, Log->Last, Log->Head, Log->Free, J->Running->Tid));
    DbgPrint("ext4: journal active on volume %p (%u blocks, next tid %u)\n",
             Vcb, Log->MaxLen, J->Running->Tid);

    return STATUS_SUCCESS;

errorout:

    if (J->Flags & JF_SB_MARKED) {
        /* undo the marker: nothing was logged (a failure leaves only the
           recovery flag, and recovery of an empty log is a no-op) */
        (void)JnlMarkSuper(J, FALSE);
    }
    if (J->Running) {
        JnlFreeTxn(J, J->Running);
        J->Running = NULL;
    }
    if (J->IoBuffer) {
        Ext2FreePool(J->IoBuffer, JNL_TAG);
        J->IoBuffer = NULL;
    }
    J->Log = NULL;
    J->Jcb = NULL;
    J->Flags = 0;
    return Status;
}

/*
 * Commit the running transaction (fsync). With Wait the caller's own
 * handle is dropped for the duration so the commit can proceed.
 */
NTSTATUS
Ext2JournalCommit(IN PEXT2_VCB Vcb, IN BOOLEAN Wait)
{
    PEXT2_JOURNAL   J = Vcb->Journal;
    KIRQL           irql;
    NTSTATUS        Status = STATUS_SUCCESS;
    tid_t           target;

    if (J && (J->Flags & JF_ABORTED))
        return STATUS_UNEXPECTED_IO_ERROR;
    if (!J || !(J->Flags & JF_ACTIVE))
        return STATUS_SUCCESS;

    JnlLock(J, irql);
    target = J->Running->Tid;
    if (J->Running->NumRecords == 0 && !Wait) {
        JnlUnlock(J, irql);
        return STATUS_SUCCESS;
    }
    if (!JfTidAtLeast(J->CommitRequest, target))
        J->CommitRequest = target;
    JnlUnlock(J, irql);

    if (!Wait) {
        KeSetEvent(&J->WakeEvent, 0, FALSE);
        return STATUS_SUCCESS;
    }

    JnlDetachSelf(J);

    KeWaitForSingleObject(&J->CommitLock, Executive, KernelMode, FALSE, NULL);
    JnlLock(J, irql);
    if (J->Flags & JF_ABORTED) {
        JnlUnlock(J, irql);
        Status = STATUS_UNEXPECTED_IO_ERROR;
    } else if (J->Running->Tid == target && J->Running->NumRecords) {
        JnlUnlock(J, irql);
        Status = JnlCommitTransaction(J);
    } else {
        JnlUnlock(J, irql);
    }
    KeReleaseMutex(&J->CommitLock, FALSE);

    return Status;
}

/*
 * Commit and retire everything: all metadata is on its home location when
 * this returns. Used by flush, dismount and shutdown.
 */
NTSTATUS
Ext2JournalFlush(IN PEXT2_VCB Vcb)
{
    PEXT2_JOURNAL   J = Vcb->Journal;
    NTSTATUS        Status = STATUS_SUCCESS;
    ULONG           rounds = 0;

    if (J && (J->Flags & JF_ABORTED))
        return STATUS_UNEXPECTED_IO_ERROR;
    if (!J || !(J->Flags & JF_ACTIVE))
        return STATUS_SUCCESS;

    /* the free totals go with what is flushed (they live in the Vcb) */
    Ext2SyncSuperTotals(NULL, Vcb);

    JnlDetachSelf(J);

    KeWaitForSingleObject(&J->CommitLock, Executive, KernelMode, FALSE, NULL);

    while (!(J->Flags & JF_ABORTED)) {
        KIRQL   irql;
        BOOLEAN more;

        Status = JnlCommitTransaction(J);
        if (!NT_SUCCESS(Status))
            break;

        for (;;) {
            JnlLock(J, irql);
            more = !IsListEmpty(&J->Checkpoint);
            JnlUnlock(J, irql);
            if (!more)
                break;
            Status = JnlCheckpointOldest(J, TRUE, TRUE);
            if (Status != STATUS_SUCCESS)
                break;
        }
        if (!NT_SUCCESS(Status))
            break;

        JnlLock(J, irql);
        more = (J->Running->NumRecords != 0) || !IsListEmpty(&J->Checkpoint);
        JnlUnlock(J, irql);
        if (!more)
            break;
        if (++rounds > 16) {
            Status = STATUS_DEVICE_BUSY;
            break;
        }
    }

    if (J->Flags & JF_ABORTED)
        Status = STATUS_UNEXPECTED_IO_ERROR;
    KeReleaseMutex(&J->CommitLock, FALSE);

    return Status;
}

/*
 * Flush everything and mark the journal empty and the fs clean (as a clean
 * unmount does). The engine stays active: the next commit re-marks the fs.
 */
NTSTATUS
Ext2JournalMarkClean(IN PEXT2_VCB Vcb)
{
    PEXT2_JOURNAL   J = Vcb->Journal;
    NTSTATUS        Status;
    KIRQL           irql;
    BOOLEAN         busy;

    if (J && (J->Flags & JF_ABORTED))
        return STATUS_UNEXPECTED_IO_ERROR;
    if (!J || !(J->Flags & JF_ACTIVE))
        return STATUS_SUCCESS;

    Status = Ext2JournalFlush(Vcb);
    if (!NT_SUCCESS(Status))
        return Status;

    KeWaitForSingleObject(&J->CommitLock, Executive, KernelMode, FALSE, NULL);

    JnlLock(J, irql);
    busy = (J->Running->NumRecords != 0) || !IsListEmpty(&J->Checkpoint);
    JnlUnlock(J, irql);

    if (!busy) {
        PEXT2_JLOG L = J->Log;

        /* the log empty: s_start 0, s_sequence the next ID. Through
           JlogSetTail, never a raw write: that is what sets the superblock
           checksum (v2/v3), and a clean unmount that skipped it once left
           a superblock e2fsck and the next mount both rejected. */
        JnlLock(J, irql);
        JnlAdvanceTail(J);
        JnlUnlock(J, irql);

        if (NT_SUCCESS(JlogSetTail(L, L->TailSequence, 0))) {
            J->DiskTail = 0;
            J->DiskTailSeq = L->TailSequence;
            if (!JnlMarkSuper(J, FALSE))
                Status = STATUS_UNEXPECTED_IO_ERROR;
        } else {
            Status = STATUS_UNEXPECTED_IO_ERROR;
        }
    }

    KeReleaseMutex(&J->CommitLock, FALSE);
    return Status;
}

/*
 * Drop every scope's reference to this journal's transactions. A thread
 * that is still inside its request keeps running; its handle is simply
 * gone (the session is over). Returns the number of handles released.
 */
static LONG JnlDropScopes(PEXT2_JOURNAL J)
{
    KIRQL   irql, irql2;
    ULONG   i, k;
    LONG    dropped = 0;

    if (!JnlScopeInited)
        return 0;

    KeAcquireSpinLock(&JnlScopeLock, &irql);
    for (i = 0; i < JNL_SCOPE_BUCKETS; i++) {
        PLIST_ENTRY e;
        for (e = JnlScopeBuckets[i].Flink; e != &JnlScopeBuckets[i]; e = e->Flink) {
            PEXT2_JSCOPE s = CONTAINING_RECORD(e, EXT2_JSCOPE, Link);
            for (k = 0; k < s->Count; k++) {
                if (s->Journal[k] == J && s->Txn[k]) {
                    PEXT2_JTXN t = s->Txn[k];
                    s->Txn[k] = NULL;
                    JnlLock(J, irql2);
                    /* as JnlDetach: the last one out tells the waiter */
                    if (--t->Updates == 0)
                        KeSetEvent(&J->UpdatesEvent, 0, FALSE);
                    JnlUnlock(J, irql2);
                    dropped++;
                }
            }
        }
    }
    KeReleaseSpinLock(&JnlScopeLock, irql);
    return dropped;
}

/*
 * Stop the session: flush, optionally mark clean, stop the thread, release
 * the jbd2 journal and the journal inode. The engine block stays with the
 * volume (see Ext2JournalDestroy). Safe to call when no session runs.
 */
VOID
Ext2JournalStop(IN PEXT2_VCB Vcb, IN BOOLEAN MarkClean)
{
    PEXT2_JOURNAL   J = Vcb->Journal;
    KIRQL           irql;
    LIST_ENTRY      records, txns;
    PEXT2_JTXN      running;

    if (!J || !(J->Flags & JF_ACTIVE))
        return;

    /* a stop goes on whatever these say: a failure has aborted the
       journal, and what is logged is replayed at the next mount */
    if (MarkClean)
        (void)Ext2JournalMarkClean(Vcb);
    else
        (void)Ext2JournalFlush(Vcb);

    /* no more buffers, no more handles */
    JnlLock(J, irql);
    SetFlag(J->Flags, JF_STOPPING);
    ClearFlag(J->Flags, JF_ACTIVE);
    JnlUnlock(J, irql);
    KeSetEvent(&J->UnlockedEvent, 0, FALSE);
    KeSetEvent(&J->WakeEvent, 0, FALSE);

    if (J->Thread) {
        KeWaitForSingleObject(J->Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(J->Thread);
        J->Thread = NULL;
    }

    /* requests still in flight lose their handles; a request that is
       between "scope removed" and "handle detached" finishes on its own */
    JnlDropScopes(J);
    /* What is left are requests between "scope removed" and "handle
       detached" (Ext2JournalLeaveScope, JnlDetachSelf): nothing waits in
       between, so they finish, and no handle starts any more (JF_ACTIVE
       is off). The last one out signals UpdatesEvent, now that JF_STOPPING
       is set, under the lock this check is made under (JnlDetach). */
    for (;;) {
        LONG updates;

        JnlLock(J, irql);
        updates = J->Running ? J->Running->Updates : 0;
        JnlUnlock(J, irql);
        if (updates <= 0)
            break;
        KeWaitForSingleObject(&J->UpdatesEvent, Executive, KernelMode, FALSE, NULL);
    }

    /* whatever is left (aborted journal, or a modification that raced
       the stop) goes the unjournaled way so it is not lost outright */
    InitializeListHead(&records);
    InitializeListHead(&txns);
    JnlLock(J, irql);
    while (!IsListEmpty(&J->Checkpoint)) {
        PEXT2_JTXN t = CONTAINING_RECORD(RemoveHeadList(&J->Checkpoint), EXT2_JTXN, Link);
        while (!IsListEmpty(&t->Records)) {
            PEXT2_JREC r = CONTAINING_RECORD(t->Records.Flink, EXT2_JREC, Link);
            JnlUnlinkRecord(J, t, r);
            InsertTailList(&records, &r->Link);
        }
        InsertTailList(&txns, &t->Link);
    }
    running = J->Running;
    J->Running = NULL;
    if (running) {
        while (!IsListEmpty(&running->Records)) {
            PEXT2_JREC r = CONTAINING_RECORD(running->Records.Flink, EXT2_JREC, Link);
            JnlUnlinkRecord(J, running, r);
            InsertTailList(&records, &r->Link);
        }
        InsertTailList(&txns, &running->Link);

    }
    JnlUnlock(J, irql);

    while (!IsListEmpty(&records)) {
        PEXT2_JREC r = CONTAINING_RECORD(RemoveHeadList(&records), EXT2_JREC, Link);
        if (r->Type == JREC_NORMAL) {
            set_buffer_dirty(r->bh);        /* legacy write on release */
        }
        JnlReleaseRecord(J, r);
    }
    while (!IsListEmpty(&txns)) {
        PEXT2_JTXN t = CONTAINING_RECORD(RemoveHeadList(&txns), EXT2_JTXN, Link);
        JnlFreeTxn(J, t);
    }

    if (J->Log) {
        /* the on-disk journal superblock is in its final state already
           (empty when clean, tail and errno otherwise) */
        JlogFree(J->Log);
        J->Log = NULL;
    }
    if (J->Jcb) {
        Ext2FreeMcb(Vcb, J->Jcb);
        J->Jcb = NULL;
    }
    if (J->IoBuffer) {
        Ext2FreePool(J->IoBuffer, JNL_TAG);
        J->IoBuffer = NULL;
    }

    J->Flags = 0;
    J->CheckpointRecords = 0;

    DbgPrint("ext4: journal stopped on volume %p\n", Vcb);
}

/*
 * Stop the session and free the engine block. Called when the volume
 * metadata stream and caches have drained, immediately before the VCB
 * resources are destroyed. Stream teardown only stops the session.
 */
VOID
Ext2JournalDestroy(IN PEXT2_VCB Vcb)
{
    PEXT2_JOURNAL J = Vcb->Journal;

    if (!J)
        return;

    Ext2JournalStop(Vcb, TRUE);

    Vcb->Journal = NULL;
    ExDeleteNPagedLookasideList(&J->RecLookaside);
    ExDeleteNPagedLookasideList(&J->TxnLookaside);
    Ext2FreePool(J, JNL_TAG);
}
