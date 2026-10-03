/**
 * JournalLog.c - journal log geometry, log writes and superblock markers.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include <linux/jbd2.h>
#include "JournalInternal.h"

/*
 * Take the next block of the log (wrapping at j_last). Caller holds
 * J->Lock and made sure there is room.
 */
unsigned long JnlNextLogBlock(PEXT2_JOURNAL J)
{
    journal_t    *jbd = J->jbd;
    unsigned long blk = jbd->j_head;

    ASSERT(jbd->j_free > 0);
    jbd->j_head++;
    jbd->j_free--;
    if (jbd->j_head == jbd->j_last)
        jbd->j_head = jbd->j_first;
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
    journal_t    *jbd = J->jbd;
    unsigned long block;
    tid_t         tid;
    unsigned long freed;

    if (!IsListEmpty(&J->Checkpoint)) {
        PEXT2_JTXN t = CONTAINING_RECORD(J->Checkpoint.Flink, EXT2_JTXN, Link);
        block = t->LogStart;
        tid   = t->Tid;
    } else {
        block = jbd->j_head;
        tid   = jbd->j_transaction_sequence;
    }

    if (block == jbd->j_tail && tid == jbd->j_tail_sequence)
        return;

    freed = block - jbd->j_tail;
    if (block < jbd->j_tail)
        freed += jbd->j_last - jbd->j_first;

    DEBUG(DL_JNL, ("Ext2Jnl: tail %lu/%u -> %lu/%u (freed %lu)\n",
                   jbd->j_tail, jbd->j_tail_sequence, block, tid, freed));

    jbd->j_free += freed;
    jbd->j_tail = block;
    jbd->j_tail_sequence = tid;
}

/*
 * Write [Count] consecutive log blocks starting at logical log block
 * [Log] from [Buffer], coalescing physically contiguous runs.
 */
NTSTATUS JnlWriteLogRun(PEXT2_JOURNAL J, unsigned long Log, ULONG Count,
                               PUCHAR Buffer, BOOLEAN WriteThrough)
{
    journal_t          *jbd = J->jbd;
    NTSTATUS            Status = STATUS_SUCCESS;
    unsigned long long  phys, next;
    ULONG               run, done = 0;

    while (done < Count) {

        if (jbd2_journal_bmap(jbd, Log, &phys) || phys == 0) {
            return STATUS_DISK_CORRUPT_ERROR;
        }

        run = 1;
        Log++;
        if (Log >= jbd->j_last)
            Log = jbd->j_first;

        while (done + run < Count) {
            if (jbd2_journal_bmap(jbd, Log, &next) || next != phys + run)
                break;
            run++;
            Log++;
            if (Log >= jbd->j_last)
                Log = jbd->j_first;
        }

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
 * Write the journal superblock (from its cached copy) straight to disk
 * and flush. jbd2/journal.c calls this through jbd2_win_write_superblock
 * instead of submitting the buffer through the volume cache.
 */
static NTSTATUS JnlWriteJournalSb(PEXT2_VCB Vcb, journal_t *jbd, BOOLEAN Flush)
{
    NTSTATUS    Status;
    PUCHAR      buf;
    ULONG       bs = jbd->j_blocksize;
    ULONGLONG   offset;

    if (!jbd->j_sb_buffer || !jbd->j_superblock)
        return STATUS_INVALID_PARAMETER;

    buf = Ext2AllocatePool(NonPagedPool, bs, JNL_TAG);
    if (!buf)
        return STATUS_INSUFFICIENT_RESOURCES;

    offset = (ULONGLONG)jbd->j_blk_offset << Ext2Log2(bs);

    RtlCopyMemory(buf, jbd->j_sb_buffer->b_data, bs);
    Status = Ext2WriteDiskSync(Vcb, offset, bs, buf, TRUE);
    Ext2FreePool(buf, JNL_TAG);

    if (NT_SUCCESS(Status) && Flush)
        Status = Ext2FlushDisk(Vcb);

    return Status;
}

int jbd2_win_write_superblock(journal_t *journal)
{
    PEXT2_VCB Vcb = (PEXT2_VCB)journal->j_dev->bd_priv;
    NTSTATUS  Status = JnlWriteJournalSb(Vcb, journal, TRUE);
    return NT_SUCCESS(Status) ? 0 : -EIO;
}

/*
 * Publish the in-memory tail to the journal superblock if it differs from
 * what is on disk. Must happen before the log reuses freed space and
 * before the very first transaction is written into an empty journal.
 */
NTSTATUS JnlPublishTail(PEXT2_JOURNAL J)
{
    journal_t    *jbd = J->jbd;
    KIRQL         irql;
    unsigned long tail;
    tid_t         seq;
    int           rc;

    JnlLock(J, irql);
    tail = jbd->j_tail;
    seq  = jbd->j_tail_sequence;
    JnlUnlock(J, irql);

    if (!(jbd->j_flags & JBD2_FLUSHED) && tail == J->DiskTail && seq == J->DiskTailSeq)
        return STATUS_SUCCESS;

    rc = jbd2_journal_update_sb_log_tail(jbd, seq, tail, 0);
    if (rc)
        return STATUS_UNEXPECTED_IO_ERROR;

    J->DiskTail = tail;
    J->DiskTailSeq = seq;
    return STATUS_SUCCESS;
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
