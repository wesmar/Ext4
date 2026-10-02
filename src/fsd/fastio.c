/**
 * fastio.c - fast I/O entry points: read, write and information queries.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2FastIoRead)
#pragma alloc_text(PAGE, Ext2FastIoWrite)
#pragma alloc_text(PAGE, Ext2FastIoCheckIfPossible)
#pragma alloc_text(PAGE, Ext2FastIoQueryBasicInfo)
#pragma alloc_text(PAGE, Ext2FastIoQueryStandardInfo)
#pragma alloc_text(PAGE, Ext2FastIoQueryNetworkOpenInfo)
#endif

FAST_IO_POSSIBLE
Ext2IsFastIoPossible(
    IN PEXT2_FCB Fcb
)
{
    FAST_IO_POSSIBLE IsPossible = FastIoIsNotPossible;

    if (!Fcb || !FsRtlOplockIsFastIoPossible(&Fcb->Oplock))
        return IsPossible;

    IsPossible = FastIoIsQuestionable;

    if (!FsRtlAreThereCurrentFileLocks(&Fcb->FileLockAnchor)) {
        if (!IsVcbReadOnly(Fcb->Vcb) && !FlagOn(Fcb->Vcb->Flags, VCB_VOLUME_LOCKED)) {
            IsPossible = FastIoIsPossible;
        }
    }

    return IsPossible;
}

BOOLEAN
Ext2FastIoCheckIfPossible (
    IN PFILE_OBJECT         FileObject,
    IN PLARGE_INTEGER       FileOffset,
    IN ULONG                Length,
    IN BOOLEAN              Wait,
    IN ULONG                LockKey,
    IN BOOLEAN              CheckForReadOperation,
    OUT PIO_STATUS_BLOCK    IoStatus,
    IN PDEVICE_OBJECT       DeviceObject
)
{
    UNREFERENCED_PARAMETER(Wait);
    UNREFERENCED_PARAMETER(IoStatus);
    BOOLEAN          bPossible = FastIoIsNotPossible;
    PEXT2_FCB        Fcb;
    PEXT2_CCB        Ccb;
    LARGE_INTEGER    lLength;

    lLength.QuadPart = Length;

    __try {

        FsRtlEnterFileSystem();

        __try {

            if (IsExt2FsDevice(DeviceObject)) {
                __leave;
            }

            Fcb = (PEXT2_FCB) FileObject->FsContext;
            if (Fcb == NULL || Fcb->Identifier.Type == EXT2VCB) {
                __leave;
            }

            ASSERT((Fcb->Identifier.Type == EXT2FCB) &&
                   (Fcb->Identifier.Size == sizeof(EXT2_FCB)));

            /* do nothing if target file was deleted */
            if (FlagOn(Fcb->Flags, FCB_DELETE_PENDING)) {
                __leave;
            }

            if (IsDirectory(Fcb)) {
                __leave;
            }

            Ccb = (PEXT2_CCB) FileObject->FsContext2;
            if (Ccb == NULL) {
                __leave;
            }

            if (CheckForReadOperation) {

                bPossible = FsRtlFastCheckLockForRead(
                                &Fcb->FileLockAnchor,
                                FileOffset,
                                &lLength,
                                LockKey,
                                FileObject,
                                PsGetCurrentProcess());

            } else {

                if (!IsVcbReadOnly(Fcb->Vcb)) {
                    bPossible = FsRtlFastCheckLockForWrite(
                                    &Fcb->FileLockAnchor,
                                    FileOffset,
                                    &lLength,
                                    LockKey,
                                    FileObject,
                                    PsGetCurrentProcess());
                }
            }

