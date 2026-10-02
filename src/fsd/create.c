/**
 * create.c - IRP_MJ_CREATE entry point, volume opens, supersede/overwrite of a file.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include <linux/ext4_xattr.h>
#include "create_internal.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2CreateVolume)
#pragma alloc_text(PAGE, Ext2Create)
#pragma alloc_text(PAGE, Ext2SupersedeOrOverWriteFile)
#endif

NTSTATUS
Ext2CreateVolume(PEXT2_IRP_CONTEXT IrpContext, PEXT2_VCB Vcb)
{
    PIO_STACK_LOCATION  IrpSp;
    PIRP                Irp;
    PEXT2_CCB           Ccb;

    NTSTATUS            Status;

    ACCESS_MASK         DesiredAccess;
    ULONG               ShareAccess;

    ULONG               Options;
    BOOLEAN             DirectoryFile;
    BOOLEAN             OpenTargetDirectory;

    ULONG               CreateDisposition;

    Irp = IrpContext->Irp;
    IrpSp = IoGetCurrentIrpStackLocation(Irp);

    Options  = IrpSp->Parameters.Create.Options;

    DirectoryFile = IsFlagOn(Options, FILE_DIRECTORY_FILE);
    OpenTargetDirectory = IsFlagOn(IrpSp->Flags, SL_OPEN_TARGET_DIRECTORY);

    CreateDisposition = (Options >> 24) & 0x000000ff;

    DesiredAccess = IrpSp->Parameters.Create.SecurityContext->DesiredAccess;
    ShareAccess   = IrpSp->Parameters.Create.ShareAccess;

    if (DirectoryFile) {
        return STATUS_NOT_A_DIRECTORY;
    }

    if (OpenTargetDirectory) {
        return STATUS_INVALID_PARAMETER;
    }

    if ( (CreateDisposition != FILE_OPEN) &&
            (CreateDisposition != FILE_OPEN_IF) ) {
        return STATUS_ACCESS_DENIED;
    }

    if ( !FlagOn(ShareAccess, FILE_SHARE_READ) &&
            Vcb->OpenVolumeCount  != 0 ) {
        return STATUS_SHARING_VIOLATION;
    }

    Ccb = Ext2AllocateCcb(0, NULL, NULL);
    if (Ccb == NULL) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto errorout;
    }

    Status = STATUS_SUCCESS;

    if (Vcb->OpenVolumeCount > 0) {
        Status = IoCheckShareAccess( DesiredAccess, ShareAccess,
                                     IrpSp->FileObject,
                                     &(Vcb->ShareAccess), TRUE);

        if (!NT_SUCCESS(Status)) {
            goto errorout;
        }
    } else {
        IoSetShareAccess( DesiredAccess, ShareAccess,
                          IrpSp->FileObject,
                          &(Vcb->ShareAccess)   );
    }


    if (Vcb->OpenVolumeCount == 0 &&
        !IsFlagOn(ShareAccess, FILE_SHARE_READ)  &&
        !IsFlagOn(ShareAccess, FILE_SHARE_WRITE) ){

        if (!IsVcbReadOnly(Vcb)) {
            Ext2FlushFiles(IrpContext, Vcb, FALSE);
            Ext2FlushVolume(IrpContext, Vcb, FALSE);
        }

        SetLongFlag(Vcb->Flags, VCB_VOLUME_LOCKED);
        Vcb->LockFile = IrpSp->FileObject;
    } else {

        if (FlagOn(IrpSp->FileObject->Flags, FO_NO_INTERMEDIATE_BUFFERING) &&
            FlagOn(DesiredAccess, FILE_READ_DATA | FILE_WRITE_DATA) ) {
            if (!IsVcbReadOnly(Vcb)) {
                Ext2FlushFiles(IrpContext, Vcb, FALSE);
                Ext2FlushVolume(IrpContext, Vcb, FALSE);
            }
        }
    }

    IrpSp->FileObject->Flags |= FO_NO_INTERMEDIATE_BUFFERING;
    IrpSp->FileObject->FsContext  = Vcb;
    IrpSp->FileObject->FsContext2 = Ccb;
    IrpSp->FileObject->Vpb = Vcb->Vpb;

    Ext2ReferXcb(&Vcb->ReferenceCount);
    Ext2ReferXcb(&Vcb->OpenHandleCount);
    Ext2ReferXcb(&Vcb->OpenVolumeCount);

    Irp->IoStatus.Information = FILE_OPENED;

errorout:

    return Status;
}

/*
 * Does this create need the volume to itself?
 *
 * Opens and creates run side by side under the shared volume resource:
 * the name cache is guarded by its stripes, the Fcb table by FcbLock, a file
 * by its own MainResource, and a directory's names by its DirResource,
 * which a create holds from finding the name free to adding it
 * (Ext2CreateFile) - so creates in different directories, measured as the
 * bulk of the waits under 8 threads, no longer queue on the volume.
 * The volume open stays exclusive, and so does the open of a rename's
 * target directory: rename and link still change the namespace with the
 * volume to themselves. So do removable media: the verify that runs
 * first updates the volume's media state.
 */
