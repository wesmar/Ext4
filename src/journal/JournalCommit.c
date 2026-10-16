/**
 * JournalCommit.c - transaction commit: descriptor, revoke and commit blocks.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "JournalInternal.h"
#include "JournalReplay.h"

#define JTXN_COMMITTING     3           /* log I/O in progress */
#define JTXN_COMMITTED      4           /* on the checkpoint list */

static __inline PUCHAR JnlRecordData(PEXT2_JREC r)
{
    return r->Snapshot ? r->Snapshot : (PUCHAR)r->bh->b_data;
}

PEXT2_JTXN JnlAllocTxn(PEXT2_JOURNAL J, tid_t Tid)
{
    PEXT2_JTXN t = ExAllocateFromNPagedLookasideList(&J->TxnLookaside);
    if (t) {
        RtlZeroMemory(t, sizeof(EXT2_JTXN));
        InitializeListHead(&t->Records);
        t->Tid = Tid;
        t->State = JTXN_RUNNING;
    }
    return t;
}

VOID JnlFreeTxn(PEXT2_JOURNAL J, PEXT2_JTXN t)
{
    ASSERT(IsListEmpty(&t->Records));
    ExFreeToNPagedLookasideList(&J->TxnLookaside, t);
}

/*
 * Release a record outside of the spin lock: put_bh() takes an ERESOURCE.
 */
VOID JnlReleaseRecord(PEXT2_JOURNAL J, PEXT2_JREC r)
{
    struct buffer_head *bh = r->bh;
    ExFreeToNPagedLookasideList(&J->RecLookaside, r);
    put_bh(bh);
}

VOID JnlReleaseRecordList(PEXT2_JOURNAL J, PLIST_ENTRY List)
{
    while (!IsListEmpty(List)) {
        PLIST_ENTRY e = RemoveHeadList(List);
        JnlReleaseRecord(J, CONTAINING_RECORD(e, EXT2_JREC, Link));
    }
}

/*
 * Unlink a record from its transaction. Caller holds J->Lock. The bh
 * reference is dropped later by JnlReleaseRecord.
 */
VOID JnlUnlinkRecord(PEXT2_JOURNAL J, PEXT2_JTXN t, PEXT2_JREC r)
{
    RemoveEntryList(&r->Link);
    t->NumRecords--;
    if (r->Type == JREC_REVOKE)
        t->NumRevokes--;
    else
        t->NumNormal--;
    if (r->bh->b_jrec == r)
        r->bh->b_jrec = NULL;
    r->bh->b_jrefs--;
    if (t->State == JTXN_COMMITTED)
        J->CheckpointRecords--;
}

/*
 * Stop the journal without a word to the disk: another node has the volume
 * (multi-mount protection), so not even the superblock may be written.
 */
VOID
Ext2JournalAbortQuiet(IN PEXT2_VCB Vcb)
{
    PEXT2_JOURNAL   J = Vcb->Journal;
    KIRQL           irql;

    /* for good this mount, as Linux's aborted file system refuses a
       remount read-write (Ext2ProcessVolumeProperty) */
    SetLongFlag(Vcb->Flags, VCB_WRITE_STOPPED);
    SetLongFlag(Vcb->Flags, VCB_READ_ONLY);
    if (J == NULL) {
        return;
    }
    JnlLock(J, irql);
    SetFlag(J->Flags, JF_ABORTED);
    JnlUnlock(J, irql);
    if (J->Log) {
        InterlockedOr((volatile LONG *)&J->Log->Flags, JLOG_ABORTED);
    }
    KeSetEvent(&J->UnlockedEvent, 0, FALSE);
}

/*
 * A change half made can be neither finished nor undone (a rollback
 * failed): what the running transaction holds must never commit. The
 * journal stops and the volume turns read-only; the disk keeps the last
 * committed state, which is whole, so nothing is marked on it. Without a
 * journal the half change may already be there: e2fsck is asked for.
 * Callable inside the group locks (the superblock's lock is taken inside
 * them).
 */
VOID
Ext2JournalAbandon(IN PEXT2_VCB Vcb, IN NTSTATUS Status)
{
    if (IsFlagOn(Vcb->Flags, VCB_READ_ONLY)) {
        return;     /* stopped before (every stop aborts the journal too) */
    }
    DbgPrint("ext4: a change could not be completed nor undone (status %08x), "
             "the volume is read-only from now on\n", Status);
    Ext2JournalAbortQuiet(Vcb);
    if (Vcb->Journal == NULL) {
        InterlockedOr16((volatile SHORT *)&Vcb->SuperBlock->s_state, EXT4_ERROR_FS);
        Ext2QueueSuperWrite(Vcb);   /* the caller holds group locks */
    }
}