#if EXT2_DEBUG
            DEBUG(DL_INF, ("Ext2FastIIOCheckPossible: %s %s %wZ\n",
                           Ext2GetCurrentProcessName(),
                           "FASTIO_CHECK_IF_POSSIBLE",
                           &Fcb->Mcb->FullName
                          ));

            DEBUG(DL_INF, (
                      "Ext2FastIIOCheckPossible: Offset: %I64xg Length: %xh Key: %u %s %s\n",
                      FileOffset->QuadPart,
                      Length,
                      LockKey,
                      (CheckForReadOperation ? "CheckForReadOperation:" :
                       "CheckForWriteOperation:"),
                      (bPossible ? "Succeeded" : "Failed")));
#endif

        } __except (EXCEPTION_EXECUTE_HANDLER) {
            bPossible = FastIoIsNotPossible;
        }

    } __finally {

        FsRtlExitFileSystem();
    }

    return bPossible;
}

BOOLEAN
Ext2FastIoRead (IN PFILE_OBJECT         FileObject,
                IN PLARGE_INTEGER       FileOffset,
                IN ULONG                Length,
                IN BOOLEAN              Wait,
                IN ULONG                LockKey,
                OUT PVOID               Buffer,
                OUT PIO_STATUS_BLOCK    IoStatus,
                IN PDEVICE_OBJECT       DeviceObject)
{
    PEXT2_FCB    Fcb;
    BOOLEAN      Status = FALSE;

    Fcb = (PEXT2_FCB) FileObject->FsContext;
    if (Fcb == NULL) {
        return FALSE;
    }

    ASSERT((Fcb->Identifier.Type == EXT2FCB) &&
           (Fcb->Identifier.Size == sizeof(EXT2_FCB)));

    Status = FsRtlCopyRead (
                 FileObject, FileOffset, Length, Wait,
                 LockKey, Buffer, IoStatus, DeviceObject);

    DEBUG(DL_IO, ("Ext2FastIoRead: %wZ Offset: %I64xh Length: %xh Key: %u Status: %d\n",
                  &Fcb->Mcb->ShortName, FileOffset->QuadPart, Length, LockKey, Status));

    return Status;
}

BOOLEAN
Ext2FastIoWrite (
    IN PFILE_OBJECT         FileObject,
    IN PLARGE_INTEGER       FileOffset,
    IN ULONG                Length,
    IN BOOLEAN              Wait,
    IN ULONG                LockKey,
    OUT PVOID               Buffer,
    OUT PIO_STATUS_BLOCK    IoStatus,
    IN PDEVICE_OBJECT       DeviceObject)
{
    PEXT2_FCB   Fcb = NULL;
    BOOLEAN     Status = FALSE;
    BOOLEAN     Locked = FALSE;

    Fcb = (PEXT2_FCB) FileObject->FsContext;
    if (Fcb == NULL)
        return FALSE;

    __try {

        FsRtlEnterFileSystem();

        ASSERT((Fcb->Identifier.Type == EXT2FCB) &&
               (Fcb->Identifier.Size == sizeof(EXT2_FCB)));

        if (IsVcbReadOnly(Fcb->Vcb)) {
            __leave;
        }

        if (!ExAcquireResourceSharedLite(Fcb->Header.Resource, Wait)) {
            __leave;
        }
        Locked = TRUE;

        if (IsWritingToEof(*FileOffset) ||
            Fcb->Header.ValidDataLength.QuadPart < FileOffset->QuadPart + Length ||
            Fcb->Header.FileSize.QuadPart < FileOffset->QuadPart + Length ) {
            Status = FALSE;
            __leave;
        }

        if (Locked) {
            ExReleaseResourceLite(Fcb->Header.Resource);
            Locked = FALSE;
        }

        Status = FsRtlCopyWrite(FileObject, FileOffset, Length, Wait,
                                LockKey, Buffer, IoStatus, DeviceObject);
        if (Status) {
            if (IoStatus)
                Length = (ULONG)IoStatus->Information;
        }

    } __finally {

        if (Locked) {
            ExReleaseResourceLite(Fcb->Header.Resource);
        }

        FsRtlExitFileSystem();
    }

    DEBUG(DL_IO, ("Ext2FastIoWrite: %wZ Offset: %I64xh Length: %xh Key: %xh Status=%d\n",
                  &Fcb->Mcb->ShortName,  FileOffset->QuadPart, Length, LockKey, Status));

    return Status;
}