static BOOLEAN
Ext2CreateNeedsExclusiveVcb(
    IN PEXT2_VCB            Vcb,
    IN PIO_STACK_LOCATION   IrpSp,
    IN PEXT2_FCBVCB         Xcb
)
{
    if ((IrpSp->FileObject->FileName.Length == 0 &&
         IrpSp->FileObject->RelatedFileObject == NULL) ||
        (Xcb && Xcb->Identifier.Type == EXT2VCB)) {
        return TRUE;                    /* the volume itself */
    }
    if (IsFlagOn(IrpSp->Flags, SL_OPEN_TARGET_DIRECTORY)) {
        return TRUE;                    /* a rename or link is on its way */
    }
    if (IsFlagOn(Vcb->Flags, (VCB_REMOVABLE_MEDIA | VCB_FLOPPY_DISK))) {
        return TRUE;
    }
    return FALSE;
}

NTSTATUS
Ext2Create (IN PEXT2_IRP_CONTEXT IrpContext)
{
    PDEVICE_OBJECT      DeviceObject;
    PIRP                Irp;
    PIO_STACK_LOCATION  IrpSp;
    PEXT2_VCB           Vcb = 0;
    NTSTATUS            Status = STATUS_OBJECT_NAME_NOT_FOUND;
    PEXT2_FCBVCB        Xcb = NULL;
    BOOLEAN             PostIrp = FALSE;
    BOOLEAN             VcbResourceAcquired = FALSE;

    DeviceObject = IrpContext->DeviceObject;
    Irp = IrpContext->Irp;
    IrpSp = IoGetCurrentIrpStackLocation(Irp);

    Xcb = (PEXT2_FCBVCB) (IrpSp->FileObject->FsContext);

    if (IsExt2FsDevice(DeviceObject)) {

        DEBUG(DL_INF, ( "Ext2Create: Create on main device object.\n"));

        Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = FILE_OPENED;

        Ext2CompleteIrpContext(IrpContext, Status);

        return Status;
    }

    __try {

        Vcb = (PEXT2_VCB) DeviceObject->DeviceExtension;
        ASSERT(Vcb->Identifier.Type == EXT2VCB);
        IrpSp->FileObject->Vpb = Vcb->Vpb;

        if (!IsMounted(Vcb)) {
            if (IsFlagOn(Vcb->Flags, VCB_DEVICE_REMOVED)) {
                Status = STATUS_NO_SUCH_DEVICE;
            } else {
                Status = STATUS_VOLUME_DISMOUNTED;
            }
            __leave;
        }

        if (Ext2CreateNeedsExclusiveVcb(Vcb, IrpSp, Xcb)) {
            ExAcquireResourceExclusiveLite(&Vcb->MainResource, TRUE);
        } else {
            ExAcquireResourceSharedLite(&Vcb->MainResource, TRUE);
        }
        VcbResourceAcquired = TRUE;

        Ext2VerifyVcb(IrpContext, Vcb);

        if (!IsMounted(Vcb) || IsFlagOn(Vcb->Flags, VCB_DISMOUNT_PENDING)) {
            Status = STATUS_VOLUME_DISMOUNTED;
            __leave;
        }

        if (FlagOn(Vcb->Flags, VCB_VOLUME_LOCKED)) {
            Status = STATUS_ACCESS_DENIED;
            __leave;
        }

        if ( ((IrpSp->FileObject->FileName.Length == 0) &&
                (IrpSp->FileObject->RelatedFileObject == NULL)) ||
                (Xcb && Xcb->Identifier.Type == EXT2VCB)  ) {
            Status = Ext2CreateVolume(IrpContext, Vcb);
        } else {

            Status = Ext2CreateFile(IrpContext, Vcb, &PostIrp);
        }

    } __finally {

        if (VcbResourceAcquired) {
            ExReleaseResourceLite(&Vcb->MainResource);
        }

        if (!IrpContext->ExceptionInProgress && !PostIrp)  {
            if ( Status == STATUS_PENDING ||
                    Status == STATUS_CANT_WAIT) {
                Status = Ext2QueueRequest(IrpContext);
            } else {
                Ext2CompleteIrpContext(IrpContext, Status);
            }
        }
    }

    return Status;
}

