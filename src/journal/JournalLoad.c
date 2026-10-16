/**
 * JournalLoad.c - loading a volume's journal: its superblock, the replay
 * of what a crash left in it, and the start of the engine.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * The journal is the inode s_journal_inum; an external journal leaves the
 * volume read-only. Its blocks are mapped once, into runs. The journal
 * superblock is checked as jbd2 checks it: the magic and the block size,
 * version 1 or 2, a log inside the file and at least 1024 blocks long, not
 * two checksum versions at once, the crc32c type and the superblock's own
 * sum for v2/v3, and no feature this driver does not know (a fast-commit
 * journal is one: read-only).
 *
 * Then, by what the two superblocks say:
 *  - the file system needs recovery and the log is not empty: the log is
 *    replayed (JournalReplay.c) and what it sends home is written and
 *    flushed; the log is marked empty, then the file system clean;
 *  - the file system is clean, the log is not: the log is discarded, as
 *    Linux discards it; only its end is looked for;
 *  - an empty log: nothing to do.
 * New transaction IDs start past every ID the scan reached, so no block of
 * the old log can read as part of the new one (jbd2 starts right after the
 * last replayed one, which an async commit's failure can leave behind valid
 * looking transactions). When the scan cannot tell, they start a whole ring
 * of IDs later: a log of N blocks holds fewer than N transactions.
 *
 * A replay that fails leaves the volume read-only, needs_recovery still on:
 * e2fsck, or Linux, replays the same log again; what went home meanwhile is
 * committed data, and writing it twice changes nothing.
 */

#include "ext4fs.h"
#include "JournalReplay.h"
#include "JournalInternal.h"

#define JL_TAG              'LJxE'
#define JL_WINDOW_BYTES     (256 * 1024)        /* log read ahead */
#define JL_MAP_BYTES        (64 * 1024 * 1024)  /* the file mapped per call */

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2RecoverJournal)
#endif

/* ---- the journal file ---- */

static PEXT2_MCB JlOpenInode(PEXT2_VCB Vcb, ULONG Ino)
{
    PEXT2_MCB Mcb = Ext2AllocateMcb(Vcb, NULL, 0);

    if (!Mcb)
        return NULL;
    /* the journal inode gets an Icb like any other */
    if (!Ext2AttachIcb(Vcb, Mcb, Ino)) {
        Ext2FreeMcb(Vcb, Mcb);
        return NULL;
    }
    if (!IsFlagOn(Mcb->Icb->Flags, ICB_INODE_LOADED)) {
        if (!Ext2LoadInode(Vcb, Mcb->Inode)) {
            Ext2FreeMcb(Vcb, Mcb);
            return NULL;
        }
        SetLongFlag(Mcb->Icb->Flags, ICB_INODE_LOADED);
    }
    return Mcb;
}

static NTSTATUS JlAddRun(PEXT2_JLOG L, PULONG Capacity, ULONG Log, ULONGLONG Phys, ULONG Count)
{
    PEXT2_JRUN last = L->RunCount ? &L->Runs[L->RunCount - 1] : NULL;

    if (last && last->Log + last->Count == Log && last->Phys + last->Count == Phys) {
        last->Count += Count;
        return STATUS_SUCCESS;
    }
    if (L->RunCount == *Capacity) {
        ULONG       grown = *Capacity ? *Capacity * 2 : 16;
        PEXT2_JRUN  runs = Ext2AllocatePool(PagedPool, grown * sizeof(EXT2_JRUN), JL_TAG);
        if (!runs)
            return STATUS_INSUFFICIENT_RESOURCES;
        if (L->Runs) {
            RtlCopyMemory(runs, L->Runs, L->RunCount * sizeof(EXT2_JRUN));
            Ext2FreePool(L->Runs, JL_TAG);
        }
        L->Runs = runs;
        *Capacity = grown;
    }
    L->Runs[L->RunCount].Log = Log;
    L->Runs[L->RunCount].Phys = Phys;
    L->Runs[L->RunCount].Count = Count;
    L->RunCount++;
    return STATUS_SUCCESS;
}

