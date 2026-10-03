/**
 * LuksVolume.c - LUKS volumes: a disk device that en- and decrypts a partition.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * What dm-crypt does on Linux. ext4ctl unlocks a LUKS1 or LUKS2 header in
 * user mode and hands the verified volume key over (IOCTL_APP_CRYPT_UNLOCK,
 * administrators only). The driver then creates \Device\Ext4Crypt<n>, a
 * disk device of its own over the encrypted segment of the partition:
 *
 *     ext4 volume (this driver's file system)
 *         \Device\Ext4Crypt<n>      sector n of the segment, AES-XTS,
 *                                   tweak = n (+ iv_tweak), plain64
 *             \Device\HarddiskVolumeN (the LUKS partition, untouched stack)
 *
 * The file system mounts on the new device like on any partition; nothing
 * in it knows about encryption. The mount manager is told about the device
 * (it has no PnP node, so the volume interface cannot be registered) and
 * gives it a drive letter and a \\?\Volume{...} name; the LUKS UUID is its
 * unique id.
 *
 * The work is split by responsibility: this file is the life cycle of a
 * device - unlock, lock, the queries, the list of devices; LuksIo.c its
 * I/O (the worker threads, inline writes, XTS); VirtualDisk.c what it shares with
 * the LVM devices - answering as a disk and the mount manager. The device
 * shows 512-byte sectors (512e): with LUKS2 sectors of 4 KiB an access to
 * part of one is a read-modify-write under a per-device lock.
 *
 * Locking (IOCTL_APP_CRYPT_LOCK, and every device at driver unload):
 * dismount whatever file system is on the device, drop the mount manager's
 * links, refuse new I/O and wait for the I/O in flight (rundown), stop the
 * workers, destroy the keys, delete the device.
 */

#include "ext4fs.h"
#include <mountdev.h>
#include <mountmgr.h>
#include <ntddvol.h>
#include <ntstrsafe.h>
#include <wdmsec.h>
#include "crypt_internal.h"

LIST_ENTRY                  Ext4CryptList;
FAST_MUTEX                  Ext4CryptListLock;      /* the list only, never across I/O */
static KMUTEX               Ext4CryptControlLock;   /* one unlock or lock at a time */
BCRYPT_ALG_HANDLE           Ext4AesEcb;
static ULONG                Ext4CryptNext;

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext4CryptControl)
#pragma alloc_text(PAGE, Ext4CryptTeardownAll)
#endif

/* ---------------------------------------------------------------- basics */

VOID
Ext4CryptInitialize(VOID)
{
    InitializeListHead(&Ext4CryptList);
    ExInitializeFastMutex(&Ext4CryptListLock);
    KeInitializeMutex(&Ext4CryptControlLock, 0);
}

BOOLEAN
Ext4IsCryptDevice(IN PDEVICE_OBJECT DeviceObject)
{
    PEXT4_CRYPT_DEVICE Crypt;

    if (DeviceObject == NULL || IsExt2FsDevice(DeviceObject)) {
        return FALSE;
    }
    Crypt = (PEXT4_CRYPT_DEVICE)DeviceObject->DeviceExtension;
    return Crypt != NULL &&
           Crypt->Identifier.Type == EXT4CRY &&
           Crypt->Identifier.Size == sizeof(EXT4_CRYPT_DEVICE);
}

BOOLEAN
Ext4IsCryptControl(IN ULONG IoControlCode)
{
    return IoControlCode == IOCTL_APP_CRYPT_UNLOCK ||
           IoControlCode == IOCTL_APP_CRYPT_LOCK ||
           IoControlCode == IOCTL_APP_CRYPT_QUERY ||
           IoControlCode == IOCTL_APP_LV_OPEN ||
           IoControlCode == IOCTL_APP_LV_CLOSE ||
           IoControlCode == IOCTL_APP_LV_QUERY;
}

/*
 * Close a device. Unloading: the file systems are already gone or
 * unregistered, and nothing may be mounted anew.
 */
