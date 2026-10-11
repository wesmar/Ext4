/**
 * FileWrite.c - IRP_MJ_WRITE: cached and paging writes to files, deferred writes.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "write_internal.h"

NTSTATUS
Ext2WriteComplete (IN PEXT2_IRP_CONTEXT IrpContext);

NTSTATUS
Ext2WriteFile (IN PEXT2_IRP_CONTEXT IrpContext);

/* Zeros [Start, End) through the cache. Alignment alone does not prove
   that the range is unwritten: truncation can leave initialized blocks
   which must be cleared. STATUS_PENDING when the zeroing would have to
   wait and may not; the error of a zeroing that failed otherwise. It used
   to answer FALSE for both, and the write was queued again, and failed
   again, for ever. */
NTSTATUS
Ext2ZeroData (
    IN PEXT2_IRP_CONTEXT IrpContext,
    IN PFILE_OBJECT      FileObject,
    IN PLARGE_INTEGER    Start,
    IN PLARGE_INTEGER    End
    )
{
    NTSTATUS        Status = STATUS_SUCCESS;

    ASSERT (End && Start && End->QuadPart > Start->QuadPart);

    __try {
        if (!CcZeroData(FileObject, Start, End, Ext2CanIWait())) {
            Status = STATUS_PENDING;
        }
    } __except (FsRtlIsNtstatusExpected(GetExceptionCode()) ?
                EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        Status = GetExceptionCode();
    }

    return Status;
}

VOID
Ext2DeferWrite(IN PEXT2_IRP_CONTEXT IrpContext, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Irp);
    ASSERT(IrpContext->Irp == Irp);

    Ext2QueueRequest(IrpContext);
}

NTSTATUS
Ext2WriteInode (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_MCB            Mcb,
    IN ULONGLONG            Offset,
    IN PVOID                Buffer,
    IN ULONG                Size,
    IN BOOLEAN              bDirectIo,
    OUT PULONG              BytesWritten
    )
{
    PEXT2_EXTENT    Chain = NULL;
    NTSTATUS        Status = STATUS_UNSUCCESSFUL;

    __try {

        if (BytesWritten) {
            *BytesWritten = 0;
        }

        Status = Ext2BuildExtents (
                     IrpContext,
                     Vcb,
                     Mcb,
                     Offset,
                     Size,
                     S_ISDIR(Mcb->Inode->i_mode) ? FALSE : TRUE,
                     &Chain
                 );

        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        if (Chain == NULL) {
            Status = Size ? STATUS_DISK_CORRUPT_ERROR : STATUS_SUCCESS;
            __leave;
        }

        {
            PEXT2_EXTENT Extent;
            ULONGLONG Covered = 0;
            for (Extent = Chain; Extent; Extent = Extent->Next) {
                if (Extent->Offset != Covered || !Extent->Length) {
                    Status = STATUS_DISK_CORRUPT_ERROR;
                    __leave;
                }
                Covered += Extent->Length;
            }
            if (Covered < Size) {
                Status = STATUS_DISK_CORRUPT_ERROR;
                __leave;
            }
        }

        if (bDirectIo) {

            ASSERT(IrpContext != NULL);

            /* We assume the offset is aligned. */

            Status = Ext2ReadWriteBlocks(
                         IrpContext,
                         Vcb,
                         Chain,
                         Size
                     );

        } else {

            PEXT2_EXTENT Extent;
            for (Extent = Chain; Extent != NULL; Extent = Extent->Next) {

                if ( !Ext2SaveBuffer(
                            IrpContext,
                            Vcb,
                            Extent->Lba,
                            Extent->Length,
                            (PVOID)((PUCHAR)Buffer + Extent->Offset)
                        )) {
                    Status = STATUS_UNEXPECTED_IO_ERROR;
                    __leave;
                }
            }

            Status = STATUS_SUCCESS;
        }

    } __finally {

        if (Chain) {
            Ext2DestroyExtentChain(Chain);
        }

        if (NT_SUCCESS(Status) && BytesWritten) {
            *BytesWritten = Size;
        }
    }

    return Status;
}

/*
 * A cached write the cache manager has no room for yet waits for it in
 * CcDeferWrite, with its buffer locked: STATUS_PENDING, and the request is
 * CcDeferWrite's. One whose buffer cannot be locked is written now, as it
 * always was: the throttle can only hold a locked buffer.
 */
