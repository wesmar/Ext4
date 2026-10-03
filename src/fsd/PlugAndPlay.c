/**
 * PlugAndPlay.c - IRP_MJ_PNP: query/cancel/surprise removal of the underlying device.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"

NTSTATUS
Ext2PnpCompletionRoutine (
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP           Irp,
    IN PVOID          Contxt     );

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2Pnp)
#pragma alloc_text(PAGE, Ext2PnpQueryRemove)
#pragma alloc_text(PAGE, Ext2PnpRemove)
#pragma alloc_text(PAGE, Ext2PnpCancelRemove)
#pragma alloc_text(PAGE, Ext2PnpSurpriseRemove)
#endif

NTSTATUS
Ext2PnpCompletionRoutine (
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Contxt
)
{
    UNREFERENCED_PARAMETER(Irp);
    PKEVENT Event = (PKEVENT) Contxt;

    KeSetEvent( Event, 0, FALSE );

    return STATUS_MORE_PROCESSING_REQUIRED;

    UNREFERENCED_PARAMETER( DeviceObject );
    UNREFERENCED_PARAMETER( Contxt );
}

NTSTATUS
Ext2Pnp (IN PEXT2_IRP_CONTEXT IrpContext)
{
    NTSTATUS            Status = STATUS_INVALID_PARAMETER;

    PIRP                Irp;
    PIO_STACK_LOCATION  IrpSp;
    PEXT2_VCB           Vcb = NULL;
    PDEVICE_OBJECT      DeviceObject;

    __try {

        ASSERT(IrpContext);

        ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
               (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

        DeviceObject = IrpContext->DeviceObject;

        Vcb = (PEXT2_VCB) DeviceObject->DeviceExtension;

        ASSERT(Vcb != NULL);

        if ( !((Vcb->Identifier.Type == EXT2VCB) &&
                (Vcb->Identifier.Size == sizeof(EXT2_VCB)))) {
            __leave; /* Status = STATUS_INVALID_PARAMETER */
        }

        Irp = IrpContext->Irp;
        IrpSp = IoGetCurrentIrpStackLocation(Irp);

        SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT);

        switch ( IrpSp->MinorFunction ) {

        case IRP_MN_QUERY_REMOVE_DEVICE:

            DEBUG(DL_PNP, ("Ext2Pnp: Ext2PnpQueryRemove...\n"));
            Status = Ext2PnpQueryRemove(IrpContext, Vcb);

            break;

        case IRP_MN_REMOVE_DEVICE:

            DEBUG(DL_PNP, ("Ext2Pnp: Ext2PnpRemove...\n"));
            Status = Ext2PnpRemove(IrpContext, Vcb);
            break;

        case IRP_MN_CANCEL_REMOVE_DEVICE:

            DEBUG(DL_PNP, ("Ext2Pnp: Ext2PnpCancelRemove...\n"));
            Status = Ext2PnpCancelRemove(IrpContext, Vcb);
            break;

        case IRP_MN_SURPRISE_REMOVAL:

            DEBUG(DL_PNP, ("Ext2Pnp: Ext2PnpSupriseRemove...\n"));
            Status = Ext2PnpSurpriseRemove(IrpContext, Vcb);
            break;

        default:
            break;
        }

    } __finally {

        if (!IrpContext->ExceptionInProgress) {
            Irp = IrpContext->Irp;

            if (Irp) {

                /* Here we need pass the IRP to the disk driver. */

                IoSkipCurrentIrpStackLocation( Irp );

                Status = IoCallDriver(Vcb->TargetDeviceObject, Irp);

                IrpContext->Irp = NULL;
            }

            Ext2CompleteIrpContext(IrpContext, Status);
        }
    }

    return Status;
}

