/**
 * dispatch.c - IRP dispatch: request queueing, worker threads, journal scopes per request.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2QueueRequest)
#pragma alloc_text(PAGE, Ext2DeQueueRequest)
#endif

/*
 *  Ext2OplockComplete
 *
 *    callback routine of FsRtlCheckOplock when an oplock break has
 *    completed, allowing an Irp to resume execution.
 *
 *  Arguments:
 *
 *    Context:  the IrpContext to be queued
 *    Irp:      the I/O request packet
 *
 *  Return Value:
 *    N/A
 */

VOID
Ext2OplockComplete (
    IN PVOID Context,
    IN PIRP Irp
)
{
    /* Check on the return value in the Irp. */

    if (Irp->IoStatus.Status == STATUS_SUCCESS) {

        /* queue the Irp context in the workqueue. */

        Ext2QueueRequest((PEXT2_IRP_CONTEXT)Context);

    } else {

        /* complete the request in case of failure */

        Ext2CompleteIrpContext( (PEXT2_IRP_CONTEXT) Context,
                                Irp->IoStatus.Status );
    }

    return;
}

/*
 *  Ext2LockIrp
 *
 *    performs buffer locking if we need pend the process of the Irp
 *
 *  Arguments:
 *    Context: the irp context
 *    Irp:     the I/O request packet.
 *
 *  Return Value:
 *    N/A
 */

VOID
Ext2LockIrp (
    IN PVOID Context,
    IN PIRP Irp
)
{
    PIO_STACK_LOCATION IrpSp;
    PEXT2_IRP_CONTEXT IrpContext;

    if (Irp == NULL) {
        return;
    }

    IrpSp = IoGetCurrentIrpStackLocation(Irp);

    IrpContext = (PEXT2_IRP_CONTEXT) Context;

    if ( IrpContext->MajorFunction == IRP_MJ_READ ||
            IrpContext->MajorFunction == IRP_MJ_WRITE ) {

        /* lock the user's buffer to MDL, if the I/O is bufferred */

        if (!IsFlagOn(IrpContext->MinorFunction, IRP_MN_MDL)) {

            Ext2LockUserBuffer( Irp, IrpSp->Parameters.Write.Length,
                                (IrpContext->MajorFunction == IRP_MJ_READ) ?
                                IoWriteAccess : IoReadAccess );
        }

    } else if (IrpContext->MajorFunction == IRP_MJ_DIRECTORY_CONTROL
               && IrpContext->MinorFunction == IRP_MN_QUERY_DIRECTORY) {

        ULONG Length = ((PEXTENDED_IO_STACK_LOCATION) IrpSp)->Parameters.QueryDirectory.Length;
        Ext2LockUserBuffer(Irp, Length, IoWriteAccess);

    } else if (IrpContext->MajorFunction == IRP_MJ_QUERY_EA) {

        ULONG Length = ((PEXTENDED_IO_STACK_LOCATION) IrpSp)->Parameters.QueryEa.Length;
        Ext2LockUserBuffer(Irp, Length, IoWriteAccess);

    } else if (IrpContext->MajorFunction == IRP_MJ_SET_EA) {
        ULONG Length = ((PEXTENDED_IO_STACK_LOCATION) IrpSp)->Parameters.SetEa.Length;
        Ext2LockUserBuffer(Irp, Length, IoReadAccess);

    } else if ( (IrpContext->MajorFunction == IRP_MJ_FILE_SYSTEM_CONTROL) &&
                (IrpContext->MinorFunction == IRP_MN_USER_FS_REQUEST) ) {
        PEXTENDED_IO_STACK_LOCATION EIrpSp = (PEXTENDED_IO_STACK_LOCATION)IrpSp;
        if ( (EIrpSp->Parameters.FileSystemControl.FsControlCode == FSCTL_GET_VOLUME_BITMAP) ||
                (EIrpSp->Parameters.FileSystemControl.FsControlCode == FSCTL_GET_RETRIEVAL_POINTERS) ||
                (EIrpSp->Parameters.FileSystemControl.FsControlCode == FSCTL_GET_RETRIEVAL_POINTER_BASE) ) {
            ULONG Length = EIrpSp->Parameters.FileSystemControl.OutputBufferLength;
            Ext2LockUserBuffer(Irp, Length, IoWriteAccess);
        }
    }

    /* Mark the request as pending status */

    IoMarkIrpPending( Irp );

    return;
}

