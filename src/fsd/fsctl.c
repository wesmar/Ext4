/**
 * fsctl.c - IRP_MJ_FILE_SYSTEM_CONTROL dispatch, user FSCTLs and oplocks.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "fsctl_internal.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2AllowExtendedDasdIo)
#pragma alloc_text(PAGE, Ext2UserFsRequest)
#pragma alloc_text(PAGE, Ext2FileSystemControl)
#endif

NTSTATUS
Ext2InvalidateVolumes ( IN PEXT2_IRP_CONTEXT IrpContext )
{
    NTSTATUS Status = STATUS_UNSUCCESSFUL;
    PIRP                Irp;
    PIO_STACK_LOCATION  IrpSp;

    HANDLE              Handle;
    PLIST_ENTRY         ListEntry;

    ULONG               InputLength = 0;
    PFILE_OBJECT        FileObject;
    PDEVICE_OBJECT      DeviceObject;
    BOOLEAN             GlobalResourceAcquired = FALSE;

    LUID Privilege = {SE_TCB_PRIVILEGE, 0};

    __try {

        Irp   = IrpContext->Irp;
        IrpSp = IoGetCurrentIrpStackLocation(Irp);

        if (!IsExt2FsDevice(IrpSp->DeviceObject)) {
            Status = STATUS_INVALID_DEVICE_REQUEST;
            __leave;
        }

        if (!SeSinglePrivilegeCheck(Privilege, Irp->RequestorMode)) {
            Status = STATUS_PRIVILEGE_NOT_HELD;
            __leave;
        }

        InputLength = IrpSp->Parameters.FileSystemControl.InputBufferLength;

#if defined(_WIN64)
        if (IoIs32bitProcess(Irp)) {
            if (InputLength != sizeof(UINT32)) {
                Status = STATUS_INVALID_PARAMETER;
                __leave;
            }
            Handle = (HANDLE) LongToHandle( (*(PUINT32)Irp->AssociatedIrp.SystemBuffer) );
        } else
#endif
        {
            if (InputLength != sizeof(HANDLE)) {
                Status = STATUS_INVALID_PARAMETER;
                __leave;
            }
            Handle = *(PHANDLE)Irp->AssociatedIrp.SystemBuffer;
        }

        Status = ObReferenceObjectByHandle( Handle,
                                            0,
                                            *IoFileObjectType,
                                            KernelMode,
                                            &FileObject,
                                            NULL );

        if (!NT_SUCCESS(Status)) {
            __leave;
        } else {
            DeviceObject = FileObject->DeviceObject;
            ObDereferenceObject(FileObject);
        }

        ExAcquireResourceExclusiveLite(&Ext2Global->Resource,  TRUE);
        GlobalResourceAcquired = TRUE;

        ListEntry = Ext2Global->VcbList.Flink;
        while (ListEntry != &Ext2Global->VcbList)  {

            PEXT2_VCB Vcb = CONTAINING_RECORD(ListEntry, EXT2_VCB, Next);
            ListEntry = ListEntry->Flink;

            DEBUG(DL_DBG, ( "Ext2InvalidateVolumes: Vcb=%xh Vcb->Vpb=%xh "
                            "Blink = %p &Vcb->Next = %p\n",
                            Vcb, Vcb->Vpb, ListEntry->Blink, &Vcb->Next));

            if (Vcb->Vpb && (Vcb->Vpb->RealDevice == DeviceObject)) {

                DEBUG(DL_DBG, ( "Ext2InvalidateVolumes: Got Vcb=%xh Vcb->Vpb=%xh "
                                "Blink = %p &Vcb->Next = %p\n",
                                Vcb, Vcb->Vpb, ListEntry->Blink, &Vcb->Next));
                /* dismount the volume */
                Ext2CheckDismount(IrpContext, Vcb, FALSE);
            }
        }

    } __finally {

        if (GlobalResourceAcquired) {
            ExReleaseResourceLite(&Ext2Global->Resource);
        }

        if (!IrpContext->ExceptionInProgress) {
            Ext2CompleteIrpContext(IrpContext,  Status);
        }
    }

    return Status;
}

