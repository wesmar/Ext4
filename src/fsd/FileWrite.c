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

BOOLEAN
Ext2ZeroData (
    IN PEXT2_IRP_CONTEXT IrpContext,
    IN PEXT2_VCB         Vcb,
    IN PFILE_OBJECT      FileObject,
    IN PLARGE_INTEGER    Start,
    IN PLARGE_INTEGER    End
    )
{
    UNREFERENCED_PARAMETER(Vcb);
    PEXT2_FCB       Fcb;
    BOOLEAN         rc = TRUE;

    ASSERT (End && Start && End->QuadPart > Start->QuadPart);
    Fcb = (PEXT2_FCB) FileObject->FsContext;

    /* Alignment alone does not prove that the range is unwritten.
       Truncation can leave initialized blocks which must be cleared. */

    /* clear data in range [Start, End) */
    __try {
        rc = CcZeroData(FileObject, Start, End, Ext2CanIWait());
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        rc = FALSE;
    }

    return rc;
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

NTSTATUS
Ext2WriteFile(IN PEXT2_IRP_CONTEXT IrpContext)
{
    PEXT2_VCB           Vcb = NULL;
    PEXT2_FCB           Fcb = NULL;
    PEXT2_CCB           Ccb = NULL;
    PFILE_OBJECT        FileObject = NULL;

    PDEVICE_OBJECT      DeviceObject = NULL;

    PIRP                Irp = NULL;
    PIO_STACK_LOCATION  IoStackLocation = NULL;
    PUCHAR              Buffer = NULL;

    LARGE_INTEGER ByteOffset = { 0 };
    ULONG               ReturnedLength = 0;
    ULONG               Length;

    NTSTATUS            Status = STATUS_UNSUCCESSFUL;

    BOOLEAN             OpPostIrp = FALSE;
    BOOLEAN             PagingIo = FALSE;
    BOOLEAN             Nocache = FALSE;
    BOOLEAN             SynchronousIo = FALSE;

    BOOLEAN             RecursiveWriteThrough = FALSE;
    BOOLEAN             MainResourceAcquired = FALSE;
    BOOLEAN             PagingIoResourceAcquired = FALSE;

    BOOLEAN             bDeferred = FALSE;
    BOOLEAN             FileSizesChanged = FALSE;
    BOOLEAN             rc;


    __try {

        ASSERT(IrpContext);
        ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
               (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

        DeviceObject = IrpContext->DeviceObject;
        Vcb = (PEXT2_VCB) DeviceObject->DeviceExtension;
        ASSERT(Vcb != NULL);
        ASSERT((Vcb->Identifier.Type == EXT2VCB) &&
               (Vcb->Identifier.Size == sizeof(EXT2_VCB)));

        FileObject = IrpContext->FileObject;
        Fcb = (PEXT2_FCB) FileObject->FsContext;
        Ccb = (PEXT2_CCB) FileObject->FsContext2;
        ASSERT(Fcb);
        ASSERT((Fcb->Identifier.Type == EXT2FCB) &&
               (Fcb->Identifier.Size == sizeof(EXT2_FCB)));

        Irp = IrpContext->Irp;
        IoStackLocation = IoGetCurrentIrpStackLocation(Irp);

        Length = IoStackLocation->Parameters.Write.Length;
        ByteOffset = IoStackLocation->Parameters.Write.ByteOffset;

        PagingIo = IsFlagOn(Irp->Flags, IRP_PAGING_IO);
        Nocache = IsFlagOn(Irp->Flags, IRP_NOCACHE);
        SynchronousIo = IsFlagOn(FileObject->Flags, FO_SYNCHRONOUS_IO);

        if (PagingIo) {
            ASSERT(Nocache);
        }

        DEBUG(DL_INF, ("Ext2WriteFile: %wZ Offset=%I64xh Length=%xh Paging=%xh Nocache=%xh\n",
                       &Fcb->Mcb->ShortName, ByteOffset.QuadPart, Length, PagingIo, Nocache));

        if (IsSpecialFile(Fcb) || IsInodeSymLink(Fcb->Inode) ) {
            Status = STATUS_INVALID_DEVICE_REQUEST;
            __leave;
        }

        /* The lazy writer or the mapped page writer still flushing a file
           that has been deleted meanwhile: its data has nowhere to go and
           nobody can read it again. Take the write as done, the way NTFS
           and FAT do. Refused, the cache manager kept the pages dirty and
           retried for ever, its shared cache map kept the file object -
           no IRP_MJ_CLOSE, an Fcb reference that never went, and a volume
           (and driver) that could not be released. */
        if (PagingIo && IsInodeDeleted(Fcb)) {
            Irp->IoStatus.Information = Length;
            Status = STATUS_SUCCESS;
            __leave;
        }

        if (IsInodeDeleted(Fcb) ||
            (IsSymLink(Fcb) && Ext2IsLinkDangling(Vcb, Fcb->Mcb)) ) {
            Status = STATUS_FILE_DELETED;
            __leave;
        }

        if (Length == 0) {
            Irp->IoStatus.Information = 0;
            Status = STATUS_SUCCESS;
            __leave;
        }

        if (ByteOffset.LowPart == FILE_USE_FILE_POINTER_POSITION &&
            ByteOffset.HighPart == -1) {
            ByteOffset = FileObject->CurrentByteOffset;
        } else if (IsWritingToEof(ByteOffset)) {
            ByteOffset.QuadPart = Fcb->Header.FileSize.QuadPart;
        }

        /* Check subtraction before any offset+length arithmetic or cache update. */
        Status = Ext4CheckWriteRange(Vcb, Fcb->Inode, ByteOffset.QuadPart, Length);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        /* an append-only file (chattr +a) grows at its end only; the I/O
           manager already sends an append handle's writes there */
        if (!PagingIo && Fcb->Inode && Ext4IsAppendOnly(Fcb->Inode) &&
            ByteOffset.QuadPart < Fcb->Header.FileSize.QuadPart) {
            Status = STATUS_ACCESS_DENIED;
            __leave;
        }

        if (Nocache && !PagingIo &&
            ( (ByteOffset.LowPart & (SECTOR_SIZE - 1)) ||
               (Length & (SECTOR_SIZE - 1))) ) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        if (FlagOn(IrpContext->MinorFunction, IRP_MN_DPC)) {
            ClearFlag(IrpContext->MinorFunction, IRP_MN_DPC);
            Status = STATUS_PENDING;
            __leave;
        }

        if (!Nocache) {

            BOOLEAN bAgain = IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_DEFERRED);
            BOOLEAN bWait  = IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT);
            BOOLEAN bQueue = IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_REQUEUED);

            if ( !CcCanIWrite(
                        FileObject,
                        Length,
                        (bWait && bQueue),
                        bAgain ) ) {

                Status = Ext2LockUserBuffer(
                             IrpContext->Irp,
                             Length,
                             IoReadAccess);

                if (NT_SUCCESS(Status)) {
                    SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_DEFERRED);
                    CcDeferWrite( FileObject,
                                  (PCC_POST_DEFERRED_WRITE)Ext2DeferWrite,
                                  IrpContext,
                                  Irp,
                                  Length,
                                  bAgain );
                    bDeferred = TRUE;
                    Status = STATUS_PENDING;
                    __leave;
                }
            }
        }

        if (IsDirectory(Fcb) && !PagingIo) {
            Status = STATUS_INVALID_DEVICE_REQUEST;
            __leave;
        }

        if (IsFlagOn(Irp->Flags, IRP_SYNCHRONOUS_PAGING_IO) && !IrpContext->IsTopLevel) {

            PIRP TopIrp;

            TopIrp = IoGetTopLevelIrp();

            if ( (ULONG_PTR)TopIrp > FSRTL_MAX_TOP_LEVEL_IRP_FLAG &&
                    NodeType(TopIrp) == IO_TYPE_IRP) {

                PIO_STACK_LOCATION IrpStack;

                IrpStack = IoGetCurrentIrpStackLocation(TopIrp);

                if ((IrpStack->MajorFunction == IRP_MJ_WRITE) &&
                    (IrpStack->FileObject->FsContext == FileObject->FsContext) &&
                    !FlagOn(TopIrp->Flags, IRP_NOCACHE) ) {

                    SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_WRITE_THROUGH);
                    RecursiveWriteThrough = TRUE;
                }
            }
        }

        if (PagingIo) {

            if (!ExAcquireResourceSharedLite(&Fcb->PagingIoResource, TRUE)) {
                Status = STATUS_PENDING;
                __leave;
            }
            PagingIoResourceAcquired = TRUE;

            /* Discard paging writes entirely beyond EOF before subtracting.
               The last partial page is bounded by the logical file size. */
            ReturnedLength = Ext4PagingWriteLength(ByteOffset.QuadPart,
                Fcb->Header.FileSize.QuadPart, Length);
            if (!ReturnedLength) {
                Status = STATUS_SUCCESS;
                Irp->IoStatus.Information = 0;
                __leave;
            }

            /* a mapped view written to a file Linux keeps inline: the
               cached writes would have converted it, a mapping did not */
            if (Ext4IsInline(Fcb->Inode)) {
                ExReleaseResourceLite(&Fcb->PagingIoResource);
                PagingIoResourceAcquired = FALSE;
                ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
                PagingIoResourceAcquired = TRUE;
                /* Another writer may have converted the inode meanwhile. */
                Status = Ext4UninlineFile(IrpContext, Vcb, Fcb->Mcb, TRUE);
                if (!NT_SUCCESS(Status)) {
                    __leave;
                }
                ExConvertExclusiveToSharedLite(&Fcb->PagingIoResource);
            }

            ReturnedLength = Ext4PagingWriteLength(ByteOffset.QuadPart,
                Fcb->Header.FileSize.QuadPart, Length);
            if (!ReturnedLength) {
                Status = STATUS_SUCCESS;
                Irp->IoStatus.Information = 0;
                __leave;
            }

            if ( (ByteOffset.QuadPart + Length) > Fcb->Header.FileSize.QuadPart) {

                if (ByteOffset.QuadPart >= Fcb->Header.AllocationSize.QuadPart) {

                    Status = STATUS_SUCCESS;
                    Irp->IoStatus.Information = 0;
                    __leave;

                } else {

                    ReturnedLength = (ULONG)(Fcb->Header.FileSize.QuadPart - ByteOffset.QuadPart);
                    if (ByteOffset.QuadPart + Length > Fcb->Header.AllocationSize.QuadPart)
                        Length = (ULONG)(Fcb->Header.AllocationSize.QuadPart - ByteOffset.QuadPart);
                }

            } else {

                ReturnedLength = Length;
            }

        } else {

            if (!Ext2CheckFileAccess(Vcb, Fcb->Mcb, Ext2FileCanWrite)) {
                Status = STATUS_ACCESS_DENIED;
                __leave;
            }

            if (IsDirectory(Fcb)) {
                __leave;
            }

            if (!ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE)) {
                Status = STATUS_PENDING;
                __leave;
            }
            MainResourceAcquired = TRUE;

            /* inline data (Linux keeps small files in the inode) goes to a
               block before anything is written */
            if (Ext4IsInline(Fcb->Inode)) {
                ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
                PagingIoResourceAcquired = TRUE;
                Status = Ext4UninlineFile(IrpContext, Vcb, Fcb->Mcb, TRUE);
                if (!NT_SUCCESS(Status)) {
                    __leave;
                }
                ExReleaseResourceLite(&Fcb->PagingIoResource);
                PagingIoResourceAcquired = FALSE;
            }

            /* Do flushing for such cases */
            if (Nocache && Ccb != NULL && Fcb->SectionObject.DataSectionObject != NULL)  {

                ExAcquireSharedStarveExclusive( &Fcb->PagingIoResource, TRUE);
                ExReleaseResourceLite(&Fcb->PagingIoResource);

                CcFlushCache( &(Fcb->SectionObject),
                              &ByteOffset,
                              CEILING_ALIGNED(ULONG, Length, BLOCK_SIZE),
                              &(Irp->IoStatus));
                ClearLongFlag(Fcb->Flags, FCB_FILE_MODIFIED);

                if (!NT_SUCCESS(Irp->IoStatus.Status)) {
                    Status = Irp->IoStatus.Status;
                    __leave;
                }

                ExAcquireSharedStarveExclusive( &Fcb->PagingIoResource, TRUE);
                ExReleaseResourceLite(&Fcb->PagingIoResource);

                CcPurgeCacheSection( &(Fcb->SectionObject),
                                     &(ByteOffset),
                                     CEILING_ALIGNED(ULONG, Length, BLOCK_SIZE),
                                     FALSE );
            }

            if (!FsRtlCheckLockForWriteAccess(&Fcb->FileLockAnchor, Irp)) {
                Status = STATUS_FILE_LOCK_CONFLICT;
                __leave;
            }

            if (Ccb != NULL) {

                Status = FsRtlCheckOplock( &Fcb->Oplock,
                                           Irp,
                                           IrpContext,
                                           Ext2OplockComplete,
                                           Ext2LockIrp );

                if (Status != STATUS_SUCCESS) {
                    OpPostIrp = TRUE;
                    __leave;
                }

                /* Set the flag indicating if Fast I/O is possible */

                Fcb->Header.IsFastIoPossible = Ext2IsFastIoPossible(Fcb);
            }

            /* Extend the inode size when the i/o is beyond the file end ? */

            if ((ByteOffset.QuadPart + Length) > Fcb->Header.FileSize.QuadPart) {

                LARGE_INTEGER AllocationSize, Last;

                if (!ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE)) {
                    Status = STATUS_PENDING;
                    __leave;
                }
                PagingIoResourceAcquired = TRUE;

                /* let this irp wait, since it has to be synchronous */
                SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT);

                Last.QuadPart = Fcb->Header.AllocationSize.QuadPart;
                AllocationSize.QuadPart = (LONGLONG)(ByteOffset.QuadPart + Length);
                AllocationSize.QuadPart = CEILING_ALIGNED(ULONGLONG,
                                          (ULONGLONG)AllocationSize.QuadPart,
                                          (ULONGLONG)BLOCK_SIZE);

                /* growth outside a paging write: unwritten extents (or zeroed
                   blocks on indirect-mapped files), so the gap reads as zeros */
                IrpContext->MajorFunction += IRP_MJ_MAXIMUM_FUNCTION;
                Status = Ext2ExpandFile(IrpContext, Vcb, Fcb->Mcb, &AllocationSize);
                IrpContext->MajorFunction -= IRP_MJ_MAXIMUM_FUNCTION;
                if (AllocationSize.QuadPart > Last.QuadPart) {
                    Fcb->Header.AllocationSize.QuadPart = AllocationSize.QuadPart;
                    SetLongFlag(Fcb->Flags, FCB_ALLOC_IN_WRITE);
                }
                ExReleaseResourceLite(&Fcb->PagingIoResource);
                PagingIoResourceAcquired = FALSE;

                /* all or nothing, as NTFS and FAT do it: a write that does
                   not fit fails with STATUS_DISK_FULL and changes nothing.
                   It used to be cut to what could be allocated and reported
                   as a success, and the next write then failed with
                   STATUS_INSUFFICIENT_RESOURCES. What was allocated stays
                   beyond the end of file, like any preallocation. */
                if (ByteOffset.QuadPart + Length > Fcb->Header.AllocationSize.QuadPart) {
                    if (NT_SUCCESS(Status)) {
                        Status = STATUS_DISK_FULL;
                    }
                    __leave;
                }

                Fcb->Header.FileSize.QuadPart = Fcb->Inode->i_size = ByteOffset.QuadPart + Length;
                if (!Ext2SaveInode(IrpContext, Vcb, Fcb->Inode)) {
                    Status = STATUS_UNEXPECTED_IO_ERROR;
                    __leave;
                }

                if (CcIsFileCached(FileObject)) {
                    CcSetFileSizes(FileObject, (PCC_FILE_SIZES)(&(Fcb->Header.AllocationSize)));
                }

                FileObject->Flags |= FO_FILE_SIZE_CHANGED | FO_FILE_MODIFIED;
                FileSizesChanged = TRUE;

                if (Fcb->Header.FileSize.QuadPart >= 0x80000000 &&
                        !IsFlagOn(SUPER_BLOCK->s_feature_ro_compat, EXT2_FEATURE_RO_COMPAT_LARGE_FILE)) {
                    Ext2SetSuperRoCompat(IrpContext, Vcb, EXT2_FEATURE_RO_COMPAT_LARGE_FILE);
                }

                DEBUG(DL_IO, ("Ext2WriteFile: expanding %wZ to FS: %I64xh FA: %I64xh\n",
                              &Fcb->Mcb->ShortName, Fcb->Header.FileSize.QuadPart,
                              Fcb->Header.AllocationSize.QuadPart));
            }

            ReturnedLength = Length;
        }

        if (!Nocache) {

            if (FileObject->PrivateCacheMap == NULL) {
                CcInitializeCacheMap(
                    FileObject,
                    (PCC_FILE_SIZES)(&Fcb->Header.AllocationSize),
                    FALSE,
                    &Ext2Global->CacheManagerCallbacks,
                    Fcb );

                CcSetReadAheadGranularity(
                    FileObject,
                    READ_AHEAD_GRANULARITY );
            }

            if (FlagOn(IrpContext->MinorFunction, IRP_MN_MDL)) {

                CcPrepareMdlWrite(
                    FileObject,
                    &ByteOffset,
                    Length,
                    &Irp->MdlAddress,
                    &Irp->IoStatus );

                Status = Irp->IoStatus.Status;

            } else {

                Buffer = Ext2GetUserBuffer(Irp);
                if (Buffer == NULL) {
                    Status = STATUS_INVALID_USER_BUFFER;
                    __leave;
                }

                if (ByteOffset.QuadPart > Fcb->Header.ValidDataLength.QuadPart) {

                    /* let this irp wait, since it has to be synchronous */
                    SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT);

                    rc = Ext2ZeroData(IrpContext, Vcb, FileObject,
                                      &Fcb->Header.ValidDataLength, &ByteOffset);
                    if (!rc) {
                        Status = STATUS_PENDING;
                        __leave;
                    }
                }

                CcCopyWrite(FileObject, &ByteOffset, Length, TRUE, Buffer);

                if (ByteOffset.QuadPart + Length > Fcb->Header.ValidDataLength.QuadPart ) {

                    if (Fcb->Header.FileSize.QuadPart < ByteOffset.QuadPart + Length) {
                        Fcb->Header.ValidDataLength.QuadPart = Fcb->Header.FileSize.QuadPart;
                    } else {
                        if (Fcb->Header.ValidDataLength.QuadPart < ByteOffset.QuadPart + Length)
                            Fcb->Header.ValidDataLength.QuadPart = ByteOffset.QuadPart + Length;
                    }

                    CcSetFileSizes(FileObject, (PCC_FILE_SIZES)(&(Fcb->Header.AllocationSize)));
                    FileSizesChanged = TRUE;
                }

                Status = STATUS_SUCCESS;
            }

            if (NT_SUCCESS(Status)) {
                Irp->IoStatus.Information = Length;
            }

        } else {

            if (!PagingIo && !RecursiveWriteThrough && !IsLazyWriter(Fcb)) {
                if (ByteOffset.QuadPart > Fcb->Header.ValidDataLength.QuadPart) {

                    /* let this irp wait, since it has to be synchronous */
                    SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT);
                    rc = Ext2ZeroData(IrpContext, Vcb, FileObject,
                                      &Fcb->Header.ValidDataLength,
                                      &ByteOffset);
                    if (!rc) {
                        Status = STATUS_PENDING;
                        __leave;
                    }
                }
            }

            Status = Ext2LockUserBuffer(
                         IrpContext->Irp,
                         Length,
                         IoReadAccess );

            if (!NT_SUCCESS(Status)) {
                __leave;
            }

            Irp->IoStatus.Status = STATUS_SUCCESS;
            Irp->IoStatus.Information = ReturnedLength;

            Status = Ext2WriteInode(
                         IrpContext,
                         Vcb,
                         Fcb->Mcb,
                         (ULONGLONG)(ByteOffset.QuadPart),
                         NULL,
                         ReturnedLength,
                         TRUE,
                         &Length
                     );

            Irp = IrpContext->Irp;

            if (NT_SUCCESS(Status) && !RecursiveWriteThrough && !IsLazyWriter(Fcb)) {

                if (ByteOffset.QuadPart + Length > Fcb->Header.ValidDataLength.QuadPart ) {

                    FileSizesChanged = TRUE;

                    if (Fcb->Header.FileSize.QuadPart < ByteOffset.QuadPart + Length) {
                        if (!PagingIo)
                            Fcb->Header.FileSize.QuadPart = ByteOffset.QuadPart + Length;
                        Fcb->Header.ValidDataLength.QuadPart = Fcb->Header.FileSize.QuadPart;
                    } else {
                        if (Fcb->Header.ValidDataLength.QuadPart < ByteOffset.QuadPart + Length)
                            Fcb->Header.ValidDataLength.QuadPart = ByteOffset.QuadPart + Length;
                    }

                    if (!PagingIo && CcIsFileCached(FileObject)) {
                        CcSetFileSizes(FileObject, (PCC_FILE_SIZES)(&(Fcb->Header.AllocationSize)));
                    }

                    DEBUG(DL_IO, ("Ext2WriteFile: %wZ written FS: %I64xh FA: %I64xh BO: %I64xh LEN: %u\n",
                                  &Fcb->Mcb->ShortName, Fcb->Header.FileSize.QuadPart,
                                   Fcb->Header.AllocationSize.QuadPart, ByteOffset.QuadPart, Length));
                }
            }
        }

        if (FileSizesChanged) {
            FileObject->Flags |= FO_FILE_SIZE_CHANGED | FO_FILE_MODIFIED;
            Ext2NotifyReportChange( IrpContext,  Vcb, Fcb->Mcb,
                                    FILE_NOTIFY_CHANGE_SIZE,
                                    FILE_ACTION_MODIFIED );
        }

    } __finally {

        /*
         *  in case we got excpetions, we need revert MajorFunction
         *  back to IRP_MJ_WRITE. The reason we do this, is to tell
         *  Ext2ExpandFile to allocate unwritten extent or don't add
         *  new blocks for indirect files.
         */
        if (IrpContext->MajorFunction > IRP_MJ_MAXIMUM_FUNCTION)
            IrpContext->MajorFunction -= IRP_MJ_MAXIMUM_FUNCTION;

        if (Irp) {
            if (PagingIoResourceAcquired) {
                ExReleaseResourceLite(&Fcb->PagingIoResource);
            }

            if (MainResourceAcquired) {
                ExReleaseResourceLite(&Fcb->MainResource);
            }
        }

        if (!OpPostIrp && !IrpContext->ExceptionInProgress) {

            if (Irp) {

                if (Status == STATUS_PENDING ||
                        Status == STATUS_CANT_WAIT ) {

                    if (!bDeferred) {
                        Status = Ext2QueueRequest(IrpContext);
                    }

                } else {

                    if (NT_SUCCESS(Status) && !PagingIo) {

                        if (SynchronousIo) {
                            FileObject->CurrentByteOffset.QuadPart =
                                ByteOffset.QuadPart + Irp->IoStatus.Information;
                        }

                        SetFlag(FileObject->Flags, FO_FILE_MODIFIED);
                        SetLongFlag(Fcb->Flags, FCB_FILE_MODIFIED);
                    }

                    Ext2CompleteIrpContext(IrpContext, Status);
                }
            } else {
                Ext2FreeIrpContext(IrpContext);
            }
        }
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