NTSTATUS
Ext2SupersedeOrOverWriteFile(
    IN PEXT2_IRP_CONTEXT IrpContext,
    IN PFILE_OBJECT      FileObject,
    IN PEXT2_VCB         Vcb,
    IN PEXT2_FCB         Fcb,
    IN PLARGE_INTEGER    AllocationSize,
    IN ULONG             Disposition
)
{
    LARGE_INTEGER   CurrentTime;
    LARGE_INTEGER   Size;

    KeQuerySystemTime(&CurrentTime);

    Size.QuadPart = 0;
    if (!MmCanFileBeTruncated(&(Fcb->SectionObject), &(Size))) {
        return STATUS_USER_MAPPED_FILE;
    }

    /* purge all file cache and shrink cache windows size */
    CcPurgeCacheSection(&Fcb->SectionObject, NULL, 0, FALSE);
    Fcb->Header.AllocationSize.QuadPart =
        Fcb->Header.FileSize.QuadPart =
            Fcb->Header.ValidDataLength.QuadPart = 0;
    CcSetFileSizes(FileObject,
                   (PCC_FILE_SIZES)&Fcb->Header.AllocationSize);

    Size.QuadPart = CEILING_ALIGNED(ULONGLONG,
                                    (ULONGLONG)AllocationSize->QuadPart,
                                    (ULONGLONG)BLOCK_SIZE);

    if ((loff_t)Size.QuadPart > Fcb->Inode->i_size) {
        Ext2ExpandFile(IrpContext, Vcb, Fcb->Mcb, &Size);
    } else {
        Ext2TruncateFile(IrpContext, Vcb, Fcb->Mcb, &Size);
    }

    Fcb->Header.AllocationSize = Size;
    if (Fcb->Header.AllocationSize.QuadPart > 0) {
        SetLongFlag(Fcb->Flags, FCB_ALLOC_IN_CREATE);
        CcSetFileSizes(FileObject,
                       (PCC_FILE_SIZES)&Fcb->Header.AllocationSize );
    }

    /* remove all extent mappings */
    DEBUG(DL_EXT, ("Ext2SuperSede ...: %wZ\n", &Fcb->Mcb->FullName));
    Fcb->Inode->i_size = 0;

    if (Disposition == FILE_SUPERSEDE) {
        Ext2SetInodeTime(&CurrentTime, &Fcb->Inode->i_crtime, &Fcb->Inode->i_crtime_extra);
        Ext2SetInodeTime(&CurrentTime, &Fcb->Inode->i_ctime, &Fcb->Inode->i_ctime_extra);
    }
    Ext2SetInodeTime(&CurrentTime, &Fcb->Inode->i_mtime, &Fcb->Inode->i_mtime_extra);
    Ext2SetInodeTime(&CurrentTime, &Fcb->Inode->i_atime, &Fcb->Inode->i_atime_extra);
    Ext2SaveInode(IrpContext, Vcb, Fcb->Inode);

    /* See if we need to overwrite EA of the file */
    return Ext2OverwriteEa(IrpContext, Vcb, Fcb, &IrpContext->Irp->IoStatus);
}