/* the journal file's blocks [0, Blocks) as runs; a hole is corruption */
static NTSTATUS JlMapFile(PEXT2_VCB Vcb, PEXT2_MCB Mcb, ULONG Blocks, PEXT2_JLOG L)
{
    ULONG       capacity = 0, next = 0;
    ULONGLONG   total = (ULONGLONG)Blocks << L->BlockBits;
    ULONGLONG   offset;
    NTSTATUS    Status = STATUS_SUCCESS;

    for (offset = 0; offset < total && NT_SUCCESS(Status); offset += JL_MAP_BYTES) {
        ULONG           size = (ULONG)min((ULONGLONG)JL_MAP_BYTES, total - offset);
        PEXT2_EXTENT    chain = NULL, e;

        Status = Ext2BuildExtents(NULL, Vcb, Mcb, offset, size, FALSE, &chain);
        for (e = chain; NT_SUCCESS(Status) && e; e = e->Next) {
            ULONG log = (ULONG)((offset + e->Offset) >> L->BlockBits);
            ULONG count = e->Length >> L->BlockBits;

            if (log != next || count == 0 || e->Lba == 0) {
                Status = STATUS_DISK_CORRUPT_ERROR;     /* a hole in the journal */
                break;
            }
            Status = JlAddRun(L, &capacity, log, (ULONGLONG)e->Lba >> L->BlockBits, count);
            next += count;
        }
        if (chain)
            Ext2DestroyExtentChain(chain);
    }
    if (NT_SUCCESS(Status) && next != Blocks)
        Status = STATUS_DISK_CORRUPT_ERROR;
    return Status;
}

NTSTATUS JlogMap(IN PEXT2_JLOG L, IN ULONG LogBlock, OUT PULONGLONG Phys, OUT PULONG Contiguous)
{
    ULONG lo = 0, hi = L->RunCount;

    while (lo < hi) {
        ULONG       mid = lo + (hi - lo) / 2;
        PEXT2_JRUN  r = &L->Runs[mid];

        if (LogBlock < r->Log) {
            hi = mid;
        } else if (LogBlock - r->Log >= r->Count) {
            lo = mid + 1;
        } else {
            *Phys = r->Phys + (LogBlock - r->Log);
            if (Contiguous)
                *Contiguous = r->Count - (LogBlock - r->Log);
            return STATUS_SUCCESS;
        }
    }
    return STATUS_DISK_CORRUPT_ERROR;
}

/* ---- the superblock ---- */

static ULONG JlSuperSum(PUCHAR Super)
{
    UCHAR   saved[4];
    ULONG   sum;

    RtlCopyMemory(saved, Super + JF_S_CHECKSUM, 4);
    RtlZeroMemory(Super + JF_S_CHECKSUM, 4);
    sum = crc32c(~0u, Super, JF_SUPERBLOCK_SIZE);
    RtlCopyMemory(Super + JF_S_CHECKSUM, saved, 4);
    return sum;
}

/* jbd2's checks (journal_get_superblock, jbd2_journal_load, journal_reset) */
static NTSTATUS JlCheckSuper(PEXT2_JLOG L, ULONG FileBlocks)
{
    PUCHAR  s = L->Super;
    ULONG   type = JfGet32(s + JF_H_TYPE);

    if (JfGet32(s + JF_H_MAGIC) != JF_MAGIC || JfGet32(s + JF_S_BLOCKSIZE) != L->BlockSize)
        return STATUS_DISK_CORRUPT_ERROR;
    if (type == JF_SUPERBLOCK_V1)
        L->Version = 1;
    else if (type == JF_SUPERBLOCK_V2)
        L->Version = 2;
    else
        return STATUS_DISK_CORRUPT_ERROR;

    L->MaxLen = JfGet32(s + JF_S_MAXLEN);
    if (L->MaxLen > FileBlocks)
        return STATUS_DISK_CORRUPT_ERROR;       /* the file is too short */
    L->First = JfGet32(s + JF_S_FIRST);
    L->Last = L->MaxLen;
    if (L->First == 0 || L->First >= L->MaxLen ||
        (ULONGLONG)L->First + JF_MIN_LOG_BLOCKS > (ULONGLONG)L->Last + 1)
        return STATUS_DISK_CORRUPT_ERROR;

    if (L->Version >= 2) {
        L->Compat = JfGet32(s + JF_S_COMPAT);
        L->Incompat = JfGet32(s + JF_S_INCOMPAT);
        if ((L->Incompat & ~JF_KNOWN_INCOMPAT) || (JfGet32(s + JF_S_RO_COMPAT) & ~JF_KNOWN_RO_COMPAT))
            return STATUS_NOT_SUPPORTED;
    } else {
        L->Compat = L->Incompat = 0;
    }
    if ((L->Incompat & JF_INCOMPAT_CSUM_V2) && (L->Incompat & JF_INCOMPAT_CSUM_V3))
        return STATUS_DISK_CORRUPT_ERROR;
    if (JfHasCsumV2V3(L->Incompat)) {
        if (L->Compat & JF_COMPAT_CHECKSUM)
            return STATUS_DISK_CORRUPT_ERROR;
        if (s[JF_S_CHECKSUM_TYPE] != JF_CRC32C_CHECKSUM)
            return STATUS_DISK_CORRUPT_ERROR;
        if (JlSuperSum(s) != JfGet32(s + JF_S_CHECKSUM))
            return STATUS_CRC_ERROR;
        L->CsumSeed = crc32c(~0u, s + JF_S_UUID, 16);
    }
    L->Errno = (LONG)JfGet32(s + JF_S_ERRNO);
    return STATUS_SUCCESS;
}

