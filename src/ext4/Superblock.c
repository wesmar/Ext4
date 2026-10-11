/**
 * Superblock.c - superblock load, store and refresh.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"

NTSTATUS
Ext2LoadSuper(IN PEXT2_VCB      Vcb,
              IN BOOLEAN        bVerify,
              OUT PEXT2_SUPER_BLOCK * Sb)
{
    NTSTATUS          Status;
    PEXT2_SUPER_BLOCK Ext2Sb = NULL;

    Ext2Sb = (PEXT2_SUPER_BLOCK)
             Ext2AllocatePool(
                 PagedPool,
                 sizeof(EXT2_SUPER_BLOCK),
                 EXT2_SB_MAGIC
             );
    if (!Ext2Sb) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto errorout;
    }

    Status = Ext2ReadDisk(
                 Vcb,
                 (ULONGLONG) SUPER_BLOCK_OFFSET,
                 sizeof(EXT2_SUPER_BLOCK),
                 (PVOID) Ext2Sb,
                 bVerify );

    if (!NT_SUCCESS(Status)) {
        Ext2FreePool(Ext2Sb, EXT2_SB_MAGIC);
        Ext2Sb = NULL;
    }

errorout:

    *Sb = Ext2Sb;
    return Status;
}

/*
 * The superblock in memory changes under SuperLock, and is written under
 * it: the checksum covers every field, so a change made by one thread while
 * another computes it would leave a superblock that Linux and e2fsck reject.
 * Taken inside a group lock (the allocators adjust the totals), never
 * around one; recursive, so a holder may call Ext2SaveSuper. The journal
 * is joined first: a commit writes the superblock under this lock too
 * (JnlMarkSuper), so a holder must never wait for one.
 */
VOID
Ext2LockSuper(IN PEXT2_VCB Vcb)
{
    Ext2JournalJoin(Vcb);
    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&Vcb->SuperLock, TRUE);
}

VOID
Ext2UnlockSuper(IN PEXT2_VCB Vcb)
{
    ExReleaseResourceLite(&Vcb->SuperLock);
    KeLeaveCriticalRegion();
}

/* set ro_compat feature Feature, once, and write the superblock */
VOID
Ext2SetSuperRoCompat(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN ULONG Feature)
{
    Ext2LockSuper(Vcb);
    if (!IsFlagOn(Vcb->SuperBlock->s_feature_ro_compat, Feature)) {
        SetFlag(Vcb->SuperBlock->s_feature_ro_compat, Feature);
        /* fails only once the volume stopped writing: then what needs the
           feature is not written either */
        (void)Ext2SaveSuper(IrpContext, Vcb);
    }
    Ext2UnlockSuper(Vcb);
}

/* a file of Size bytes: from 2 GiB on the volume needs large_file, as
   Linux marks it when such a file is written */
VOID
Ext2NoteFileSize(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN LONGLONG Size)
{
    if (Size >= EXT2_LARGE_FILE_SIZE &&
        !IsFlagOn(Vcb->SuperBlock->s_feature_ro_compat, EXT2_FEATURE_RO_COMPAT_LARGE_FILE)) {
        Ext2SetSuperRoCompat(IrpContext, Vcb, EXT2_FEATURE_RO_COMPAT_LARGE_FILE);
    }
}

BOOLEAN
Ext2SaveSuper(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb
)
{
    LONGLONG    offset;
    BOOLEAN     rc;

    Ext2LockSuper(Vcb);
    ext4_superblock_csum_set(&Vcb->sb);
    offset = (LONGLONG) SUPER_BLOCK_OFFSET;
    rc = Ext2SaveBuffer( IrpContext,
                         Vcb,
                         offset,
                         sizeof(EXT2_SUPER_BLOCK),
                         Vcb->SuperBlock
                       );
    Ext2UnlockSuper(Vcb);

    return rc;
}