static NTSTATUS
Ext4CryptClose(IN PEXT4_CRYPT_DEVICE Crypt, IN BOOLEAN Force)
{
    NTSTATUS Status;

    /* the LVM volumes read through this one go first */
    if (Ext4LvUsing(Crypt->Number)) {
        if (!Force) {
            return STATUS_DEVICE_BUSY;
        }
        Ext4LvCloseOn(Crypt->Number);
    }

    Status = Ext4DismountDevice(Crypt->Self, &Crypt->Name, Force);
    if (!NT_SUCCESS(Status) && !Force) {
        return Status;
    }

    Ext4ForgetDevice(&Crypt->Name);

    /* no new I/O; the I/O in flight finishes */
    InterlockedExchange(&Crypt->Closing, TRUE);
    ExWaitForRundownProtectionRelease(&Crypt->Rundown);

    Ext4CryptStopWorkers(Crypt);
    KeRundownQueue(&Crypt->Queue);
    Ext4CryptFreeWorkers(Crypt);
    ExDeleteResourceLite(&Crypt->Rmw);

    ExAcquireFastMutex(&Ext4CryptListLock);
    RemoveEntryList(&Crypt->Link);
    ExReleaseFastMutex(&Ext4CryptListLock);

    DbgPrint("ext4: %wZ locked (%ws)\n", &Crypt->Name, Crypt->Source);

    ObDereferenceObject(Crypt->Lower);
    ObDereferenceObject(Crypt->LowerFile);
    Crypt->Lower = NULL;
    Crypt->LowerFile = NULL;

    /* a file system that is still being torn down keeps the device object
       (not the device) until it lets go; Closing refuses its I/O */
    IoDeleteDevice(Crypt->Self);
    return STATUS_SUCCESS;
}

static PEXT4_CRYPT_DEVICE
Ext4CryptFind(IN ULONG Number)
{
    PLIST_ENTRY         List;
    PEXT4_CRYPT_DEVICE  Found = NULL;

    ExAcquireFastMutex(&Ext4CryptListLock);
    for (List = Ext4CryptList.Flink; List != &Ext4CryptList; List = List->Flink) {
        PEXT4_CRYPT_DEVICE Crypt = CONTAINING_RECORD(List, EXT4_CRYPT_DEVICE, Link);
        if (Crypt->Number == Number) {
            Found = Crypt;
            break;
        }
    }
    ExReleaseFastMutex(&Ext4CryptListLock);
    return Found;
}

/* ---------------------------------------------------------------- IOCTLs */

/*
 * The LUKS device an LV is read from: referenced, with its size. NULL when
 * there is no such device or it is going away. Called under the control
 * lock, like every lock of a LUKS device.
 */
PDEVICE_OBJECT
Ext4CryptReference(IN ULONG Number, OUT PULONGLONG Size)
{
    PEXT4_CRYPT_DEVICE Crypt = Ext4CryptFind(Number);

    if (Crypt == NULL || Crypt->Closing) {
        return NULL;
    }
    ObReferenceObject(Crypt->Self);
    *Size = Crypt->Size;
    return Crypt->Self;
}

static BOOLEAN
Ext4CryptTerminated(IN const WCHAR *Text, IN SIZE_T Chars)
{
    return wcsnlen(Text, Chars) < Chars;
}

static NTSTATUS
Ext4CryptOpenAes(VOID)
{
    NTSTATUS Status;

    if (Ext4AesEcb) {
        return STATUS_SUCCESS;
    }
    Status = BCryptOpenAlgorithmProvider(&Ext4AesEcb, BCRYPT_AES_ALGORITHM, NULL, 0);
    if (NT_SUCCESS(Status)) {
        Status = BCryptSetProperty(Ext4AesEcb, BCRYPT_CHAINING_MODE,
                                   (PUCHAR)BCRYPT_CHAIN_MODE_ECB,
                                   sizeof(BCRYPT_CHAIN_MODE_ECB), 0);
        if (!NT_SUCCESS(Status)) {
            BCryptCloseAlgorithmProvider(Ext4AesEcb, 0);
            Ext4AesEcb = NULL;
        }
    }
    return Status;
}

/* the request, field by field */
static BOOLEAN
Ext4CryptRequestValid(IN PEXT4_CRYPT_UNLOCK In)
{
    static const WCHAR DevicePrefix[] = L"\\Device\\";

    return In->Version == EXT4_CRYPT_VERSION &&
           (In->Cipher == EXT4_CIPHER_AES_XTS_PLAIN64 || In->Cipher == EXT4_CIPHER_AES_XTS_PLAIN) &&
           (In->KeyBytes == EXT4_CRYPT_KEY_AES128_XTS || In->KeyBytes == EXT4_CRYPT_KEY_AES256_XTS) &&
           In->SectorSize >= EXT4_SECTOR && In->SectorSize <= EXT4_XTS_MAX_UNIT &&
           (In->SectorSize & (In->SectorSize - 1)) == 0 &&
           (In->PayloadOffset & (EXT4_SECTOR - 1)) == 0 &&
           (In->Flags & ~(EXT4_CRYPT_READ_ONLY | EXT4_CRYPT_IV_LARGE_SECTORS)) == 0 &&
           Ext4CryptTerminated(In->Device, EXT4_CRYPT_DEVICE_CHARS) &&
           strnlen(In->Uuid, EXT4_CRYPT_UUID_CHARS) < EXT4_CRYPT_UUID_CHARS &&
           _wcsnicmp(In->Device, DevicePrefix, ARRAYSIZE(DevicePrefix) - 1) == 0;
}

