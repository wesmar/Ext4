/**
 * RequestContext.c - IRP context allocation.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "core_internal.h"

PEXT2_IRP_CONTEXT
Ext2AllocateIrpContext (IN PDEVICE_OBJECT   DeviceObject,
                        IN PIRP             Irp )
{
    PIO_STACK_LOCATION   irpSp;
    PEXT2_IRP_CONTEXT    IrpContext;

    ASSERT(DeviceObject != NULL);
    ASSERT(Irp != NULL);

    irpSp = IoGetCurrentIrpStackLocation(Irp);

    IrpContext = (PEXT2_IRP_CONTEXT) (
                     ExAllocateFromNPagedLookasideList(
                         &(Ext2Global->Ext2IrpContextLookasideList)));

    if (IrpContext == NULL) {
        return NULL;
    }

    RtlZeroMemory(IrpContext, sizeof(EXT2_IRP_CONTEXT) );

    IrpContext->Identifier.Type = EXT2ICX;
    IrpContext->Identifier.Size = sizeof(EXT2_IRP_CONTEXT);

    IrpContext->Irp = Irp;
    IrpContext->MajorFunction = irpSp->MajorFunction;
    IrpContext->MinorFunction = irpSp->MinorFunction;
    IrpContext->DeviceObject = DeviceObject;
    IrpContext->FileObject = irpSp->FileObject;
    if (NULL != IrpContext->FileObject) {
        IrpContext->Fcb = (PEXT2_FCB)IrpContext->FileObject->FsContext;
        IrpContext->Ccb = (PEXT2_CCB)IrpContext->FileObject->FsContext2;
    }

    if (IrpContext->FileObject != NULL) {
        IrpContext->RealDevice = IrpContext->FileObject->DeviceObject;
    } else if (IrpContext->MajorFunction == IRP_MJ_FILE_SYSTEM_CONTROL) {
        if (irpSp->Parameters.MountVolume.Vpb) {
            IrpContext->RealDevice = irpSp->Parameters.MountVolume.Vpb->RealDevice;
        }
    }

    if (IsFlagOn(irpSp->Flags, SL_WRITE_THROUGH)) {
        SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_WRITE_THROUGH);
    }

    if (IsFlagOn(irpSp->Flags, SL_OVERRIDE_VERIFY_VOLUME)) {
        SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_VERIFY_READ);
    }

    /* IoIsOperationSynchronous dereferences the stack location's file
       object without checking it; an IRP built inside the kernel (e.g. a
       device control the driver sends itself) has none and is always
       waited for by its sender */
    if (IrpContext->MajorFunction == IRP_MJ_CLEANUP ||
        IrpContext->MajorFunction == IRP_MJ_CLOSE ||
        IrpContext->MajorFunction == IRP_MJ_SHUTDOWN ||
        IrpContext->FileObject == NULL ||
        IoIsOperationSynchronous(Irp)) {

        SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT);
    }

    IrpContext->IsTopLevel = (IoGetTopLevelIrp() == Irp);
    IrpContext->ExceptionInProgress = FALSE;
    INC_IRP_COUNT(IrpContext);

    return IrpContext;
}

VOID
Ext2FreeIrpContext (IN PEXT2_IRP_CONTEXT IrpContext)
{
    ASSERT(IrpContext != NULL);

    ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
           (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

    /* free the IrpContext to NonPagedList */
    IrpContext->Identifier.Type = 0;
    IrpContext->Identifier.Size = 0;

    DEC_IRP_COUNT(IrpContext);
    ExFreeToNPagedLookasideList(&(Ext2Global->Ext2IrpContextLookasideList), IrpContext);
}
