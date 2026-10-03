/**
 * FastIoLocks.c - fast I/O byte range lock entry points.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2FastIoLock)
#pragma alloc_text(PAGE, Ext2FastIoUnlockSingle)
#pragma alloc_text(PAGE, Ext2FastIoUnlockAll)
#pragma alloc_text(PAGE, Ext2FastIoUnlockAll)
#endif

BOOLEAN
Ext2FastIoLock (
    IN PFILE_OBJECT         FileObject,
    IN PLARGE_INTEGER       FileOffset,
    IN PLARGE_INTEGER       Length,
    IN PEPROCESS            Process,
    IN ULONG                Key,
    IN BOOLEAN              FailImmediately,
    IN BOOLEAN              ExclusiveLock,
    OUT PIO_STATUS_BLOCK    IoStatus,
    IN PDEVICE_OBJECT       DeviceObject
)
{
    BOOLEAN     Status = FALSE;
    PEXT2_FCB   Fcb;

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

            ASSERT((Fcb->Identifier.Type == EXT2FCB) &&
                   (Fcb->Identifier.Size == sizeof(EXT2_FCB)));

            if (IsDirectory(Fcb)) {
                IoStatus->Status = STATUS_INVALID_PARAMETER;
                __leave;
            }
#if EXT2_DEBUG
            DEBUG(DL_INF, (
                      "Ext2FastIoLock: %s %s %wZ\n",
                      Ext2GetCurrentProcessName(),
                      "FASTIO_LOCK",
                      &Fcb->Mcb->FullName        ));

            DEBUG(DL_INF, (
                      "Ext2FastIoLock: Offset: %I64xh Length: %I64xh Key: %u %s%s\n",
                      FileOffset->QuadPart,
                      Length->QuadPart,
                      Key,
                      (FailImmediately ? "FailImmediately " : ""),
                      (ExclusiveLock ? "ExclusiveLock " : "") ));
#endif

            if (!FsRtlOplockIsFastIoPossible(&Fcb->Oplock)) {
                __leave;
            }

            Status = FsRtlFastLock(
                         &Fcb->FileLockAnchor,
                         FileObject,
                         FileOffset,
                         Length,
                         Process,
                         Key,
                         FailImmediately,
                         ExclusiveLock,
                         IoStatus,
                         NULL,
                         FALSE);

            if (Status) {
                Fcb->Header.IsFastIoPossible = Ext2IsFastIoPossible(Fcb);
            }

        } __except (EXCEPTION_EXECUTE_HANDLER) {
            IoStatus->Status = GetExceptionCode();
        }

    } __finally {

        FsRtlExitFileSystem();
    }

#if EXT2_DEBUG
    if (Status == FALSE) {
        DEBUG(DL_ERR, (
                  "Ext2FastIoLock: %s %s *** Status: FALSE ***\n",
                  (PUCHAR) Process + ProcessNameOffset,
                  "FASTIO_LOCK"
              ));
    } else if (IoStatus->Status != STATUS_SUCCESS) {
        DEBUG(DL_ERR, (
                  "Ext2FastIoLock: %s %s *** Status: %s (%#x) ***\n",
                  (PUCHAR) Process + ProcessNameOffset,
                  "FASTIO_LOCK",
                  Ext2NtStatusToString(IoStatus->Status),
                  IoStatus->Status
              ));
    }
#endif

    return Status;
}

BOOLEAN
Ext2FastIoUnlockSingle (
    IN PFILE_OBJECT         FileObject,
    IN PLARGE_INTEGER       FileOffset,
    IN PLARGE_INTEGER       Length,
    IN PEPROCESS            Process,
    IN ULONG                Key,
    OUT PIO_STATUS_BLOCK    IoStatus,
    IN PDEVICE_OBJECT       DeviceObject
)
{
    BOOLEAN     Status = FALSE;
    PEXT2_FCB   Fcb;

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

            ASSERT((Fcb->Identifier.Type == EXT2FCB) &&
                   (Fcb->Identifier.Size == sizeof(EXT2_FCB)));

            if (IsDirectory(Fcb)) {
                IoStatus->Status = STATUS_INVALID_PARAMETER;
                __leave;
            }

#if EXT2_DEBUG
            DEBUG(DL_INF, (
                      "Ext2FastIoUnlockSingle: %s %s %wZ\n",
                      (PUCHAR) Process + ProcessNameOffset,
                      "FASTIO_UNLOCK_SINGLE",
                      &Fcb->Mcb->FullName        ));

            DEBUG(DL_INF, (
                      "Ext2FastIoUnlockSingle: Offset: %I64xh Length: %I64xh Key: %u\n",
                      FileOffset->QuadPart,
                      Length->QuadPart,
                      Key     ));
#endif

            if (!FsRtlOplockIsFastIoPossible(&Fcb->Oplock)) {
                __leave;
            }

            IoStatus->Status = FsRtlFastUnlockSingle(
                                   &Fcb->FileLockAnchor,
                                   FileObject,
                                   FileOffset,
                                   Length,
                                   Process,
                                   Key,
                                   NULL,
                                   FALSE);

            IoStatus->Information = 0;
            Status =  TRUE;

            Fcb->Header.IsFastIoPossible = Ext2IsFastIoPossible(Fcb);

        } __except (EXCEPTION_EXECUTE_HANDLER) {
            IoStatus->Status = GetExceptionCode();
        }

    } __finally {

        FsRtlExitFileSystem();
    }

#if EXT2_DEBUG
    if (Status == FALSE) {

        DEBUG(DL_ERR, (
                  "Ext2FastIoUnlockSingle: %s %s *** Status: FALSE ***\n",
                  (PUCHAR) Process + ProcessNameOffset,
                  "FASTIO_UNLOCK_SINGLE"          ));
    } else if (IoStatus->Status != STATUS_SUCCESS) {
        DEBUG(DL_ERR, (
                  "Ext2FastIoUnlockSingle: %s %s *** Status: %s (%#x) ***\n",
                  (PUCHAR) Process + ProcessNameOffset,
                  "FASTIO_UNLOCK_SINGLE",
                  Ext2NtStatusToString(IoStatus->Status),
                  IoStatus->Status            ));
    }
#endif

    return Status;
}

BOOLEAN
Ext2FastIoUnlockAll (
    IN PFILE_OBJECT         FileObject,
    IN PEPROCESS            Process,
    OUT PIO_STATUS_BLOCK    IoStatus,
    IN PDEVICE_OBJECT       DeviceObject)
{
    BOOLEAN     Status = FALSE;
    PEXT2_FCB   Fcb;

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

            ASSERT((Fcb->Identifier.Type == EXT2FCB) &&
                   (Fcb->Identifier.Size == sizeof(EXT2_FCB)));

            if (IsDirectory(Fcb)) {
                IoStatus->Status = STATUS_INVALID_PARAMETER;
                __leave;
            }
#if EXT2_DEBUG
            DEBUG(DL_INF, (
                      "Ext2FastIoUnlockSingle: %s %s %wZ\n",
                      (PUCHAR) Process + ProcessNameOffset,
                      "FASTIO_UNLOCK_ALL",
                      &Fcb->Mcb->FullName
                  ));
#endif

            if (!FsRtlOplockIsFastIoPossible(&Fcb->Oplock)) {
                __leave;
            }

            IoStatus->Status = FsRtlFastUnlockAll(
                                   &Fcb->FileLockAnchor,
                                   FileObject,
                                   Process,
                                   NULL        );

            IoStatus->Information = 0;
            Status =  TRUE;

            Fcb->Header.IsFastIoPossible = Ext2IsFastIoPossible(Fcb);

        } __except (EXCEPTION_EXECUTE_HANDLER) {
            IoStatus->Status = GetExceptionCode();
        }

    } __finally {

        FsRtlExitFileSystem();
    }

#if EXT2_DEBUG
    if (Status == FALSE) {

        DEBUG(DL_ERR, (
                  "Ext2FastIoUnlockSingle: %s %s *** Status: FALSE ***\n",
                  (PUCHAR) Process + ProcessNameOffset,
                  "FASTIO_UNLOCK_ALL"
              ));
    } else if (IoStatus->Status != STATUS_SUCCESS) {
        DEBUG(DL_ERR, (
                  "Ext2FastIoUnlockSingle: %s %s *** Status: %s (%#x) ***\n",
                  (PUCHAR) Process + ProcessNameOffset,
                  "FASTIO_UNLOCK_ALL",
                  Ext2NtStatusToString(IoStatus->Status),
                  IoStatus->Status
              ));
    }
#endif

    return Status;
}

BOOLEAN
Ext2FastIoUnlockAllByKey (
    IN PFILE_OBJECT         FileObject,
    IN PEPROCESS            Process,
    IN ULONG                Key,
    OUT PIO_STATUS_BLOCK    IoStatus,
    IN PDEVICE_OBJECT       DeviceObject
)
{
    BOOLEAN     Status = FALSE;
    PEXT2_FCB   Fcb;

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

            ASSERT((Fcb->Identifier.Type == EXT2FCB) &&
                   (Fcb->Identifier.Size == sizeof(EXT2_FCB)));

            if (IsDirectory(Fcb)) {
                IoStatus->Status = STATUS_INVALID_PARAMETER;
                __leave;
            }

#if EXT2_DEBUG
            DEBUG(DL_INF, (
                      "Ext2FastIoUnlockAllByKey: %s %s %wZ\n",
                      (PUCHAR) Process + ProcessNameOffset,
                      "FASTIO_UNLOCK_ALL_BY_KEY",
                      &Fcb->Mcb->FullName
                  ));

            DEBUG(DL_INF, (
                      "Ext2FastIoUnlockAllByKey: Key: %u\n",
                      Key
                  ));
#endif

            if (!FsRtlOplockIsFastIoPossible(&Fcb->Oplock)) {
                __leave;
            }

            IoStatus->Status = FsRtlFastUnlockAllByKey(
                                   &Fcb->FileLockAnchor,
                                   FileObject,
                                   Process,
                                   Key,
                                   NULL
                               );

            IoStatus->Information = 0;
            Status =  TRUE;

            Fcb->Header.IsFastIoPossible = Ext2IsFastIoPossible(Fcb);

        } __except (EXCEPTION_EXECUTE_HANDLER) {
            IoStatus->Status = GetExceptionCode();
        }

    } __finally {

        FsRtlExitFileSystem();
    }

#if EXT2_DEBUG
    if (Status == FALSE) {

        DEBUG(DL_ERR, (
                  "Ext2FastIoUnlockAllByKey: %s %s *** Status: FALSE ***\n",
                  (PUCHAR) Process + ProcessNameOffset,
                  "FASTIO_UNLOCK_ALL_BY_KEY"
              ));
    } else if (IoStatus->Status != STATUS_SUCCESS) {

        DEBUG(DL_ERR, (
                  "Ext2FastIoUnlockAllByKey: %s %s *** Status: %s (%#x) ***\n",
                  (PUCHAR) Process + ProcessNameOffset,
                  "FASTIO_UNLOCK_ALL_BY_KEY",
                  Ext2NtStatusToString(IoStatus->Status),
                  IoStatus->Status
              ));
    }
#endif

    return Status;
}
