/**
 * JournalLog.c - journal log geometry, log writes and superblock markers.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "JournalInternal.h"

/*
 * Take the next block of the log (wrapping at Last). Caller holds
 * J->Lock and made sure there is room.
 */
unsigned long JnlNextLogBlock(PEXT2_JOURNAL J)
{
    PEXT2_JLOG    L = J->Log;
    unsigned long blk = L->Head;

    ASSERT(L->Free > 0);
    L->Head++;
    L->Free--;
    if (L->Head == L->Last)
        L->Head = L->First;
    return blk;
}

/*
 * Number of log blocks a transaction needs: revoke blocks, descriptor
 * blocks, data blocks and the commit block. Runs under J->Lock: nothing
 * pageable (the journal superblock is) may be touched here.
 */
ULONG JnlLogBlocksNeeded(PEXT2_JOURNAL J, ULONG Normal, ULONG Revokes)
{
    ULONG      per_revoke = max(J->RevokesPerBlock, 1);
    ULONG      total = 1; /* commit block */

    if (Revokes)
        total += (Revokes + per_revoke - 1) / per_revoke;
    if (Normal)
        total += Normal + (Normal + J->GroupBlocks - 1) / J->GroupBlocks;
    return total;
}

/*
 * Advance the in-memory log tail past the transactions that are gone.
 * Caller holds J->Lock. The on-disk copy is published lazily, before the
 * next transaction is written (see JnlPublishTail).
 */
VOID JnlAdvanceTail(PEXT2_JOURNAL J)
{
    PEXT2_JLOG    L = J->Log;
    unsigned long block;
    tid_t         tid;
    unsigned long freed;

    if (!IsListEmpty(&J->Checkpoint)) {
        PEXT2_JTXN t = CONTAINING_RECORD(J->Checkpoint.Flink, EXT2_JTXN, Link);
        block = t->LogStart;
        tid   = t->Tid;
    } else {
        block = L->Head;
        tid   = L->TransactionSequence;
    }

    if (block == L->Tail && tid == L->TailSequence)
        return;

    freed = block - L->Tail;
    if (block < L->Tail)
        freed += L->Last - L->First;

    DEBUG(DL_JNL, ("Ext2Jnl: tail %lu/%u -> %lu/%u (freed %lu)\n",
                   L->Tail, L->TailSequence, block, tid, freed));

    L->Free += freed;
    L->Tail = block;
    L->TailSequence = tid;

    /* a handle that found the log full may fit now (JnlActivate) */
    KeSetEvent(&J->UnlockedEvent, IO_NO_INCREMENT, FALSE);
}

/*
 * Write [Count] consecutive log blocks starting at logical log block
 * [Log] from [Buffer], coalescing physically contiguous runs.
 */
NTSTATUS JnlWriteLogRun(PEXT2_JOURNAL J, unsigned long Log, ULONG Count,
                               PUCHAR Buffer, BOOLEAN WriteThrough)
{
    PEXT2_JLOG          L = J->Log;
    NTSTATUS            Status = STATUS_SUCCESS;
    ULONGLONG           phys;
    ULONG               run, done = 0;

    while (done < Count) {

        /* as far as the journal file is contiguous, and the ring allows */
        Status = JlogMap(L, Log, &phys, &run);
        if (!NT_SUCCESS(Status))
            return Status;
        run = min(run, Count - done);
        run = min(run, L->Last - Log);
        Log += run;
        if (Log >= L->Last)
            Log = L->First;

        Status = Ext2WriteDiskSync(J->Vcb, phys << J->BlockBits,
                                   run << J->BlockBits,
                                   Buffer + ((ULONGLONG)done << J->BlockBits),
                                   WriteThrough);
        if (!NT_SUCCESS(Status)) {
            DEBUG(DL_ERR, ("Ext2Jnl: log write failed at %I64u (%u blocks): %xh\n",
                           phys, run, Status));
            return Status;
        }
        done += run;
    }

    return Status;
}

/*
 * Publish the in-memory tail to the journal superblock if it differs from
 * what is on disk. Must happen before the log reuses freed space and
 * before the very first transaction is written into an empty journal.
 * The write flushes first (JlogSetTail): the checkpoint's home writes
 * reach the medium before the log stops covering them.
 */
NTSTATUS JnlPublishTail(PEXT2_JOURNAL J)
{
    PEXT2_JLOG    L = J->Log;
    KIRQL         irql;
    unsigned long tail;
    tid_t         seq;

    JnlLock(J, irql);
    tail = L->Tail;
    seq  = L->TailSequence;
    JnlUnlock(J, irql);

    if (!(L->Flags & JLOG_FLUSHED) && tail == J->DiskTail && seq == J->DiskTailSeq)
        return STATUS_SUCCESS;

    if (!NT_SUCCESS(JlogSetTail(L, seq, tail)))
        return STATUS_UNEXPECTED_IO_ERROR;

    J->DiskTail = tail;
    J->DiskTailSeq = seq;
    return STATUS_SUCCESS;
}

/*
 * Publish the tail only when it has to move, as jbd2's commit does: every
 * publication is a superblock write between two flushes, and doing it at
 * every commit tripled the device round trips of an fsync. The tail on disk
 * may lag behind the one in memory - recovery then replays transactions
 * already written home, which only writes the same blocks again, revokes
 * included - as long as the log never overwrites what that older tail still
 * covers. So it moves when the log on disk is empty (s_start 0: a new
 * transaction would be invisible to recovery), when fewer than Needed blocks
 * (plus the slack) remain before the head reaches it, or when a quarter of
 * the log has been freed behind it (jbd2 waits for max_txn_bufs), which
 * bounds what recovery replays.
 */