NTSTATUS
Ext2ThrottleWrite(IN PEXT2_IRP_CONTEXT IrpContext, IN PFILE_OBJECT FileObject, IN ULONG Length)
{
    BOOLEAN bAgain = IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_DEFERRED);
    BOOLEAN bWait  = IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT);
    BOOLEAN bQueue = IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_REQUEUED);

    if (CcCanIWrite(FileObject, Length, (bWait && bQueue), bAgain)) {
        return STATUS_SUCCESS;
    }
    if (!NT_SUCCESS(Ext2LockUserBuffer(IrpContext->Irp, Length, IoReadAccess))) {
        return STATUS_SUCCESS;
    }

    SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_DEFERRED);
    CcDeferWrite(FileObject, (PCC_POST_DEFERRED_WRITE)Ext2DeferWrite,
                 IrpContext, IrpContext->Irp, Length, bAgain);
    return STATUS_PENDING;
}

/*
 * A non-cached write over a stream that is also cached: what the cache
 * holds of [Offset, Offset + Range) goes to the disk first and is purged
 * after, so that neither copy hides the other. The paging I/O in flight is
 * let through before each.
 */
NTSTATUS
Ext2FlushPurgeRange(IN PSECTION_OBJECT_POINTERS Section, IN PERESOURCE PagingIoResource,
                    IN PLARGE_INTEGER Offset, IN ULONG Range, OUT PIO_STATUS_BLOCK IoStatus)
{
    ExAcquireSharedStarveExclusive(PagingIoResource, TRUE);
    ExReleaseResourceLite(PagingIoResource);

    CcFlushCache(Section, Offset, Range, IoStatus);
    if (!NT_SUCCESS(IoStatus->Status)) {
        return IoStatus->Status;
    }

    ExAcquireSharedStarveExclusive(PagingIoResource, TRUE);
    ExReleaseResourceLite(PagingIoResource);

    CcPurgeCacheSection(Section, Offset, Range, FALSE);
    return STATUS_SUCCESS;
}

/* one write to a file, as Ext2WriteFile carries it from step to step */
typedef struct _EXT2_WRITE {
    PIRP            Irp;            /* NULL once the write went asynchronous */
    PFILE_OBJECT    FileObject;
    PEXT2_FCB       Fcb;
    PEXT2_CCB       Ccb;
    LARGE_INTEGER   ByteOffset;
    ULONG           Length;         /* bytes written */
    ULONG           ReturnedLength; /* bytes the caller is told: a paging
                                       write past the end of file writes
                                       the whole page, the part inside it
                                       is reported */
    BOOLEAN         PagingIo;
    BOOLEAN         Nocache;
    BOOLEAN         SynchronousIo;
    BOOLEAN         RecursiveWriteThrough;
    BOOLEAN         MainResourceAcquired;
    BOOLEAN         PagingIoResourceAcquired;
    BOOLEAN         Deferred;       /* CcDeferWrite owns the request */
    BOOLEAN         OpPostIrp;      /* the oplock package owns the request */
    BOOLEAN         FileSizesChanged;
    BOOLEAN         Done;           /* finished before writing anything */
} EXT2_WRITE, *PEXT2_WRITE;

static VOID
Ext2WriteParameters(IN PEXT2_IRP_CONTEXT IrpContext, OUT PEXT2_WRITE W)
{
    PIO_STACK_LOCATION IrpSp;

    RtlZeroMemory(W, sizeof(EXT2_WRITE));
    W->Irp = IrpContext->Irp;
    W->FileObject = IrpContext->FileObject;
    W->Fcb = (PEXT2_FCB) W->FileObject->FsContext;
    W->Ccb = (PEXT2_CCB) W->FileObject->FsContext2;
    ASSERT(W->Fcb);
    ASSERT((W->Fcb->Identifier.Type == EXT2FCB) &&
           (W->Fcb->Identifier.Size == sizeof(EXT2_FCB)));

    IrpSp = IoGetCurrentIrpStackLocation(W->Irp);
    W->Length = IrpSp->Parameters.Write.Length;
    W->ByteOffset = IrpSp->Parameters.Write.ByteOffset;

    W->PagingIo = IsFlagOn(W->Irp->Flags, IRP_PAGING_IO);
    W->Nocache = IsFlagOn(W->Irp->Flags, IRP_NOCACHE);
    W->SynchronousIo = IsFlagOn(W->FileObject->Flags, FO_SYNCHRONOUS_IO);
    if (W->PagingIo) {
        ASSERT(W->Nocache);
    }
}

/* a write finished before it starts; Information is what the caller is told */
static NTSTATUS
Ext2WriteDone(IN OUT PEXT2_WRITE W, IN ULONG_PTR Information)
{
    W->Irp->IoStatus.Information = Information;
    W->Done = TRUE;
    return STATUS_SUCCESS;
}

