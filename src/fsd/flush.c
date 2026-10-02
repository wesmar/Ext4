/**
 * flush.c - IRP_MJ_FLUSH_BUFFERS.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"

NTSTATUS
Ext2FlushCompletionRoutine (
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Context  )

{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Context);
    if (Irp->PendingReturned)
        IoMarkIrpPending( Irp );

    if (Irp->IoStatus.Status == STATUS_INVALID_DEVICE_REQUEST)
        Irp->IoStatus.Status = STATUS_SUCCESS;

    return STATUS_SUCCESS;
}

NTSTATUS
Ext2FlushVolume (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN BOOLEAN              bShutDown
)
{
    UNREFERENCED_PARAMETER(IrpContext);
    UNREFERENCED_PARAMETER(bShutDown);
    DEBUG(DL_INF, ( "Ext2FlushVolume: Flushing Vcb ...\n"));

    ExAcquireSharedStarveExclusive(&Vcb->PagingIoResource, TRUE);
    ExReleaseResourceLite(&Vcb->PagingIoResource);

    return Ext2FlushVcb(Vcb);
}

NTSTATUS
Ext2FlushFile (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_FCB            Fcb,
    IN PEXT2_CCB            Ccb
)
{
    IO_STATUS_BLOCK     IoStatus = {0};

    ASSERT(Fcb != NULL);
    ASSERT((Fcb->Identifier.Type == EXT2FCB) &&
           (Fcb->Identifier.Size == sizeof(EXT2_FCB)));

    __try {

        /* do nothing if target file was deleted */
        if (FlagOn(Fcb->Flags, FCB_DELETE_PENDING)) {
            IoStatus.Status = STATUS_FILE_DELETED;
            __leave;
        }

        /* update timestamp and achieve attribute */
        if (Ccb != NULL) {

            if (!IsFlagOn(Ccb->Flags, CCB_LAST_WRITE_UPDATED)) {

                LARGE_INTEGER   SysTime;
                KeQuerySystemTime(&SysTime);

                Ext2SetInodeTime(&SysTime, &Fcb->Inode->i_mtime, &Fcb->Inode->i_mtime_extra);
                Fcb->Mcb->Icb->LastWriteTime = Ext2GetInodeTime(Fcb->Inode->i_mtime, Fcb->Inode->i_mtime_extra);
                if (!Ext2SaveInode(IrpContext, Fcb->Vcb, Fcb->Inode)) {
                    IoStatus.Status = STATUS_UNEXPECTED_IO_ERROR;
                    __leave;
                }
            }
        }

        if (IsDirectory(Fcb)) {
            IoStatus.Status = STATUS_SUCCESS;
            __leave;
        }

        DEBUG(DL_INF, ( "Ext2FlushFile: Flushing File Inode=%xh %S ...\n",
                        Fcb->Inode->i_ino, Fcb->Mcb->ShortName.Buffer));

        CcFlushCache(&(Fcb->SectionObject), NULL, 0, &IoStatus);
        if (NT_SUCCESS(IoStatus.Status))
            ClearLongFlag(Fcb->Flags, FCB_FILE_MODIFIED);

    } __finally {

        /* nothing held: CcFlushCache runs under the caller's resources */
    }

    return IoStatus.Status;
}

NTSTATUS
Ext2FlushFiles(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN BOOLEAN              bShutDown
)
{
    UNREFERENCED_PARAMETER(bShutDown);
    IO_STATUS_BLOCK    IoStatus;

    PEXT2_FCB       Fcb;
    PLIST_ENTRY     ListEntry;

    if (IsVcbReadOnly(Vcb)) {
        return STATUS_SUCCESS;
    }

    IoStatus.Status = STATUS_SUCCESS;

    DEBUG(DL_INF, ( "Flushing Files ...\n"));

    /* the list changes under FcbLock (opens, closes, the reaper): walked
       under it too */
    ExAcquireResourceSharedLite(&Vcb->FcbLock, TRUE);

    /* Flush all Fcbs in Vcb list queue. */
    for (ListEntry = Vcb->FcbList.Flink;
            ListEntry != &Vcb->FcbList;
            ListEntry = ListEntry->Flink ) {

        Fcb = CONTAINING_RECORD(ListEntry, EXT2_FCB, Next);
        ExAcquireResourceExclusiveLite(
            &Fcb->MainResource, TRUE);
        {
            NTSTATUS FileStatus = Ext2FlushFile(IrpContext, Fcb, NULL);
            /* Deleted files do not need persistence. Preserve the first
               real failure while still flushing the remaining files. */
            if (!NT_SUCCESS(FileStatus) && FileStatus != STATUS_FILE_DELETED &&
                NT_SUCCESS(IoStatus.Status))
                IoStatus.Status = FileStatus;
        }
        ExReleaseResourceLite(&Fcb->MainResource);
    }
    ExReleaseResourceLite(&Vcb->FcbLock);

    return IoStatus.Status;
}

