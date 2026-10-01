/**
 * jnl_commit.c - transaction commit: descriptor, revoke and commit blocks.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include <linux/jbd2.h>
#include "jnl_internal.h"

u32 crc32_be(u32 crc, unsigned char const *p, size_t len);

#define JTXN_COMMITTING     3           /* log I/O in progress */
#define JTXN_COMMITTED      4           /* on the checkpoint list */

#define JNL_DRAIN_TIMEOUT       ((LONGLONG)30 * 10 * 1000 * 1000)  /* 30 s  */

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
        KeQuerySystemTime(&t->StartTime);
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

    SetLongFlag(Vcb->Flags, VCB_READ_ONLY);
    if (J == NULL) {
        return;
    }
    JnlLock(J, irql);
    SetFlag(J->Flags, JF_ABORTED);
    JnlUnlock(J, irql);
    if (J->jbd) {
        J->jbd->j_flags |= JBD2_ABORT;
    }
    KeSetEvent(&J->UnlockedEvent, 0, FALSE);
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

    SetLongFlag(J->Vcb->Flags, VCB_READ_ONLY);
    J->jbd->j_flags |= JBD2_ABORT;
    J->jbd->j_errno = -EIO;

    /* let e2fsck know */
    J->Vcb->SuperBlock->s_state |= EXT4_ERROR_FS;
    Ext2SaveSuperDirect(J->Vcb);
    jbd2_journal_update_sb_errno(J->jbd);

    KeSetEvent(&J->UnlockedEvent, 0, FALSE);
    KeSetEvent(&J->UpdatesEvent, 0, FALSE);
}

/*
 * Fill a descriptor block and the data blocks that follow it in the I/O
 * buffer. Returns the number of data blocks placed; *Iter advances.
 * Layout and flags follow Linux jbd2_journal_commit_transaction().
 */
static ULONG JnlFillGroup(PEXT2_JOURNAL J, PEXT2_JTXN t, PLIST_ENTRY *Iter,
                          PUCHAR Buffer, __u32 *Crc32Sum)
{
    journal_t           *jbd = J->jbd;
    ULONG                bs = J->BlockSize;
    ULONG                tag_bytes = (ULONG)journal_tag_bytes(jbd);
    ULONG                csum_size = jbd2_journal_has_csum_v2or3(jbd) ? sizeof(struct jbd2_journal_block_tail) : 0;
    journal_header_t    *header = (journal_header_t *)Buffer;
    PUCHAR               tagp = Buffer + sizeof(journal_header_t);
    LONG                 space_left = (LONG)(bs - sizeof(journal_header_t));
    journal_block_tag_t *tag = NULL;
    ULONG                n = 0;
    BOOLEAN              first = TRUE;

    RtlZeroMemory(Buffer, bs);
    header->h_magic     = cpu_to_be32(JBD2_MAGIC_NUMBER);
    header->h_blocktype = cpu_to_be32(JBD2_DESCRIPTOR_BLOCK);
    header->h_sequence  = cpu_to_be32(t->Tid);

    while (*Iter != &t->Records && n < J->GroupBlocks) {

        PEXT2_JREC  r = CONTAINING_RECORD(*Iter, EXT2_JREC, Link);
        PUCHAR      dst, src;
        ULONG       flags = 0;
        __be32      seq;
        __u32       csum32;

        if (r->Type != JREC_NORMAL) {
            *Iter = (*Iter)->Flink;
            continue;
        }

        if (space_left < (LONG)(tag_bytes + 16 + csum_size))
            break;

        *Iter = (*Iter)->Flink;

        src = JnlRecordData(r);
        dst = Buffer + ((ULONGLONG)(n + 1) << J->BlockBits);
        RtlCopyMemory(dst, src, bs);

        if (*((__be32 *)dst) == cpu_to_be32(JBD2_MAGIC_NUMBER)) {
            *((__be32 *)dst) = 0;
            flags |= JBD2_FLAG_ESCAPE;
        }
        if (!first)
            flags |= JBD2_FLAG_SAME_UUID;

        tag = (journal_block_tag_t *)tagp;
        tag->t_blocknr = cpu_to_be32((__u32)(r->Block & 0xffffffffULL));
        if (jbd2_has_feature_64bit(jbd))
            tag->t_blocknr_high = cpu_to_be32((__u32)((r->Block >> 31) >> 1));
        tag->t_flags = cpu_to_be16((__u16)flags);

        if (jbd2_journal_has_csum_v2or3(jbd)) {
            seq = cpu_to_be32(t->Tid);
            csum32 = jbd2_chksum(jbd, jbd->j_csum_seed, (__u8 *)&seq, sizeof(seq));
            csum32 = jbd2_chksum(jbd, csum32, dst, bs);
            if (jbd2_has_feature_csum3(jbd))
                ((journal_block_tag3_t *)tag)->t_checksum = cpu_to_be32(csum32);
            else
                tag->t_checksum = cpu_to_be16((__u16)csum32);
        }

        tagp += tag_bytes;
        space_left -= tag_bytes;

        if (first) {
            RtlCopyMemory(tagp, jbd->j_superblock->s_uuid, 16);
            tagp += 16;
            space_left -= 16;
            first = FALSE;
        }
        n++;
    }

    if (n == 0)
        return 0;

    tag->t_flags |= cpu_to_be16(JBD2_FLAG_LAST_TAG);

    if (csum_size) {
        struct jbd2_journal_block_tail *tail =
            (struct jbd2_journal_block_tail *)(Buffer + bs - csum_size);
        tail->t_checksum = 0;
        tail->t_checksum = cpu_to_be32(jbd2_chksum(jbd, jbd->j_csum_seed, Buffer, bs));
    }

    if (jbd2_has_feature_checksum(jbd)) {
        ULONG i;
        for (i = 0; i <= n; i++)
            *Crc32Sum = crc32_be(*Crc32Sum, Buffer + ((ULONGLONG)i << J->BlockBits), bs);
    }

    return n;
}