static NTSTATUS JlLoad(PEXT2_VCB Vcb, PEXT2_MCB Mcb, PEXT2_JLOG *Result)
{
    PEXT2_JLOG  L;
    ULONG       fileBlocks;
    ULONGLONG   phys = 0;
    NTSTATUS    Status;

    *Result = NULL;
    L = Ext2AllocatePool(NonPagedPool, sizeof(EXT2_JLOG), JL_TAG);
    if (!L)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(L, sizeof(EXT2_JLOG));
    L->Vcb = Vcb;
    L->BlockSize = BLOCK_SIZE;
    L->BlockBits = BLOCK_BITS;
    KeInitializeEvent(&L->SuperGate, SynchronizationEvent, TRUE);

    if (Mcb->Inode->i_size >> L->BlockBits > MAXULONG) {
        Status = STATUS_DISK_CORRUPT_ERROR;
        goto errorout;
    }
    fileBlocks = (ULONG)(Mcb->Inode->i_size >> L->BlockBits);
    if (fileBlocks == 0) {
        Status = STATUS_DISK_CORRUPT_ERROR;
        goto errorout;
    }
    L->Super = Ext2AllocatePool(NonPagedPool, L->BlockSize, JL_TAG);
    if (!L->Super) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto errorout;
    }

    Status = JlMapFile(Vcb, Mcb, fileBlocks, L);
    if (NT_SUCCESS(Status))
        Status = JlogMap(L, 0, &phys, NULL);
    if (NT_SUCCESS(Status))
        Status = Ext2ReadSync(Vcb, phys << L->BlockBits, L->BlockSize, L->Super);
    if (NT_SUCCESS(Status))
        Status = JlCheckSuper(L, fileBlocks);
    if (!NT_SUCCESS(Status))
        goto errorout;

    *Result = L;
    return STATUS_SUCCESS;

errorout:
    JlogFree(L);
    return Status;
}

VOID JlogFree(IN PEXT2_JLOG L)
{
    if (!L)
        return;
    if (L->Runs)
        Ext2FreePool(L->Runs, JL_TAG);
    if (L->Super)
        Ext2FreePool(L->Super, JL_TAG);
    Ext2FreePool(L, JL_TAG);
}

/*
 * The superblock goes to disk between two flushes, under the gate: the
 * changes and the writes of two callers keep their order. The first flush:
 * the superblock moves the log's tail - or empties the log - past
 * transactions whose blocks the checkpoint has just written home, and those
 * writes must be on the medium before the log stops covering them (found by
 * tests/crash-replay.py --reorder). The second: the new tail is durable
 * before the log reuses what it freed.
 */
static NTSTATUS JlWriteSuper(PEXT2_JLOG L)
{
    PEXT2_VCB   Vcb = L->Vcb;
    PUCHAR      copy;
    ULONGLONG   phys;
    NTSTATUS    Status;

    Status = JlogMap(L, 0, &phys, NULL);
    if (!NT_SUCCESS(Status))
        return Status;
    copy = Ext2AllocatePool(NonPagedPool, L->BlockSize, JL_TAG);
    if (!copy)
        return STATUS_INSUFFICIENT_RESOURCES;

    if (JfHasCsumV2V3(L->Incompat))
        JfPut32(L->Super + JF_S_CHECKSUM, JlSuperSum(L->Super));
    RtlCopyMemory(copy, L->Super, L->BlockSize);

    Status = Ext2FlushDisk(Vcb);
    if (NT_SUCCESS(Status))
        Status = Ext2WriteDiskSync(Vcb, phys << L->BlockBits, L->BlockSize, copy, TRUE);
    if (NT_SUCCESS(Status))
        Status = Ext2FlushDisk(Vcb);

    Ext2FreePool(copy, JL_TAG);
    return Status;
}

