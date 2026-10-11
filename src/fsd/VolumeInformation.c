/**
 * VolumeInformation.c - IRP_MJ_QUERY/SET_VOLUME_INFORMATION.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2QueryVolumeInformation)
#pragma alloc_text(PAGE, Ext2SetVolumeInformation)
#endif

/*
 * A structure that ends in a name: Header bytes, then the name, cut to what
 * fits - with STATUS_BUFFER_OVERFLOW, and Information covering what was
 * filled, as NTFS answers (a cut label used to be left out entirely).
 */
static NTSTATUS
Ext2QueryFsName(OUT PVOID Name, IN PCWSTR Source, IN ULONG Bytes,
                IN ULONG Header, IN ULONG Length, OUT PULONG_PTR Information)
{
    ULONG Copied = min(Bytes, Length - Header);

    RtlCopyMemory(Name, Source, Copied);
    *Information = Header + Copied;
    return Copied < Bytes ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}

/* the allocation units: blocks, of whole sectors */
static VOID
Ext2QueryFsUnits(IN PEXT2_VCB Vcb, OUT PULONG SectorsPerUnit, OUT PULONG BytesPerSector)
{
    *BytesPerSector = Vcb->DiskGeometry.BytesPerSector;
    *SectorsPerUnit = Vcb->BlockSize / Vcb->DiskGeometry.BytesPerSector;
}

static NTSTATUS
Ext2QueryFsClass(IN PEXT2_VCB Vcb, IN FS_INFORMATION_CLASS Class, OUT PVOID Buffer,
                 IN ULONG Length, OUT PULONG_PTR Information)
{
    switch (Class) {

    case FileFsVolumeInformation:
    {
        PFILE_FS_VOLUME_INFORMATION FFVI = (PFILE_FS_VOLUME_INFORMATION)Buffer;
        const ULONG Header = FIELD_OFFSET(FILE_FS_VOLUME_INFORMATION, VolumeLabel);

        if (Length < Header) {
            return STATUS_INFO_LENGTH_MISMATCH;
        }
        FFVI->VolumeCreationTime = Ext2GetSuperTime(Vcb->SuperBlock, s_mkfs_time);
        FFVI->VolumeSerialNumber = Vcb->Vpb->SerialNumber;
        FFVI->VolumeLabelLength = Vcb->Vpb->VolumeLabelLength;
        FFVI->SupportsObjects = FALSE;      /* no object ids */
        return Ext2QueryFsName(FFVI->VolumeLabel, Vcb->Vpb->VolumeLabel,
                               Vcb->Vpb->VolumeLabelLength, Header, Length, Information);
    }

    case FileFsSizeInformation:
    {
        PFILE_FS_SIZE_INFORMATION FFSI = (PFILE_FS_SIZE_INFORMATION)Buffer;

        if (Length < sizeof(FILE_FS_SIZE_INFORMATION)) {
            return STATUS_INFO_LENGTH_MISMATCH;
        }
        FFSI->TotalAllocationUnits.QuadPart = ext3_blocks_count(Vcb->SuperBlock);
        FFSI->AvailableAllocationUnits.QuadPart = Ext2FreeBlocks(Vcb);
        Ext2QueryFsUnits(Vcb, &FFSI->SectorsPerAllocationUnit, &FFSI->BytesPerSector);
        *Information = sizeof(FILE_FS_SIZE_INFORMATION);
        return STATUS_SUCCESS;
    }

    case FileFsFullSizeInformation:
    {
        PFILE_FS_FULL_SIZE_INFORMATION FFFSI = (PFILE_FS_FULL_SIZE_INFORMATION)Buffer;

        if (Length < sizeof(FILE_FS_FULL_SIZE_INFORMATION)) {
            return STATUS_INFO_LENGTH_MISMATCH;
        }
        FFFSI->TotalAllocationUnits.QuadPart = ext3_blocks_count(Vcb->SuperBlock);
        /* the blocks reserved for root (s_r_blocks_count) are not held back
           from Windows, which has no root: every free block is the caller's */
        FFFSI->CallerAvailableAllocationUnits.QuadPart = Ext2FreeBlocks(Vcb);
        FFFSI->ActualAvailableAllocationUnits.QuadPart = Ext2FreeBlocks(Vcb);
        Ext2QueryFsUnits(Vcb, &FFFSI->SectorsPerAllocationUnit, &FFFSI->BytesPerSector);
        *Information = sizeof(FILE_FS_FULL_SIZE_INFORMATION);
        return STATUS_SUCCESS;
    }

    case FileFsDeviceInformation:
    {
        PFILE_FS_DEVICE_INFORMATION FFDI = (PFILE_FS_DEVICE_INFORMATION)Buffer;

        if (Length < sizeof(FILE_FS_DEVICE_INFORMATION)) {
            return STATUS_INFO_LENGTH_MISMATCH;
        }
        FFDI->DeviceType = Vcb->TargetDeviceObject->DeviceType;
        FFDI->Characteristics = Vcb->TargetDeviceObject->Characteristics;
        if (IsVcbReadOnly(Vcb)) {
            SetFlag(FFDI->Characteristics, FILE_READ_ONLY_DEVICE);
        }
        *Information = sizeof(FILE_FS_DEVICE_INFORMATION);
        return STATUS_SUCCESS;
    }

    case FileFsAttributeInformation:
    {
        PFILE_FS_ATTRIBUTE_INFORMATION FFAI = (PFILE_FS_ATTRIBUTE_INFORMATION)Buffer;
        const ULONG Header = FIELD_OFFSET(FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName);
        PCWSTR      Name;

        if (Length < Header) {
            return STATUS_INFO_LENGTH_MISMATCH;
        }
        FFAI->FileSystemAttributes = FILE_SUPPORTS_HARD_LINKS |
            FILE_CASE_SENSITIVE_SEARCH | FILE_CASE_PRESERVED_NAMES |
            FILE_SUPPORTS_REPARSE_POINTS | FILE_SUPPORTS_EXTENDED_ATTRIBUTES;
        if (IsVcbReadOnly(Vcb)) {
            SetFlag(FFAI->FileSystemAttributes, FILE_READ_ONLY_VOLUME);
        }
        FFAI->MaximumComponentNameLength = EXT2_NAME_LEN;

        Name = IsFlagOn(Vcb->SuperBlock->s_feature_incompat, EXT4_FEATURE_INCOMPAT_EXTENTS) ? L"EXT4" :
               Vcb->IsExt3fs ? L"EXT3" : L"EXT2";
        FFAI->FileSystemNameLength = (ULONG)(wcslen(Name) * sizeof(WCHAR));
        return Ext2QueryFsName(FFAI->FileSystemName, Name, FFAI->FileSystemNameLength,
                               Header, Length, Information);
    }

    default:
        return STATUS_INVALID_INFO_CLASS;
    }
}