NTSTATUS
Ext2AllowExtendedDasdIo(IN PEXT2_IRP_CONTEXT IrpContext)
{
    PIO_STACK_LOCATION IrpSp;
    PEXT2_VCB Vcb;
    PEXT2_CCB Ccb;
    NTSTATUS  status;

    IrpSp = IoGetCurrentIrpStackLocation(IrpContext->Irp);

    Vcb = (PEXT2_VCB) IrpSp->FileObject->FsContext;
    Ccb = (PEXT2_CCB) IrpSp->FileObject->FsContext2;

    ASSERT(Vcb != NULL);

    ASSERT((Vcb->Identifier.Type == EXT2VCB) &&
           (Vcb->Identifier.Size == sizeof(EXT2_VCB)));

    if (!IsMounted(Vcb) || IsFlagOn(Vcb->Flags, VCB_DISMOUNT_PENDING)) {
        status = STATUS_VOLUME_DISMOUNTED;
        Ext2CompleteIrpContext(IrpContext, status);
        return status;
    }

    if (Ccb) {
        SetLongFlag(Ccb->Flags, CCB_ALLOW_EXTENDED_DASD_IO);
        status = STATUS_SUCCESS;
    } else {
        status = STATUS_INVALID_PARAMETER;
    }

    Ext2CompleteIrpContext(IrpContext, status);
    return status;
}

/*
 *  Ext2OplockRequest
 *
 *    oplock requests handler routine
 *
 *  Arguments:
 *    IrpContext: the ext2 irp context
 *
 *  Return Value:
 *    NTSTATUS:  The return status for the operation
 *
 */