static VOID JlEnterGate(PEXT2_JLOG L)
{
    KeWaitForSingleObject(&L->SuperGate, Executive, KernelMode, FALSE, NULL);
}

static VOID JlLeaveGate(PEXT2_JLOG L)
{
    KeSetEvent(&L->SuperGate, IO_NO_INCREMENT, FALSE);
}

NTSTATUS JlogSetTail(IN PEXT2_JLOG L, IN ULONG Sequence, IN ULONG Start)
{
    NTSTATUS Status;

    if (L->Flags & JLOG_ABORTED)
        return STATUS_UNEXPECTED_IO_ERROR;
    JlEnterGate(L);
    JfPut32(L->Super + JF_S_SEQUENCE, Sequence);
    JfPut32(L->Super + JF_S_START, Start);
    Status = JlWriteSuper(L);
    JlLeaveGate(L);
    if (NT_SUCCESS(Status)) {
        if (Start == 0)
            InterlockedOr((volatile LONG *)&L->Flags, JLOG_FLUSHED);
        else
            InterlockedAnd((volatile LONG *)&L->Flags, ~JLOG_FLUSHED);
    }
    return Status;
}

NTSTATUS JlogRecordErrno(IN PEXT2_JLOG L)
{
    NTSTATUS Status;

    /* written also after an abort: that is when there is one to record */
    JlEnterGate(L);
    JfPut32(L->Super + JF_S_ERRNO, (ULONG)L->Errno);
    Status = JlWriteSuper(L);
    JlLeaveGate(L);
    return Status;
}

NTSTATUS JlogSetFeatures(IN PEXT2_JLOG L, IN ULONG Compat, IN ULONG Incompat)
{
    NTSTATUS Status;

    if (L->Version < 2)
        return STATUS_SUCCESS;      /* a version 1 superblock has no features */
    if (Compat == L->Compat && Incompat == L->Incompat)
        return STATUS_SUCCESS;
    JlEnterGate(L);
    JfPut32(L->Super + JF_S_COMPAT, Compat);
    JfPut32(L->Super + JF_S_INCOMPAT, Incompat);
    L->Super[JF_S_CHECKSUM_TYPE] = JfHasCsumV2V3(Incompat) ? JF_CRC32C_CHECKSUM : 0;
    L->Compat = Compat;
    L->Incompat = Incompat;
    if (JfHasCsumV2V3(Incompat))
        L->CsumSeed = crc32c(~0u, L->Super + JF_S_UUID, 16);
    /* the checksum feature just switched on is valid only with a fresh sum,
       which the write computes */
    Status = JlWriteSuper(L);
    JlLeaveGate(L);
    return Status;
}

/* ---- the replay's I/O ---- */

typedef struct _JL_IO {
    PEXT2_VCB   Vcb;
    PEXT2_JLOG  Log;
    PUCHAR      Window;         /* consecutive log blocks, read ahead */
    ULONG       WindowBlocks;
    ULONG       WindowStart;
    ULONG       WindowCount;
} JL_IO, *PJL_IO;

static int JlReadLog(void *Context, unsigned int LogBlock, unsigned char *Buffer)
{
    PJL_IO      io = (PJL_IO)Context;
    PEXT2_JLOG  L = io->Log;

    if (LogBlock < io->WindowStart || LogBlock - io->WindowStart >= io->WindowCount) {
        ULONG count = min(io->WindowBlocks, L->MaxLen - LogBlock), done = 0;

        io->WindowStart = LogBlock;
        io->WindowCount = 0;
        while (done < count) {
            ULONGLONG   phys;
            ULONG       run;
            NTSTATUS    Status = JlogMap(L, LogBlock + done, &phys, &run);

            if (NT_SUCCESS(Status)) {
                run = min(run, count - done);
                Status = Ext2ReadSync(io->Vcb, phys << L->BlockBits, run << L->BlockBits,
                                      io->Window + ((SIZE_T)done << L->BlockBits));
            }
            if (!NT_SUCCESS(Status))
                break;          /* what was read stays; this block fails below */
            done += run;
        }
        io->WindowCount = done;
        if (done == 0)
            return JR_EIO;
    }
    RtlCopyMemory(Buffer, io->Window + ((SIZE_T)(LogBlock - io->WindowStart) << L->BlockBits), L->BlockSize);
    return JR_OK;
}