NTSTATUS
Ext2QueueRequest (IN PEXT2_IRP_CONTEXT IrpContext)
{
    ASSERT(IrpContext);

    ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
           (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

    /* set the flags of "can wait" and "queued" */
    SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT);
    SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_REQUEUED);

    /* make sure the buffer is kept valid in system context */
    Ext2LockIrp(IrpContext, IrpContext->Irp);

    /* An I/O work item references the device object, and through it the
       driver: a queued request keeps the image loaded until it has run,
       which ExQueueWorkItem does not guarantee to an unloadable driver. */
    IrpContext->WorkItem = IoAllocateWorkItem(IrpContext->DeviceObject);
    if (IrpContext->WorkItem == NULL) {
        /* the IRP is already marked pending: complete it, still say pending */
        Ext2CompleteIrpContext(IrpContext, STATUS_INSUFFICIENT_RESOURCES);
        return STATUS_PENDING;
    }

    IoQueueWorkItem(IrpContext->WorkItem, Ext2DeQueueRequest, CriticalWorkQueue, IrpContext);

    return STATUS_PENDING;
}

VOID
Ext2DeQueueRequest (IN PDEVICE_OBJECT DeviceObject, IN PVOID Context)
{
    PEXT2_IRP_CONTEXT IrpContext = (PEXT2_IRP_CONTEXT) Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    ASSERT(IrpContext);

    /* the I/O manager keeps its device reference until this routine returns;
       the item itself may go now, before the request frees the IrpContext */
    IoFreeWorkItem(IrpContext->WorkItem);
    IrpContext->WorkItem = NULL;

    ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
           (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

    __try {

        __try {

            FsRtlEnterFileSystem();

            if (!IrpContext->IsTopLevel) {
                IoSetTopLevelIrp((PIRP) FSRTL_FSP_TOP_LEVEL_IRP);
            }

            Ext2DispatchRequest(IrpContext);

        } __except (Ext2ExceptionFilter(IrpContext, GetExceptionInformation())) {

            Ext2ExceptionHandler(IrpContext);
        }

    } __finally {

        IoSetTopLevelIrp(NULL);

        FsRtlExitFileSystem();
    }
}

/*
 * Requests that may modify metadata run inside a journal scope: the
 * thread's handle becomes active at its first modification and the
 * commit waits for the request to finish (see jbd2\commit.c).
 * Paging writes never touch metadata and stay outside.
 */
static BOOLEAN
Ext2RequestNeedsJournalScope(IN PEXT2_IRP_CONTEXT IrpContext)
{
    switch (IrpContext->MajorFunction) {
    case IRP_MJ_WRITE:
        /* A paging write of a file can allocate blocks (unwritten extents
           are converted, sparse indirect files get blocks) - metadata, so
           it needs a scope like any other modifying request. The volume
           stream is the exception: its paging writes only carry pinned
           metadata pages to disk and must never join a transaction. */
        if (IrpContext->Irp == NULL ||
            !IsFlagOn(IrpContext->Irp->Flags, IRP_PAGING_IO)) {
            return TRUE;
        }
        return !(IrpContext->FileObject &&
                 IrpContext->FileObject->FsContext &&
                 ((PEXT2_VCB)IrpContext->FileObject->FsContext)->Identifier.Type == EXT2VCB);
    case IRP_MJ_CREATE:
    case IRP_MJ_CLOSE:
    case IRP_MJ_FLUSH_BUFFERS:
    case IRP_MJ_SET_INFORMATION:
    case IRP_MJ_SET_VOLUME_INFORMATION:
    case IRP_MJ_FILE_SYSTEM_CONTROL:
    case IRP_MJ_DEVICE_CONTROL:
    case IRP_MJ_CLEANUP:
    case IRP_MJ_SHUTDOWN:
    case IRP_MJ_SET_EA:
    case IRP_MJ_PNP:
        return TRUE;
    default:
        return FALSE;
    }
}

static NTSTATUS
Ext2DispatchRequestWorker (IN PEXT2_IRP_CONTEXT IrpContext)
{
    switch (IrpContext->MajorFunction) {

    case IRP_MJ_CREATE:
        return Ext2Create(IrpContext);

    case IRP_MJ_CLOSE:
        return Ext2Close(IrpContext);

    case IRP_MJ_READ:
        return Ext2Read(IrpContext);

    case IRP_MJ_WRITE:
        return Ext2Write(IrpContext);

    case IRP_MJ_FLUSH_BUFFERS:
        return Ext2Flush(IrpContext);

    case IRP_MJ_QUERY_INFORMATION:
        return Ext2QueryFileInformation(IrpContext);

    case IRP_MJ_SET_INFORMATION:
        return Ext2SetFileInformation(IrpContext);

    case IRP_MJ_QUERY_VOLUME_INFORMATION:
        return Ext2QueryVolumeInformation(IrpContext);

    case IRP_MJ_SET_VOLUME_INFORMATION:
        return Ext2SetVolumeInformation(IrpContext);

    case IRP_MJ_DIRECTORY_CONTROL:
        return Ext2DirectoryControl(IrpContext);

    case IRP_MJ_FILE_SYSTEM_CONTROL:
        return Ext2FileSystemControl(IrpContext);

    case IRP_MJ_DEVICE_CONTROL:
        return Ext2DeviceControl(IrpContext);

    case IRP_MJ_LOCK_CONTROL:
        return Ext2LockControl(IrpContext);

    case IRP_MJ_CLEANUP:
        return Ext2Cleanup(IrpContext);

    case IRP_MJ_SHUTDOWN:
        return Ext2ShutDown(IrpContext);

	case IRP_MJ_QUERY_EA:
		return Ext2QueryEa(IrpContext);

	case IRP_MJ_SET_EA:
		return Ext2SetEa(IrpContext);

    case IRP_MJ_PNP:
        return Ext2Pnp(IrpContext);
    default:
        DEBUG(DL_ERR, ( "Ext2DispatchRequest: Unexpected major function: %xh\n",
                        IrpContext->MajorFunction));
        Ext2CompleteIrpContext(IrpContext, STATUS_DRIVER_INTERNAL_ERROR);
        return STATUS_DRIVER_INTERNAL_ERROR;
    }
}

NTSTATUS
Ext2DispatchRequest (IN PEXT2_IRP_CONTEXT IrpContext)
{
    PEXT2_VCB   Vcb = NULL;
    NTSTATUS    Status;
    BOOLEAN     Scoped;

    ASSERT(IrpContext);

    ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
           (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

    Scoped = Ext2RequestNeedsJournalScope(IrpContext);
    if (Scoped) {
        if (IrpContext->DeviceObject && !IsExt2FsDevice(IrpContext->DeviceObject)) {
            Vcb = (PEXT2_VCB) IrpContext->DeviceObject->DeviceExtension;
            if (Vcb && (Vcb->Identifier.Type != EXT2VCB ||
                        Vcb->Identifier.Size != sizeof(EXT2_VCB))) {
                Vcb = NULL;
            }
        }
        Ext2JournalEnterScope(Vcb);
    }

    __try {
        Status = Ext2DispatchRequestWorker(IrpContext);
    } __finally {
        if (Scoped) {
            Ext2JournalLeaveScope(Vcb);
        }
    }

    return Status;
}

NTSTATUS
Ext2BuildRequest (PDEVICE_OBJECT   DeviceObject, PIRP Irp)
{
    BOOLEAN             AtIrqlPassiveLevel = FALSE;
    BOOLEAN             IsTopLevelIrp = FALSE;
    PEXT2_IRP_CONTEXT   IrpContext = NULL;
    NTSTATUS            Status = STATUS_UNSUCCESSFUL;

    /* the disk device of an unlocked LUKS volume: below the file systems,
       none of their machinery applies */
    if (Ext4IsCryptDevice(DeviceObject)) {
        return Ext4CryptDispatch(DeviceObject, Irp);
    }
    if (Ext4IsLvDevice(DeviceObject)) {
        return Ext4LvDispatch(DeviceObject, Irp);
    }

    /* unlocking and locking LUKS volumes mount and dismount file systems,
       ours included: handled before this request becomes one of them */
    if (IsExt2FsDevice(DeviceObject)) {
        PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
        if (IrpSp->MajorFunction == IRP_MJ_DEVICE_CONTROL &&
            Ext4IsCryptControl(IrpSp->Parameters.DeviceIoControl.IoControlCode)) {
            return Ext4CryptControl(Irp);
        }
    }

    __try {

        __try {

#if EXT2_DEBUG
            Ext2DbgPrintCall(DeviceObject, Irp);
#endif

            AtIrqlPassiveLevel = (KeGetCurrentIrql() == PASSIVE_LEVEL);

            if (AtIrqlPassiveLevel) {
                FsRtlEnterFileSystem();
            }

            if (!IoGetTopLevelIrp()) {
                IsTopLevelIrp = TRUE;
                IoSetTopLevelIrp(Irp);
            }

            IrpContext = Ext2AllocateIrpContext(DeviceObject, Irp);

            if (!IrpContext) {

                Status = STATUS_INSUFFICIENT_RESOURCES;
                Irp->IoStatus.Status = Status;

                Ext2CompleteRequest(Irp, TRUE, IO_NO_INCREMENT);

            } else {
                Status = Ext2DispatchRequest(IrpContext);
            }
        } __except (Ext2ExceptionFilter(IrpContext, GetExceptionInformation())) {

            Status = Ext2ExceptionHandler(IrpContext);
        }

    } __finally  {

        if (IsTopLevelIrp) {
            IoSetTopLevelIrp(NULL);
        }

        if (AtIrqlPassiveLevel) {
            FsRtlExitFileSystem();
        }
    }

    return Status;
}