BOOLEAN
Ext2RefreshSuper (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb
)
{
    UNREFERENCED_PARAMETER(IrpContext);
    LONGLONG        offset;
    IO_STATUS_BLOCK iosb;

    offset = (LONGLONG) SUPER_BLOCK_OFFSET;
    if (!CcCopyRead(
                Vcb->Volume,
                (PLARGE_INTEGER)&offset,
                sizeof(EXT2_SUPER_BLOCK),
                TRUE,
                (PVOID)Vcb->SuperBlock,
                &iosb )) {
        return FALSE;
    }

    if (!NT_SUCCESS(iosb.Status)) {
        return FALSE;
    }

    /* reload root inode */
    if (Vcb->McbTree) {

        if (!Ext2LoadInode(Vcb, Vcb->McbTree->Inode))
            return FALSE;

        /* initialize root node */
        Vcb->McbTree->Icb->LastAccessTime = Ext2GetInodeTime(Vcb->McbTree->Inode->i_atime, Vcb->McbTree->Inode->i_atime_extra);
        Vcb->McbTree->Icb->LastWriteTime = Ext2GetInodeTime(Vcb->McbTree->Inode->i_mtime, Vcb->McbTree->Inode->i_mtime_extra);
        Vcb->McbTree->Icb->ChangeTime = Ext2GetInodeTime(Vcb->McbTree->Inode->i_ctime, Vcb->McbTree->Inode->i_ctime_extra);
        if (Vcb->McbTree->Inode->i_crtime)
            Vcb->McbTree->Icb->CreationTime = Ext2GetInodeTime(Vcb->McbTree->Inode->i_crtime, Vcb->McbTree->Inode->i_crtime_extra);
        else
            Vcb->McbTree->Icb->CreationTime = Ext2GetInodeTime(Vcb->McbTree->Inode->i_ctime, Vcb->McbTree->Inode->i_ctime_extra);
    }

    return TRUE;
}

/*
 * The free block and inode totals of the volume.
 *
 * They are the sum of the group descriptors. Summing those after every
 * allocation cost O(groups); writing the superblock after every one made
 * all allocations of the volume take turns on it - one checksum and one
 * journaled block per block or inode allocated or freed. So the totals
 * live in the Vcb, moved by each allocator by the exact change it made to
 * its descriptor with one atomic add, and reach the superblock when the
 * volume is flushed: on flush, shutdown and dismount (Ext2SyncSuperTotals).
 * Linux keeps them the same way, in per-cpu counters written at sync. After
 * a crash the superblock may be behind; mount sums the descriptors again
 * (as Linux does, and e2fsck accepts).
 */
VOID
Ext2AdjustVcbStat(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN LONGLONG             BlockDelta,
    IN LONGLONG             InodeDelta
)
{
    UNREFERENCED_PARAMETER(IrpContext);

    if (BlockDelta != 0) {
        InterlockedAdd64(&Vcb->FreeBlocks, BlockDelta);
    }
    if (InodeDelta != 0) {
        InterlockedAdd64(&Vcb->FreeInodes, InodeDelta);
    }
}

/* the totals, clamped: a descriptor that was wrong before we touched it
   must not drive them below zero or past the volume */
ULONGLONG
Ext2FreeBlocks(IN PEXT2_VCB Vcb)
{
    LONGLONG Free = ReadNoFence64(&Vcb->FreeBlocks);

    Free = max(Free, 0);
    return (ULONGLONG)min(Free, (LONGLONG)ext3_blocks_count(SUPER_BLOCK));
}

ULONG
Ext2FreeInodes(IN PEXT2_VCB Vcb)
{
    LONGLONG Free = ReadNoFence64(&Vcb->FreeInodes);

    Free = max(Free, 0);
    return (ULONG)min(Free, (LONGLONG)le32_to_cpu(SUPER_BLOCK->s_inodes_count));
}

/* ---------------------------------------------------------------- errors */

#define EXT2_ERRORS_LOGGED      32      /* reports printed per mount; every one is counted */
#define EXT2_ERROR_FUNC_CHARS   32      /* s_*_error_func, not terminated when full */

static VOID
Ext2RecordError(OUT __le32 *Time, OUT __u8 *TimeHi, OUT __le32 *Ino, OUT __le64 *Block,
                OUT __u8 *Func, OUT __le32 *Line, OUT __u8 *Code, IN LONGLONG Now,
                IN ULONG Inode, IN ULONGLONG Where, IN PCSTR Function, IN ULONG At,
                IN __u8 ErrCode)
{
    size_t Length = strlen(Function);

    *Time = cpu_to_le32((__u32)Now);
    *TimeHi = (__u8)(Now >> 32);
    *Ino = cpu_to_le32(Inode);
    *Block = cpu_to_le64(Where);
    RtlZeroMemory(Func, EXT2_ERROR_FUNC_CHARS);
    RtlCopyMemory(Func, Function, min(Length, (size_t)EXT2_ERROR_FUNC_CHARS));
    *Line = cpu_to_le32(At);
    *Code = ErrCode;
}

