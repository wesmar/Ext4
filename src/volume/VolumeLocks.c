/**
 * VolumeLocks.c - FSCTL_LOCK_VOLUME / UNLOCK_VOLUME and VPB flag handling.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "../fsd/fsctl_internal.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2IsHandleCountZero)
#pragma alloc_text(PAGE, Ext2LockVcb)
#pragma alloc_text(PAGE, Ext2LockVolume)
#pragma alloc_text(PAGE, Ext2UnlockVcb)
#pragma alloc_text(PAGE, Ext2UnlockVolume)
#endif

VOID
Ext2SetVpbFlag (
    IN PVPB     Vpb,
    IN USHORT   Flag )
{
    KIRQL OldIrql;

    IoAcquireVpbSpinLock(&OldIrql);
    Vpb->Flags |= Flag;
    IoReleaseVpbSpinLock(OldIrql);
}

VOID
Ext2ClearVpbFlag (
    IN PVPB     Vpb,
    IN USHORT   Flag )
{
    KIRQL OldIrql;

    IoAcquireVpbSpinLock(&OldIrql);
    Vpb->Flags &= ~Flag;
    IoReleaseVpbSpinLock(OldIrql);
}

BOOLEAN
Ext2IsHandleCountZero(IN PEXT2_VCB Vcb)
{
    PEXT2_FCB   Fcb;
    PLIST_ENTRY List;
    BOOLEAN     Zero = TRUE;

    /* the list changes under FcbLock (opens, closes, the reaper) */
    ExAcquireResourceSharedLite(&Vcb->FcbLock, TRUE);
    for ( List = Vcb->FcbList.Flink;
            List != &Vcb->FcbList;
            List = List->Flink )  {

        Fcb = CONTAINING_RECORD(List, EXT2_FCB, Next);

        ASSERT((Fcb->Identifier.Type == EXT2FCB) &&
               (Fcb->Identifier.Size == sizeof(EXT2_FCB)));

        if (Fcb->OpenHandleCount) {
            Zero = FALSE;
            break;
        }
    }
    ExReleaseResourceLite(&Vcb->FcbLock);

    return Zero;
}

NTSTATUS
Ext2LockVcb (IN PEXT2_VCB    Vcb,
             IN PFILE_OBJECT FileObject)
{
    NTSTATUS Status = STATUS_SUCCESS;

    __try {

        if (FlagOn(Vcb->Flags, VCB_VOLUME_LOCKED)) {
            DEBUG(DL_INF, ( "Ext2LockVolume: Volume is already locked.\n"));
            Status = STATUS_ACCESS_DENIED;
            __leave;
        }

        if (Vcb->OpenHandleCount > (FileObject ? 1 : 0)) {
            DEBUG(DL_INF, ( "Ext2LockVcb: There are still opened files.\n"));

            Status = STATUS_ACCESS_DENIED;
            __leave;
        }

        if (!Ext2IsHandleCountZero(Vcb)) {
            DEBUG(DL_INF, ( "Ext2LockVcb: Thare are still opened files.\n"));

            Status = STATUS_ACCESS_DENIED;
            __leave;
        }

        SetLongFlag(Vcb->Flags, VCB_VOLUME_LOCKED);
        Ext2SetVpbFlag(Vcb->Vpb, VPB_LOCKED);
        Vcb->LockFile = FileObject;

        DEBUG(DL_INF, ( "Ext2LockVcb: Volume locked.\n"));

    } __finally {
        /* Nothing */
    }

    return Status;
}