NTSTATUS
Ext2QueryVolumeInformation (IN PEXT2_IRP_CONTEXT IrpContext)
{
    PDEVICE_OBJECT          DeviceObject;
    PEXT2_VCB               Vcb = NULL;
    PIRP                    Irp;
    PIO_STACK_LOCATION      IrpSp;
    NTSTATUS                Status = STATUS_UNSUCCESSFUL;
    BOOLEAN                 VcbResourceAcquired = FALSE;

    ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
           (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

    __try {

        DeviceObject = IrpContext->DeviceObject;

        /* This request is not allowed on the main device object */
        if (IsExt2FsDevice(DeviceObject)) {
            Status = STATUS_INVALID_DEVICE_REQUEST;
            __leave;
        }

        Vcb = (PEXT2_VCB) DeviceObject->DeviceExtension;
        ASSERT((Vcb->Identifier.Type == EXT2VCB) &&
               (Vcb->Identifier.Size == sizeof(EXT2_VCB)));
        if (!IsMounted(Vcb)) {
            Status = STATUS_VOLUME_DISMOUNTED;
            __leave;
        }

        if (!ExAcquireResourceSharedLite(&Vcb->MainResource,
                                         IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT))) {
            Status = STATUS_PENDING;
            __leave;
        }
        VcbResourceAcquired = TRUE;

        Irp = IrpContext->Irp;
        IrpSp = IoGetCurrentIrpStackLocation(Irp);
        RtlZeroMemory(Irp->AssociatedIrp.SystemBuffer, IrpSp->Parameters.QueryVolume.Length);

        Status = Ext2QueryFsClass(Vcb, IrpSp->Parameters.QueryVolume.FsInformationClass,
                                  Irp->AssociatedIrp.SystemBuffer,
                                  IrpSp->Parameters.QueryVolume.Length,
                                  &Irp->IoStatus.Information);

    } __finally {

        if (VcbResourceAcquired) {
            ExReleaseResourceLite(&Vcb->MainResource);
        }

        if (!IrpContext->ExceptionInProgress) {
            if (Status == STATUS_PENDING) {
                Status = Ext2QueueRequest(IrpContext);
            } else {
                Ext2CompleteIrpContext(IrpContext, Status);
            }
        }
    }

    return Status;
}

/*
 * A new label: in the superblock (s_volume_name, the volume's code page)
 * and in the VPB. Converted first: a label the superblock cannot hold
 * changes nothing (it used to change the VPB all the same). The
 * superblock's copy changes under its lock, as every field its checksum
 * covers does - a concurrent write of it computed the checksum over a half
 * written label.
 */
