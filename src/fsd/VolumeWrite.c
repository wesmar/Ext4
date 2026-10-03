/**
 * VolumeWrite.c - IRP_MJ_WRITE on the volume stream: dirty extent gated metadata writes.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "write_internal.h"

NTSTATUS
Ext2WriteVolume (IN PEXT2_IRP_CONTEXT IrpContext)
{
    NTSTATUS            Status = STATUS_UNSUCCESSFUL;

    PEXT2_VCB           Vcb = NULL;
    PEXT2_CCB           Ccb = NULL;
    PEXT2_FCBVCB        FcbOrVcb = NULL;
    PFILE_OBJECT        FileObject = NULL;

    PDEVICE_OBJECT      DeviceObject = NULL;

    PIRP                Irp = NULL;
    PIO_STACK_LOCATION  IoStackLocation = NULL;

    ULONG Length = 0;
    LARGE_INTEGER ByteOffset = { 0 };

    BOOLEAN             PagingIo = FALSE;
    BOOLEAN             Nocache = FALSE;
    BOOLEAN             SynchronousIo = FALSE;
    BOOLEAN             MainResourceAcquired = FALSE;

    BOOLEAN             bDeferred = FALSE;

    PUCHAR              Buffer = NULL;
    PEXT2_EXTENT        Chain = NULL;
    EXT2_EXTENT         BlockArray;

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
        FcbOrVcb = (PEXT2_FCBVCB) FileObject->FsContext;
        ASSERT(FcbOrVcb);

        if (!(FcbOrVcb->Identifier.Type == EXT2VCB && (PVOID)FcbOrVcb == (PVOID)Vcb)) {
            Status = STATUS_INVALID_DEVICE_REQUEST;
            __leave;
        }

        Ccb = (PEXT2_CCB) FileObject->FsContext2;
        Irp = IrpContext->Irp;
        IoStackLocation = IoGetCurrentIrpStackLocation(Irp);

        Length = IoStackLocation->Parameters.Write.Length;
        ByteOffset = IoStackLocation->Parameters.Write.ByteOffset;

        PagingIo = IsFlagOn(Irp->Flags, IRP_PAGING_IO);
        Nocache = IsFlagOn(Irp->Flags, IRP_NOCACHE) || (Ccb != NULL);
        SynchronousIo = IsFlagOn(FileObject->Flags, FO_SYNCHRONOUS_IO);

        if (PagingIo) {
            ASSERT(Nocache);
        }

        DEBUG(DL_INF, ("Ext2WriteVolume: Off=%I64xh Len=%xh Paging=%xh Nocache=%xh\n",
                       ByteOffset.QuadPart, Length, PagingIo, Nocache));

        if (Length == 0) {
            Irp->IoStatus.Information = 0;
            Status = STATUS_SUCCESS;
            __leave;
        }

        if (Nocache &&
                (ByteOffset.LowPart & (SECTOR_SIZE - 1) ||
                 Length & (SECTOR_SIZE - 1))) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        if (FlagOn(IrpContext->MinorFunction, IRP_MN_DPC)) {
            ClearFlag(IrpContext->MinorFunction, IRP_MN_DPC);
            Status = STATUS_PENDING;
            __leave;
        }

        if (ByteOffset.QuadPart >=
                Vcb->PartitionInformation.PartitionLength.QuadPart  ) {
            Irp->IoStatus.Information = 0;
            Status = STATUS_END_OF_FILE;
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

        /*
         * User direct volume access
         */

        if (Ccb != NULL && !PagingIo) {

            if (!FlagOn(Ccb->Flags, CCB_VOLUME_DASD_PURGE)) {

                if (!FlagOn(Vcb->Flags, VCB_VOLUME_LOCKED)) {
                    Status = Ext2PurgeVolume( Vcb, TRUE);
                }

                SetLongFlag(Ccb->Flags, CCB_VOLUME_DASD_PURGE);
            }

            if (!IsFlagOn(Ccb->Flags, CCB_ALLOW_EXTENDED_DASD_IO)) {
                if (ByteOffset.QuadPart + Length > Vcb->Header.FileSize.QuadPart) {
                    Length = (ULONG)(Vcb->Header.FileSize.QuadPart - ByteOffset.QuadPart);
                }
            }

        } else if (Nocache && !PagingIo && (Vcb->SectionObject.DataSectionObject != NULL))  {

            ExAcquireResourceExclusiveLite(&Vcb->MainResource, TRUE);
            MainResourceAcquired = TRUE;

            ExAcquireSharedStarveExclusive(&Vcb->PagingIoResource, TRUE);
            ExReleaseResourceLite(&Vcb->PagingIoResource);

            CcFlushCache( &(Vcb->SectionObject),
                          &ByteOffset,
                          Length,
                          &(Irp->IoStatus));

            if (!NT_SUCCESS(Irp->IoStatus.Status))  {
                Status = Irp->IoStatus.Status;
                __leave;
            }

            ExAcquireSharedStarveExclusive(&Vcb->PagingIoResource, TRUE);
            ExReleaseResourceLite(&Vcb->PagingIoResource);

            CcPurgeCacheSection( &(Vcb->SectionObject),
                                 (PLARGE_INTEGER)&(ByteOffset),
                                 Length,
                                 FALSE );

            ExReleaseResourceLite(&Vcb->MainResource);
            MainResourceAcquired = FALSE;
        }

        if ( (ByteOffset.QuadPart + Length) > Vcb->Header.FileSize.QuadPart) {
            Length = (ULONG)(Vcb->Header.FileSize.QuadPart - ByteOffset.QuadPart);
        }

        if (!Nocache) {

            if (FlagOn(IrpContext->MinorFunction, IRP_MN_MDL)) {

                CcPrepareMdlWrite (
                    Vcb->Volume,
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

                /* the paging write of the stream writes registered ranges only:
                   register first, a range the cache holds unregistered would
                   never reach the disk */
                if (!Ext2AddVcbExtent(Vcb, ByteOffset.QuadPart, (LONGLONG)Length)) {
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    __leave;
                }

                CcCopyWrite(Vcb->Volume, (PLARGE_INTEGER)(&ByteOffset), Length, TRUE, Buffer);

                Status = Irp->IoStatus.Status;
            }

            if (NT_SUCCESS(Status)) {
                Irp->IoStatus.Information = Length;
            }

        } else if (PagingIo) {

            LONGLONG            DirtyStart;
            LONGLONG            DirtyLba;
            LONGLONG            DirtyLength;
            LONGLONG            RemainLength;

            PEXT2_EXTENT        Extent = NULL;
            PEXT2_EXTENT        List = NULL;

            Length &= ~((ULONG)SECTOR_SIZE - 1);

            Status = Ext2LockUserBuffer(IrpContext->Irp, Length, IoReadAccess);
            if (!NT_SUCCESS(Status)) {
                __leave;
            }

            DirtyLba = ByteOffset.QuadPart;
            RemainLength = (LONGLONG) Length;

            ASSERT(Length >= SECTOR_SIZE);

            while (RemainLength > 0) {

                DirtyStart = DirtyLba;
                ASSERT(DirtyStart >= ByteOffset.QuadPart);
                ASSERT(DirtyStart <= ByteOffset.QuadPart + Length);

                if (Ext2LookupVcbExtent(Vcb, DirtyStart, &DirtyLba, &DirtyLength)) {

                    if (DirtyLba == -1) {

                        DirtyLba = DirtyStart + DirtyLength;
                        if (ByteOffset.QuadPart + Length > DirtyLba) {
                            RemainLength = ByteOffset.QuadPart + Length - DirtyLba;
                            ASSERT(DirtyStart >= ByteOffset.QuadPart);
                            ASSERT(DirtyStart <= ByteOffset.QuadPart + Length);
                        } else {
                            RemainLength = 0;
                        }
                        continue;
                    }

                    ASSERT(DirtyLba <= DirtyStart);
                    Extent = Ext2AllocateExtent();

                    if (!Extent) {
                        DEBUG(DL_ERR, ( "Ext2WriteVolume: failed to allocate Extent\n"));
                        Status = STATUS_INSUFFICIENT_RESOURCES;
                        __leave;
                    }

                    Extent->Irp = NULL;
                    Extent->Lba = DirtyStart;
                    Extent->Offset = (ULONG)( DirtyStart + Length -
                                              RemainLength - DirtyLba );
                    ASSERT(Extent->Offset <= Length);

                    if (DirtyLba + DirtyLength >= DirtyStart + RemainLength) {
                        Extent->Length = (ULONG)( DirtyLba +
                                                  RemainLength -
                                                  DirtyStart );
                        ASSERT(Extent->Length <= Length);
                        RemainLength = 0;
                    } else {
                        Extent->Length = (ULONG)(DirtyLength + DirtyLba - DirtyStart);
                        RemainLength = RemainLength - Extent->Length;
/*
                        RemainLength =  (DirtyStart + RemainLength) -
                                        (DirtyLba + DirtyLength);
*/
                        ASSERT(RemainLength <= (LONGLONG)Length);
                        ASSERT(Extent->Length <= Length);
                    }

                    ASSERT(Extent->Length >= SECTOR_SIZE);
                    DirtyLba = DirtyStart + Extent->Length;

                    if (List) {
                        List->Next = Extent;
                        List = Extent;
                    } else {
                        Chain = List = Extent;
                    }

                } else {

                    if (RemainLength > SECTOR_SIZE) {
                        DirtyLba = DirtyStart + SECTOR_SIZE;
                        RemainLength -= SECTOR_SIZE;
                    } else {
                        RemainLength = 0;
                    }
                }
            }

            if (Chain) {
                Status = Ext2ReadWriteBlocks(IrpContext,
                                             Vcb,
                                             Chain,
                                             Length );
                Irp = IrpContext->Irp;

                if (NT_SUCCESS(Status)) {
                    for (Extent = Chain; Extent != NULL; Extent = Extent->Next) {
                        Ext2RemoveVcbExtent(Vcb, Extent->Lba, Extent->Length);
                    }
                }

                if (!Irp) {
                    __leave;
                }

            } else {

                Irp->IoStatus.Information = Length;
                Status = STATUS_SUCCESS;
                __leave;
            }

        } else {

            Length &= ~((ULONG)SECTOR_SIZE - 1);

            Status = Ext2LockUserBuffer(
                         IrpContext->Irp,
                         Length,
                         IoWriteAccess );

            if (!NT_SUCCESS(Status)) {
                __leave;
            }

            BlockArray.Irp = NULL;
            BlockArray.Lba = ByteOffset.QuadPart;
            BlockArray.Offset = 0;
            BlockArray.Length = Length;
            BlockArray.Next = NULL;

            Status = Ext2ReadWriteBlocks(IrpContext,
                                         Vcb,
                                         &BlockArray,
                                         Length );

            if (NT_SUCCESS(Status)) {
                Irp->IoStatus.Information = Length;
            }

            Irp = IrpContext->Irp;
            if (!Irp) {
                __leave;
            }
        }

    } __finally {

        if (MainResourceAcquired) {
            ExReleaseResourceLite(&Vcb->MainResource);
        }

        if (!IrpContext->ExceptionInProgress) {

            if (Irp) {

                if (Status == STATUS_PENDING) {

                    if (!bDeferred) {
                        Status = Ext2LockUserBuffer(
                                     IrpContext->Irp,
                                     Length,
                                     IoReadAccess );

                        if (NT_SUCCESS(Status)) {
                            Status = Ext2QueueRequest(IrpContext);
                        } else {
                            Ext2CompleteIrpContext(IrpContext, Status);
                        }
                    }

                } else {

                    if (NT_SUCCESS(Status)) {

                        if (SynchronousIo && !PagingIo) {
                            FileObject->CurrentByteOffset.QuadPart =
                                ByteOffset.QuadPart + Irp->IoStatus.Information;
                        }

                        if (!PagingIo) {
                            SetFlag(FileObject->Flags, FO_FILE_MODIFIED);
                        }
                    }

                    Ext2CompleteIrpContext(IrpContext, Status);
                }

            } else {

                Ext2FreeIrpContext(IrpContext);
            }
        }

        if (Chain) {
            Ext2DestroyExtentChain(Chain);
        }
    }

    return Status;
}