/* one device per partition, and no more than the table holds */
static NTSTATUS
Ext4CryptRoomFor(IN const WCHAR *Source)
{
    PLIST_ENTRY List;
    ULONG       Count = 0;
    NTSTATUS    Status = STATUS_SUCCESS;

    ExAcquireFastMutex(&Ext4CryptListLock);
    for (List = Ext4CryptList.Flink; List != &Ext4CryptList; List = List->Flink) {
        PEXT4_CRYPT_DEVICE Other = CONTAINING_RECORD(List, EXT4_CRYPT_DEVICE, Link);
        if (_wcsicmp(Other->Source, Source) == 0) {
            Status = STATUS_OBJECT_NAME_COLLISION;
        }
        Count++;
    }
    ExReleaseFastMutex(&Ext4CryptListLock);
    if (NT_SUCCESS(Status) && Count >= EXT4_CRYPT_MAX_VOLUMES) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
    }
    return Status;
}

/*
 * The partition: an attributes-only open, which mounts nothing on it; the
 * requests go to the top of its own stack. *Size: the bytes of payload it
 * holds, whole sectors. On failure nothing is held.
 */
static NTSTATUS
Ext4CryptOpenLower(IN PEXT4_CRYPT_UNLOCK In, OUT PFILE_OBJECT *LowerFile,
                   OUT PDEVICE_OBJECT *Lower, OUT PULONGLONG Size)
{
    UNICODE_STRING          Source;
    GET_LENGTH_INFORMATION  Length;
    PDEVICE_OBJECT          Top;
    NTSTATUS                Status;

    RtlInitUnicodeString(&Source, In->Device);
    Status = IoGetDeviceObjectPointer(&Source, FILE_READ_ATTRIBUTES, LowerFile, &Top);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    *Lower = IoGetAttachedDeviceReference((*LowerFile)->DeviceObject);

    Status = Ext4CryptLowerIoctl(*Lower, IOCTL_DISK_GET_LENGTH_INFO, &Length, sizeof(Length));
    if (NT_SUCCESS(Status) && In->PayloadOffset >= (ULONGLONG)Length.Length.QuadPart) {
        Status = STATUS_INVALID_PARAMETER;
    }
    if (NT_SUCCESS(Status)) {
        *Size = (ULONGLONG)Length.Length.QuadPart - In->PayloadOffset;
        if (In->PayloadSize != 0) {
            if (In->PayloadSize > *Size) {
                Status = STATUS_INVALID_PARAMETER;
            }
            *Size = In->PayloadSize;
        }
        *Size &= ~(ULONGLONG)(In->SectorSize - 1);
        if (*Size == 0) {
            Status = STATUS_INVALID_PARAMETER;
        }
    }
    if (!NT_SUCCESS(Status)) {
        ObDereferenceObject(*Lower);
        ObDereferenceObject(*LowerFile);
        *Lower = NULL;
        *LowerFile = NULL;
    }
    return Status;
}