/* what makes a write wrong, or finished, before anything is locked */
static NTSTATUS
Ext2WriteCheck(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN OUT PEXT2_WRITE W)
{
    PEXT2_FCB   Fcb = W->Fcb;
    NTSTATUS    Status;

    if (IsSpecialFile(Fcb) || IsInodeSymLink(Fcb->Inode)) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    /* The lazy writer or the mapped page writer still flushing a file
       that has been deleted meanwhile: its data has nowhere to go and
       nobody can read it again. Take the write as done, the way NTFS
       and FAT do. Refused, the cache manager kept the pages dirty and
       retried for ever, its shared cache map kept the file object -
       no IRP_MJ_CLOSE, an Fcb reference that never went, and a volume
       (and driver) that could not be released. */
    if (W->PagingIo && IsInodeDeleted(Fcb)) {
        return Ext2WriteDone(W, W->Length);
    }

    if (IsInodeDeleted(Fcb) ||
        (IsSymLink(Fcb) && Ext2IsLinkDangling(Vcb, Fcb->Mcb))) {
        return STATUS_FILE_DELETED;
    }

    /* the offset first: even a write of nothing leaves the file pointer
       after it (Ext2WriteFinish), and the special offsets are no place */
    if (W->ByteOffset.LowPart == FILE_USE_FILE_POINTER_POSITION &&
        W->ByteOffset.HighPart == -1) {
        W->ByteOffset = W->FileObject->CurrentByteOffset;
    } else if (IsWritingToEof(W->ByteOffset)) {
        W->ByteOffset.QuadPart = Fcb->Header.FileSize.QuadPart;
    }

    if (W->Length == 0) {
        return Ext2WriteDone(W, 0);
    }

    /* Check subtraction before any offset+length arithmetic or cache update. */
    Status = Ext4CheckWriteRange(Vcb, Fcb->Inode, W->ByteOffset.QuadPart, W->Length);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    /* an append-only file (chattr +a) grows at its end only; the I/O
       manager already sends an append handle's writes there */
    if (!W->PagingIo && Fcb->Inode && Ext4IsAppendOnly(Fcb->Inode) &&
        W->ByteOffset.QuadPart < Fcb->Header.FileSize.QuadPart) {
        return STATUS_ACCESS_DENIED;
    }

    if (W->Nocache && !W->PagingIo &&
        ((W->ByteOffset.LowPart & (SECTOR_SIZE - 1)) ||
         (W->Length & (SECTOR_SIZE - 1)))) {
        return STATUS_INVALID_PARAMETER;
    }

    if (FlagOn(IrpContext->MinorFunction, IRP_MN_DPC)) {
        ClearFlag(IrpContext->MinorFunction, IRP_MN_DPC);
        return STATUS_PENDING;
    }

    /* a directory's blocks are written by the cache manager only */
    if (IsDirectory(Fcb) && !W->PagingIo) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    return STATUS_SUCCESS;
}

/* A synchronous paging write under a cached write to the same file: the
   cache manager flushing what that write dirtied, from inside it. It goes
   through to the disk, and the sizes are the outer write's to set. */
static BOOLEAN
Ext2WriteIsRecursive(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_WRITE W)
{
    PIRP                TopIrp;
    PIO_STACK_LOCATION  IrpStack;

    if (!IsFlagOn(W->Irp->Flags, IRP_SYNCHRONOUS_PAGING_IO) || IrpContext->IsTopLevel) {
        return FALSE;
    }

    TopIrp = IoGetTopLevelIrp();
    if ((ULONG_PTR)TopIrp <= FSRTL_MAX_TOP_LEVEL_IRP_FLAG ||
        NodeType(TopIrp) != IO_TYPE_IRP) {
        return FALSE;
    }

    IrpStack = IoGetCurrentIrpStackLocation(TopIrp);
    if (IrpStack->MajorFunction != IRP_MJ_WRITE ||
        IrpStack->FileObject->FsContext != W->FileObject->FsContext ||
        FlagOn(TopIrp->Flags, IRP_NOCACHE)) {
        return FALSE;
    }

    SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_WRITE_THROUGH);
    return TRUE;
}

/* A paging write - the lazy writer's or the mapped page writer's pages -
   cut to the file. The shared PagingIoResource keeps the file from
   shrinking under it. */