/*
 * An aborted journal records why in its own superblock (Linux:
 * jbd2_journal_update_sb_errno at the abort). Called by the superblock
 * writer (Ext2QueueSuperWrite), never by the aborting thread, which may be
 * unable to wait for a write. A journal stopped quietly (s_errors, MMP)
 * has no error of its own to record.
 */
VOID
Ext2JournalRecordAbort(IN PEXT2_VCB Vcb)
{
    PEXT2_JOURNAL   J = Vcb->Journal;
    PEXT2_JLOG      L;

    if (J == NULL || J->Log == NULL) {
        return;
    }
    L = J->Log;
    if (L->Errno != 0 && JfGet32(L->Super + JF_S_ERRNO) != (ULONG)L->Errno) {
        (void)JlogRecordErrno(L);
    }
}

VOID JnlAbort(PEXT2_JOURNAL J, NTSTATUS Status)
{
    KIRQL   irql;
    BOOLEAN first;

    JnlLock(J, irql);
    first = !(J->Flags & JF_ABORTED);
    SetFlag(J->Flags, JF_ABORTED);
    JnlUnlock(J, irql);

    if (!first)
        return;

    DEBUG(DL_ERR, ("Ext2Jnl: journal aborted (%xh), volume is now read-only\n", Status));
    DbgPrint("ext4: journal aborted (status %08x), volume is now read-only\n", Status);

    SetLongFlag(J->Vcb->Flags, VCB_WRITE_STOPPED);
    SetLongFlag(J->Vcb->Flags, VCB_READ_ONLY);
    InterlockedOr((volatile LONG *)&J->Log->Flags, JLOG_ABORTED);
    J->Log->Errno = -EIO;

    /* let e2fsck know: the flag is one bit set, which a concurrent writer
       cannot lose. Both superblocks are written by a worker - this thread
       may be joining a handle inside paging I/O, where it cannot wait for
       a write (Ext2QueueSuperWrite). */
    InterlockedOr16((volatile SHORT *)&J->Vcb->SuperBlock->s_state, EXT4_ERROR_FS);
    Ext2QueueSuperWrite(J->Vcb);

    KeSetEvent(&J->UnlockedEvent, 0, FALSE);
    KeSetEvent(&J->UpdatesEvent, 0, FALSE);
}

/*
 * Fill a descriptor block and the data blocks that follow it in the I/O
 * buffer. Returns the number of data blocks placed; *Iter advances.
 * Layout and flags follow Linux jbd2_journal_commit_transaction().
 */