/* the extension of a new disk device: what the request says, the queue, the locks */
static VOID
Ext4CryptInitDevice(IN PEXT4_CRYPT_DEVICE Crypt, IN PDEVICE_OBJECT Device, IN PEXT4_CRYPT_UNLOCK In,
                    IN PDEVICE_OBJECT Lower, IN PFILE_OBJECT LowerFile, IN ULONG Number,
                    IN PCWSTR Name, IN ULONGLONG Size)
{
    RtlZeroMemory(Crypt, sizeof(EXT4_CRYPT_DEVICE));
    Crypt->Identifier.Type = EXT4CRY;
    Crypt->Identifier.Size = sizeof(EXT4_CRYPT_DEVICE);
    Crypt->Self = Device;
    Crypt->Lower = Lower;
    Crypt->LowerFile = LowerFile;
    Crypt->Number = Number;
    Crypt->Flags = In->Flags;
    Crypt->Cipher = In->Cipher;
    Crypt->KeyBytes = In->KeyBytes;
    Crypt->SectorSize = In->SectorSize;
    for (Crypt->SectorShift = 0; (1UL << Crypt->SectorShift) < In->SectorSize; Crypt->SectorShift++) {
    }
    Crypt->Offset = In->PayloadOffset;
    Crypt->Size = Size;
    Crypt->IvOffset = In->IvOffset;
    RtlStringCbCopyW(Crypt->NameBuffer, sizeof(Crypt->NameBuffer), Name);
    RtlInitUnicodeString(&Crypt->Name, Crypt->NameBuffer);
    RtlStringCbCopyW(Crypt->Source, sizeof(Crypt->Source), In->Device);
    RtlStringCbCopyA(Crypt->Uuid, sizeof(Crypt->Uuid), In->Uuid);
    KeInitializeQueue(&Crypt->Queue, 0);
    ExInitializeRundownProtection(&Crypt->Rundown);
    ExInitializeResourceLite(&Crypt->Rmw);
}

/*
 * The disk device over the partition, with its key schedules and workers;
 * raw access to the plaintext for SYSTEM and administrators only. Not
 * FILE_DEVICE_SECURE_OPEN: files on the volume are the file system's to
 * check. On success the device owns Lower and LowerFile.
 */
static NTSTATUS
Ext4CryptCreateDevice(IN PEXT4_CRYPT_UNLOCK In, IN PDEVICE_OBJECT Lower, IN PFILE_OBJECT LowerFile,
                      IN ULONGLONG Size, OUT PEXT4_CRYPT_DEVICE *Out)
{
    PEXT4_CRYPT_DEVICE  Crypt;
    PDEVICE_OBJECT      Device;
    UNICODE_STRING      Name;
    WCHAR               NameBuffer[RTL_FIELD_SIZE(EXT4_CRYPT_DEVICE, NameBuffer) / sizeof(WCHAR)];
    ULONG               Number, Workers;
    NTSTATUS            Status;

    Number = (ULONG)InterlockedIncrement((PLONG)&Ext4CryptNext) - 1;
    RtlStringCbPrintfW(NameBuffer, sizeof(NameBuffer), EXT4_CRYPT_DEVICE_PREFIX L"%u", Number);
    RtlInitUnicodeString(&Name, NameBuffer);
    Status = IoCreateDeviceSecure(Ext2Global->DiskdevObject->DriverObject,
                                  sizeof(EXT4_CRYPT_DEVICE), &Name, FILE_DEVICE_DISK, 0,
                                  FALSE, &SDDL_DEVOBJ_SYS_ALL_ADM_ALL, NULL, &Device);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Crypt = (PEXT4_CRYPT_DEVICE)Device->DeviceExtension;
    Ext4CryptInitDevice(Crypt, Device, In, Lower, LowerFile, Number, NameBuffer, Size);

    Workers = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS) * EXT4_CRYPT_PER_CPU;
    Status = Ext4CryptStartWorkers(Crypt, In->Key, min(Workers, EXT4_CRYPT_WORKERS));
    if (NT_SUCCESS(Status)) {
        Status = Ext4CryptStartInline(Crypt, In->Key);
    }
    if (!NT_SUCCESS(Status)) {
        Ext4CryptStopWorkers(Crypt);
        KeRundownQueue(&Crypt->Queue);
        Ext4CryptFreeWorkers(Crypt);
        ExDeleteResourceLite(&Crypt->Rmw);
        IoDeleteDevice(Device);
        return Status;
    }

    SetFlag(Device->Flags, DO_DIRECT_IO);
    Device->AlignmentRequirement = Lower->AlignmentRequirement;
    Device->SectorSize = EXT4_SECTOR;          /* 512e, see IOCTL_DISK_GET_DRIVE_GEOMETRY */
    ClearFlag(Device->Flags, DO_DEVICE_INITIALIZING);
    *Out = Crypt;
    return STATUS_SUCCESS;
}

/* listed, then links from the mount manager, the letter asked for, and a
   first open on a worker: that is when the file system mounts */