static NTSTATUS
Ext2WritePagingRange(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN OUT PEXT2_WRITE W)
{
    PEXT2_FCB   Fcb = W->Fcb;
    LONGLONG    End = W->ByteOffset.QuadPart + W->Length;
    NTSTATUS    Status;

    if (!ExAcquireResourceSharedLite(&Fcb->PagingIoResource, TRUE)) {
        return STATUS_PENDING;
    }
    W->PagingIoResourceAcquired = TRUE;

    /* Discard paging writes entirely beyond EOF before subtracting.
       The last partial page is bounded by the logical file size. */
    if (!Ext4PagingWriteLength(W->ByteOffset.QuadPart, Fcb->Header.FileSize.QuadPart, W->Length)) {
        return Ext2WriteDone(W, 0);
    }

    /* a mapped view written to a file Linux keeps inline: the
       cached writes would have converted it, a mapping did not */
    if (Ext4IsInline(Fcb->Inode)) {
        ExReleaseResourceLite(&Fcb->PagingIoResource);
        W->PagingIoResourceAcquired = FALSE;
        ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
        W->PagingIoResourceAcquired = TRUE;
        /* Another writer may have converted the inode meanwhile. */
        Status = Ext4UninlineFile(IrpContext, Vcb, Fcb->Mcb, TRUE);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
        ExConvertExclusiveToSharedLite(&Fcb->PagingIoResource);
    }

    /* the resource was let go for a moment: the end of file again */
    if (!Ext4PagingWriteLength(W->ByteOffset.QuadPart, Fcb->Header.FileSize.QuadPart, W->Length)) {
        return Ext2WriteDone(W, 0);
    }

    if (End <= Fcb->Header.FileSize.QuadPart) {
        W->ReturnedLength = W->Length;
        return STATUS_SUCCESS;
    }
    if (W->ByteOffset.QuadPart >= Fcb->Header.AllocationSize.QuadPart) {
        return Ext2WriteDone(W, 0);
    }
    W->ReturnedLength = (ULONG)(Fcb->Header.FileSize.QuadPart - W->ByteOffset.QuadPart);
    if (End > Fcb->Header.AllocationSize.QuadPart) {
        W->Length = (ULONG)(Fcb->Header.AllocationSize.QuadPart - W->ByteOffset.QuadPart);
    }
    return STATUS_SUCCESS;
}

/* a non-cached write over a file that is also cached: the range in whole
   blocks, written back and purged (Ext2FlushPurgeRange) */
static NTSTATUS
Ext2WriteFlushCached(IN PEXT2_VCB Vcb, IN PEXT2_WRITE W)
{
    PEXT2_FCB   Fcb = W->Fcb;
    NTSTATUS    Status;

    Status = Ext2FlushPurgeRange(&Fcb->SectionObject, &Fcb->PagingIoResource, &W->ByteOffset,
                                 CEILING_ALIGNED(ULONG, W->Length, BLOCK_SIZE), &W->Irp->IoStatus);
    if (NT_SUCCESS(Status)) {
        ClearLongFlag(Fcb->Flags, FCB_FILE_MODIFIED);
    }
    return Status;
}

/* A write past the end of file: the blocks first, then the new size,
   saved before a byte is written. All or nothing, as NTFS and FAT do it:
   a write that does not fit fails with STATUS_DISK_FULL and changes no
   size. It used to be cut to what could be allocated and reported as a
   success, and the next write then failed with
   STATUS_INSUFFICIENT_RESOURCES. What was allocated stays beyond the end
   of file, like any preallocation. */