static ULONG JnlFillGroup(PEXT2_JOURNAL J, PEXT2_JTXN t, PLIST_ENTRY *Iter,
                          PUCHAR Buffer, ULONG *Crc32Sum)
{
    PEXT2_JLOG  L = J->Log;
    ULONG       bs = J->BlockSize;
    ULONG       tag_bytes = JfTagBytes(L->Incompat);
    ULONG       csum_size = JfHasCsumV2V3(L->Incompat) ? JF_TAIL_SIZE : 0;
    PUCHAR      tagp = Buffer + JF_HEADER_SIZE;
    LONG        space_left = (LONG)(bs - JF_HEADER_SIZE);
    PUCHAR      tag = NULL;
    ULONG       n = 0;
    BOOLEAN     first = TRUE;

    RtlZeroMemory(Buffer, bs);
    JfPut32(Buffer + JF_H_MAGIC, JF_MAGIC);
    JfPut32(Buffer + JF_H_TYPE, JF_DESCRIPTOR);
    JfPut32(Buffer + JF_H_SEQUENCE, t->Tid);

    while (*Iter != &t->Records && n < J->GroupBlocks) {

        PEXT2_JREC  r = CONTAINING_RECORD(*Iter, EXT2_JREC, Link);
        PUCHAR      dst, src;
        ULONG       flags = 0;

        if (r->Type != JREC_NORMAL) {
            *Iter = (*Iter)->Flink;
            continue;
        }

        if (space_left < (LONG)(tag_bytes + JF_UUID_SIZE + csum_size))
            break;

        *Iter = (*Iter)->Flink;

        src = JnlRecordData(r);
        dst = Buffer + ((ULONGLONG)(n + 1) << J->BlockBits);
        RtlCopyMemory(dst, src, bs);

        /* a block that would read as a header is logged escaped */
        if (JfGet32(dst) == JF_MAGIC) {
            JfPut32(dst, 0);
            flags |= JF_FLAG_ESCAPE;
        }
        if (!first)
            flags |= JF_FLAG_SAME_UUID;

        tag = tagp;
        JfPut32(tag + JF_T_BLOCK, (ULONG)r->Block);
        if (L->Incompat & JF_INCOMPAT_64BIT)
            JfPut32(tag + JF_T_BLOCK_HIGH, (ULONG)(r->Block >> 32));
        JfPut16(tag + JF_T_FLAGS, flags);

        if (csum_size) {
            UCHAR id[4];
            ULONG sum;

            JfPut32(id, t->Tid);
            sum = crc32c(crc32c(L->CsumSeed, id, 4), dst, bs);
            if (L->Incompat & JF_INCOMPAT_CSUM_V3)
                JfPut32(tag + JF_T_CHECKSUM32, sum);
            else
                JfPut16(tag + JF_T_CHECKSUM16, sum & 0xFFFF);
        }

        tagp += tag_bytes;
        space_left -= tag_bytes;

        if (first) {
            RtlCopyMemory(tagp, L->Super + JF_S_UUID, JF_UUID_SIZE);
            tagp += JF_UUID_SIZE;
            space_left -= JF_UUID_SIZE;
            first = FALSE;
        }
        n++;
    }

    if (n == 0)
        return 0;

    JfPut16(tag + JF_T_FLAGS, JfGet16(tag + JF_T_FLAGS) | JF_FLAG_LAST_TAG);

    if (csum_size)
        JfPut32(Buffer + bs - JF_TAIL_SIZE, crc32c(L->CsumSeed, Buffer, bs));

    if (L->Compat & JF_COMPAT_CHECKSUM) {
        ULONG i;
        for (i = 0; i <= n; i++)
            *Crc32Sum = JrCrc32Be(*Crc32Sum, Buffer + ((ULONGLONG)i << J->BlockBits), bs);
    }

    return n;
}

/*
 * Fill one revoke block. Returns the number of revoke records placed.
 */
static ULONG JnlFillRevokeBlock(PEXT2_JOURNAL J, PEXT2_JTXN t, PLIST_ENTRY *Iter, PUCHAR Buffer)
{
    PEXT2_JLOG  L = J->Log;
    ULONG       bs = J->BlockSize;
    ULONG       csum_size = JfHasCsumV2V3(L->Incompat) ? JF_TAIL_SIZE : 0;
    ULONG       rec = (L->Incompat & JF_INCOMPAT_64BIT) ? 8 : 4;
    ULONG       offset = JF_REVOKE_HEADER_SIZE;
    ULONG       n = 0;

    RtlZeroMemory(Buffer, bs);
    JfPut32(Buffer + JF_H_MAGIC, JF_MAGIC);
    JfPut32(Buffer + JF_H_TYPE, JF_REVOKE);
    JfPut32(Buffer + JF_H_SEQUENCE, t->Tid);

    while (*Iter != &t->Records) {

        PEXT2_JREC r = CONTAINING_RECORD(*Iter, EXT2_JREC, Link);

        if (r->Type != JREC_REVOKE) {
            *Iter = (*Iter)->Flink;
            continue;
        }
        if (offset + rec > bs - csum_size)
            break;

        *Iter = (*Iter)->Flink;
        if (rec == 8)
            JfPut64(Buffer + offset, r->Block);
        else
            JfPut32(Buffer + offset, (ULONG)r->Block);
        offset += rec;
        n++;
    }

    if (n == 0)
        return 0;

    JfPut32(Buffer + JF_R_COUNT, offset);
    if (csum_size)
        JfPut32(Buffer + bs - JF_TAIL_SIZE, crc32c(L->CsumSeed, Buffer, bs));
    return n;
}