static VOID
Ext4CryptPublish(IN PEXT4_CRYPT_DEVICE Crypt, IN WCHAR Letter)
{
    NTSTATUS Status;

    ExAcquireFastMutex(&Ext4CryptListLock);
    InsertTailList(&Ext4CryptList, &Crypt->Link);
    ExReleaseFastMutex(&Ext4CryptListLock);

    DbgPrint("ext4: %ws unlocked as %wZ (%I64u bytes from %I64u, %u-byte sectors)\n",
             Crypt->Source, &Crypt->Name, Crypt->Size, Crypt->Offset, Crypt->SectorSize);

    Ext4ForgetDevice(&Crypt->Name);
    Status = Ext4AnnounceDevice(&Crypt->Name);
    if (!NT_SUCCESS(Status)) {
        DbgPrint("ext4: the mount manager did not take %wZ (%xh)\n", &Crypt->Name, Status);
    }
    if (Letter) {
        Ext4SetDeviceLetter(&Crypt->Name, Letter);
    }
    Ext2QueueVolumeProbe(&Crypt->Name);
}

static NTSTATUS
Ext4CryptUnlock(IN PEXT4_CRYPT_UNLOCK In)
{
    PEXT4_CRYPT_DEVICE  Crypt;
    PFILE_OBJECT        LowerFile;
    PDEVICE_OBJECT      Lower;
    ULONGLONG           Size = 0;
    NTSTATUS            Status;

    if (!Ext4CryptRequestValid(In)) {
        return STATUS_INVALID_PARAMETER;
    }
    Status = Ext4CryptOpenAes();
    if (!NT_SUCCESS(Status)) {
        DbgPrint("ext4: no AES from CNG (%xh)\n", Status);
        return Status;
    }
    Status = Ext4CryptRoomFor(In->Device);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = Ext4CryptOpenLower(In, &LowerFile, &Lower, &Size);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = Ext4CryptCreateDevice(In, Lower, LowerFile, Size, &Crypt);
    if (!NT_SUCCESS(Status)) {
        ObDereferenceObject(Lower);
        ObDereferenceObject(LowerFile);
        return Status;
    }
    Ext4CryptPublish(Crypt, In->Letter);
    In->Index = Crypt->Number;
    return STATUS_SUCCESS;
}

static NTSTATUS
Ext4CryptQuery(OUT PEXT4_CRYPT_QUERY Out)
{
    PLIST_ENTRY List;

    Out->Count = 0;
    ExAcquireFastMutex(&Ext4CryptListLock);
    for (List = Ext4CryptList.Flink;
         List != &Ext4CryptList && Out->Count < EXT4_CRYPT_MAX_VOLUMES;
         List = List->Flink) {
        PEXT4_CRYPT_DEVICE  Crypt = CONTAINING_RECORD(List, EXT4_CRYPT_DEVICE, Link);
        PEXT4_CRYPT_ENTRY   Entry = &Out->Entries[Out->Count++];

        RtlZeroMemory(Entry, sizeof(*Entry));
        Entry->Index = Crypt->Number;
        Entry->Flags = Crypt->Flags;
        Entry->SectorSize = Crypt->SectorSize;
        Entry->KeyBytes = Crypt->KeyBytes;
        Entry->PayloadOffset = Crypt->Offset;
        Entry->PayloadSize = Crypt->Size;
        RtlStringCbCopyW(Entry->Device, sizeof(Entry->Device), Crypt->Source);
        RtlStringCbCopyA(Entry->Uuid, sizeof(Entry->Uuid), Crypt->Uuid);
    }
    ExReleaseFastMutex(&Ext4CryptListLock);
    return STATUS_SUCCESS;
}

/*
 * IOCTL_APP_CRYPT_* on the control device. Called straight from the
 * dispatch entry, outside the file system's request machinery: locking a
 * volume opens and dismounts it, which re-enters the file systems.
 */