NTSTATUS
Ext2OplockRequest (
    IN PEXT2_IRP_CONTEXT IrpContext
)
{
    NTSTATUS Status = STATUS_UNSUCCESSFUL;

    ULONG       FsCtrlCode;
    PDEVICE_OBJECT DeviceObject;
    PFILE_OBJECT   FileObject;

    PIRP Irp = NULL;
    PIO_STACK_LOCATION IrpSp;
    PEXTENDED_IO_STACK_LOCATION EIrpSp;

    PEXT2_VCB   Vcb = NULL;
    PEXT2_FCB   Fcb = NULL;
    PEXT2_CCB   Ccb = NULL;

    ULONG OplockCount = 0;

    BOOLEAN VcbResourceAcquired = FALSE;
    BOOLEAN FcbResourceAcquired = FALSE;

    ASSERT(IrpContext);

    __try {

        Irp = IrpContext->Irp;
        ASSERT(Irp);

        IrpSp = IoGetCurrentIrpStackLocation(Irp);
        ASSERT(IrpSp);
        EIrpSp = (PEXTENDED_IO_STACK_LOCATION)IrpSp;

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

        FileObject = IrpContext->FileObject;

        Fcb = (PEXT2_FCB) FileObject->FsContext;

        /* This request is not allowed on volumes */

        if (Fcb == NULL || Fcb->Identifier.Type == EXT2VCB) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        ASSERT((Fcb->Identifier.Type == EXT2FCB) &&
               (Fcb->Identifier.Size == sizeof(EXT2_FCB)));

        if (IsInodeDeleted(Fcb)) {
            Status = STATUS_FILE_DELETED;
            __leave;
        }

        Ccb = (PEXT2_CCB) FileObject->FsContext2;
        if (Ccb == NULL) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        ASSERT((Ccb->Identifier.Type == EXT2CCB) &&
               (Ccb->Identifier.Size == sizeof(EXT2_CCB)));

        FsCtrlCode = EIrpSp->Parameters.FileSystemControl.FsControlCode;

        switch (FsCtrlCode) {

        case FSCTL_REQUEST_OPLOCK_LEVEL_1:
        case FSCTL_REQUEST_OPLOCK_LEVEL_2:
        case FSCTL_REQUEST_BATCH_OPLOCK:

            VcbResourceAcquired =
                ExAcquireResourceSharedLite(
                    &Vcb->MainResource,
                    IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT) );

            ClearFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT);

            FcbResourceAcquired =
                ExAcquireResourceExclusiveLite (
                    &Fcb->MainResource,
                    IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT));

            if (FsCtrlCode == FSCTL_REQUEST_OPLOCK_LEVEL_2) {
                OplockCount = (ULONG) FsRtlAreThereCurrentFileLocks(&Fcb->FileLockAnchor);
            } else {
                OplockCount = Fcb->OpenHandleCount;
            }

            break;

        case FSCTL_OPLOCK_BREAK_ACKNOWLEDGE:
        case FSCTL_OPBATCH_ACK_CLOSE_PENDING :
        case FSCTL_OPLOCK_BREAK_NOTIFY:
        case FSCTL_OPLOCK_BREAK_ACK_NO_2:

            FcbResourceAcquired =
                ExAcquireResourceSharedLite (
                    &Fcb->MainResource,
                    IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT));

            break;

        default:

            Ext2BugCheck(EXT2_BUGCHK_FSCTL, FsCtrlCode, 0, 0);
        }

        /* Call the FsRtl routine to grant/acknowledge oplock. */

        Status = FsRtlOplockFsctrl( &Fcb->Oplock,
                                    Irp,
                                    OplockCount );

        /* Set the flag indicating if Fast I/O is possible */

        Fcb->Header.IsFastIoPossible = Ext2IsFastIoPossible(Fcb);
        IrpContext->Irp = NULL;

    } __finally {

        if (FcbResourceAcquired) {
            ExReleaseResourceLite(&Fcb->MainResource);
        }

        if (VcbResourceAcquired) {
            ExReleaseResourceLite(&Vcb->MainResource);
        }

        if (!AbnormalTermination()) {
            Ext2CompleteIrpContext(IrpContext, Status);
        }
    }

    return Status;
}