NTSTATUS
Ext2Flush (IN PEXT2_IRP_CONTEXT IrpContext)
{
    NTSTATUS                Status = STATUS_SUCCESS;

    PIRP                    Irp = NULL;
    PIO_STACK_LOCATION      IrpSp = NULL;

    PEXT2_VCB               Vcb = NULL;
    PEXT2_FCB               Fcb = NULL;
    PEXT2_FCBVCB            FcbOrVcb = NULL;
    PEXT2_CCB               Ccb = NULL;
    PFILE_OBJECT            FileObject = NULL;

    PDEVICE_OBJECT          DeviceObject = NULL;

    BOOLEAN                 MainResourceAcquired = FALSE;
    LONG64                  FlushMark = 0;
    BOOLEAN                 DataWritten = FALSE;

    __try {

        ASSERT(IrpContext);

        ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
               (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

        DeviceObject = IrpContext->DeviceObject;

        /* This request is not allowed on the main device object */
        if (IsExt2FsDevice(DeviceObject)) {
            Status = STATUS_INVALID_DEVICE_REQUEST;
            __leave;
        }

        Vcb = (PEXT2_VCB) DeviceObject->DeviceExtension;
        ASSERT(Vcb != NULL);
        ASSERT((Vcb->Identifier.Type == EXT2VCB) &&
               (Vcb->Identifier.Size == sizeof(EXT2_VCB)));

        if (!IsMounted(Vcb) || IsFlagOn(Vcb->Flags, VCB_DISMOUNT_PENDING)) {
            Status = STATUS_VOLUME_DISMOUNTED;
            __leave;
        }

        if (IsVcbReadOnly(Vcb)) {
            /* A journal abort makes the volume read-only. It must not
               turn a failed durability request into a successful flush. */
            Status = Ext2JournalCommit(Vcb, TRUE);
            __leave;
        }

        Irp = IrpContext->Irp;
        IrpSp = IoGetCurrentIrpStackLocation(Irp);

        FileObject = IrpContext->FileObject;
        FcbOrVcb = (PEXT2_FCBVCB) FileObject->FsContext;
        ASSERT(FcbOrVcb != NULL);

        Ccb = (PEXT2_CCB) FileObject->FsContext2;
        if (Ccb == NULL) {
            Status =  STATUS_SUCCESS;
            __leave;
        }

        MainResourceAcquired =
            ExAcquireResourceExclusiveLite(&FcbOrVcb->MainResource, TRUE);
        ASSERT(MainResourceAcquired);
        DEBUG(DL_INF, ("Ext2Flush-pre:  total mcb records=%u\n",
                       FsRtlNumberOfRunsInLargeMcb(&Vcb->Extents)));

        if (FcbOrVcb->Identifier.Type == EXT2VCB) {

            Ext2VerifyVcb(IrpContext, Vcb);
            Status = Ext2FlushFiles(IrpContext, (PEXT2_VCB)(FcbOrVcb), FALSE);
            if (!NT_SUCCESS(Status)) {
                __leave;
            }

            Status = Ext2FlushVolume(IrpContext, (PEXT2_VCB)(FcbOrVcb), FALSE);

            if (NT_SUCCESS(Status) && IsFlagOn(Vcb->Volume->Flags, FO_FILE_MODIFIED)) {
                ClearFlag(Vcb->Volume->Flags, FO_FILE_MODIFIED);
            }

        } else if (FcbOrVcb->Identifier.Type == EXT2FCB) {

            Fcb = (PEXT2_FCB)(FcbOrVcb);

            Status = Ext2FlushFile(IrpContext, Fcb, Ccb);
            if (NT_SUCCESS(Status)) {
                /* the file's data has reached the device (not yet its media) */
                FlushMark = Ext2FlushMark(Vcb);
                DataWritten = TRUE;
                if (IsFlagOn(FileObject->Flags, FO_FILE_MODIFIED)) {
                    Fcb->Mcb->FileAttr |= FILE_ATTRIBUTE_ARCHIVE;
                    ClearFlag(FileObject->Flags, FO_FILE_MODIFIED);
                }
            }
        }

        DEBUG(DL_INF, ("Ext2Flush-post: total mcb records=%u\n",
                       FsRtlNumberOfRunsInLargeMcb(&Vcb->Extents)));

    } __finally {

        if (MainResourceAcquired) {
            ExReleaseResourceLite(&FcbOrVcb->MainResource);
        }

        /* fsync semantics: the metadata of this request (and everything
           before it) is durable when the flush returns. Done with no
           resource held so that other requests can finish their handles. */
        if (!IrpContext->ExceptionInProgress && Vcb && Fcb && NT_SUCCESS(Status)) {
            Status = Ext2JournalCommit(Vcb, TRUE);
        }

        if (!IrpContext->ExceptionInProgress) {

            /* The commit flushes the device cache before its commit block;
               a flush issued after the data was written covers the data too,
               and another one would only cost a round trip (Linux fsync
               relies on the commit's flush the same way). Without a commit
               - nothing but data changed - the device flush goes down. */
            if (DataWritten && NT_SUCCESS(Status) && Ext2FlushedSince(Vcb, FlushMark)) {
                Irp = NULL;
            }

            if (Vcb && Irp && IrpSp && NT_SUCCESS(Status) && !IsVcbReadOnly(Vcb)) {

                /* Call the disk driver to flush the physial media. */
                NTSTATUS DriverStatus;
                PIO_STACK_LOCATION NextIrpSp;

                NextIrpSp = IoGetNextIrpStackLocation(Irp);

                *NextIrpSp = *IrpSp;

                IoSetCompletionRoutine( Irp,
                                        Ext2FlushCompletionRoutine,
                                        NULL,
                                        TRUE,
                                        TRUE,
                                        TRUE );

                DriverStatus = IoCallDriver(Vcb->TargetDeviceObject, Irp);

                Status = (DriverStatus == STATUS_INVALID_DEVICE_REQUEST) ?
                         Status : DriverStatus;

                IrpContext->Irp = Irp = NULL;
            }

            Ext2CompleteIrpContext(IrpContext, Status);
        }
    }

    return Status;
}
