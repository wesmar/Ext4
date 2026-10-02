/**
 * mount.c - mount and verify: recognising an ext2/3/4 volume and building its VCB.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "../fsd/fsctl_internal.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2IsMediaWriteProtected)
#pragma alloc_text(PAGE, Ext2MountVolume)
#pragma alloc_text(PAGE, Ext2IsVolumeMounted)
#pragma alloc_text(PAGE, Ext2VerifyVolume)
#endif

BOOLEAN
Ext2IsMediaWriteProtected (
    IN PEXT2_IRP_CONTEXT   IrpContext,
    IN PDEVICE_OBJECT TargetDevice
)
{
    UNREFERENCED_PARAMETER(IrpContext);
    PIRP            Irp;
    KEVENT          Event;
    NTSTATUS        Status;
    IO_STATUS_BLOCK IoStatus;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    Irp = IoBuildDeviceIoControlRequest( IOCTL_DISK_IS_WRITABLE,
                                         TargetDevice,
                                         NULL,
                                         0,
                                         NULL,
                                         0,
                                         FALSE,
                                         &Event,
                                         &IoStatus );

    if (Irp == NULL) {
        return FALSE;
    }

    SetFlag(IoGetNextIrpStackLocation(Irp)->Flags, SL_OVERRIDE_VERIFY_VOLUME);

    Status = IoCallDriver(TargetDevice, Irp);

    if (Status == STATUS_PENDING) {
        LARGE_INTEGER Timeout;
        Timeout.QuadPart = (LONGLONG)-30 * 10 * 1000 * 1000; /* 30 seconds */

        Status = KeWaitForSingleObject( &Event,
                                      Executive,
                                      KernelMode,
                                      FALSE,
                                      &Timeout );

        if (Status == STATUS_TIMEOUT) {
            IoCancelIrp(Irp);
            KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
            Status = STATUS_IO_TIMEOUT;
        } else {
            Status = IoStatus.Status;
        }
    }

    return (BOOLEAN)(Status == STATUS_MEDIA_WRITE_PROTECTED);
}