static NTSTATUS
Ext2WriteExtend(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN OUT PEXT2_WRITE W)
{
    PEXT2_FCB       Fcb = W->Fcb;
    LONGLONG        End = W->ByteOffset.QuadPart + W->Length;
    LARGE_INTEGER   AllocationSize, Last;
    NTSTATUS        Status;

    if (!ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE)) {
        return STATUS_PENDING;
    }
    W->PagingIoResourceAcquired = TRUE;

    /* let this irp wait, since it has to be synchronous */
    SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT);

    Last.QuadPart = Fcb->Header.AllocationSize.QuadPart;
    AllocationSize.QuadPart = CEILING_ALIGNED(ULONGLONG, (ULONGLONG)End,
                                              (ULONGLONG)BLOCK_SIZE);

    /* growth outside a paging write: unwritten extents (or zeroed
       blocks on indirect-mapped files), so the gap reads as zeros.
       Ext2WriteFinish lowers MajorFunction again after an exception. */
    IrpContext->MajorFunction += IRP_MJ_MAXIMUM_FUNCTION;
    Status = Ext2ExpandFile(IrpContext, Vcb, Fcb->Mcb, &AllocationSize);
    IrpContext->MajorFunction -= IRP_MJ_MAXIMUM_FUNCTION;
    if (AllocationSize.QuadPart > Last.QuadPart) {
        Fcb->Header.AllocationSize.QuadPart = AllocationSize.QuadPart;
        SetLongFlag(Fcb->Flags, FCB_ALLOC_IN_WRITE);
    }
    ExReleaseResourceLite(&Fcb->PagingIoResource);
    W->PagingIoResourceAcquired = FALSE;

    /* a failed expansion fails the write even when it got the blocks:
       its error used to be dropped whenever the allocation was enough */
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    if (End > Fcb->Header.AllocationSize.QuadPart) {
        return STATUS_DISK_FULL;
    }

    Fcb->Header.FileSize.QuadPart = Fcb->Inode->i_size = End;
    if (!Ext2SaveInode(IrpContext, Vcb, Fcb->Inode)) {
        return STATUS_UNEXPECTED_IO_ERROR;
    }

    if (CcIsFileCached(W->FileObject)) {
        CcSetFileSizes(W->FileObject, (PCC_FILE_SIZES)(&(Fcb->Header.AllocationSize)));
    }

    W->FileObject->Flags |= FO_FILE_SIZE_CHANGED | FO_FILE_MODIFIED;
    W->FileSizesChanged = TRUE;

    Ext2NoteFileSize(IrpContext, Vcb, Fcb->Header.FileSize.QuadPart);

    DEBUG(DL_IO, ("Ext2WriteFile: expanding %wZ to FS: %I64xh FA: %I64xh\n",
                  &Fcb->Mcb->ShortName, Fcb->Header.FileSize.QuadPart,
                  Fcb->Header.AllocationSize.QuadPart));
    return STATUS_SUCCESS;
}

/* A write from a caller: checked for access, byte-range locks and oplocks
   under the exclusive MainResource, and the file grown to take it. */
static NTSTATUS
Ext2WriteUserRange(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN OUT PEXT2_WRITE W)
{
    PEXT2_FCB   Fcb = W->Fcb;
    NTSTATUS    Status;

    if (!Ext2CheckFileAccess(Vcb, Fcb->Mcb, Ext2FileCanWrite)) {
        return STATUS_ACCESS_DENIED;
    }

    /* growth past the allocation needs blocks; any that only a
       commit can free are waited for here, before any lock
       (ext4\FreedBlocks.c) - the read of the size is a hint */
    if (IrpContext->IsTopLevel && IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT)) {
        Ext2WaitForFreedBlocks(Vcb, Fcb->Header.AllocationSize.QuadPart,
                               W->ByteOffset.QuadPart + W->Length);
    }

    if (!ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE)) {
        return STATUS_PENDING;
    }
    W->MainResourceAcquired = TRUE;

    /* inline data (Linux keeps small files in the inode) goes to a
       block before anything is written */
    if (Ext4IsInline(Fcb->Inode)) {
        ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
        W->PagingIoResourceAcquired = TRUE;
        Status = Ext4UninlineFile(IrpContext, Vcb, Fcb->Mcb, TRUE);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
        ExReleaseResourceLite(&Fcb->PagingIoResource);
        W->PagingIoResourceAcquired = FALSE;
    }

    if (W->Nocache && W->Ccb != NULL && Fcb->SectionObject.DataSectionObject != NULL) {
        Status = Ext2WriteFlushCached(Vcb, W);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }

    if (!FsRtlCheckLockForWriteAccess(&Fcb->FileLockAnchor, W->Irp)) {
        return STATUS_FILE_LOCK_CONFLICT;
    }

    if (W->Ccb != NULL) {
        Status = FsRtlCheckOplock(&Fcb->Oplock,
                                  W->Irp,
                                  IrpContext,
                                  Ext2OplockComplete,
                                  Ext2LockIrp);
        if (Status != STATUS_SUCCESS) {
            W->OpPostIrp = TRUE;
            return Status;
        }

        /* Set the flag indicating if Fast I/O is possible */
        Fcb->Header.IsFastIoPossible = Ext2IsFastIoPossible(Fcb);
    }

    if (W->ByteOffset.QuadPart + W->Length > Fcb->Header.FileSize.QuadPart) {
        Status = Ext2WriteExtend(IrpContext, Vcb, W);
        if (Status != STATUS_SUCCESS) {
            return Status;
        }
    }

    W->ReturnedLength = W->Length;
    return STATUS_SUCCESS;
}

/* after a write that ended at End: the valid data length moves up to it,
   never past the end of file. TRUE when it moved. */
