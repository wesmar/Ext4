/**
 * JournalSession.c - journal session life cycle: start, commit, flush, clean stop.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include <linux/jbd2.h>
#include "JournalInternal.h"

#define JNL_MAX_GROUP_BLOCKS    128     /* data blocks per descriptor group */

NTSTATUS
Ext2JournalGlobalInit(VOID)
{
    ULONG i;

    for (i = 0; i < JNL_SCOPE_BUCKETS; i++)
        InitializeListHead(&JnlScopeBuckets[i]);
    KeInitializeSpinLock(&JnlScopeLock);
    ExInitializeNPagedLookasideList(&JnlScopeLookaside, NULL, NULL, 0,
                                    sizeof(EXT2_JSCOPE), JNL_TAG, 0);
    JnlScopeInited = TRUE;
    return STATUS_SUCCESS;
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
    journal_t            *jbd = J->jbd;
    journal_superblock_t *sb = jbd->j_superblock;
    __u32                 incompat = be32_to_cpu(sb->s_feature_incompat);
    __u32                 compat = be32_to_cpu(sb->s_feature_compat);
    __u32                 want_in = incompat, want_co = compat;

    if (jbd->j_format_version < 2)
        return STATUS_SUCCESS;      /* v1 journals have no feature bits */

    want_in |= JBD2_FEATURE_INCOMPAT_REVOKE;
    if (ext4_has_feature_64bit(&J->Vcb->sb))
        want_in |= JBD2_FEATURE_INCOMPAT_64BIT;

    want_in &= ~(JBD2_FEATURE_INCOMPAT_CSUM_V2 | JBD2_FEATURE_INCOMPAT_CSUM_V3);
    want_co &= ~JBD2_FEATURE_COMPAT_CHECKSUM;
    if (ext4_has_metadata_csum(&J->Vcb->sb))
        want_in |= JBD2_FEATURE_INCOMPAT_CSUM_V3;

    if (want_in != incompat || want_co != compat) {
        sb->s_feature_incompat = cpu_to_be32(want_in);
        sb->s_feature_compat = cpu_to_be32(want_co);
        if (want_in & JBD2_FEATURE_INCOMPAT_CSUM_V3)
            sb->s_checksum_type = JBD2_CRC32C_CHKSUM;
        else
            sb->s_checksum_type = 0;
    }

    /* the csum "driver" is a flag in this port; the sb csum depends on it */
    if (want_in & (JBD2_FEATURE_INCOMPAT_CSUM_V2 | JBD2_FEATURE_INCOMPAT_CSUM_V3)) {
        jbd->j_chksum_driver = (struct crypto_shash *)(ULONG_PTR)1;
        jbd->j_csum_seed = jbd2_chksum(jbd, ~0u, sb->s_uuid, sizeof(sb->s_uuid));
    } else {
        jbd->j_chksum_driver = NULL;
    }

    /* through jbd2_write_superblock: the checksum feature just switched
       on is only valid together with a freshly computed checksum */
    if (want_in != incompat || want_co != compat) {
        if (jbd2_journal_write_superblock(jbd))
            return STATUS_UNEXPECTED_IO_ERROR;
    }

    return STATUS_SUCCESS;
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
Ext2JournalStart(IN PEXT2_VCB Vcb, IN journal_t *journal, IN PEXT2_MCB Jcb)
{
    PEXT2_JOURNAL     J = Vcb->Journal;
    NTSTATUS          Status;
    HANDLE            hThread = NULL;
    OBJECT_ATTRIBUTES oa;
    ULONG             tag_bytes, csum_size, tags;

    if (J && (J->Flags & JF_ACTIVE))
        return STATUS_ALREADY_INITIALIZED;

    if (!(journal->j_flags & JBD2_LOADED) || (journal->j_flags & JBD2_ABORT)) {
        DbgPrint("ext4: journal start: not loaded or aborted (j_flags %lx)\n", journal->j_flags);
        return STATUS_INVALID_PARAMETER;
    }

    if (!J) {
        J = JnlCreate(Vcb);
        if (!J)
            return STATUS_INSUFFICIENT_RESOURCES;
        Vcb->Journal = J;
    }

    ASSERT(J->jbd == NULL && J->Running == NULL && IsListEmpty(&J->Checkpoint));

    J->Flags = 0;
    J->jbd = journal;
    J->Jcb = Jcb;
    J->BlockSize = journal->j_blocksize;
    J->BlockBits = Ext2Log2(J->BlockSize);
    J->MountState = Vcb->SuperBlock->s_state;
    J->CheckpointRecords = 0;
    J->NoScopeReports = 0;
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
    tag_bytes = (ULONG)journal_tag_bytes(journal);
    csum_size = jbd2_journal_has_csum_v2or3(journal) ? sizeof(struct jbd2_journal_block_tail) : 0;
    {
        LONG space = (LONG)(J->BlockSize - sizeof(journal_header_t));
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
    J->RevokesPerBlock = (J->BlockSize - sizeof(jbd2_journal_revoke_header_t) - csum_size) /
                         (jbd2_has_feature_64bit(journal) ? 8 : 4);
    J->IoBufferSize = (tags + 1) << J->BlockBits;
    J->IoBuffer = Ext2AllocatePool(NonPagedPool, J->IoBufferSize, JNL_TAG);
    if (!J->IoBuffer) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto errorout;
    }

    J->MaxTxnBlocks = journal->j_maxlen / 8;
    if (J->MaxTxnBlocks < 64)
        J->MaxTxnBlocks = 64;
    if (J->MaxTxnBlocks > journal->j_maxlen / 4)
        J->MaxTxnBlocks = journal->j_maxlen / 4;

    /* what the on-disk superblock says right now */
    J->DiskTail = be32_to_cpu(journal->j_superblock->s_start);
    J->DiskTailSeq = be32_to_cpu(journal->j_superblock->s_sequence);

    J->Running = JnlAllocTxn(J, journal->j_transaction_sequence);
    if (!J->Running) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto errorout;
    }
    J->CommitSequence = journal->j_transaction_sequence - 1;
    J->CommitRequest = J->CommitSequence;
    journal->j_commit_sequence = J->CommitSequence;
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
                   journal->j_maxlen, journal->j_first, journal->j_last,
                   journal->j_head, journal->j_free, J->Running->Tid));
    DbgPrint("ext4: journal active on volume %p (%u blocks, next tid %u)\n",
             Vcb, journal->j_maxlen, J->Running->Tid);

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
    J->jbd = NULL;
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
    if (!tid_geq(J->CommitRequest, target))
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
        journal_t *jbd = J->jbd;

        /* jbd2_mark_journal_empty: s_start = 0, s_sequence = next tid.
           Through jbd2_journal_update_sb_log_tail, never a raw write: that
           is the path that sets the superblock checksum (csum v2/v3), and
           a clean unmount that skipped it left a superblock e2fsck and the
           next mount both rejected as corrupt. */
        JnlLock(J, irql);
        JnlAdvanceTail(J);
        JnlUnlock(J, irql);

        if (jbd2_journal_update_sb_log_tail(jbd, jbd->j_tail_sequence, 0, 0) == 0) {
            jbd->j_flags |= JBD2_FLUSHED;
            J->DiskTail = 0;
            J->DiskTailSeq = jbd->j_tail_sequence;
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
                    t->Updates--;
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
    ULONGLONG       deadline;

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
    /* JnlDetach signals UpdatesEvent when the last one goes, now that
       JF_STOPPING is set (the commit thread, its other waiter, is gone) */
    deadline = KeQueryInterruptTime() + JNL_STOP_DRAIN;
    for (;;) {
        LONG            updates;
        LONGLONG        left;
        LARGE_INTEGER   wait;

        JnlLock(J, irql);
        updates = J->Running ? J->Running->Updates : 0;
        JnlUnlock(J, irql);
        left = (LONGLONG)(deadline - KeQueryInterruptTime());
        if (updates <= 0 || left <= 0)
            break;
        wait.QuadPart = -left;
        KeWaitForSingleObject(&J->UpdatesEvent, Executive, KernelMode, FALSE, &wait);
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
        if (running->Updates <= 0)
            InsertTailList(&txns, &running->Link);
        else
            DbgPrint("ext4: journal stop with %d handles open, transaction leaked\n",
                     running->Updates);
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

    if (J->jbd) {
        /* the on-disk journal superblock is already in its final state
           (empty when clean, tail/errno otherwise): keep destroy from
           rewriting it */
        J->jbd->j_flags |= JBD2_ABORT;
        jbd2_journal_destroy(J->jbd);
        J->jbd = NULL;
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