NTSTATUS
Ext2MountVolume (IN PEXT2_IRP_CONTEXT IrpContext)
{
    PDEVICE_OBJECT              MainDeviceObject;
    BOOLEAN                     GlobalDataResourceAcquired = FALSE;
    PIRP                        Irp;
    PIO_STACK_LOCATION          IoStackLocation;
    PDEVICE_OBJECT              TargetDeviceObject;
    NTSTATUS                    Status = STATUS_UNRECOGNIZED_VOLUME;
    PDEVICE_OBJECT              VolumeDeviceObject = NULL;
    PEXT2_VCB                   Vcb = NULL, OldVcb = NULL;
    PVPB                        OldVpb = NULL, Vpb = NULL;
    PEXT2_SUPER_BLOCK           Ext2Sb = NULL;
    ULONG                       dwBytes;
    DISK_GEOMETRY               DiskGeometry;
    LARGE_INTEGER               SysTime;

    /*
     * Counted before the unload state is looked at, uncounted after the
     * volume device is gone: "sc stop" marks only the devices that exist
     * when it runs, and the I/O manager calls DriverUnload only when such
     * a marked device is the last one to go. A mount that is still on its
     * way (an MMP wait above all) creates and deletes a device of its own,
     * so the drain thread must not let go of the control devices before
     * it is done (Ext2PrepareToUnload checks the count).
     */
    InterlockedIncrement(&Ext2Global->MountsInFlight);

    __try {

        ASSERT(IrpContext != NULL);
        ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
               (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

        MainDeviceObject = IrpContext->DeviceObject;

        /* Make sure we can wait. */

        SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT);

        /* This request is only allowed on the main device object */
        if (!IsExt2FsDevice(MainDeviceObject)) {
            Status = STATUS_INVALID_DEVICE_REQUEST;
            __leave;
        }

        if (Ext2Global->UnloadState != EXT2_UNLOAD_IDLE ||
            IsFlagOn(Ext2Global->Flags, EXT2_UNLOAD_PENDING)) {
            Status = STATUS_UNRECOGNIZED_VOLUME;
            __leave;
        }


        Irp = IrpContext->Irp;
        IoStackLocation = IoGetCurrentIrpStackLocation(Irp);
        TargetDeviceObject =
            IoStackLocation->Parameters.MountVolume.DeviceObject;

        dwBytes = sizeof(DISK_GEOMETRY);
        Status = Ext2DiskIoControl(
                     TargetDeviceObject,
                     IOCTL_DISK_GET_DRIVE_GEOMETRY,
                     NULL,
                     0,
                     &DiskGeometry,
                     &dwBytes );

        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        Status = IoCreateDevice(
                     MainDeviceObject->DriverObject,
                     sizeof(EXT2_VCB),
                     NULL,
                     FILE_DEVICE_DISK_FILE_SYSTEM,
                     0,
                     FALSE,
                     &VolumeDeviceObject );

        if (!NT_SUCCESS(Status)) {
            __leave;
        }
        INC_MEM_COUNT(PS_VCB, VolumeDeviceObject, sizeof(EXT2_VCB));


        VolumeDeviceObject->StackSize = (CCHAR)(TargetDeviceObject->StackSize + 1);
        ClearFlag(VolumeDeviceObject->Flags, DO_DEVICE_INITIALIZING);

/*
        These are for buffer-address alignment requirements.
        Never do this check, unless you want fail user requests :)

        if (TargetDeviceObject->AlignmentRequirement >
                VolumeDeviceObject->AlignmentRequirement) {

            VolumeDeviceObject->AlignmentRequirement =
                TargetDeviceObject->AlignmentRequirement;
        }

        if (DiskGeometry.BytesPerSector - 1 >
                VolumeDeviceObject->AlignmentRequirement) {
            VolumeDeviceObject->AlignmentRequirement =
                DiskGeometry.BytesPerSector - 1;
            TargetDeviceObject->AlignmentRequirement =
                DiskGeometry.BytesPerSector - 1;
        }
*/
        (IoStackLocation->Parameters.MountVolume.Vpb)->DeviceObject =
            VolumeDeviceObject;
        Vpb = IoStackLocation->Parameters.MountVolume.Vpb;

        Vcb = (PEXT2_VCB) VolumeDeviceObject->DeviceExtension;

        RtlZeroMemory(Vcb, sizeof(EXT2_VCB));
        Vcb->Identifier.Type = EXT2VCB;
        Vcb->Identifier.Size = sizeof(EXT2_VCB);
        Vcb->TargetDeviceObject = TargetDeviceObject;
        Vcb->DiskGeometry = DiskGeometry;
        InitializeListHead(&Vcb->Next);

        Status = Ext2LoadSuper(Vcb, FALSE, &Ext2Sb);
        if (!NT_SUCCESS(Status)) {
            Vcb = NULL;
            Status = STATUS_UNRECOGNIZED_VOLUME;
            __leave;
        }
        ASSERT (NULL != Ext2Sb);

        /* check Linux Ext2/Ext3 volume magic */
        if (Ext2Sb->s_magic == EXT2_SUPER_MAGIC) {
            DEBUG(DL_INF, ( "Volume of ext2 file system is found.\n"));
        } else  {
            Status = STATUS_UNRECOGNIZED_VOLUME;
            Vcb = NULL;
            __leave;
        }

        DEBUG(DL_DBG, ("Ext2MountVolume: DevObject=%p Vcb=%p\n", VolumeDeviceObject, Vcb));

        /* initialize Vcb structure */
        Status = Ext2InitializeVcb( IrpContext, Vcb, Ext2Sb,
                                    TargetDeviceObject,
                                    VolumeDeviceObject, Vpb);

        if (NT_SUCCESS(Status))  {

            PLIST_ENTRY List;

            ExAcquireResourceExclusiveLite(&(Ext2Global->Resource), TRUE);
            GlobalDataResourceAcquired = TRUE;

            if (Ext2Global->UnloadState != EXT2_UNLOAD_IDLE ||
                IsFlagOn(Ext2Global->Flags, EXT2_UNLOAD_PENDING)) {
                Status = STATUS_UNRECOGNIZED_VOLUME;
                __leave;
            }

            for (List = Ext2Global->VcbList.Flink;
                    List != &Ext2Global->VcbList;
                    List = List->Flink) {

                OldVcb = CONTAINING_RECORD(List, EXT2_VCB, Next);
                OldVpb = OldVcb->Vpb;

                /* in case we are already in the queue, should not happen */
                if (OldVpb == Vpb) {
                    continue;
                }

                if ( (OldVpb->SerialNumber == Vpb->SerialNumber) &&
                        (!IsMounted(OldVcb)) && (IsFlagOn(OldVcb->Flags, VCB_NEW_VPB)) &&
                        (OldVpb->RealDevice == TargetDeviceObject) &&
                        (OldVpb->VolumeLabelLength == Vpb->VolumeLabelLength) &&
                        (RtlEqualMemory(&OldVpb->VolumeLabel[0],
                                        &Vpb->VolumeLabel[0],
                                        Vpb->VolumeLabelLength)) &&
                        (RtlEqualMemory(&OldVcb->SuperBlock->s_uuid[0],
                                        &Vcb->SuperBlock->s_uuid[0], 16)) ) {
                    ClearLongFlag(OldVcb->Flags, VCB_MOUNTED);
                }
            }

            if (!ext4_superblock_csum_verify(&Vcb->sb, Ext2Sb)) {
                DEBUG(DL_ERR, ( "Found ext4 filesystem with invalid superblock checksum. Run e2fsck?\n"));
            }

            /* update fs mount time */
            KeQuerySystemTime(&SysTime);
            Ext2SetSuperTime(Vcb->SuperBlock, s_mtime, Ext2UnixTime(&SysTime));

            SetLongFlag(Vcb->Flags, VCB_MOUNTED);
            SetFlag(Vcb->Vpb->Flags, VPB_MOUNTED);
            Ext2InsertVcb(Vcb);

            /* a drive letter, if the mount manager gave it none (letter.c);
               done on a worker, never from inside the mount */
            Ext2QueueLetterAssign(Vcb);

            Vcb = NULL;
            Vpb = NULL;
            ObDereferenceObject(TargetDeviceObject);

        } else {

            Vcb = NULL;
        }

    } __finally {

        if (GlobalDataResourceAcquired) {
            ExReleaseResourceLite(&Ext2Global->Resource);
        }

        if (!NT_SUCCESS(Status)) {

            if (!NT_SUCCESS(Status)) {
                if ( Vpb != NULL ) {
                    Vpb->DeviceObject = NULL;
                }
            }

            if (Vcb) {
                Ext2DestroyVcb(Vcb);
            } else {
                if (Ext2Sb) {
                    Ext2FreePool(Ext2Sb, EXT2_SB_MAGIC);
                }
                if (VolumeDeviceObject) {
                    IoDeleteDevice(VolumeDeviceObject);
                    DEC_MEM_COUNT(PS_VCB, VolumeDeviceObject, sizeof(EXT2_VCB));
                }
            }
        }

        /* the device is deleted or mounted (then the volume lists hold it) */
        if (InterlockedDecrement(&Ext2Global->MountsInFlight) == 0) {
            Ext2UnloadKick();
        }

        if (!IrpContext->ExceptionInProgress) {
            Ext2CompleteIrpContext(IrpContext,  Status);
        }
    }

    return Status;
}