static BOOLEAN
Ext2WriteAdvanceValid(IN PEXT2_FCB Fcb, IN LONGLONG End)
{
    if (End <= Fcb->Header.ValidDataLength.QuadPart) {
        return FALSE;
    }
    Fcb->Header.ValidDataLength.QuadPart = min(End, Fcb->Header.FileSize.QuadPart);
    return TRUE;
}

/* a cached write: copied into the cache, the gap after the valid data
   length zeroed first */
static NTSTATUS
Ext2WriteCached(IN PEXT2_IRP_CONTEXT IrpContext, IN OUT PEXT2_WRITE W)
{
    PEXT2_FCB       Fcb = W->Fcb;
    PFILE_OBJECT    FileObject = W->FileObject;
    PUCHAR          Buffer;
    NTSTATUS        Status;

    if (FileObject->PrivateCacheMap == NULL) {
        CcInitializeCacheMap(FileObject,
                             (PCC_FILE_SIZES)(&Fcb->Header.AllocationSize),
                             FALSE,
                             &Ext2Global->CacheManagerCallbacks,
                             Fcb);
        CcSetReadAheadGranularity(FileObject, READ_AHEAD_GRANULARITY);
    }

    if (FlagOn(IrpContext->MinorFunction, IRP_MN_MDL)) {

        CcPrepareMdlWrite(FileObject, &W->ByteOffset, W->Length,
                          &W->Irp->MdlAddress, &W->Irp->IoStatus);
        Status = W->Irp->IoStatus.Status;

    } else {

        Buffer = Ext2GetUserBuffer(W->Irp);
        if (Buffer == NULL) {
            return STATUS_INVALID_USER_BUFFER;
        }

        if (W->ByteOffset.QuadPart > Fcb->Header.ValidDataLength.QuadPart) {
            /* let this irp wait, since it has to be synchronous */
            SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT);
            Status = Ext2ZeroData(IrpContext, FileObject,
                                  &Fcb->Header.ValidDataLength, &W->ByteOffset);
            if (Status != STATUS_SUCCESS) {
                return Status;
            }
        }

        CcCopyWrite(FileObject, &W->ByteOffset, W->Length, TRUE, Buffer);

        if (Ext2WriteAdvanceValid(Fcb, W->ByteOffset.QuadPart + W->Length)) {
            CcSetFileSizes(FileObject, (PCC_FILE_SIZES)(&(Fcb->Header.AllocationSize)));
            W->FileSizesChanged = TRUE;
        }
        Status = STATUS_SUCCESS;
    }

    if (NT_SUCCESS(Status)) {
        W->Irp->IoStatus.Information = W->Length;
    }
    return Status;
}

/* A non-cached write: straight to the blocks. A caller's write zeroes the
   gap after the valid data length through the cache first; the cache
   manager's own writes are inside the valid data. */
static NTSTATUS
Ext2WriteNoncached(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN OUT PEXT2_WRITE W)
{
    PEXT2_FCB   Fcb = W->Fcb;
    BOOLEAN     Outer = !W->RecursiveWriteThrough && !IsLazyWriter(Fcb);
    NTSTATUS    Status;

    if (!W->PagingIo && Outer &&
        W->ByteOffset.QuadPart > Fcb->Header.ValidDataLength.QuadPart) {
        /* let this irp wait, since it has to be synchronous */
        SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT);
        Status = Ext2ZeroData(IrpContext, W->FileObject,
                              &Fcb->Header.ValidDataLength, &W->ByteOffset);
        if (Status != STATUS_SUCCESS) {
            return Status;
        }
    }

    Status = Ext2LockUserBuffer(W->Irp, W->Length, IoReadAccess);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    W->Irp->IoStatus.Status = STATUS_SUCCESS;
    W->Irp->IoStatus.Information = W->ReturnedLength;

    Status = Ext2WriteInode(IrpContext, Vcb, Fcb->Mcb,
                            (ULONGLONG)(W->ByteOffset.QuadPart), NULL,
                            W->ReturnedLength, TRUE, &W->Length);

    /* gone asynchronous: the IRP, and the resource it was written under,
       are its completion routine's now (Ext2ReadWriteBlocks) */
    W->Irp = IrpContext->Irp;

    if (NT_SUCCESS(Status) && Outer &&
        Ext2WriteAdvanceValid(Fcb, W->ByteOffset.QuadPart + W->Length)) {

        W->FileSizesChanged = TRUE;
        if (!W->PagingIo && CcIsFileCached(W->FileObject)) {
            CcSetFileSizes(W->FileObject, (PCC_FILE_SIZES)(&(Fcb->Header.AllocationSize)));
        }

        DEBUG(DL_IO, ("Ext2WriteFile: %wZ written FS: %I64xh FA: %I64xh BO: %I64xh LEN: %u\n",
                      &Fcb->Mcb->ShortName, Fcb->Header.FileSize.QuadPart,
                      Fcb->Header.AllocationSize.QuadPart, W->ByteOffset.QuadPart, W->Length));
    }
    return Status;
}