static IO_WORKITEM_ROUTINE Ext2SuperWriteWorker;

static VOID
Ext2SuperWriteWorker(IN PDEVICE_OBJECT DeviceObject, IN PVOID Context)
{
    PEXT2_VCB Vcb = (PEXT2_VCB)DeviceObject->DeviceExtension;

    /* what changes from here on queues the next write */
    InterlockedExchange(&Vcb->SuperWritePending, FALSE);

    /* A writable volume takes it as any superblock change: through the
       journal, in order with the others, and into the volume's cached
       copy - written past that copy, the copy's next flush would undo it.
       A volume stopped by its errors (s_errors, a journal abort) is told
       why straight on the disk, as Linux's ext4_commit_super does. A disk
       gone, or another node's now, is not written. */
    if (!FlagOn(Vcb->Flags, VCB_DEVICE_REMOVED | VCB_FOREIGN_OWNER)) {
        BOOLEAN Saved = FALSE;

        if (!IsVcbReadOnly(Vcb)) {
            Ext2JournalEnterScope(Vcb);
            Saved = Ext2SaveSuper(NULL, Vcb);
            Ext2JournalLeaveScope(Vcb);
        }
        if (!Saved) {
            (void)Ext2SaveSuperDirect(Vcb);     /* best effort: the memory copy has it either way */
        }
        Ext2JournalRecordAbort(Vcb);
    }

    IoFreeWorkItem((PIO_WORKITEM)Context);
    ExReleaseRundownProtection(&Vcb->SuperWriteRundown);
}

/*
 * The superblock in memory says what the disk must learn soon - errors
 * found, a journal stopped - and the thread that found it may not write it
 * itself: it may hold group locks, or be a paging read running with APCs
 * disabled, where a synchronous IRP never completes (its completion is an
 * APC to that very thread; the robustness run hung there). A system worker
 * writes it, as Linux leaves it to s_sb_upd_work. Asked again while a write
 * is queued, the queued one carries it. The volume's teardown waits for the
 * write (Ext2DrainSuperWrites); asked after that, the teardown's own last
 * superblock write carries it. Callable at IRQL <= APC_LEVEL.
 */
VOID
Ext2QueueSuperWrite(IN PEXT2_VCB Vcb)
{
    PIO_WORKITEM Item;

    if (InterlockedExchange(&Vcb->SuperWritePending, TRUE)) {
        return;
    }
    if (!ExAcquireRundownProtection(&Vcb->SuperWriteRundown)) {
        return;
    }
    Item = IoAllocateWorkItem(Vcb->DeviceObject);
    if (Item == NULL) {
        /* no memory: the next request or superblock write carries it */
        InterlockedExchange(&Vcb->SuperWritePending, FALSE);
        ExReleaseRundownProtection(&Vcb->SuperWriteRundown);
        return;
    }
    IoQueueWorkItem(Item, Ext2SuperWriteWorker, DelayedWorkQueue, Item);
}

/* the volume goes: the write queued last is waited for. Once per Vcb. */
VOID
Ext2DrainSuperWrites(IN PEXT2_VCB Vcb)
{
    ExWaitForRundownProtectionRelease(&Vcb->SuperWriteRundown);
}

/*
 * Corruption found on disk, as Linux's ext4_error handles it: the
 * superblock says the volume has errors (EXT4_ERROR_FS: e2fsck checks it
 * at the next boot) and where the first and the last were found; then the
 * volume does what its superblock asks for (s_errors) - goes on, or stops
 * writing; panic, which no file system on Windows does to the system,
 * stops writing too. Every report reaches the disk through
 * Ext2QueueSuperWrite. Callable inside the group locks and from paging I/O.
 */