/* a replayed block goes through the volume's buffers, as every metadata
   write does: no cached copy is left stale; sync_blockdev writes them */
static int JlWriteHome(void *Context, unsigned long long Block, const unsigned char *Buffer)
{
    PJL_IO              io = (PJL_IO)Context;
    PEXT2_VCB           Vcb = io->Vcb;
    struct buffer_head *bh;

    if (Block >= TOTAL_BLOCKS)
        return JR_EIO;
    bh = __getblk(&Vcb->bd, (sector_t)Block, BLOCK_SIZE);
    if (!bh)
        return JR_EIO;
    lock_buffer(bh);
    RtlCopyMemory(bh->b_data, Buffer, BLOCK_SIZE);
    set_buffer_uptodate(bh);
    mark_buffer_dirty(bh);
    unlock_buffer(bh);
    brelse(bh);
    return JR_OK;
}

static unsigned int JlCrc32c(unsigned int Crc, const unsigned char *Data, unsigned int Length)
{
    return crc32c(Crc, Data, Length);
}

static void *JlAllocate(void *Context, unsigned int Bytes)
{
    UNREFERENCED_PARAMETER(Context);
    return Ext2AllocatePool(PagedPool, Bytes, JL_TAG);
}

static void JlRelease(void *Context, void *Memory)
{
    UNREFERENCED_PARAMETER(Context);
    Ext2FreePool(Memory, JL_TAG);
}

/* replay (Recover) or only find the end; JR_* and the result */
static int JlWalk(PEXT2_VCB Vcb, PEXT2_JLOG L, BOOLEAN Recover, JR_RESULT *R)
{
    JL_IO       io;
    JR_JOURNAL  J;
    int         rc;

    RtlZeroMemory(&io, sizeof(io));
    io.Vcb = Vcb;
    io.Log = L;
    io.WindowBlocks = max(JL_WINDOW_BYTES >> L->BlockBits, 1);
    io.Window = Ext2AllocatePool(PagedPool, io.WindowBlocks << L->BlockBits, JL_TAG);
    if (!io.Window)
        return JR_ENOMEM;

    RtlZeroMemory(&J, sizeof(J));
    J.BlockSize = L->BlockSize;
    J.First = L->First;
    J.Last = L->Last;
    J.TotalLen = L->MaxLen;
    J.Compat = L->Compat;
    J.Incompat = L->Incompat;
    J.CsumSeed = L->CsumSeed;
    J.Start = JfGet32(L->Super + JF_S_START);
    J.Sequence = JfGet32(L->Super + JF_S_SEQUENCE);
    J.Context = &io;
    J.ReadLog = JlReadLog;
    J.WriteHome = JlWriteHome;
    J.Crc32c = JlCrc32c;
    J.Allocate = JlAllocate;
    J.Release = JlRelease;

    rc = Recover ? JrRecover(&J, R) : JrScan(&J, R);
    Ext2FreePool(io.Window, JL_TAG);
    return rc;
}

/* the file system's needs_recovery flag cleared, on disk */
static BOOLEAN JlMarkRecovered(PEXT2_IRP_CONTEXT IrpContext, PEXT2_VCB Vcb)
{
    ClearLongFlag(Vcb->SuperBlock->s_feature_incompat, EXT3_FEATURE_INCOMPAT_RECOVER);
    if (!Ext2SaveSuper(IrpContext, Vcb) || sync_blockdev(&Vcb->bd) != 0) {
        SetLongFlag(Vcb->SuperBlock->s_feature_incompat, EXT3_FEATURE_INCOMPAT_RECOVER);
        return FALSE;
    }
    ClearLongFlag(Vcb->Flags, VCB_JOURNAL_RECOVER);
    return TRUE;
}