NTSTATUS
Ext2PnpQueryRemove (
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB         Vcb
)
{
    NTSTATUS Status = STATUS_SUCCESS;
    KEVENT   Event;
    BOOLEAN  bDeleted = FALSE;
    BOOLEAN  VcbAcquired = FALSE;

    __try {

        CcWaitForCurrentLazyWriterActivity();

        VcbAcquired = ExAcquireResourceExclusiveLite(
                          &Vcb->MainResource, TRUE );

        Ext2FlushFiles(IrpContext, Vcb, FALSE);
        Ext2FlushVolume(IrpContext, Vcb, FALSE);

        DEBUG(DL_PNP, ("Ext2PnpQueryRemove: Ext2LockVcb: Vcb=%xh FileObject=%xh ...\n",
                       Vcb, IrpContext->FileObject));
        Status = Ext2LockVcb(Vcb, IrpContext->FileObject);

        if (VcbAcquired) {
            ExReleaseResourceLite(&Vcb->MainResource);
            VcbAcquired = FALSE;
        }

        DEBUG(DL_PNP, ("Ext2PnpQueryRemove: Ext2PurgeVolume ...\n"));
        Ext2PurgeVolume(Vcb, TRUE);

        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        IoCopyCurrentIrpStackLocationToNext(IrpContext->Irp);

        KeInitializeEvent( &Event, NotificationEvent, FALSE );
        IoSetCompletionRoutine( IrpContext->Irp,
                                Ext2PnpCompletionRoutine,
                                &Event,
                                TRUE,
                                TRUE,
                                TRUE );

        DEBUG(DL_PNP, ("Ext2PnpQueryRemove: Call lower level driver...\n"));
        Status = IoCallDriver( Vcb->TargetDeviceObject,
                               IrpContext->Irp);

        if (Status == STATUS_PENDING) {
            LARGE_INTEGER Timeout;
            Timeout.QuadPart = (LONGLONG)-30 * 10 * 1000 * 1000; /* 30 seconds */

            Status = KeWaitForSingleObject( &Event,
                                   Executive,
                                   KernelMode,
                                   FALSE,
                                   &Timeout );

            if (Status == STATUS_TIMEOUT) {
                IoCancelIrp(IrpContext->Irp);
                KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
            }

            Status = IrpContext->Irp->IoStatus.Status;
        }

        if (NT_SUCCESS(Status)) {
            ASSERT(!VcbAcquired);
            DEBUG(DL_PNP, ("Ext2PnpQueryRemove: Ext2CheckDismount ...\n"));
            bDeleted = Ext2CheckDismount(IrpContext, Vcb, TRUE);
            DEBUG(DL_PNP, ("Ext2PnpQueryRemove: Ext2FlushVolume bDelted=%xh ...\n", bDeleted));
        }

    } __finally {

        if (VcbAcquired) {
            ExReleaseResourceLite(&Vcb->MainResource);
        }

        IrpContext->Irp->IoStatus.Status = Status;
        Ext2CompleteRequest(
            IrpContext->Irp, FALSE, (CCHAR)(NT_SUCCESS(Status)?
                                            IO_DISK_INCREMENT : IO_NO_INCREMENT) );

        IrpContext->Irp = NULL;
    }

    return Status;
}

NTSTATUS
Ext2PnpRemove (
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB         Vcb      )
{
    NTSTATUS Status = STATUS_SUCCESS;
    KEVENT   Event;
    BOOLEAN  bDeleted;

    __try {

        DEBUG(DL_PNP, ("Ext2PnpRemove by Ext2Pnp ...\n"));

        CcWaitForCurrentLazyWriterActivity();
        ExAcquireResourceExclusiveLite(
            &Vcb->MainResource,  TRUE );
        Status = Ext2LockVcb(Vcb, IrpContext->FileObject);
        ExReleaseResourceLite(&Vcb->MainResource);

        /* multi-mount protection ends while the device is still there: the
           volume is released (CLEAN) before the remove goes down */
        Ext4MmpStop(Vcb, TRUE);

        /* Mark device removed early so concurrent I/O threads fail fast */
        SetLongFlag(Vcb->Flags, VCB_DEVICE_REMOVED);

        /* the device goes away with its drive letter: the mount manager
           does not track hidden volumes and would leave ours dangling */
        Ext2ReleaseLetter(Vcb, FALSE);

        /* Setup the Irp. We'll send it to the lower disk driver. */

        IoCopyCurrentIrpStackLocationToNext(IrpContext->Irp);

        KeInitializeEvent( &Event, NotificationEvent, FALSE );
        IoSetCompletionRoutine( IrpContext->Irp,
                                Ext2PnpCompletionRoutine,
                                &Event,
                                TRUE,
                                TRUE,
                                TRUE );

        Status = IoCallDriver( Vcb->TargetDeviceObject,
                               IrpContext->Irp);

        if (Status == STATUS_PENDING) {
            LARGE_INTEGER Timeout;
            Timeout.QuadPart = (LONGLONG)-30 * 10 * 1000 * 1000; /* 30 seconds */

            Status = KeWaitForSingleObject( &Event,
                                   Executive,
                                   KernelMode,
                                   FALSE,
                                   &Timeout );

            if (Status == STATUS_TIMEOUT) {
                IoCancelIrp(IrpContext->Irp);
                KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
            }

            Status = IrpContext->Irp->IoStatus.Status;
        }

        /* purge volume cache */
        Ext2PurgeVolume(Vcb, FALSE);

        /* dismount volume */
        bDeleted = Ext2CheckDismount(IrpContext, Vcb, TRUE);

    } __finally {

        IrpContext->Irp->IoStatus.Status = Status;
        Ext2CompleteRequest(
            IrpContext->Irp, FALSE, (CCHAR)(NT_SUCCESS(Status)?
                                            IO_DISK_INCREMENT : IO_NO_INCREMENT) );

        IrpContext->Irp = NULL;
    }

    return Status;
}