static VOID JnlFillCommitBlock(PEXT2_JOURNAL J, PEXT2_JTXN t, PUCHAR Buffer, ULONG Crc32Sum)
{
    PEXT2_JLOG  L = J->Log;
    LONGLONG    now = JnlNow() - 116444736000000000LL;     /* 100ns since 1970 */

    RtlZeroMemory(Buffer, J->BlockSize);
    JfPut32(Buffer + JF_H_MAGIC, JF_MAGIC);
    JfPut32(Buffer + JF_H_TYPE, JF_COMMIT);
    JfPut32(Buffer + JF_H_SEQUENCE, t->Tid);

    if (L->Compat & JF_COMPAT_CHECKSUM) {
        Buffer[JF_C_CHECKSUM_TYPE] = JF_CRC32_CHECKSUM;
        Buffer[JF_C_CHECKSUM_SIZE] = JF_CRC32_CHECKSUM_SIZE;
        JfPut32(Buffer + JF_C_CHECKSUM, Crc32Sum);
    }

    if (now < 0)
        now = 0;
    JfPut64(Buffer + JF_C_COMMIT_SEC, (ULONGLONG)(now / TICKSPERSEC));
    JfPut32(Buffer + JF_C_COMMIT_NSEC, (ULONG)((now % TICKSPERSEC) * EXT2_NS_PER_TICK));

    /* v2/v3: the block's own crc32c, taken with the checksum word zero */
    if (JfHasCsumV2V3(L->Incompat))
        JfPut32(Buffer + JF_C_CHECKSUM, crc32c(L->CsumSeed, Buffer, J->BlockSize));
}

/*
 * Make sure the log has room for [Needed] blocks (plus slack), retiring
 * old transactions if it does not. Runs under CommitLock.
 */
static NTSTATUS JnlEnsureLogSpace(PEXT2_JOURNAL J, ULONG Needed)
{
    KIRQL irql;
    ULONG tries = 0;

    for (;;) {
        unsigned long free_blocks;
        BOOLEAN       empty;
        NTSTATUS      Status;

        JnlLock(J, irql);
        free_blocks = J->Log->Free;
        empty = IsListEmpty(&J->Checkpoint);
        JnlUnlock(J, irql);

        if (free_blocks >= Needed + JNL_LOG_SLACK)
            return STATUS_SUCCESS;

        if (empty || tries++ > 200) {
            DbgPrint("ext4: journal: transaction of %u blocks does not fit "
                     "(free %lu of %lu, checkpoint list %s, %u tries)\n",
                     Needed, free_blocks, J->Log->MaxLen,
                     empty ? "empty" : "not empty", tries);
            return STATUS_DISK_FULL;
        }

        Status = JnlCheckpointOldest(J, TRUE, FALSE);
        if (!NT_SUCCESS(Status) && Status != STATUS_PENDING)
            return Status;
        if (Status == STATUS_PENDING) {
            /* the oldest transaction is pinned by the running one: the
               only remaining space is what is free right now */
            JnlLock(J, irql);
            free_blocks = J->Log->Free;
            JnlUnlock(J, irql);
            if (free_blocks >= Needed + JNL_LOG_SLACK)
                return STATUS_SUCCESS;
            DbgPrint("ext4: journal: transaction of %u blocks does not fit "
                     "(free %lu of %lu, oldest pinned by the running one)\n",
                     Needed, free_blocks, J->Log->MaxLen);
            return STATUS_DISK_FULL;
        }
    }
}

/*
 * Commit the running transaction. Runs under CommitLock, at PASSIVE_LEVEL,
 * with no journal handle held by the caller.
 */