NTSTATUS
Ext2LockVolume (IN PEXT2_IRP_CONTEXT IrpContext)
{
    PIO_STACK_LOCATION IrpSp;
    PDEVICE_OBJECT  DeviceObject;
    PEXT2_VCB       Vcb = NULL;
    NTSTATUS Status = STATUS_UNSUCCESSFUL;
    BOOLEAN VcbResourceAcquired = FALSE;

    __try {

        ASSERT(IrpContext != NULL);

        ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
               (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

        DeviceObject = IrpContext->DeviceObject;

        Status = STATUS_UNSUCCESSFUL;

        /* This request is not allowed on the main device object */
        if (IsExt2FsDevice(DeviceObject)) {
            Status = STATUS_INVALID_PARAMETER;
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

        IrpSp = IoGetCurrentIrpStackLocation(IrpContext->Irp);

        CcWaitForCurrentLazyWriterActivity();
        ExAcquireResourceExclusiveLite(
            &Vcb->MainResource,
            TRUE            );

        VcbResourceAcquired = TRUE;

        /* flush dirty data before locking the volume */
        if (!IsVcbReadOnly(Vcb)) {
            Ext2FlushFiles(IrpContext, Vcb, FALSE);
            Ext2FlushVolume(IrpContext, Vcb, FALSE);
        }

        Status = Ext2LockVcb(Vcb, IrpSp->FileObject);

    } __finally {

        if (VcbResourceAcquired) {
            ExReleaseResourceLite(&Vcb->MainResource);
        }

        if (!IrpContext->ExceptionInProgress) {
            Ext2CompleteIrpContext(IrpContext,  Status);
        }
    }

    return Status;
}

NTSTATUS
Ext2UnlockVcb ( IN PEXT2_VCB    Vcb,
                IN PFILE_OBJECT FileObject )
{
    NTSTATUS        Status;

    __try {

        if (FileObject && FileObject->FsContext != Vcb) {
            Status = STATUS_NOT_LOCKED;
            __leave;
        }

        if (!FlagOn(Vcb->Flags, VCB_VOLUME_LOCKED)) {
            DEBUG(DL_ERR, ( ": Ext2UnlockVcb: Volume is not locked.\n"));
            Status = STATUS_NOT_LOCKED;
            __leave;
        }

        if (Vcb->LockFile == FileObject) {
            ClearLongFlag(Vcb->Flags, VCB_VOLUME_LOCKED);
            Ext2ClearVpbFlag(Vcb->Vpb, VPB_LOCKED);
            DEBUG(DL_INF, ( "Ext2UnlockVcb: Volume unlocked.\n"));
            Status = STATUS_SUCCESS;
        } else {
            Status = STATUS_NOT_LOCKED;
        }

    } __finally {
        /* Nothing */
    }

    return Status;
}

NTSTATUS
Ext2UnlockVolume (
    IN PEXT2_IRP_CONTEXT IrpContext
)
{
    PIO_STACK_LOCATION IrpSp = NULL;
    PDEVICE_OBJECT  DeviceObject = NULL;
    PEXT2_VCB       Vcb = NULL;
    NTSTATUS Status = STATUS_UNSUCCESSFUL;
    BOOLEAN         VcbResourceAcquired = FALSE;

    __try {

        ASSERT(IrpContext != NULL);
        ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
               (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

        DeviceObject = IrpContext->DeviceObject;
        IrpSp = IoGetCurrentIrpStackLocation(IrpContext->Irp);

        /* This request is not allowed on the main device object */
        if (IsExt2FsDevice(DeviceObject)) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        Vcb = (PEXT2_VCB) DeviceObject->DeviceExtension;
        ASSERT(Vcb != NULL);
        ASSERT((Vcb->Identifier.Type == EXT2VCB) &&
               (Vcb->Identifier.Size == sizeof(EXT2_VCB)));

        ExAcquireResourceExclusiveLite(
            &Vcb->MainResource,
            TRUE );
        VcbResourceAcquired = TRUE;

        Status = Ext2UnlockVcb(Vcb, IrpSp->FileObject);

    } __finally {

        if (VcbResourceAcquired) {
            ExReleaseResourceLite(&Vcb->MainResource);
        }

        if (!IrpContext->ExceptionInProgress) {
            Ext2CompleteIrpContext(IrpContext,  Status);
        }
    }

    return Status;
}