NTSTATUS JnlPublishTailIfNeeded(PEXT2_JOURNAL J, ULONG Needed)
{
    PEXT2_JLOG    L = J->Log;
    KIRQL         irql;
    unsigned long span, room, behind;
    BOOLEAN       publish;

    JnlLock(J, irql);
    span = L->Last - L->First;
    if ((L->Flags & JLOG_FLUSHED) || J->DiskTail == 0) {
        publish = TRUE;
    } else {
        room = J->DiskTail == L->Head ? span :
               (J->DiskTail + span - L->Head) % span;
        behind = (L->Tail + span - J->DiskTail) % span;
        publish = room < Needed + JNL_LOG_SLACK || behind >= span / 4;
    }
    JnlUnlock(J, irql);

    return publish ? JnlPublishTail(J) : STATUS_SUCCESS;
}

/* Ext2SaveSuperDirect, under SuperLock */
static BOOLEAN
JnlWriteSuper(IN PEXT2_VCB Vcb)
{
    NTSTATUS    Status;
    PUCHAR      buf;
    ULONGLONG   lba;
    ULONG       len, delta;

    ext4_superblock_csum_set(&Vcb->sb);

    lba   = SUPER_BLOCK_OFFSET & ~((ULONGLONG)SECTOR_SIZE - 1);
    delta = (ULONG)(SUPER_BLOCK_OFFSET - lba);
    len   = (ULONG)((delta + sizeof(EXT2_SUPER_BLOCK) + SECTOR_SIZE - 1) & ~((ULONGLONG)SECTOR_SIZE - 1));

    buf = Ext2AllocatePool(NonPagedPool, len, JNL_TAG);
    if (!buf)
        return FALSE;

    if (delta != 0 || len != sizeof(EXT2_SUPER_BLOCK)) {
        Status = Ext2ReadSync(Vcb, lba, len, buf);
        if (!NT_SUCCESS(Status)) {
            Ext2FreePool(buf, JNL_TAG);
            return FALSE;
        }
    }
    RtlCopyMemory(buf + delta, Vcb->SuperBlock, sizeof(EXT2_SUPER_BLOCK));

    Status = Ext2WriteDiskSync(Vcb, lba, len, buf, TRUE);
    Ext2FreePool(buf, JNL_TAG);
    if (NT_SUCCESS(Status))
        Status = Ext2FlushDisk(Vcb);

    return NT_SUCCESS(Status);
}

/*
 * Write the fs superblock (Vcb->SuperBlock) straight to disk, then flush.
 * Handles 4Kn media by a read-modify-write of the containing sector. Under
 * the superblock's own lock (Ext2LockSuper), without joining a transaction:
 * this is the journal writing it.
 */
BOOLEAN
Ext2SaveSuperDirect(IN PEXT2_VCB Vcb)
{
    BOOLEAN Done;

    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&Vcb->SuperLock, TRUE);
    Done = JnlWriteSuper(Vcb);
    ExReleaseResourceLite(&Vcb->SuperLock);
    KeLeaveCriticalRegion();

    return Done;
}

/*
 * Mark the fs as "mounted, needs recovery" or as cleanly unmounted, the
 * way Linux ext4_setup_super / ext4_put_super do for a journaled fs: the
 * needs_recovery feature flag is the marker, s_state is left alone (only
 * an unjournaled fs clears VALID_FS at mount). Both are written
 * synchronously.
 */
BOOLEAN JnlMarkSuper(PEXT2_JOURNAL J, BOOLEAN Dirty)
{
    PEXT2_VCB           Vcb = J->Vcb;
    PEXT2_SUPER_BLOCK   sb  = Vcb->SuperBlock;
    LARGE_INTEGER       SysTime;
    LONGLONG            Now;
    BOOLEAN             Written;

    KeQuerySystemTime(&SysTime);
    Now = Ext2UnixTime(&SysTime);

    if (Dirty ? (J->Flags & JF_SB_MARKED) != 0 : (J->Flags & JF_SB_MARKED) == 0) {
        return TRUE;
    }

    /* the fields change and are written under the superblock's lock */
    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&Vcb->SuperLock, TRUE);
    if (Dirty) {
        SetFlag(sb->s_feature_incompat, EXT4_FEATURE_INCOMPAT_RECOVER);
        sb->s_mnt_count++;
        Ext2SetSuperTime(sb, s_mtime, Now);
    } else {
        ClearFlag(sb->s_feature_incompat, EXT4_FEATURE_INCOMPAT_RECOVER);
        Ext2SetSuperTime(sb, s_wtime, Now);
    }
    Written = JnlWriteSuper(Vcb);
    ExReleaseResourceLite(&Vcb->SuperLock);
    KeLeaveCriticalRegion();

    if (!Written) {
        DEBUG(DL_ERR, ("Ext2Jnl: failed to write superblock marker (dirty=%d)\n", Dirty));
        return FALSE;
    }

    if (Dirty)
        SetFlag(J->Flags, JF_SB_MARKED);
    else
        ClearFlag(J->Flags, JF_SB_MARKED);

    return TRUE;
}