INT
Ext2RecoverJournal(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb
)
{
    PEXT2_SUPER_BLOCK   sb = Vcb->SuperBlock;
    PEXT2_MCB           jcb = NULL;
    PEXT2_JLOG          L = NULL;
    JR_RESULT           R;
    BOOLEAN             needsRecovery;
    ULONG               start, sequence, next;
    NTSTATUS            Status;
    INT                 rc = 0;
    int                 jr;

    PAGED_CODE();

    ExAcquireResourceExclusiveLite(&Vcb->MainResource, TRUE);

    /* the engine already owns the journal */
    if (Ext2JournalIsActive(Vcb))
        goto out;

    needsRecovery = IsFlagOn(sb->s_feature_incompat, EXT3_FEATURE_INCOMPAT_RECOVER);
    if (needsRecovery)
        SetLongFlag(Vcb->Flags, VCB_JOURNAL_RECOVER);

    /* read-only, no journal inode, or an external journal */
    if (IsVcbReadOnly(Vcb) || sb->s_journal_inum == 0 || sb->s_journal_dev != 0) {
        rc = -EOPNOTSUPP;
        goto out;
    }

    jcb = JlOpenInode(Vcb, sb->s_journal_inum);
    if (!jcb) {
        rc = -EIO;
        goto out;
    }
    Status = JlLoad(Vcb, jcb, &L);
    if (!NT_SUCCESS(Status)) {
        DbgPrint("ext4: the journal cannot be used (status %08x)\n", Status);
        rc = Ext2LinuxError(Status);
        goto out;
    }

    start = JfGet32(L->Super + JF_S_START);
    sequence = JfGet32(L->Super + JF_S_SEQUENCE);
    next = sequence + 1;

    if (start != 0) {
        RtlZeroMemory(&R, sizeof(R));
        jr = JlWalk(Vcb, L, needsRecovery, &R);
        if (needsRecovery) {
            /* what went home is committed data, whatever the result */
            int flushed = sync_blockdev(&Vcb->bd);
            if (jr != JR_OK || R.HasFailedCommit || R.WriteError || flushed != 0) {
                DbgPrint("ext4: journal replay failed (%d%s, %u blocks home), "
                         "the volume stays read-only\n", jr,
                         R.HasFailedCommit ? ", a commit is corrupt" : "", R.Replayed);
                rc = jr ? jr : -EIO;
                goto out;
            }
            DbgPrint("ext4: journal replayed: transactions %u to %u, %u blocks home, "
                     "%u revoked\n", R.StartTid, R.EndTid - 1, R.Replayed, R.RevokeHits);
            next = R.ScanEndTid + 1;
        } else {
            /* a clean file system's log is stale: it is discarded */
            next = jr == JR_OK ? R.ScanEndTid + 1 : sequence + (L->Last - L->First) + 1;
        }
        /* the log is empty from here on */
        Status = JlogSetTail(L, next, 0);
        if (!NT_SUCCESS(Status)) {
            rc = -EIO;
            goto out;
        }
        if (needsRecovery &&
            (!Ext2RefreshSuper(IrpContext, Vcb) || !Ext2RefreshGroup(IrpContext, Vcb))) {
            rc = -EIO;
            goto out;
        }
    } else {
        L->Flags |= JLOG_FLUSHED;
    }

    if (needsRecovery && !JlMarkRecovered(IrpContext, Vcb)) {
        rc = -EIO;
        goto out;
    }

    /* the engine starts with an empty log, its tail published before the
       first transaction is written (JnlPublishTail) */
    L->Head = L->Tail = L->First;
    L->Free = L->Last - L->First;
    L->TransactionSequence = L->TailSequence = next;
    L->CommitSequence = next - 1;

    /* An orphan file that is not sound: Linux refuses the mount; here the
       volume stays read-only, the error recorded before the engine runs -
       so the superblock goes straight to disk, nothing logged. */
    if (!IsVcbReadOnly(Vcb) && !Ext4OrphanFileSound(Vcb)) {
        DbgPrint("ext4: the orphan file is not sound, the volume stays read-only\n");
        Ext2LockSuper(Vcb);
        sb->s_state |= cpu_to_le16(EXT4_ERROR_FS);
        (void)Ext2SaveSuper(IrpContext, Vcb);
        Ext2UnlockSuper(Vcb);
        (void)sync_blockdev(&Vcb->bd);
        SetLongFlag(Vcb->Flags, VCB_READ_ONLY);
    }

    if (!IsVcbReadOnly(Vcb)) {
        Status = Ext2JournalStart(Vcb, L, jcb);
        if (NT_SUCCESS(Status)) {
            L = NULL;           /* the engine's now */
            jcb = NULL;
        } else {
            DbgPrint("ext4: the journal did not start (status %08x)\n", Status);
            rc = Ext2LinuxError(Status);
        }
    }

out:
    JlogFree(L);
    if (jcb)
        Ext2FreeMcb(Vcb, jcb);
    ExReleaseResourceLite(&Vcb->MainResource);
    return rc;
}