VOID
Ext2VerifyVcb (IN PEXT2_IRP_CONTEXT IrpContext,
               IN PEXT2_VCB         Vcb )
{
    NTSTATUS                Status = STATUS_SUCCESS;

    BOOLEAN                 bVerify = FALSE;
    ULONG                   ChangeCount = 0;
    ULONG                   dwBytes;

    PIRP                    Irp;
    PEXTENDED_IO_STACK_LOCATION      IrpSp;

    __try {

        ASSERT(IrpContext != NULL);

        ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
               (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

        Irp = IrpContext->Irp;
        IrpSp = (PEXTENDED_IO_STACK_LOCATION)IoGetCurrentIrpStackLocation(Irp);

        bVerify = IsFlagOn(Vcb->Vpb->RealDevice->Flags, DO_VERIFY_VOLUME);

        if ( (IsFlagOn(Vcb->Flags, VCB_REMOVABLE_MEDIA) ||
                IsFlagOn(Vcb->Flags, VCB_FLOPPY_DISK)) && !bVerify ) {

            dwBytes = sizeof(ULONG);
            Status = Ext2DiskIoControl(
                         Vcb->TargetDeviceObject,
                         IOCTL_DISK_CHECK_VERIFY,
                         NULL,
                         0,
                         &ChangeCount,
                         &dwBytes );

            if ( STATUS_VERIFY_REQUIRED == Status ||
                    STATUS_DEVICE_NOT_READY == Status ||
                    STATUS_NO_MEDIA_IN_DEVICE == Status ||
                    (NT_SUCCESS(Status) &&
                     (ChangeCount != Vcb->ChangeCount))) {

                KIRQL Irql;

                IoAcquireVpbSpinLock(&Irql);
                if (Vcb->Vpb == Vcb->Vpb->RealDevice->Vpb) {
                    SetFlag(Vcb->Vpb->RealDevice->Flags, DO_VERIFY_VOLUME);
                }
                IoReleaseVpbSpinLock(Irql);

            } else {

                if (!NT_SUCCESS(Status)) {
                    Ext2NormalizeAndRaiseStatus(IrpContext, Status);
                }
            }
        }

        if ( IsFlagOn(Vcb->Vpb->RealDevice->Flags, DO_VERIFY_VOLUME)) {
            IoSetHardErrorOrVerifyDevice( Irp, Vcb->Vpb->RealDevice );
            Ext2NormalizeAndRaiseStatus ( IrpContext,
                                          STATUS_VERIFY_REQUIRED );
        }

        if (IsMounted(Vcb)) {

            if ( (IrpContext->MajorFunction == IRP_MJ_WRITE) ||
                    (IrpContext->MajorFunction == IRP_MJ_SET_INFORMATION) ||
                    (IrpContext->MajorFunction == IRP_MJ_SET_EA) ||
                    (IrpContext->MajorFunction == IRP_MJ_FLUSH_BUFFERS) ||
                    (IrpContext->MajorFunction == IRP_MJ_SET_VOLUME_INFORMATION) ||
                    (IrpContext->MajorFunction == IRP_MJ_FILE_SYSTEM_CONTROL &&
                     IrpContext->MinorFunction == IRP_MN_USER_FS_REQUEST &&
                     IrpSp->Parameters.FileSystemControl.FsControlCode ==
                     FSCTL_MARK_VOLUME_DIRTY)) {

                if (IsFlagOn(Vcb->Flags, VCB_WRITE_PROTECTED)) {

                    KIRQL Irql;

                    IoAcquireVpbSpinLock(&Irql);
                    if (Vcb->Vpb == Vcb->Vpb->RealDevice->Vpb) {
                        SetFlag (Vcb->Vpb->RealDevice->Flags, DO_VERIFY_VOLUME);
                    }
                    IoReleaseVpbSpinLock(Irql);

                    IoSetHardErrorOrVerifyDevice( Irp, Vcb->Vpb->RealDevice );

                    Ext2RaiseStatus(IrpContext, STATUS_MEDIA_WRITE_PROTECTED);
                }
            }
        }

    } __finally {

    }

}

NTSTATUS
Ext2VerifyVolume (IN PEXT2_IRP_CONTEXT IrpContext)
{
    PDEVICE_OBJECT          DeviceObject;
    NTSTATUS                Status = STATUS_UNSUCCESSFUL;
    PEXT2_SUPER_BLOCK       ext2_sb = NULL;
    PEXT2_VCB               Vcb = NULL;
    BOOLEAN                 VcbResourceAcquired = FALSE;
    PIRP                    Irp;
    ULONG                   ChangeCount = 0;
    ULONG                   dwBytes;

    __try {

        ASSERT(IrpContext != NULL);
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

        VcbResourceAcquired =
            ExAcquireResourceExclusiveLite(
                &Vcb->MainResource,
                TRUE );

        if (!FlagOn(Vcb->TargetDeviceObject->Flags, DO_VERIFY_VOLUME)) {
            Status = STATUS_SUCCESS;
            __leave;
        }

        if (!IsMounted(Vcb)) {
            Status = STATUS_WRONG_VOLUME;
            __leave;
        }

        dwBytes = sizeof(ULONG);
        Status = Ext2DiskIoControl(
                     Vcb->TargetDeviceObject,
                     IOCTL_DISK_CHECK_VERIFY,
                     NULL,
                     0,
                     &ChangeCount,
                     &dwBytes );

        if (!NT_SUCCESS(Status)) {
            Status = STATUS_WRONG_VOLUME;
            __leave;
        } else {
            Vcb->ChangeCount = ChangeCount;
        }

        Irp = IrpContext->Irp;

        Status = Ext2LoadSuper(Vcb, TRUE, &ext2_sb);

        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        ASSERT(NULL != ext2_sb);
        if ((ext2_sb->s_magic == EXT2_SUPER_MAGIC) &&
                (memcmp(ext2_sb->s_uuid, SUPER_BLOCK->s_uuid, 16) == 0) &&
                (memcmp(ext2_sb->s_volume_name, SUPER_BLOCK->s_volume_name, 16) ==0)) {

            ClearFlag(Vcb->TargetDeviceObject->Flags, DO_VERIFY_VOLUME);

            if (Ext2IsMediaWriteProtected(IrpContext, Vcb->TargetDeviceObject)) {
                SetLongFlag(Vcb->Flags, VCB_WRITE_PROTECTED);
            } else {
                ClearLongFlag(Vcb->Flags, VCB_WRITE_PROTECTED);
            }

            DEBUG(DL_INF, ( "Ext2VerifyVolume: Volume verify succeeded.\n"));

        } else {

            Status = STATUS_WRONG_VOLUME;
            Ext2PurgeVolume(Vcb, FALSE);

            SetLongFlag(Vcb->Flags, VCB_DISMOUNT_PENDING);
            ClearFlag(Vcb->TargetDeviceObject->Flags, DO_VERIFY_VOLUME);

            DEBUG(DL_INF, ( "Ext2VerifyVolume: Volume verify failed.\n"));
        }

    } __finally {

        if (ext2_sb)
            Ext2FreePool(ext2_sb, EXT2_SB_MAGIC);

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
Ext2IsVolumeMounted (IN PEXT2_IRP_CONTEXT IrpContext)
{
    PDEVICE_OBJECT      DeviceObject;
    PEXT2_VCB           Vcb = 0;
    NTSTATUS            Status = STATUS_SUCCESS;

    ASSERT(IrpContext);

    ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
           (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

    DeviceObject = IrpContext->DeviceObject;
    Vcb = (PEXT2_VCB) DeviceObject->DeviceExtension;

    if (!IsMounted(Vcb) || IsFlagOn(Vcb->Flags, VCB_DISMOUNT_PENDING)) {
        Status = STATUS_VOLUME_DISMOUNTED;
    } else {
        Ext2VerifyVcb (IrpContext, Vcb);
    }

    Ext2CompleteIrpContext(IrpContext,  Status);

    return Status;
}