BOOLEAN
Ext2FastIoQueryBasicInfo (
    IN PFILE_OBJECT             FileObject,
    IN BOOLEAN                  Wait,
    OUT PFILE_BASIC_INFORMATION Buffer,
    OUT PIO_STATUS_BLOCK        IoStatus,
    IN PDEVICE_OBJECT           DeviceObject)
{
    PEXT2_FCB   Fcb = NULL;
    PEXT2_CCB   Ccb = NULL;
    PEXT2_MCB   Mcb = NULL;
    BOOLEAN     Status = FALSE;
    BOOLEAN     FcbMainResourceAcquired = FALSE;

    __try {

        FsRtlEnterFileSystem();

        __try {

            if (IsExt2FsDevice(DeviceObject)) {
                IoStatus->Status = STATUS_INVALID_DEVICE_REQUEST;
                __leave;
            }

            Fcb = (PEXT2_FCB) FileObject->FsContext;
            if (Fcb == NULL || Fcb->Identifier.Type == EXT2VCB) {
                IoStatus->Status = STATUS_INVALID_PARAMETER;
                __leave;
            }
            Ccb = (PEXT2_CCB) FileObject->FsContext2;
            Mcb = Fcb->Mcb;
            ASSERT((Fcb->Identifier.Type == EXT2FCB) &&
                   (Fcb->Identifier.Size == sizeof(EXT2_FCB)));
#if EXT2_DEBUG
            DEBUG(DL_INF, (
                      "Ext2FastIoQueryBasicInfo: %s %s %wZ\n",
                      Ext2GetCurrentProcessName(),
                      "FASTIO_QUERY_BASIC_INFO",
                      &Fcb->Mcb->FullName
                  ));
#endif
            if (!IsFlagOn(Fcb->Flags, FCB_PAGE_FILE)) {
                if (!ExAcquireResourceSharedLite(
                            &Fcb->MainResource,
                            Wait)) {
                    __leave;
                }
                FcbMainResourceAcquired = TRUE;
            }

            RtlZeroMemory(Buffer, sizeof(FILE_BASIC_INFORMATION));

            /*
            typedef struct _FILE_BASIC_INFORMATION {
            LARGE_INTEGER   CreationTime;
            LARGE_INTEGER   LastAccessTime;
            LARGE_INTEGER   LastWriteTime;
            LARGE_INTEGER   ChangeTime;
            ULONG           FileAttributes;
            } FILE_BASIC_INFORMATION, *PFILE_BASIC_INFORMATION;
            */

            Buffer->CreationTime = Mcb->Icb->CreationTime;
            Buffer->LastAccessTime = Mcb->Icb->LastAccessTime;
            Buffer->LastWriteTime = Mcb->Icb->LastWriteTime;
            Buffer->ChangeTime = Mcb->Icb->ChangeTime;
            Buffer->FileAttributes = Mcb->FileAttr;
            if (Buffer->FileAttributes == 0) {
                Buffer->FileAttributes = FILE_ATTRIBUTE_NORMAL;
            }

            IoStatus->Information = sizeof(FILE_BASIC_INFORMATION);
            IoStatus->Status = STATUS_SUCCESS;

            Status =  TRUE;

        } __except (EXCEPTION_EXECUTE_HANDLER) {
            IoStatus->Status = GetExceptionCode();
        }

    } __finally {

        if (FcbMainResourceAcquired) {
            ExReleaseResourceLite(&Fcb->MainResource);
        }

        FsRtlExitFileSystem();
    }

#if EXT2_DEBUG

    if (Status == FALSE) {

        DEBUG(DL_ERR, ("Ext2FastIoQueryBasicInfo: %s %s Status: FALSE ***\n",
                       Ext2GetCurrentProcessName(),
                       "FASTIO_QUERY_BASIC_INFO"));

    } else if (IoStatus->Status != STATUS_SUCCESS) {

        DEBUG(DL_ERR, (
                  "Ext2FastIoQueryBasicInfo: %s %s Status: %#x ***\n",
                  Ext2FastIoQueryBasicInfo,
                  "FASTIO_QUERY_BASIC_INFO",
                  IoStatus->Status
              ));
    }
#endif

    return Status;
}