NTSTATUS
Ext2IsVolumeDirty (
    IN PEXT2_IRP_CONTEXT IrpContext
)
{
    NTSTATUS status = STATUS_SUCCESS;
    PIRP  Irp;
    PEXTENDED_IO_STACK_LOCATION IrpSp;
    PULONG VolumeState;

    __try {

        Irp = IrpContext->Irp;
        IrpSp = (PEXTENDED_IO_STACK_LOCATION)IoGetCurrentIrpStackLocation(Irp);

        /* Get a pointer to the output buffer.  Look at the system buffer field in the
           irp first.  Then the Irp Mdl. */

        if (Irp->AssociatedIrp.SystemBuffer != NULL) {

            VolumeState = Irp->AssociatedIrp.SystemBuffer;

        } else if (Irp->MdlAddress != NULL) {

            VolumeState = MmGetSystemAddressForMdlSafe(Irp->MdlAddress,
                                                       NormalPagePriority | MdlMappingNoExecute);
            if (VolumeState == NULL) {
                status = STATUS_INSUFFICIENT_RESOURCES;
                __leave;
            }

        } else {

            status = STATUS_INVALID_USER_BUFFER;
            __leave;
        }

        if (IrpSp->Parameters.FileSystemControl.OutputBufferLength < sizeof(ULONG)) {
            status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        *VolumeState = 0;

    } __finally {

        if (!IrpContext->ExceptionInProgress) {
            Ext2CompleteIrpContext(IrpContext,  status);
        }
    }

    return status;
}

NTSTATUS
Ext2UserFsRequest (IN PEXT2_IRP_CONTEXT IrpContext)
{
    PIRP                Irp;
    PIO_STACK_LOCATION  IoStackLocation;
    ULONG               FsControlCode;
    NTSTATUS            Status;

    ASSERT(IrpContext);

    ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
           (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

    Irp = IrpContext->Irp;
    IoStackLocation = IoGetCurrentIrpStackLocation(Irp);

    FsControlCode =
        IoStackLocation->Parameters.FileSystemControl.FsControlCode;

    switch (FsControlCode) {

    case FSCTL_GET_REPARSE_POINT:
        Status = Ext2GetReparsePoint(IrpContext);
        break;
        
    case FSCTL_SET_REPARSE_POINT:
        Status = Ext2SetReparsePoint(IrpContext);
        break;
        
    case FSCTL_DELETE_REPARSE_POINT:
        Status = Ext2DeleteReparsePoint(IrpContext);
        break;

    case FSCTL_LOCK_VOLUME:
        Status = Ext2LockVolume(IrpContext);
        break;

    case FSCTL_UNLOCK_VOLUME:
        Status = Ext2UnlockVolume(IrpContext);
        break;

    case FSCTL_DISMOUNT_VOLUME:
        Status = Ext2DismountVolume(IrpContext);
        break;

    case FSCTL_IS_VOLUME_MOUNTED:
        Status = Ext2IsVolumeMounted(IrpContext);
        break;

    case FSCTL_INVALIDATE_VOLUMES:
        Status = Ext2InvalidateVolumes(IrpContext);
        break;

    case FSCTL_ALLOW_EXTENDED_DASD_IO:
        Status = Ext2AllowExtendedDasdIo(IrpContext);
        break;

    case FSCTL_REQUEST_OPLOCK_LEVEL_1:
    case FSCTL_REQUEST_OPLOCK_LEVEL_2:
    case FSCTL_REQUEST_BATCH_OPLOCK:
    case FSCTL_OPLOCK_BREAK_ACKNOWLEDGE:
    case FSCTL_OPBATCH_ACK_CLOSE_PENDING:
    case FSCTL_OPLOCK_BREAK_NOTIFY:
    case FSCTL_OPLOCK_BREAK_ACK_NO_2:

        Status = Ext2OplockRequest(IrpContext);
        break;

    case FSCTL_IS_VOLUME_DIRTY:
        Status = Ext2IsVolumeDirty(IrpContext);
        break;

    case FSCTL_QUERY_RETRIEVAL_POINTERS:
        Status = Ext2QueryRetrievalPointers(IrpContext);
        break;

    case FSCTL_GET_RETRIEVAL_POINTERS:
        Status = Ext2GetRetrievalPointers(IrpContext);
        break;

    case FSCTL_GET_RETRIEVAL_POINTER_BASE:
        Status = Ext2GetRetrievalPointerBase(IrpContext);
        break;

    default:

        DEBUG(DL_INF, ( "Ext2UserFsRequest: Invalid User Request: %xh.\n", FsControlCode));
        Status = STATUS_INVALID_DEVICE_REQUEST;

        Ext2CompleteIrpContext(IrpContext,  Status);
    }

    return Status;
}

NTSTATUS
Ext2FileSystemControl (IN PEXT2_IRP_CONTEXT IrpContext)
{
    NTSTATUS    Status;

    ASSERT(IrpContext);

    ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
           (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

    switch (IrpContext->MinorFunction) {

    case IRP_MN_USER_FS_REQUEST:
        Status = Ext2UserFsRequest(IrpContext);
        break;

    case IRP_MN_MOUNT_VOLUME:
        Status = Ext2MountVolume(IrpContext);
        break;

    case IRP_MN_VERIFY_VOLUME:
        Status = Ext2VerifyVolume(IrpContext);
        break;

    default:

        DEBUG(DL_DBG, ( "Ext2FilsSystemControl: Invalid Device Request.\n"));
        Status = STATUS_INVALID_DEVICE_REQUEST;
        Ext2CompleteIrpContext(IrpContext,  Status);
    }

    return Status;
}
