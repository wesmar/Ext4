/**
 * debug_irp.c - checked-build tracing of IRPs entering and leaving the driver.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"

#if EXT2_DEBUG

static const PCSTR IrpMjStrings[] = {
    "IRP_MJ_CREATE",
    "IRP_MJ_CREATE_NAMED_PIPE",
    "IRP_MJ_CLOSE",
    "IRP_MJ_READ",
    "IRP_MJ_WRITE",
    "IRP_MJ_QUERY_INFORMATION",
    "IRP_MJ_SET_INFORMATION",
    "IRP_MJ_QUERY_EA",
    "IRP_MJ_SET_EA",
    "IRP_MJ_FLUSH_BUFFERS",
    "IRP_MJ_QUERY_VOLUME_INFORMATION",
    "IRP_MJ_SET_VOLUME_INFORMATION",
    "IRP_MJ_DIRECTORY_CONTROL",
    "IRP_MJ_FILE_SYSTEM_CONTROL",
    "IRP_MJ_DEVICE_CONTROL",
    "IRP_MJ_INTERNAL_DEVICE_CONTROL",
    "IRP_MJ_SHUTDOWN",
    "IRP_MJ_LOCK_CONTROL",
    "IRP_MJ_CLEANUP",
    "IRP_MJ_CREATE_MAILSLOT",
    "IRP_MJ_QUERY_SECURITY",
    "IRP_MJ_SET_SECURITY",
    "IRP_MJ_POWER",
    "IRP_MJ_SYSTEM_CONTROL",
    "IRP_MJ_DEVICE_CHANGE",
    "IRP_MJ_QUERY_QUOTA",
    "IRP_MJ_SET_QUOTA",
    "IRP_MJ_PNP"
};

static const PCSTR FileInformationClassStrings[] = {
    "Unknown FileInformationClass 0",
    "FileDirectoryInformation",
    "FileFullDirectoryInformation",
    "FileBothDirectoryInformation",
    "FileBasicInformation",
    "FileStandardInformation",
    "FileInternalInformation",
    "FileEaInformation",
    "FileAccessInformation",
    "FileNameInformation",
    "FileRenameInformation",
    "FileLinkInformation",
    "FileNamesInformation",
    "FileDispositionInformation",
    "FilePositionInformation",
    "FileFullEaInformation",
    "FileModeInformation",
    "FileAlignmentInformation",
    "FileAllInformation",
    "FileAllocationInformation",
    "FileEndOfFileInformation",
    "FileAlternateNameInformation",
    "FileStreamInformation",
    "FilePipeInformation",
    "FilePipeLocalInformation",
    "FilePipeRemoteInformation",
    "FileMailslotQueryInformation",
    "FileMailslotSetInformation",
    "FileCompressionInformation",
    "FileObjectIdInformation",
    "FileCompletionInformation",
    "FileMoveClusterInformation",
    "FileQuotaInformation",
    "FileReparsePointInformation",
    "FileNetworkOpenInformation",
    "FileAttributeTagInformation",
    "FileTrackingInformation"
};

static const PCSTR FsInformationClassStrings[] = {
    "Unknown FsInformationClass 0",
    "FileFsVolumeInformation",
    "FileFsLabelInformation",
    "FileFsSizeInformation",
    "FileFsDeviceInformation",
    "FileFsAttributeInformation",
    "FileFsControlInformation",
    "FileFsFullSizeInformation",
    "FileFsObjectIdInformation"
};

VOID
Ext2DbgPrintCall (IN PDEVICE_OBJECT   DeviceObject,
                  IN PIRP             Irp )
{
    PIO_STACK_LOCATION      IoStackLocation;
    PFILE_OBJECT            FileObject;
    PWCHAR                  FileName;
    PEXT2_FCB               Fcb;
    FILE_INFORMATION_CLASS  FileInformationClass;
    FS_INFORMATION_CLASS    FsInformationClass;

    IoStackLocation = IoGetCurrentIrpStackLocation(Irp);

    FileObject = IoStackLocation->FileObject;

    FileName = L"Unknown";

    if (DeviceObject == Ext2Global->DiskdevObject) {

        FileName = DEVICE_NAME;

    } else if (DeviceObject == Ext2Global->CdromdevObject) {

        FileName = CDROM_NAME;

    } else if (FileObject && FileObject->FsContext) {

        Fcb = (PEXT2_FCB) FileObject->FsContext;

        if (Fcb->Identifier.Type == EXT2VCB)  {
            FileName = L"\\Volume";
        } else if (Fcb->Identifier.Type == EXT2FCB && Fcb->Mcb->FullName.Buffer) {
            FileName = Fcb->Mcb->FullName.Buffer;
        }
    }

    switch (IoStackLocation->MajorFunction) {

    case IRP_MJ_CREATE:

        FileName = NULL;

        if (DeviceObject == Ext2Global->DiskdevObject) {
            FileName = DEVICE_NAME;
        } else if (DeviceObject == Ext2Global->CdromdevObject) {
            FileName = CDROM_NAME;
        } else if (IoStackLocation->FileObject->FileName.Length == 0) {
            FileName = L"\\Volume";
        }

        if (FileName) {
            DEBUGNI(DL_FUN, ("%s %s %S\n",
                             Ext2GetCurrentProcessName(),
                             IrpMjStrings[IoStackLocation->MajorFunction],
                             FileName
                            ));
        } else if (IoStackLocation->FileObject->FileName.Buffer) {
            DEBUGNI(DL_FUN, ("%s %s %S\n",
                             Ext2GetCurrentProcessName(),
                             IrpMjStrings[IoStackLocation->MajorFunction],
                             IoStackLocation->FileObject->FileName.Buffer
                            ));
        } else {
            DEBUGNI(DL_FUN, ("%s %s %s\n",
                             Ext2GetCurrentProcessName(),
                             IrpMjStrings[IoStackLocation->MajorFunction],
                             "Unknown"
                            ));
        }

        break;

    case IRP_MJ_CLOSE:

        DEBUGNI(DL_FUN, ("%s %s %S\n",
                         Ext2GetCurrentProcessName(),
                         IrpMjStrings[IoStackLocation->MajorFunction],
                         FileName
                        ));

        break;

    case IRP_MJ_READ:

        if (IoStackLocation->MinorFunction & IRP_MN_COMPLETE) {
            DEBUGNI(DL_FUN, ("%s %s %S IRP_MN_COMPLETE\n",
                             Ext2GetCurrentProcessName(),
                             IrpMjStrings[IoStackLocation->MajorFunction],
                             FileName
                            ));
        } else {
            DEBUGNI(DL_FUN, ("%s %s %S Offset: %I64xh Length: %xh %s%s%s%s%s%s\n",
                             Ext2GetCurrentProcessName(),
                             IrpMjStrings[IoStackLocation->MajorFunction],
                             FileName,
                             IoStackLocation->Parameters.Read.ByteOffset.QuadPart,
                             IoStackLocation->Parameters.Read.Length,
                             (IoStackLocation->MinorFunction & IRP_MN_DPC ? "IRP_MN_DPC " : " "),
                             (IoStackLocation->MinorFunction & IRP_MN_MDL ? "IRP_MN_MDL " : " "),
                             (IoStackLocation->MinorFunction & IRP_MN_COMPRESSED ? "IRP_MN_COMPRESSED " : " "),
                             (Irp->Flags & IRP_PAGING_IO ? "IRP_PAGING_IO " : " "),
                             (Irp->Flags & IRP_NOCACHE ? "IRP_NOCACHE " : " "),
                             (FileObject->Flags & FO_SYNCHRONOUS_IO ? "FO_SYNCHRONOUS_IO " : " ")
                            ));
        }

        break;

    case IRP_MJ_WRITE:

        if (IoStackLocation->MinorFunction & IRP_MN_COMPLETE)  {
            DEBUGNI(DL_FUN, ("%s %s %S IRP_MN_COMPLETE\n",
                             Ext2GetCurrentProcessName(),
                             IrpMjStrings[IoStackLocation->MajorFunction],
                             FileName
                            ));
        } else {
            DEBUGNI(DL_FUN, ("%s %s %S Offset: %I64xh Length: %xh %s%s%s%s%s%s\n",
                             Ext2GetCurrentProcessName(),
                             IrpMjStrings[IoStackLocation->MajorFunction],
                             FileName,
                             IoStackLocation->Parameters.Read.ByteOffset.QuadPart,
                             IoStackLocation->Parameters.Read.Length,
                             (IoStackLocation->MinorFunction & IRP_MN_DPC ? "IRP_MN_DPC " : " "),
                             (IoStackLocation->MinorFunction & IRP_MN_MDL ? "IRP_MN_MDL " : " "),
                             (IoStackLocation->MinorFunction & IRP_MN_COMPRESSED ? "IRP_MN_COMPRESSED " : " "),
                             (Irp->Flags & IRP_PAGING_IO ? "IRP_PAGING_IO " : " "),
                             (Irp->Flags & IRP_NOCACHE ? "IRP_NOCACHE " : " "),
                             (FileObject->Flags & FO_SYNCHRONOUS_IO ? "FO_SYNCHRONOUS_IO " : " ")
                            ));
        }

        break;

    case IRP_MJ_QUERY_INFORMATION:

        FileInformationClass =
            IoStackLocation->Parameters.QueryFile.FileInformationClass;

        if (FileInformationClass <= FileMaximumInformation) {
            DEBUGNI(DL_FUN, ("%s %s %S %s\n",
                             Ext2GetCurrentProcessName(),
                             IrpMjStrings[IoStackLocation->MajorFunction],
                             FileName,
                             FileInformationClassStrings[FileInformationClass]
                            ));
        } else {
            DEBUGNI(DL_FUN, ("%s %s %S Unknown FileInformationClass %u\n",
                             Ext2GetCurrentProcessName(),
                             IrpMjStrings[IoStackLocation->MajorFunction],
                             FileName,
                             FileInformationClass
                            ));
        }

        break;

    case IRP_MJ_SET_INFORMATION:

        FileInformationClass =
            IoStackLocation->Parameters.SetFile.FileInformationClass;

        if (FileInformationClass <= FileMaximumInformation) {
            DEBUGNI(DL_FUN, ("%s %s %S %s\n",
                             Ext2GetCurrentProcessName(),
                             IrpMjStrings[IoStackLocation->MajorFunction],
                             FileName,
                             FileInformationClassStrings[FileInformationClass]
                            ));
        } else {
            DEBUGNI(DL_FUN, ("%s %s %S Unknown FileInformationClass %u\n",
                             Ext2GetCurrentProcessName(),
                             IrpMjStrings[IoStackLocation->MajorFunction],
                             FileName,
                             FileInformationClass
                            ));
        }

        break;

    case IRP_MJ_QUERY_VOLUME_INFORMATION:

        FsInformationClass =
            IoStackLocation->Parameters.QueryVolume.FsInformationClass;

        if (FsInformationClass <= FileFsMaximumInformation) {
            DEBUGNI(DL_FUN, ("%s %s %S %s\n",
                             Ext2GetCurrentProcessName(),
                             IrpMjStrings[IoStackLocation->MajorFunction],
                             FileName,
                             FsInformationClassStrings[FsInformationClass]
                            ));
        } else {
            DEBUGNI(DL_FUN, ("%s %s %S Unknown FsInformationClass %u\n",
                             Ext2GetCurrentProcessName(),
                             IrpMjStrings[IoStackLocation->MajorFunction],
                             FileName,
                             FsInformationClass
                            ));
        }

        break;

    case IRP_MJ_DIRECTORY_CONTROL:

        if (IoStackLocation->MinorFunction & IRP_MN_QUERY_DIRECTORY) {

            FileInformationClass =
                IoStackLocation->Parameters.QueryDirectory.FileInformationClass;

            if (FileInformationClass <= FileMaximumInformation) {
                DEBUGNI(DL_FUN, ("%s %s %S %s\n",
                                 Ext2GetCurrentProcessName(),
                                 IrpMjStrings[IoStackLocation->MajorFunction],
                                 FileName,
                                 FileInformationClassStrings[FileInformationClass]
                                ));

                if (
                    IoStackLocation->Parameters.QueryDirectory.FileName
                ) {
                    DEBUGNI(DL_FUN, ("%s FileName: %.*S FileIndex: %x %s%s%s\n",
                                     Ext2GetCurrentProcessName(),

                                     IoStackLocation->Parameters.QueryDirectory.FileName->Length / 2,
                                     IoStackLocation->Parameters.QueryDirectory.FileName->Buffer,
                                     IoStackLocation->Parameters.QueryDirectory.FileIndex,
                                     (IoStackLocation->Flags & SL_RESTART_SCAN ? "SL_RESTART_SCAN " : ""),
                                     (IoStackLocation->Flags & SL_RETURN_SINGLE_ENTRY ? "SL_RETURN_SINGLE_ENTRY " : ""),
                                     ((IoStackLocation->Flags & SL_INDEX_SPECIFIED) ? "SL_INDEX_SPECIFIED " : "")
                                    ));

                } else {
                    DEBUGNI(DL_FUN, ("%s FileName: FileIndex: %#x %s%s%s\n",
                                     Ext2GetCurrentProcessName(),
                                     IoStackLocation->Parameters.QueryDirectory.FileIndex,
                                     (IoStackLocation->Flags & SL_RESTART_SCAN ? "SL_RESTART_SCAN " : ""),
                                     (IoStackLocation->Flags & SL_RETURN_SINGLE_ENTRY ? "SL_RETURN_SINGLE_ENTRY " : ""),
                                     (IoStackLocation->Flags & SL_INDEX_SPECIFIED ? "SL_INDEX_SPECIFIED " : "")
                                    ));
                }
            } else {
                DEBUGNI(DL_FUN, ("%s %s %S Unknown FileInformationClass %u\n",
                                 Ext2GetCurrentProcessName(),
                                 IrpMjStrings[IoStackLocation->MajorFunction],
                                 FileName,
                                 FileInformationClass
                                ));
            }
        } else if (IoStackLocation->MinorFunction & IRP_MN_NOTIFY_CHANGE_DIRECTORY) {
            DEBUGNI(DL_FUN, ("%s %s %S IRP_MN_NOTIFY_CHANGE_DIRECTORY\n",
                             Ext2GetCurrentProcessName(),
                             IrpMjStrings[IoStackLocation->MajorFunction],
                             FileName
                            ));
        } else {
            DEBUGNI(DL_FUN, ("%s %s %S Unknown minor function %#x\n",
                             Ext2GetCurrentProcessName(),
                             IrpMjStrings[IoStackLocation->MajorFunction],
                             FileName,
                             IoStackLocation->MinorFunction
                            ));
        }

        break;

    case IRP_MJ_FILE_SYSTEM_CONTROL:

        if (IoStackLocation->MinorFunction == IRP_MN_USER_FS_REQUEST) {
            DEBUGNI(DL_FUN, ( "%s %s %S IRP_MN_USER_FS_REQUEST FsControlCode: %#x\n",
                              Ext2GetCurrentProcessName(),
                              IrpMjStrings[IoStackLocation->MajorFunction],
                              FileName,
                              IoStackLocation->Parameters.FileSystemControl.FsControlCode
                            ));
        } else if (IoStackLocation->MinorFunction == IRP_MN_MOUNT_VOLUME) {
            DEBUGNI(DL_FUN, ("%s %s %S IRP_MN_MOUNT_VOLUME DeviceObject: %#x\n",
                             Ext2GetCurrentProcessName(),
                             IrpMjStrings[IoStackLocation->MajorFunction],
                             FileName,
                             IoStackLocation->Parameters.MountVolume.DeviceObject
                            ));
        } else if (IoStackLocation->MinorFunction == IRP_MN_VERIFY_VOLUME) {
            DEBUGNI(DL_FUN, ("%s %s %S IRP_MN_VERIFY_VOLUME DeviceObject: %#x\n",
                             Ext2GetCurrentProcessName(),
                             IrpMjStrings[IoStackLocation->MajorFunction],
                             FileName,
                             IoStackLocation->Parameters.VerifyVolume.DeviceObject
                            ));
        } else if (IoStackLocation->MinorFunction == IRP_MN_LOAD_FILE_SYSTEM) {
            DEBUGNI(DL_FUN, ("%s %s %S IRP_MN_LOAD_FILE_SYSTEM\n",
                             Ext2GetCurrentProcessName(),
                             IrpMjStrings[IoStackLocation->MajorFunction],
                             FileName
                            ));
        }
        else if (IoStackLocation->MinorFunction == IRP_MN_KERNEL_CALL) {
            DEBUGNI(DL_FUN, ("%s %s %S IRP_MN_KERNEL_CALL\n",
                             Ext2GetCurrentProcessName(),
                             IrpMjStrings[IoStackLocation->MajorFunction],
                             FileName
                            ));
        }
        else {
            DEBUGNI(DL_FUN, ("%s %s %S Unknown minor function %#x\n",
                             Ext2GetCurrentProcessName(),
                             IrpMjStrings[IoStackLocation->MajorFunction],
                             FileName,
                             IoStackLocation->MinorFunction
                            ));
        }

        break;

    case IRP_MJ_DEVICE_CONTROL:

        DEBUGNI(DL_FUN, ("%s %s %S IoControlCode: %#x\n",
                         Ext2GetCurrentProcessName(),
                         IrpMjStrings[IoStackLocation->MajorFunction],
                         FileName,
                         IoStackLocation->Parameters.DeviceIoControl.IoControlCode
                        ));

        break;

    case IRP_MJ_LOCK_CONTROL:

        if (IoStackLocation->MinorFunction & IRP_MN_LOCK) {
            DEBUGNI(DL_FUN, ("%s %s %S IRP_MN_LOCK Offset: %I64xh Length: %I64xh Key: %u %s%s\n",
                             Ext2GetCurrentProcessName(),
                             IrpMjStrings[IoStackLocation->MajorFunction],
                             FileName,
                             IoStackLocation->Parameters.LockControl.ByteOffset.QuadPart,
                             IoStackLocation->Parameters.LockControl.Length->QuadPart,
                             IoStackLocation->Parameters.LockControl.Key,
                             (IoStackLocation->Flags & SL_FAIL_IMMEDIATELY ? "SL_FAIL_IMMEDIATELY " : ""),
                             (IoStackLocation->Flags & SL_EXCLUSIVE_LOCK ? "SL_EXCLUSIVE_LOCK " : "")
                            ));
        } else if (IoStackLocation->MinorFunction & IRP_MN_UNLOCK_SINGLE) {
            DEBUGNI(DL_FUN, ("%s %s %S IRP_MN_UNLOCK_SINGLE Offset: %I64xh Length: %I64xh Key: %u\n",
                             Ext2GetCurrentProcessName(),
                             IrpMjStrings[IoStackLocation->MajorFunction],
                             FileName,
                             IoStackLocation->Parameters.LockControl.ByteOffset.QuadPart,
                             IoStackLocation->Parameters.LockControl.Length->QuadPart,
                             IoStackLocation->Parameters.LockControl.Key
                            ));
        } else if (IoStackLocation->MinorFunction & IRP_MN_UNLOCK_ALL) {
            DEBUGNI(DL_FUN, ("%s %s %S IRP_MN_UNLOCK_ALL\n",
                             Ext2GetCurrentProcessName(),
                             IrpMjStrings[IoStackLocation->MajorFunction],
                             FileName
                            ));
        } else if (IoStackLocation->MinorFunction & IRP_MN_UNLOCK_ALL_BY_KEY) {
            DEBUGNI(DL_FUN, ("%s %s %S IRP_MN_UNLOCK_ALL_BY_KEY Key: %u\n",
                             Ext2GetCurrentProcessName(),
                             IrpMjStrings[IoStackLocation->MajorFunction],
                             FileName,
                             IoStackLocation->Parameters.LockControl.Key
                            ));
        } else {
            DEBUGNI(DL_FUN, ("%s %s %S Unknown minor function %#x\n",
                             Ext2GetCurrentProcessName(),
                             IrpMjStrings[IoStackLocation->MajorFunction],
                             FileName,
                             IoStackLocation->MinorFunction
                            ));
        }

        break;

    case IRP_MJ_CLEANUP:

        DEBUGNI(DL_FUN, ("%s %s %S\n",
                         Ext2GetCurrentProcessName(),
                         IrpMjStrings[IoStackLocation->MajorFunction],
                         FileName
                        ));

        break;

    case IRP_MJ_SHUTDOWN:

        DEBUGNI(DL_FUN, ("%s %s %S\n",
                         Ext2GetCurrentProcessName(),
                         IrpMjStrings[IoStackLocation->MajorFunction],
                         FileName
                        ));

        break;

    case IRP_MJ_PNP:

        DEBUGNI(DL_FUN, ( "%s %s %S\n",
                          Ext2GetCurrentProcessName(),
                          IrpMjStrings[IoStackLocation->MajorFunction],
                          FileName
                        ));
        break;

    default:

        DEBUGNI(DL_FUN, ("%s %s %S\n",
                         Ext2GetCurrentProcessName(),
                         IrpMjStrings[IoStackLocation->MajorFunction],
                         FileName
                        ));
    }
}

VOID
Ext2DbgPrintComplete (IN PIRP Irp, IN BOOLEAN bPrint)
{
    PIO_STACK_LOCATION IoStackLocation;

    if (!Irp)
        return;

    if (Irp->IoStatus.Status != STATUS_SUCCESS) {

        IoStackLocation = IoGetCurrentIrpStackLocation(Irp);

        if (bPrint) {
            DEBUGNI(DL_FUN, ("%s %s Status: %s (%#x).\n",
                             Ext2GetCurrentProcessName(),
                             IrpMjStrings[IoStackLocation->MajorFunction],
                             Ext2NtStatusToString(Irp->IoStatus.Status),
                             Irp->IoStatus.Status
                            ));
        }
    }
}

#endif /* EXT2_DEBUG */