NTSTATUS JnlCommitTransaction(PEXT2_JOURNAL J)
{
    PEXT2_JLOG      L = J->Log;
    PEXT2_JTXN      t, next = NULL;
    KIRQL           irql;
    NTSTATUS        Status = STATUS_SUCCESS;
    PLIST_ENTRY     e;
    PUCHAR          snapshot = NULL;
    ULONG           normal, revokes, needed, n;
    ULONG           crc32_sum = ~0u;
    BOOLEAN         unlocked = FALSE;

    /* 0. everything that does disk I/O of its own happens before the
       transaction is locked, so that the sealed window (during which
       modifications wait) is only a memcpy long. The tail can only move
       under CommitLock, which we hold; the marker is written once. */
    JnlLock(J, irql);
    t = J->Running;
    if (t->NumRecords == 0) {
        /* nothing to commit (the interval runs from the first record) */
        JnlUnlock(J, irql);
        return STATUS_SUCCESS;
    }
    normal  = t->NumNormal;
    revokes = t->NumRevokes;
    JnlUnlock(J, irql);

    if (!JnlMarkSuper(J, TRUE)) {
        Status = STATUS_UNEXPECTED_IO_ERROR;
        goto errorout;
    }

    /* the tail on disk, if this transaction - at its largest - could reach it */
    Status = JnlPublishTailIfNeeded(J, JnlLogBlocksNeeded(J, normal + J->MaxTxnBlocks, revokes + 64));
    if (!NT_SUCCESS(Status))
        goto errorout;

    /* room for what is there now plus what the open handles may add;
       best effort here (old transactions pinned by the running one can
       only be retired after this commit), the exact check comes later */
    needed = JnlLogBlocksNeeded(J, normal + J->MaxTxnBlocks / 4, revokes + 64);
    (void)JnlEnsureLogSpace(J, needed);

    next = JnlAllocTxn(J, t->Tid + 1);
    if (!next) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto errorout;
    }

    /* 1. lock the running transaction: joins are still allowed until
       the active handles have drained */
    JnlLock(J, irql);
    if (t->NumRecords == 0) {
        JnlUnlock(J, irql);
        JnlFreeTxn(J, next);
        return STATUS_SUCCESS;
    }
    t->State = JTXN_LOCKED;
    JnlUnlock(J, irql);

    /* 2. wait for handles in flight to finish - never snapshot buffers
       while a metadata writer is active. They all finish: no commit runs
       on a thread that holds a handle (its callers detach first, or are
       the journal thread), a handle holder never waits for the journal,
       and whoever commits holds no lock a handle holder waits for (a
       file's fsync commits with none held, a volume flush holds the
       volume exclusively, which no handle holder takes). The last one
       out signals UpdatesEvent under the lock this check is made under
       (JnlDetach), so the signal is not missed. */
    for (;;) {
        JnlLock(J, irql);
        if (t->Updates <= 0) {
            /* seal: from now on modifications wait for the next txn */
            t->Sealed = TRUE;
            KeClearEvent(&J->UnlockedEvent);
            JnlUnlock(J, irql);
            break;
        }
        JnlUnlock(J, irql);
        KeWaitForSingleObject(&J->UpdatesEvent, Executive, KernelMode, FALSE, NULL);
    }

    /* 3. the block set is now frozen: nobody modifies these buffers */
    JnlLock(J, irql);
    normal  = t->NumNormal;
    revokes = t->NumRevokes;
    for (e = t->Records.Flink; e != &t->Records; e = e->Flink) {
        PEXT2_JREC r = CONTAINING_RECORD(e, EXT2_JREC, Link);
        r->bh->b_jrec = NULL;   /* later dirtying goes to the next txn */
    }
    JnlUnlock(J, irql);

    /* exact size now; only a transaction that outgrew the estimate has to
       make room while sealed */
    needed = JnlLogBlocksNeeded(J, normal, revokes);
    Status = JnlEnsureLogSpace(J, needed);
    if (NT_SUCCESS(Status))
        Status = JnlPublishTailIfNeeded(J, needed);
    if (!NT_SUCCESS(Status))
        goto errorout;

    /* 4. snapshot the data so new handles can start while we write */
    if (normal) {
        snapshot = Ext2AllocatePool(NonPagedPool, (SIZE_T)normal << J->BlockBits, JNL_TAG);
        if (snapshot) {
            PUCHAR p = snapshot;
            for (e = t->Records.Flink; e != &t->Records; e = e->Flink) {
                PEXT2_JREC r = CONTAINING_RECORD(e, EXT2_JREC, Link);
                if (r->Type != JREC_NORMAL)
                    continue;
                RtlCopyMemory(p, r->bh->b_data, J->BlockSize);
                r->Snapshot = p;
                p += J->BlockSize;
            }
        }
    }

    JnlLock(J, irql);
    t->State = JTXN_COMMITTING;
    t->LogStart = L->Head;
    L->TransactionSequence = next->Tid;
    if (snapshot || normal == 0) {
        J->Running = next;
        next = NULL;
        unlocked = TRUE;
    }
    JnlUnlock(J, irql);
    if (unlocked)
        KeSetEvent(&J->UnlockedEvent, 0, FALSE);

    /* 5. revoke blocks */
    e = t->Records.Flink;
    while (revokes) {
        unsigned long log;
        n = JnlFillRevokeBlock(J, t, &e, J->IoBuffer);
        if (n == 0)
            break;
        revokes -= n;
        JnlLock(J, irql);
        log = JnlNextLogBlock(J);
        JnlUnlock(J, irql);
        Status = JnlWriteLogRun(J, log, 1, J->IoBuffer, FALSE);
        if (!NT_SUCCESS(Status))
            goto errorout;
    }

    /* 6. descriptor + data groups */
    e = t->Records.Flink;
    while (normal) {
        unsigned long log;
        ULONG i;
        n = JnlFillGroup(J, t, &e, J->IoBuffer, &crc32_sum);
        if (n == 0)
            break;
        normal -= n;
        JnlLock(J, irql);
        log = JnlNextLogBlock(J);
        for (i = 0; i < n; i++)
            JnlNextLogBlock(J);
        JnlUnlock(J, irql);
        Status = JnlWriteLogRun(J, log, n + 1, J->IoBuffer, FALSE);
        if (!NT_SUCCESS(Status))
            goto errorout;
    }

    /* 7. barrier, then the commit block written through (FUA): it is durable
       when the write completes, as jbd2 has it with REQ_PREFLUSH | REQ_FUA.
       A device without FUA gets a cache flush after the write from the
       class driver instead; a second flush here cost every commit (and
       every fsync) one more device round trip for nothing. */
    Status = Ext2FlushDisk(J->Vcb);
    if (!NT_SUCCESS(Status))
        goto errorout;

    {
        unsigned long log;
        JnlFillCommitBlock(J, t, J->IoBuffer, crc32_sum);
        JnlLock(J, irql);
        log = JnlNextLogBlock(J);
        JnlUnlock(J, irql);
        Status = JnlWriteLogRun(J, log, 1, J->IoBuffer, TRUE);
        if (!NT_SUCCESS(Status))
            goto errorout;
    }

    /* 8. committed: move to the checkpoint list. Every block in it now
       has a committed copy in the log newer than any older transaction's:
       that is what lets the checkpoint retire those older records without
       writing them (jbd2 moves the buffer's b_cp_transaction the same way). */
    JnlLock(J, irql);
    t->State = JTXN_COMMITTED;
    InsertTailList(&J->Checkpoint, &t->Link);
    J->CheckpointRecords += t->NumRecords;
    J->CommitSequence = t->Tid;
    L->CommitSequence = t->Tid;
    if (!unlocked) {
        J->Running = next;
        next = NULL;
        unlocked = TRUE;
    }
    for (e = t->Records.Flink; e != &t->Records; e = e->Flink) {
        PEXT2_JREC r = CONTAINING_RECORD(e, EXT2_JREC, Link);
        r->Snapshot = NULL;
        r->bh->b_jctid = t->Tid;
    }
    JnlUnlock(J, irql);
    KeSetEvent(&J->UnlockedEvent, 0, FALSE);

    KeQuerySystemTime(&J->LastCommitTime);

    /* the log is fuller and the idle clock starts again: the thread looks
       at both (when it committed this itself, one look more) */
    KeSetEvent(&J->WakeEvent, 0, FALSE);

    DEBUG(DL_JNL, ("Ext2Jnl: committed tid %u (%u blocks, %u revokes) head=%lu free=%lu\n",
                   t->Tid, t->NumNormal, t->NumRevokes, L->Head, L->Free));

errorout:

    if (snapshot)
        Ext2FreePool(snapshot, JNL_TAG);

    if (!NT_SUCCESS(Status)) {
        /* the log write failed: the transaction is parked on the
           checkpoint list (unmarked as committed) so its buffers are
           released at stop; the journal is aborted and the volume goes
           read-only. If it was never sealed, unlock it instead. */
        JnlLock(J, irql);
        if (!t->Sealed && !unlocked) {
            t->State = JTXN_RUNNING;
        } else {
            t->State = JTXN_COMMITTED;
            for (e = t->Records.Flink; e != &t->Records; e = e->Flink)
                CONTAINING_RECORD(e, EXT2_JREC, Link)->Snapshot = NULL;
            InsertTailList(&J->Checkpoint, &t->Link);
            J->CheckpointRecords += t->NumRecords;
            if (!unlocked && next) {
                J->Running = next;
                next = NULL;
            }
        }
        JnlUnlock(J, irql);
        if (next)
            JnlFreeTxn(J, next);
        JnlAbort(J, Status);
    }

    return Status;
}