static NTSTATUS
Ext2SetVolumeLabel(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb,
                   IN PFILE_FS_LABEL_INFORMATION Info, IN ULONG Length)
{
    /* room for the terminator the conversion insists on writing: a label
       of all 16 bytes is legal, unterminated, on disk */
    UCHAR           Label[EXT2_LABEL_BYTES + 1] = { 0 };
    UNICODE_STRING  Unicode;
    OEM_STRING      Oem;
    BOOLEAN         Saved;

    const ULONG     Header = FIELD_OFFSET(FILE_FS_LABEL_INFORMATION, VolumeLabel);

    if (Length < Header || Info->VolumeLabelLength > Length - Header ||
        Info->VolumeLabelLength > sizeof(Vcb->Vpb->VolumeLabel) ||
        (Info->VolumeLabelLength & (sizeof(WCHAR) - 1))) {
        return STATUS_INVALID_VOLUME_LABEL;
    }

    Unicode.Buffer = Info->VolumeLabel;
    Unicode.Length = Unicode.MaximumLength = (USHORT)Info->VolumeLabelLength;
    Oem.Buffer = (PCHAR)Label;
    Oem.Length = 0;
    Oem.MaximumLength = sizeof(Label);
    if (Unicode.Length != 0 &&
        (!NT_SUCCESS(Ext2UnicodeToOEM(Vcb, &Oem, &Unicode)) || Oem.Length > EXT2_LABEL_BYTES)) {
        return STATUS_INVALID_VOLUME_LABEL;
    }

    Ext2LockSuper(Vcb);
    RtlCopyMemory(Vcb->SuperBlock->s_volume_name, Label, EXT2_LABEL_BYTES);
    Saved = Ext2SaveSuper(IrpContext, Vcb);
    Ext2UnlockSuper(Vcb);
    if (!Saved) {
        return STATUS_UNEXPECTED_IO_ERROR;
    }

    RtlCopyMemory(Vcb->Vpb->VolumeLabel, Info->VolumeLabel, Info->VolumeLabelLength);
    Vcb->Vpb->VolumeLabelLength = (USHORT)Info->VolumeLabelLength;
    return STATUS_SUCCESS;
}

NTSTATUS
Ext2SetVolumeInformation (IN PEXT2_IRP_CONTEXT IrpContext)
{
    PDEVICE_OBJECT          DeviceObject;
    NTSTATUS                Status = STATUS_UNSUCCESSFUL;
    PEXT2_VCB               Vcb = NULL;
    PIRP                    Irp;
    PIO_STACK_LOCATION      IrpSp;
    BOOLEAN                 VcbResourceAcquired = FALSE;

    ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
           (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

    __try {

        DeviceObject = IrpContext->DeviceObject;

        /* This request is not allowed on the main device object */
        if (IsExt2FsDevice(DeviceObject)) {
            Status = STATUS_INVALID_DEVICE_REQUEST;
            __leave;
        }

        Vcb = (PEXT2_VCB) DeviceObject->DeviceExtension;
        ASSERT((Vcb->Identifier.Type == EXT2VCB) &&
               (Vcb->Identifier.Size == sizeof(EXT2_VCB)));
        if (!IsMounted(Vcb) || IsFlagOn(Vcb->Flags, VCB_DISMOUNT_PENDING)) {
            Status = STATUS_VOLUME_DISMOUNTED;
            __leave;
        }
        if (IsVcbReadOnly(Vcb)) {
            Status = STATUS_MEDIA_WRITE_PROTECTED;
            __leave;
        }

        ExAcquireResourceExclusiveLite(&Vcb->MainResource, TRUE);
        VcbResourceAcquired = TRUE;

        Ext2VerifyVcb(IrpContext, Vcb);

        Irp = IrpContext->Irp;
        IrpSp = IoGetCurrentIrpStackLocation(Irp);
        Irp->IoStatus.Information = 0;

        /* the set parameters have the query's layout */
        switch (IrpSp->Parameters.QueryVolume.FsInformationClass) {

        case FileFsLabelInformation:
            Status = Ext2SetVolumeLabel(IrpContext, Vcb,
                                        (PFILE_FS_LABEL_INFORMATION)Irp->AssociatedIrp.SystemBuffer,
                                        IrpSp->Parameters.QueryVolume.Length);
            break;

        default:
            Status = STATUS_INVALID_INFO_CLASS;
        }

    } __finally {

        if (VcbResourceAcquired) {
            ExReleaseResourceLite(&Vcb->MainResource);
        }

        if (!IrpContext->ExceptionInProgress) {
            if (Status == STATUS_PENDING) {
                Status = Ext2QueueRequest(IrpContext);
            } else {
                Ext2CompleteIrpContext(IrpContext, Status);
            }
        }
    }

    return Status;
}