NTSTATUS
Ext4CryptControl(IN PIRP Irp)
{
    PIO_STACK_LOCATION  IrpSp = IoGetCurrentIrpStackLocation(Irp);
    ULONG               Code = IrpSp->Parameters.DeviceIoControl.IoControlCode;
    ULONG               InLength = IrpSp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG               OutLength = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID               Buffer = Irp->AssociatedIrp.SystemBuffer;
    ULONG_PTR           Information = 0;
    NTSTATUS            Status;

    PAGED_CODE();

    Status = Ext2CheckAppIoctl(Irp, Code);
    if (!NT_SUCCESS(Status)) {
        return Ext4DiskComplete(Irp, Status, 0);
    }
    if (Ext2Global->UnloadState != EXT2_UNLOAD_IDLE ||
        IsFlagOn(Ext2Global->Flags, EXT2_UNLOAD_PENDING)) {
        return Ext4DiskComplete(Irp, STATUS_TOO_LATE, 0);
    }
    if (*(PULONG)Buffer != EXT4_CRYPT_MAGIC) {
        return Ext4DiskComplete(Irp, STATUS_INVALID_PARAMETER, 0);
    }

    KeWaitForSingleObject(&Ext4CryptControlLock, Executive, KernelMode, FALSE, NULL);

    switch (Code) {

    case IOCTL_APP_CRYPT_UNLOCK:
        if (InLength < sizeof(EXT4_CRYPT_UNLOCK) || OutLength < sizeof(EXT4_CRYPT_UNLOCK)) {
            Status = STATUS_INVALID_PARAMETER;
        } else {
            Status = Ext4CryptUnlock((PEXT4_CRYPT_UNLOCK)Buffer);
        }
        /* the key goes back out with the answer otherwise */
        if (InLength >= FIELD_OFFSET(EXT4_CRYPT_UNLOCK, Key) + EXT4_CRYPT_MAX_KEY) {
            RtlSecureZeroMemory(((PEXT4_CRYPT_UNLOCK)Buffer)->Key, EXT4_CRYPT_MAX_KEY);
        }
        Information = NT_SUCCESS(Status) ? sizeof(EXT4_CRYPT_UNLOCK) : 0;
        break;

    case IOCTL_APP_CRYPT_LOCK:
        if (InLength < sizeof(EXT4_CRYPT_LOCK)) {
            Status = STATUS_INVALID_PARAMETER;
        } else {
            PEXT4_CRYPT_LOCK    Lock = (PEXT4_CRYPT_LOCK)Buffer;
            PEXT4_CRYPT_DEVICE  Crypt = Ext4CryptFind(Lock->Index);
            Status = Crypt ? Ext4CryptClose(Crypt, IsFlagOn(Lock->Flags, EXT4_CRYPT_FORCE)) :
                             STATUS_NOT_FOUND;
        }
        break;

    case IOCTL_APP_CRYPT_QUERY:
        if (OutLength < sizeof(EXT4_CRYPT_QUERY)) {
            Status = STATUS_BUFFER_TOO_SMALL;
        } else {
            Status = Ext4CryptQuery((PEXT4_CRYPT_QUERY)Buffer);
            Information = sizeof(EXT4_CRYPT_QUERY);
        }
        break;

    case IOCTL_APP_LV_OPEN:
        Status = Ext4LvOpen((PEXT4_LV_OPEN)Buffer, InLength);
        Information = NT_SUCCESS(Status) ? FIELD_OFFSET(EXT4_LV_OPEN, Run) : 0;
        break;

    case IOCTL_APP_LV_CLOSE:
        Status = InLength < sizeof(EXT4_LV_CLOSE) ? STATUS_INVALID_PARAMETER :
                 Ext4LvClose((PEXT4_LV_CLOSE)Buffer);
        break;

    case IOCTL_APP_LV_QUERY:
        if (OutLength < sizeof(EXT4_LV_QUERY)) {
            Status = STATUS_BUFFER_TOO_SMALL;
        } else {
            Status = Ext4LvQuery((PEXT4_LV_QUERY)Buffer);
            Information = sizeof(EXT4_LV_QUERY);
        }
        break;

    default:
        Status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    KeReleaseMutex(&Ext4CryptControlLock, FALSE);
    return Ext4DiskComplete(Irp, Status, Information);
}

/*
 * Driver unload, after the file systems have let go of their volumes:
 * every device goes, whatever is left on it.
 */
VOID
Ext4CryptTeardownAll(VOID)
{
    PAGED_CODE();

    if (Ext4CryptList.Flink == NULL) {
        return;                             /* never initialized */
    }

    KeWaitForSingleObject(&Ext4CryptControlLock, Executive, KernelMode, FALSE, NULL);
    Ext4LvTeardownAll();
    for (;;) {
        PEXT4_CRYPT_DEVICE Crypt = NULL;

        ExAcquireFastMutex(&Ext4CryptListLock);
        if (!IsListEmpty(&Ext4CryptList)) {
            Crypt = CONTAINING_RECORD(Ext4CryptList.Flink, EXT4_CRYPT_DEVICE, Link);
        }
        ExReleaseFastMutex(&Ext4CryptListLock);
        if (Crypt == NULL) {
            break;
        }
        Ext4CryptClose(Crypt, TRUE);
    }
    if (Ext4AesEcb) {
        BCryptCloseAlgorithmProvider(Ext4AesEcb, 0);
        Ext4AesEcb = NULL;
    }
    KeReleaseMutex(&Ext4CryptControlLock, FALSE);
}