BOOLEAN
Ext2FastIoQueryStandardInfo (
    IN PFILE_OBJECT                 FileObject,
    IN BOOLEAN                      Wait,
    OUT PFILE_STANDARD_INFORMATION  Buffer,
    OUT PIO_STATUS_BLOCK            IoStatus,
    IN PDEVICE_OBJECT               DeviceObject
)
{

    BOOLEAN     Status = FALSE;
    PEXT2_VCB   Vcb = NULL;
    PEXT2_FCB   Fcb = NULL;
    BOOLEAN     FcbMainResourceAcquired = FALSE;

    __try {

        FsRtlEnterFileSystem();

        __try {

            if (IsExt2FsDevice(DeviceObject)) {
                IoStatus->Status = STATUS_INVALID_DEVICE_REQUEST;
                __leave;
            }

            Fcb = (PEXT2_FCB) FileObject->FsContext;
            if (Fcb == NULL || Fcb->Identifier.Type == EXT2VCB)  {
                IoStatus->Status = STATUS_INVALID_PARAMETER;
                __leave;
            }

            ASSERT((Fcb->Identifier.Type == EXT2FCB) &&
                   (Fcb->Identifier.Size == sizeof(EXT2_FCB)));

#if EXT2_DEBUG
            DEBUG(DL_INF, (
                      "Ext2FastIoQueryStandardInfo: %s %s %wZ\n",
                      Ext2GetCurrentProcessName(),
                      "FASTIO_QUERY_STANDARD_INFO",
                      &Fcb->Mcb->FullName ));
#endif
            Vcb = Fcb->Vcb;

            if (!IsFlagOn(Fcb->Flags, FCB_PAGE_FILE)) {
                if (!ExAcquireResourceSharedLite(
                            &Fcb->MainResource,
                            Wait        )) {
                    __leave;
                }
                FcbMainResourceAcquired = TRUE;
            }

            RtlZeroMemory(Buffer, sizeof(FILE_STANDARD_INFORMATION));

            /*
            typedef struct _FILE_STANDARD_INFORMATION {
            LARGE_INTEGER   AllocationSize;
            LARGE_INTEGER   EndOfFile;
            ULONG           NumberOfLinks;
            BOOLEAN         DeletePending;
            BOOLEAN         Directory;
            } FILE_STANDARD_INFORMATION, *PFILE_STANDARD_INFORMATION;
            */

            Buffer->NumberOfLinks = Fcb->Inode->i_nlink;
            Buffer->DeletePending = IsFlagOn(Fcb->Flags, FCB_DELETE_PENDING);

            if (IsDirectory(Fcb)) {
                Buffer->Directory = IsDirectory(Fcb);
                Buffer->AllocationSize.QuadPart = 0;
                Buffer->EndOfFile.QuadPart = 0;
            } else {
                Buffer->Directory = FALSE;
                Buffer->AllocationSize = Fcb->Header.AllocationSize;
                Buffer->EndOfFile = Fcb->Header.FileSize;
            }

            IoStatus->Information = sizeof(FILE_STANDARD_INFORMATION);
            IoStatus->Status = STATUS_SUCCESS;
#if EXT2_DEBUG
            DEBUG(DL_INF, ( "Ext2FastIoQueryStandInfo: AllocatieonSize = %I64xh FileSize = %I64xh\n",
                            Buffer->AllocationSize.QuadPart, Buffer->EndOfFile.QuadPart));
#endif
            Status =  TRUE;

        } __except (EXCEPTION_EXECUTE_HANDLER) {
            IoStatus->Status = GetExceptionCode();
        }

    } __finally {

        if (FcbMainResourceAcquired) {
            ExReleaseResourceLite(&Fcb->MainResource);
        }

        FsRtlExitFileSystem();
    }

#if EXT2_DEBUG
    if (Status == FALSE) {
        DEBUG(DL_INF, (
                  "Ext2FastIoQueryStandardInfo: %s %s Status: FALSE ***\n",
                  Ext2GetCurrentProcessName(),
                  "FASTIO_QUERY_STANDARD_INFO"            ));
    } else if (IoStatus->Status != STATUS_SUCCESS) {
        DEBUG(DL_INF, (
                  "Ext2FastIoQueryStandardInfo: %s %s Status: %#x ***\n",
                  Ext2GetCurrentProcessName(),
                  "FASTIO_QUERY_STANDARD_INFO",
                  IoStatus->Status            ));
    }
#endif

    return Status;
}