static NTSTATUS
Ext2WriteFinish(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_WRITE W, IN NTSTATUS Status)
{
    /* An exception inside Ext2ExpandFile leaves MajorFunction raised
       (Ext2WriteExtend): back to IRP_MJ_WRITE, which tells
       Ext2ExpandFile whether to allocate unwritten extents. */
    if (IrpContext->MajorFunction > IRP_MJ_MAXIMUM_FUNCTION) {
        IrpContext->MajorFunction -= IRP_MJ_MAXIMUM_FUNCTION;
    }

    if (W->Irp != NULL) {
        if (W->PagingIoResourceAcquired) {
            ExReleaseResourceLite(&W->Fcb->PagingIoResource);
        }
        if (W->MainResourceAcquired) {
            ExReleaseResourceLite(&W->Fcb->MainResource);
        }
    }

    if (W->OpPostIrp || IrpContext->ExceptionInProgress) {
        return Status;
    }

    if (W->Irp == NULL) {
        Ext2FreeIrpContext(IrpContext);
        return Status;
    }

    if (Status == STATUS_PENDING || Status == STATUS_CANT_WAIT) {
        if (!W->Deferred) {
            Status = Ext2QueueRequest(IrpContext);
        }
        return Status;
    }

    if (NT_SUCCESS(Status) && !W->PagingIo) {
        if (W->SynchronousIo) {
            W->FileObject->CurrentByteOffset.QuadPart =
                W->ByteOffset.QuadPart + W->Irp->IoStatus.Information;
        }
        SetFlag(W->FileObject->Flags, FO_FILE_MODIFIED);
        SetLongFlag(W->Fcb->Flags, FCB_FILE_MODIFIED);
    }

    Ext2CompleteIrpContext(IrpContext, Status);
    return Status;
}