VOID
Ext2ReportCorruptionAt(IN PEXT2_VCB Vcb, IN ULONG Inode, IN ULONGLONG Block,
                       IN int Err, IN PCSTR Function, IN ULONG Line)
{
    PEXT2_SUPER_BLOCK   sb = Vcb->SuperBlock;
    LARGE_INTEGER       SysTime;
    LONGLONG            Now;
    LONG                Reports = InterlockedIncrement(&Vcb->ErrorReports);
    __u8                Code;

    if (Reports <= EXT2_ERRORS_LOGGED) {
        DbgPrint("ext4: %s:%u: corrupt on disk (inode %u, block %I64u, error %d)%s\n",
                 Function, Line, Inode, Block, Err,
                 Reports == EXT2_ERRORS_LOGGED ? "; further reports are only counted" : "");
    }
    if (IsVcbReadOnly(Vcb)) {
        return;     /* mounted read-only, or stopped writing already */
    }

    Code = Err == -EFSBADCRC ? EXT4_ERR_EFSBADCRC :
           Err == -EIO ? EXT4_ERR_EIO : EXT4_ERR_EFSCORRUPTED;
    KeQuerySystemTime(&SysTime);
    Now = Ext2UnixTime(&SysTime);

    /* the fields change under the superblock's lock, as its checksum
       covers them; the error bit is one bit set, as the journal sets it */
    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&Vcb->SuperLock, TRUE);
    InterlockedOr16((volatile SHORT *)&sb->s_state, EXT4_ERROR_FS);
    le32_add_cpu(&sb->s_error_count, 1);
    if (sb->s_first_error_time == 0 && sb->s_first_error_time_hi == 0) {
        Ext2RecordError(&sb->s_first_error_time, &sb->s_first_error_time_hi,
                        &sb->s_first_error_ino, &sb->s_first_error_block,
                        sb->s_first_error_func, &sb->s_first_error_line,
                        &sb->s_first_error_errcode, Now, Inode, Block, Function, Line, Code);
    }
    Ext2RecordError(&sb->s_last_error_time, &sb->s_last_error_time_hi,
                    &sb->s_last_error_ino, &sb->s_last_error_block,
                    sb->s_last_error_func, &sb->s_last_error_line,
                    &sb->s_last_error_errcode, Now, Inode, Block, Function, Line, Code);
    ExReleaseResourceLite(&Vcb->SuperLock);
    KeLeaveCriticalRegion();

    /* stopped first, so that the write finds the volume as it will stay */
    if (le16_to_cpu(sb->s_errors) == EXT4_ERRORS_RO || le16_to_cpu(sb->s_errors) == EXT4_ERRORS_PANIC) {
        DbgPrint("ext4: the volume asks to stop writing on errors: read-only from now on\n");
        Ext2JournalAbortQuiet(Vcb);
    }
    Ext2QueueSuperWrite(Vcb);
}

/*
 * Inode, unlinked, goes on the orphan list: the superblock's s_last_orphan
 * names it, its i_dtime the inode that headed the list before (Linux:
 * ext4_orphan_add without the orphan file, whose list Linux and e2fsck
 * still empty when the file exists). The caller writes the inode in the
 * same transaction.
 */
BOOLEAN
Ext2AddOrphan(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN struct inode *Inode)
{
    BOOLEAN Saved;

    Ext2LockSuper(Vcb);
    Inode->i_dtime = le32_to_cpu(Vcb->SuperBlock->s_last_orphan);
    Vcb->SuperBlock->s_last_orphan = cpu_to_le32((__u32)Inode->i_ino);
    Saved = Ext2SaveSuper(IrpContext, Vcb);
    Ext2UnlockSuper(Vcb);
    return Saved;
}

/* the totals into the superblock, written when they changed */
VOID
Ext2SyncSuperTotals(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb)
{
    ULONGLONG   Blocks;
    ULONG       Inodes;

    if (IsVcbReadOnly(Vcb)) {
        return;
    }

    Ext2LockSuper(Vcb);
    Blocks = Ext2FreeBlocks(Vcb);
    Inodes = Ext2FreeInodes(Vcb);
    if (ext3_free_blocks_count(SUPER_BLOCK) != Blocks ||
        le32_to_cpu(SUPER_BLOCK->s_free_inodes_count) != Inodes) {
        ext3_free_blocks_count_set(SUPER_BLOCK, (ext4_fsblk_t)Blocks);
        SUPER_BLOCK->s_free_inodes_count = cpu_to_le32(Inodes);
        (void)Ext2SaveSuper(IrpContext, Vcb);   /* totals only: e2fsck and Linux recount them */
    }
    Ext2UnlockSuper(Vcb);
}