BOOLEAN
Ext2FastIoQueryNetworkOpenInfo (
    IN PFILE_OBJECT         FileObject,
    IN BOOLEAN              Wait,
    IN OUT PFILE_NETWORK_OPEN_INFORMATION PFNOI,
    OUT PIO_STATUS_BLOCK    IoStatus,
    IN PDEVICE_OBJECT       DeviceObject
)
{
    PEXT2_FCB   Fcb = NULL;
    PEXT2_CCB   Ccb = NULL;
    PEXT2_MCB   Mcb = NULL;

    BOOLEAN     bResult = FALSE;
    BOOLEAN FcbResourceAcquired = FALSE;

    __try {

        FsRtlEnterFileSystem();

        if (IsExt2FsDevice(DeviceObject)) {
            IoStatus->Status = STATUS_INVALID_DEVICE_REQUEST;
            __leave;
        }

        Fcb = (PEXT2_FCB) FileObject->FsContext;
        if (Fcb == NULL || Fcb->Identifier.Type == EXT2VCB) {
            IoStatus->Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        ASSERT((Fcb->Identifier.Type == EXT2FCB) &&
               (Fcb->Identifier.Size == sizeof(EXT2_FCB)));
        Ccb = (PEXT2_CCB) FileObject->FsContext2;
        Mcb = Fcb->Mcb;

#if EXT2_DEBUG
        DEBUG(DL_INF, (
                  "%-31s %wZ\n",
                  "FASTIO_QUERY_NETWORK_OPEN_INFO",
                  &Fcb->Mcb->FullName
              ));
#endif

        if (!Ccb) {
            __leave;
        }

        if (!IsFlagOn(Fcb->Flags, FCB_PAGE_FILE)) {

            if (!ExAcquireResourceSharedLite(
                        &Fcb->MainResource,
                        Wait
                    )) {
                __leave;
            }

            FcbResourceAcquired = TRUE;
        }

        if (IsDirectory(Fcb)) {
            PFNOI->AllocationSize.QuadPart = 0;
            PFNOI->EndOfFile.QuadPart = 0;
        } else {
            PFNOI->AllocationSize = Fcb->Header.AllocationSize;
            PFNOI->EndOfFile      = Fcb->Header.FileSize;
        }

        PFNOI->FileAttributes = Mcb->FileAttr;
        if (PFNOI->FileAttributes == 0) {
            PFNOI->FileAttributes = FILE_ATTRIBUTE_NORMAL;
        }

        PFNOI->CreationTime   = Mcb->Icb->CreationTime;
        PFNOI->LastAccessTime = Mcb->Icb->LastAccessTime;
        PFNOI->LastWriteTime  = Mcb->Icb->LastWriteTime;
        PFNOI->ChangeTime     = Mcb->Icb->ChangeTime;

        bResult = TRUE;

        IoStatus->Status = STATUS_SUCCESS;
        IoStatus->Information = sizeof(FILE_NETWORK_OPEN_INFORMATION);

    } __finally {

        if (FcbResourceAcquired) {
            ExReleaseResourceLite(&Fcb->MainResource);
        }

        FsRtlExitFileSystem();
    }

    return bResult;
}