NTSTATUS
Ext2WriteFile(IN PEXT2_IRP_CONTEXT IrpContext)
{
    PEXT2_VCB   Vcb;
    EXT2_WRITE  W;
    NTSTATUS    Status = STATUS_UNSUCCESSFUL;

    ASSERT(IrpContext);
    ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
           (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

    Vcb = (PEXT2_VCB) IrpContext->DeviceObject->DeviceExtension;
    ASSERT(Vcb != NULL);
    ASSERT((Vcb->Identifier.Type == EXT2VCB) &&
           (Vcb->Identifier.Size == sizeof(EXT2_VCB)));

    Ext2WriteParameters(IrpContext, &W);

    DEBUG(DL_INF, ("Ext2WriteFile: %wZ Offset=%I64xh Length=%xh Paging=%xh Nocache=%xh\n",
                   &W.Fcb->Mcb->ShortName, W.ByteOffset.QuadPart, W.Length, W.PagingIo, W.Nocache));

    __try {

        Status = Ext2WriteCheck(IrpContext, Vcb, &W);
        if (Status != STATUS_SUCCESS || W.Done) {
            __leave;
        }

        if (!W.Nocache) {
            Status = Ext2ThrottleWrite(IrpContext, W.FileObject, W.Length);
            if (Status != STATUS_SUCCESS) {
                W.Deferred = Status == STATUS_PENDING;
                __leave;
            }
        }

        W.RecursiveWriteThrough = Ext2WriteIsRecursive(IrpContext, &W);

        if (W.PagingIo) {
            Status = Ext2WritePagingRange(IrpContext, Vcb, &W);
        } else {
            Status = Ext2WriteUserRange(IrpContext, Vcb, &W);
        }
        if (Status != STATUS_SUCCESS || W.Done) {
            __leave;
        }

        if (W.Nocache) {
            Status = Ext2WriteNoncached(IrpContext, Vcb, &W);
        } else {
            Status = Ext2WriteCached(IrpContext, &W);
        }

        /* a size that changed is reported even when the copy then failed:
           the new size is on the disk */
        if (W.FileSizesChanged) {
            W.FileObject->Flags |= FO_FILE_SIZE_CHANGED | FO_FILE_MODIFIED;
            Ext2NotifyReportChange(IrpContext, Vcb, W.Fcb->Mcb,
                                   FILE_NOTIFY_CHANGE_SIZE,
                                   FILE_ACTION_MODIFIED);
        }

    } __finally {

        Status = Ext2WriteFinish(IrpContext, &W, Status);
    }

    return Status;
}

NTSTATUS
Ext2WriteComplete (IN PEXT2_IRP_CONTEXT IrpContext)
{
    NTSTATUS        Status = STATUS_UNSUCCESSFUL;
    PFILE_OBJECT    FileObject;
    PIRP            Irp;
    PIO_STACK_LOCATION IrpSp;

    __try {

        ASSERT(IrpContext);
        ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
               (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

        FileObject = IrpContext->FileObject;

        Irp = IrpContext->Irp;
        IrpSp = IoGetCurrentIrpStackLocation(Irp);

        CcMdlWriteComplete(FileObject, &(IrpSp->Parameters.Write.ByteOffset), Irp->MdlAddress);
        Irp->MdlAddress = NULL;
        Status = STATUS_SUCCESS;

    } __finally {

        if (!IrpContext->ExceptionInProgress) {
            Ext2CompleteIrpContext(IrpContext, Status);
        }
    }

    return Status;
}

NTSTATUS
Ext2Write (IN PEXT2_IRP_CONTEXT IrpContext)
{
    NTSTATUS Status = STATUS_UNSUCCESSFUL;
    PEXT2_FCBVCB        FcbOrVcb;
    PDEVICE_OBJECT      DeviceObject;
    PFILE_OBJECT        FileObject;
    PEXT2_VCB           Vcb;
    BOOLEAN             bCompleteRequest = TRUE;

    ASSERT(IrpContext);

    ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
           (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

    __try {

        if (IsFlagOn(IrpContext->MinorFunction, IRP_MN_COMPLETE)) {

            Status =  Ext2WriteComplete(IrpContext);
            bCompleteRequest = FALSE;

        } else {

            DeviceObject = IrpContext->DeviceObject;
            if (IsExt2FsDevice(DeviceObject)) {
                Status = STATUS_INVALID_DEVICE_REQUEST;
                __leave;
            }
            FileObject = IrpContext->FileObject;

            Vcb = (PEXT2_VCB) DeviceObject->DeviceExtension;

            if (Vcb->Identifier.Type != EXT2VCB ||
                    Vcb->Identifier.Size != sizeof(EXT2_VCB) ) {
                Status = STATUS_INVALID_PARAMETER;
                __leave;
            }

            if (IsVcbReadOnly(Vcb)) {
                Status = STATUS_MEDIA_WRITE_PROTECTED;
                __leave;
            }

            /* Recovery may flush the internal stream, never user data. */
            if (IsFlagOn(Vcb->Flags, VCB_WRITE_TRANSITION) &&
                FileObject != Vcb->Volume) {
                Status = STATUS_DEVICE_NOT_READY;
                __leave;
            }

            if (IsFlagOn(Vcb->Flags, VCB_DEVICE_REMOVED)) {
                Status = STATUS_NO_SUCH_DEVICE;
                __leave;
            }

            if (FlagOn(Vcb->Flags, VCB_VOLUME_LOCKED) &&
                Vcb->LockFile != FileObject ) {
                Status = STATUS_ACCESS_DENIED;
                __leave;
            }

            FcbOrVcb = (PEXT2_FCBVCB) FileObject->FsContext;

            if (FcbOrVcb->Identifier.Type == EXT2VCB) {

                Status = Ext2WriteVolume(IrpContext);
                bCompleteRequest = FALSE;

            } else if (FcbOrVcb->Identifier.Type == EXT2FCB) {

                /* Dirty pages of a deleted file, flushed by the mapped page
                   writer - possibly while the volume dismounts: accepted
                   and dropped (see Ext2WriteFile). Refused here as "too
                   late", the writer kept them and the section's control
                   area kept its file object: no close, no dismount. */
                if (IsFlagOn(IrpContext->Irp->Flags, IRP_PAGING_IO) &&
                    IsInodeDeleted((PEXT2_FCB)FcbOrVcb)) {
                    IrpContext->Irp->IoStatus.Information =
                        IoGetCurrentIrpStackLocation(IrpContext->Irp)->Parameters.Write.Length;
                    Status = STATUS_SUCCESS;
                    __leave;
                }

                if (IsFlagOn(Vcb->Flags, VCB_DISMOUNT_PENDING)) {
                    Status = STATUS_TOO_LATE;
                    __leave;
                }

                Status = Ext2WriteFile(IrpContext);

                bCompleteRequest = FALSE;
            } else {
                Status = STATUS_INVALID_PARAMETER;
            }
        }

    } __finally {

        if (bCompleteRequest) {
            Ext2CompleteIrpContext(IrpContext, Status);
        }
    }

    return Status;
}