NTSTATUS
Ext2PnpSurpriseRemove (
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB         Vcb      )
{
    NTSTATUS Status = STATUS_UNSUCCESSFUL;
    KEVENT   Event;
    BOOLEAN  bDeleted;

    __try {

        DEBUG(DL_PNP, ("Ext2PnpSupriseRemove by Ext2Pnp ...\n"));

        CcWaitForCurrentLazyWriterActivity();
        ExAcquireResourceExclusiveLite(
            &Vcb->MainResource,  TRUE );

        Status = Ext2LockVcb(Vcb, IrpContext->FileObject);

        ExReleaseResourceLite(&Vcb->MainResource);

        /* Mark device removed early so concurrent I/O threads fail fast */
        SetLongFlag(Vcb->Flags, VCB_DEVICE_REMOVED);

        /* the device is already gone: the multi-mount protection thread stops
           without its last write, which would go to a removed device and
           might never complete */
        Ext4MmpStop(Vcb, FALSE);

        /* the device goes away with its drive letter: the mount manager
           does not track hidden volumes and would leave ours dangling */
        Ext2ReleaseLetter(Vcb, FALSE);

        /* Setup the Irp. We'll send it to the lower disk driver. */

        IoCopyCurrentIrpStackLocationToNext(IrpContext->Irp);

        KeInitializeEvent( &Event, NotificationEvent, FALSE );
        IoSetCompletionRoutine( IrpContext->Irp,
                                Ext2PnpCompletionRoutine,
                                &Event,
                                TRUE,
                                TRUE,
                                TRUE );

        Status = IoCallDriver( Vcb->TargetDeviceObject,
                               IrpContext->Irp);

        if (Status == STATUS_PENDING) {
            LARGE_INTEGER Timeout;
            Timeout.QuadPart = (LONGLONG)-30 * 10 * 1000 * 1000; /* 30 seconds */

            Status = KeWaitForSingleObject( &Event,
                                   Executive,
                                   KernelMode,
                                   FALSE,
                                   &Timeout );

            if (Status == STATUS_TIMEOUT) {
                IoCancelIrp(IrpContext->Irp);
                KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
            }

            Status = IrpContext->Irp->IoStatus.Status;
        }

        /* purge volume cache */
        Ext2PurgeVolume(Vcb, FALSE);

        /* dismount volume */
        bDeleted = Ext2CheckDismount(IrpContext, Vcb, TRUE);

    } __finally {

        IrpContext->Irp->IoStatus.Status = Status;
        Ext2CompleteRequest(
            IrpContext->Irp, FALSE, (CCHAR)(NT_SUCCESS(Status)?
                                            IO_DISK_INCREMENT : IO_NO_INCREMENT) );

        IrpContext->Irp = NULL;
    }

    return Status;
}

NTSTATUS
Ext2PnpCancelRemove (
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB         Vcb
)
{
    NTSTATUS Status;

    DEBUG(DL_PNP, ("Ext2PnpCancelRemove by Ext2Pnp ...\n"));

    ExAcquireResourceExclusiveLite(
        &Vcb->MainResource,  TRUE );

    Status = Ext2UnlockVcb(Vcb, IrpContext->FileObject);

    ExReleaseResourceLite(&Vcb->MainResource);

    IoSkipCurrentIrpStackLocation(IrpContext->Irp);

    Status = IoCallDriver(Vcb->TargetDeviceObject, IrpContext->Irp);

    IrpContext->Irp = NULL;

    return Status;
}