/*
 * Fill one revoke block. Returns the number of revoke records placed.
 */
static ULONG JnlFillRevokeBlock(PEXT2_JOURNAL J, PEXT2_JTXN t, PLIST_ENTRY *Iter, PUCHAR Buffer)
{
    journal_t                    *jbd = J->jbd;
    ULONG                         bs = J->BlockSize;
    ULONG                         csum_size = jbd2_journal_has_csum_v2or3(jbd) ? sizeof(struct jbd2_journal_block_tail) : 0;
    ULONG                         rec = jbd2_has_feature_64bit(jbd) ? 8 : 4;
    jbd2_journal_revoke_header_t *header = (jbd2_journal_revoke_header_t *)Buffer;
    ULONG                         offset = sizeof(jbd2_journal_revoke_header_t);
    ULONG                         n = 0;

    RtlZeroMemory(Buffer, bs);
    header->r_header.h_magic     = cpu_to_be32(JBD2_MAGIC_NUMBER);
    header->r_header.h_blocktype = cpu_to_be32(JBD2_REVOKE_BLOCK);
    header->r_header.h_sequence  = cpu_to_be32(t->Tid);

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
            *((__be64 *)(Buffer + offset)) = cpu_to_be64(r->Block);
        else
            *((__be32 *)(Buffer + offset)) = cpu_to_be32((__u32)r->Block);
        offset += rec;
        n++;
    }

    if (n == 0)
        return 0;

    header->r_count = cpu_to_be32(offset);

    if (csum_size) {
        struct jbd2_journal_block_tail *tail =
            (struct jbd2_journal_block_tail *)(Buffer + bs - csum_size);
        tail->t_checksum = 0;
        tail->t_checksum = cpu_to_be32(jbd2_chksum(jbd, jbd->j_csum_seed, Buffer, bs));
    }
    return n;
}

static VOID JnlFillCommitBlock(PEXT2_JOURNAL J, PEXT2_JTXN t, PUCHAR Buffer, __u32 Crc32Sum)
{
    journal_t            *jbd = J->jbd;
    struct commit_header *h = (struct commit_header *)Buffer;
    LONGLONG              now = JnlNow() - 116444736000000000LL;   /* 100ns since 1970 */

    RtlZeroMemory(Buffer, J->BlockSize);
    h->h_magic     = cpu_to_be32(JBD2_MAGIC_NUMBER);
    h->h_blocktype = cpu_to_be32(JBD2_COMMIT_BLOCK);
    h->h_sequence  = cpu_to_be32(t->Tid);

    if (jbd2_has_feature_checksum(jbd)) {
        h->h_chksum_type = JBD2_CRC32_CHKSUM;
        h->h_chksum_size = JBD2_CRC32_CHKSUM_SIZE;
        h->h_chksum[0]   = cpu_to_be32(Crc32Sum);
    }

    if (now < 0)
        now = 0;
    h->h_commit_sec  = cpu_to_be64((__u64)(now / 10000000LL));
    h->h_commit_nsec = cpu_to_be32((__u32)((now % 10000000LL) * 100));

    if (jbd2_journal_has_csum_v2or3(jbd)) {
        h->h_chksum_type = 0;
        h->h_chksum_size = 0;
        h->h_chksum[0]   = 0;
        h->h_chksum[0]   = cpu_to_be32(jbd2_chksum(jbd, jbd->j_csum_seed, Buffer, J->BlockSize));
    }
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
        free_blocks = J->jbd->j_free;
        empty = IsListEmpty(&J->Checkpoint);
        JnlUnlock(J, irql);

        if (free_blocks >= Needed + JNL_LOG_SLACK)
            return STATUS_SUCCESS;

        if (empty || tries++ > 200) {
            DbgPrint("ext4: journal: transaction of %u blocks does not fit "
                     "(free %lu of %lu, checkpoint list %s, %u tries)\n",
                     Needed, free_blocks, J->jbd->j_maxlen,
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
            free_blocks = J->jbd->j_free;
            JnlUnlock(J, irql);
            if (free_blocks >= Needed + JNL_LOG_SLACK)
                return STATUS_SUCCESS;
            DbgPrint("ext4: journal: transaction of %u blocks does not fit "
                     "(free %lu of %lu, oldest pinned by the running one)\n",
                     Needed, free_blocks, J->jbd->j_maxlen);
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
    journal_t      *jbd = J->jbd;
    PEXT2_JTXN      t, next = NULL;
    KIRQL           irql;
    LARGE_INTEGER   tick;
    NTSTATUS        Status = STATUS_SUCCESS;
    PLIST_ENTRY     e;
    PUCHAR          snapshot = NULL;
    ULONG           normal, revokes, needed, n;
    __u32           crc32_sum = ~0u;
    BOOLEAN         unlocked = FALSE;

    tick.QuadPart = -JNL_WAIT_TICK;

    /* 0. everything that does disk I/O of its own happens before the
       transaction is locked, so that the sealed window (during which
       modifications wait) is only a memcpy long. The tail can only move
       under CommitLock, which we hold; the marker is written once. */
    JnlLock(J, irql);
    t = J->Running;
    if (t->NumRecords == 0) {
        /* nothing to do; just restart the clock */
        KeQuerySystemTime(&t->StartTime);
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

    Status = JnlPublishTail(J);
    if (!NT_SUCCESS(Status))
        goto errorout;

    /* room for what is there now plus what the open handles may add;
       best effort here (old transactions pinned by the running one can
       only be retired after this commit), the exact check comes later */
    needed = JnlLogBlocksNeeded(J, normal + J->MaxTxnBlocks / 4, revokes + 64);
    JnlEnsureLogSpace(J, needed);

    next = JnlAllocTxn(J, t->Tid + 1);
    if (!next) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto errorout;
    }

    /* 1. lock the running transaction: joins are still allowed until
       the active handles have drained */
    JnlLock(J, irql);
    if (t->NumRecords == 0) {
        KeQuerySystemTime(&t->StartTime);
        JnlUnlock(J, irql);
        JnlFreeTxn(J, next);
        return STATUS_SUCCESS;
    }
    t->State = JTXN_LOCKED;
    JnlUnlock(J, irql);

    /* 2. wait for handles in flight to finish. A handle holder never
       waits for the journal, so this terminates unless a flusher waits
       for us while holding a resource a handle holder needs - a bug in
       the caller, which we report and survive instead of hanging. */
    {
        LONGLONG deadline = JnlNow() + JNL_DRAIN_TIMEOUT;
        for (;;) {
            LONG updates;
            JnlLock(J, irql);
            updates = t->Updates;
            if (updates <= 0) {
                /* seal: from now on modifications wait for the next txn */
                t->Sealed = TRUE;
                KeClearEvent(&J->UnlockedEvent);
                JnlUnlock(J, irql);
                break;
            }
            JnlUnlock(J, irql);
            /* Never snapshot buffers while a metadata writer is active. */
            if (JnlNow() > deadline) {
                Status = STATUS_IO_TIMEOUT;
                goto errorout;
            }
            KeWaitForSingleObject(&J->UpdatesEvent, Executive, KernelMode, FALSE, &tick);
        }
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
    t->LogStart = jbd->j_head;
    jbd->j_transaction_sequence = next->Tid;
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
    jbd->j_commit_sequence = t->Tid;
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

    DEBUG(DL_JNL, ("Ext2Jnl: committed tid %u (%u blocks, %u revokes) head=%lu free=%lu\n",
                   t->Tid, t->NumNormal, t->NumRevokes, jbd->j_head, jbd->j_free));

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
