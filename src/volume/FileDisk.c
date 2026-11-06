/**
 * FileDisk.c - a file as a disk: ext4 volumes in image files.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * What a loop device does on Linux. ext4ctl names a file and the driver puts a
 * disk device of its own over it; the file system mounts on that device like
 * on any partition, and nothing in it knows:
 *
 *     ext4 volume (this driver's file system)
 *         \Device\Ext4File<n>      block n is byte n * 512 of the file
 *             C:\images\disk.img   on any other file system
 *
 * The file is opened without the cache, asynchronously, by workers that do
 * nothing else: the bytes reach the disk below as they would from a real one,
 * so the file system above keeps its write ordering, and memory pressure does
 * not make a write here depend on a write there. Without the cache a file
 * takes whole sectors of its own volume; a request that is not (a block of
 * 1 KiB on a disk of 4 KiB sectors) goes through a buffer of its own, and if it
 * is part of a sector, under a lock of its own, read and merged first.
 *
 * A trim of the device (IOCTL_STORAGE_MANAGE_DATA_SET_ATTRIBUTES, what
 * ext4ctl trim sends) becomes FSCTL_SET_ZERO_DATA on a sparse file: the blocks
 * the volume no longer uses are given back to the volume the file is on. A
 * file that is not sparse could only be written with zeros, and is told so.
 *
 * A file on a volume of this very driver is refused: its I/O would come back
 * into the file system whose I/O it serves, and wait for itself.
 *
 * Unmounting dismounts whatever is on the device, drops the mount manager's
 * links, lets the I/O in flight finish, flushes the file and closes it. Every
 * file goes at driver unload too. Opening, closing and querying run under the
 * LUKS control lock (LuksVolume.c).
 */

#include "ext4fs.h"
#include <wdmsec.h>
#include <ntstrsafe.h>
#include <ntddstor.h>
#include "ext4crypt.h"

#define FILE_DISK_TAG           'KF4E'
#define FILE_DISK_WORKERS       4
#define FILE_DISK_UNIQUE_PREFIX "EXT4-FILE-"

struct _EXT4_FILE_DISK;

typedef struct _EXT4_FILE_WORKER {
    struct _EXT4_FILE_DISK *Disk;
    PETHREAD        Thread;
    HANDLE          Event;              /* what its I/O on the file signals */
    LIST_ENTRY      Stop;               /* its entry in the queue when it is to end */
} EXT4_FILE_WORKER, *PEXT4_FILE_WORKER;

typedef struct _EXT4_FILE_DISK {
    EXT2_IDENTIFIER     Identifier;     /* EXT4FIL */
    LIST_ENTRY          Link;
    PDEVICE_OBJECT      Self;
    HANDLE              File;           /* a kernel handle: asynchronous, no cache */
    ULONG               Number;         /* \Device\Ext4File<Number>, never reused */
    ULONG               Flags;          /* EXT4_CRYPT_READ_ONLY */
    ULONG               SectorSize;     /* of the volume the file is on */
    ULONG               AlignMask;      /* what a buffer must be aligned to, for it */
    BOOLEAN             Sparse;
    ULONGLONG           Size;
    KQUEUE              Queue;
    EX_RUNDOWN_REF      Rundown;        /* requests in flight */
    ERESOURCE           Partial;        /* one read and merge of a sector at a time */
    volatile LONG       Closing;
    UNICODE_STRING      Name;
    WCHAR               NameBuffer[48];
    CHAR                Id[EXT4_CRYPT_UUID_CHARS];
    WCHAR               Path[EXT4_FILE_PATH_CHARS];
    ULONG               Workers;
    EXT4_FILE_WORKER    Worker[FILE_DISK_WORKERS];
} EXT4_FILE_DISK, *PEXT4_FILE_DISK;

static LIST_ENTRY       Ext4FileList;
static FAST_MUTEX       Ext4FileListLock;       /* the list only, never across I/O */
static ULONG            Ext4FileNext;
static BOOLEAN          Ext4FileReady;

static KSTART_ROUTINE Ext4FileWorkerThread;

static VOID
Ext4FileInitialize(VOID)
{
    if (!Ext4FileReady) {
        InitializeListHead(&Ext4FileList);
        ExInitializeFastMutex(&Ext4FileListLock);
        Ext4FileReady = TRUE;
    }
}

BOOLEAN
Ext4IsFileDisk(IN PDEVICE_OBJECT DeviceObject)
{
    PEXT4_FILE_DISK Disk;

    if (DeviceObject == NULL || IsExt2FsDevice(DeviceObject)) {
        return FALSE;
    }
    Disk = (PEXT4_FILE_DISK)DeviceObject->DeviceExtension;
    return Disk != NULL && Disk->Identifier.Type == EXT4FIL &&
           Disk->Identifier.Size == sizeof(EXT4_FILE_DISK);
}

/* ---------------------------------------------------------------- I/O */

/* one read, write or control of the file, waited for */
static NTSTATUS
Ext4FileFinish(IN PEXT4_FILE_WORKER Worker, IN NTSTATUS Status, IN PIO_STATUS_BLOCK Iosb)
{
    if (Status == STATUS_PENDING) {
        (void)ZwWaitForSingleObject(Worker->Event, FALSE, NULL);
        Status = Iosb->Status;
    }
    return Status;
}

static NTSTATUS
Ext4FileIo(IN PEXT4_FILE_WORKER Worker, IN BOOLEAN Write, IN PVOID Buffer, IN ULONG Length, IN ULONGLONG Byte)
{
    PEXT4_FILE_DISK Disk = Worker->Disk;
    IO_STATUS_BLOCK Iosb;
    LARGE_INTEGER   Offset;
    NTSTATUS        Status;

    Offset.QuadPart = (LONGLONG)Byte;
    Iosb.Status = STATUS_SUCCESS;
    Iosb.Information = 0;
    Status = Write ? ZwWriteFile(Disk->File, Worker->Event, NULL, NULL, &Iosb, Buffer, Length, &Offset, NULL) :
                     ZwReadFile(Disk->File, Worker->Event, NULL, NULL, &Iosb, Buffer, Length, &Offset, NULL);
    Status = Ext4FileFinish(Worker, Status, &Iosb);
    if (NT_SUCCESS(Status) && Iosb.Information != Length) {
        Status = STATUS_UNEXPECTED_IO_ERROR;
    }
    return Status;
}

/* A request the file cannot take as it is: through whole sectors of a buffer
   of its own, the sectors it only touches read first. */
static NTSTATUS
Ext4FileThroughBuffer(IN PEXT4_FILE_WORKER Worker, IN BOOLEAN Write, IN PUCHAR Data,
                      IN ULONG Length, IN ULONGLONG Byte)
{
    PEXT4_FILE_DISK Disk = Worker->Disk;
    ULONGLONG       Mask = (ULONGLONG)Disk->SectorSize - 1;
    ULONGLONG       First = Byte & ~Mask;
    ULONG           Span = (ULONG)(((Byte + Length + Mask) & ~Mask) - First);
    BOOLEAN         Partial = ((Byte | Length) & Mask) != 0;
    PUCHAR          Buffer;
    NTSTATUS        Status = STATUS_SUCCESS;

    Buffer = Ext2AllocatePool(NonPagedPoolNx, max(Span, PAGE_SIZE), FILE_DISK_TAG);   /* page aligned */
    if (Buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (Write && Partial) {
        KeEnterCriticalRegion();
        ExAcquireResourceExclusiveLite(&Disk->Partial, TRUE);
    }
    if (!Write || Partial) {
        Status = Ext4FileIo(Worker, FALSE, Buffer, Span, First);
    }
    if (NT_SUCCESS(Status)) {
        if (Write) {
            RtlCopyMemory(Buffer + (Byte - First), Data, Length);
            Status = Ext4FileIo(Worker, TRUE, Buffer, Span, First);
        } else {
            RtlCopyMemory(Data, Buffer + (Byte - First), Length);
        }
    }
    if (Write && Partial) {
        ExReleaseResourceLite(&Disk->Partial);
        KeLeaveCriticalRegion();
    }
    Ext2FreePool(Buffer, FILE_DISK_TAG);
    return Status;
}

static NTSTATUS
Ext4FileTransfer(IN PEXT4_FILE_WORKER Worker, IN PIRP Irp, IN BOOLEAN Write)
{
    PEXT4_FILE_DISK     Disk = Worker->Disk;
    PIO_STACK_LOCATION  Sp = IoGetCurrentIrpStackLocation(Irp);
    ULONGLONG           Byte = (ULONGLONG)Sp->Parameters.Read.ByteOffset.QuadPart;
    ULONG               Length = Sp->Parameters.Read.Length;
    PUCHAR              Data;

    Data = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, HighPagePriority | MdlMappingNoExecute);
    if (Data == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (((Byte | Length) & (Disk->SectorSize - 1)) != 0 || ((ULONG_PTR)Data & Disk->AlignMask) != 0) {
        return Ext4FileThroughBuffer(Worker, Write, Data, Length, Byte);
    }
    return Ext4FileIo(Worker, Write, Data, Length, Byte);
}

/* the ranges of a trim given back to the file's volume */
static NTSTATUS
Ext4FileTrim(IN PEXT4_FILE_WORKER Worker, IN PIRP Irp)
{
    PEXT4_FILE_DISK                     Disk = Worker->Disk;
    PDEVICE_MANAGE_DATA_SET_ATTRIBUTES  In = Irp->AssociatedIrp.SystemBuffer;
    PDEVICE_DATA_SET_RANGE              Range = (PDEVICE_DATA_SET_RANGE)((PUCHAR)In + In->DataSetRangesOffset);
    ULONG                               Count = In->DataSetRangesLength / sizeof(DEVICE_DATA_SET_RANGE);
    ULONG                               i;
    NTSTATUS                            Status = STATUS_SUCCESS;

    for (i = 0; i < Count && NT_SUCCESS(Status); i++) {
        FILE_ZERO_DATA_INFORMATION  Zero;
        IO_STATUS_BLOCK             Iosb;

        Zero.FileOffset.QuadPart = Range[i].StartingOffset;
        Zero.BeyondFinalZero.QuadPart = Range[i].StartingOffset + (LONGLONG)Range[i].LengthInBytes;
        Iosb.Status = STATUS_SUCCESS;
        Status = ZwFsControlFile(Disk->File, Worker->Event, NULL, NULL, &Iosb, FSCTL_SET_ZERO_DATA,
                                 &Zero, sizeof(Zero), NULL, 0);
        Status = Ext4FileFinish(Worker, Status, &Iosb);
    }
    return Status;
}

static VOID
Ext4FileWorkerThread(IN PVOID Context)
{
    PEXT4_FILE_WORKER   Worker = (PEXT4_FILE_WORKER)Context;
    PEXT4_FILE_DISK     Disk = Worker->Disk;

    for (;;) {
        PLIST_ENTRY         Entry = KeRemoveQueue(&Disk->Queue, KernelMode, NULL);
        PIRP                Irp;
        PIO_STACK_LOCATION  Sp;
        IO_STATUS_BLOCK     Iosb;
        NTSTATUS            Status;
        BOOLEAN             Stop = FALSE;
        ULONG               i;

        for (i = 0; i < Disk->Workers; i++) {
            Stop |= Entry == &Disk->Worker[i].Stop;
        }
        if (Stop) {
            break;
        }

        Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
        Sp = IoGetCurrentIrpStackLocation(Irp);
        switch (Sp->MajorFunction) {
        case IRP_MJ_READ:
        case IRP_MJ_WRITE:
            Status = Ext4FileTransfer(Worker, Irp, Sp->MajorFunction == IRP_MJ_WRITE);
            if (!NT_SUCCESS(Status)) {
                DbgPrint("ext4: %wZ %s of %lu bytes at %I64u failed (%xh)\n", &Disk->Name,
                         Sp->MajorFunction == IRP_MJ_WRITE ? "write" : "read",
                         Sp->Parameters.Read.Length, Sp->Parameters.Read.ByteOffset.QuadPart, Status);
            }
            Ext4DiskComplete(Irp, Status, NT_SUCCESS(Status) ? Sp->Parameters.Read.Length : 0);
            break;

        case IRP_MJ_FLUSH_BUFFERS:
            Ext4DiskComplete(Irp, ZwFlushBuffersFile(Disk->File, &Iosb), 0);
            break;

        default:
            Ext4DiskComplete(Irp, Ext4FileTrim(Worker, Irp), 0);
            break;
        }
        ExReleaseRundownProtection(&Disk->Rundown);
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* a request for the workers; the caller's status */
static NTSTATUS
Ext4FileQueue(IN PEXT4_FILE_DISK Disk, IN PIRP Irp)
{
    if (!ExAcquireRundownProtection(&Disk->Rundown)) {
        return Ext4DiskComplete(Irp, STATUS_DEVICE_DOES_NOT_EXIST, 0);
    }
    IoMarkIrpPending(Irp);
    KeInsertQueue(&Disk->Queue, &Irp->Tail.Overlay.ListEntry);
    return STATUS_PENDING;
}

/* a trim is taken, and ranges inside the device */
static NTSTATUS
Ext4FileCheckTrim(IN PEXT4_FILE_DISK Disk, IN PIRP Irp)
{
    PIO_STACK_LOCATION                  Sp = IoGetCurrentIrpStackLocation(Irp);
    ULONG                               Size = Sp->Parameters.DeviceIoControl.InputBufferLength;
    PDEVICE_MANAGE_DATA_SET_ATTRIBUTES  In = Irp->AssociatedIrp.SystemBuffer;
    PDEVICE_DATA_SET_RANGE              Range;
    ULONG                               i;

    if (IsFlagOn(Disk->Flags, EXT4_CRYPT_READ_ONLY)) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }
    if (In == NULL || Size < sizeof(DEVICE_MANAGE_DATA_SET_ATTRIBUTES) ||
        In->Size < sizeof(DEVICE_MANAGE_DATA_SET_ATTRIBUTES) ||
        In->DataSetRangesOffset > Size || In->DataSetRangesLength > Size - In->DataSetRangesOffset) {
        return STATUS_INVALID_PARAMETER;
    }
    if ((In->Action & ~DeviceDsmActionFlag_NonDestructive) != DeviceDsmAction_Trim) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    if (!Disk->Sparse) {
        return STATUS_NOT_SUPPORTED;        /* nothing to give back: it would only write zeros */
    }
    Range = (PDEVICE_DATA_SET_RANGE)((PUCHAR)In + In->DataSetRangesOffset);
    for (i = 0; i < In->DataSetRangesLength / sizeof(DEVICE_DATA_SET_RANGE); i++) {
        if (Range[i].StartingOffset < 0 || (ULONGLONG)Range[i].StartingOffset > Disk->Size ||
            Range[i].LengthInBytes > Disk->Size - (ULONGLONG)Range[i].StartingOffset) {
            return STATUS_INVALID_PARAMETER;
        }
    }
    return STATUS_SUCCESS;
}

/* Every IRP sent to a file's device: through Ext2BuildRequest, before any of
   the file system's own machinery runs. */
NTSTATUS
Ext4FileDispatch(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp)
{
    PEXT4_FILE_DISK     Disk = (PEXT4_FILE_DISK)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION  Sp = IoGetCurrentIrpStackLocation(Irp);
    BOOLEAN             ReadOnly = IsFlagOn(Disk->Flags, EXT4_CRYPT_READ_ONLY);

    switch (Sp->MajorFunction) {

    case IRP_MJ_CREATE:
        /* the device itself only: files live on the file system above */
        if (Sp->FileObject && Sp->FileObject->FileName.Length != 0) {
            return Ext4DiskComplete(Irp, STATUS_OBJECT_PATH_NOT_FOUND, 0);
        }
        return Ext4DiskComplete(Irp, Disk->Closing ? STATUS_DEVICE_DOES_NOT_EXIST : STATUS_SUCCESS, 0);

    case IRP_MJ_CLOSE:
    case IRP_MJ_CLEANUP:
        return Ext4DiskComplete(Irp, STATUS_SUCCESS, 0);

    case IRP_MJ_READ:
    case IRP_MJ_WRITE: {
        ULONGLONG   Byte = (ULONGLONG)Sp->Parameters.Read.ByteOffset.QuadPart;
        ULONG       Length = Sp->Parameters.Read.Length;

        if (Disk->Closing) {
            return Ext4DiskComplete(Irp, STATUS_DEVICE_DOES_NOT_EXIST, 0);
        }
        if (Sp->MajorFunction == IRP_MJ_WRITE && ReadOnly) {
            return Ext4DiskComplete(Irp, STATUS_MEDIA_WRITE_PROTECTED, 0);
        }
        if (Length == 0) {
            return Ext4DiskComplete(Irp, STATUS_SUCCESS, 0);
        }
        if (Sp->Parameters.Read.ByteOffset.QuadPart < 0 || ((Byte | Length) & (EXT4_SECTOR - 1)) != 0 ||
            Irp->MdlAddress == NULL) {
            return Ext4DiskComplete(Irp, STATUS_INVALID_PARAMETER, 0);
        }
        if (Byte >= Disk->Size || Length > Disk->Size - Byte) {
            return Ext4DiskComplete(Irp, Sp->MajorFunction == IRP_MJ_READ ? STATUS_END_OF_FILE :
                                                                            STATUS_DISK_FULL, 0);
        }
        return Ext4FileQueue(Disk, Irp);
    }

    case IRP_MJ_FLUSH_BUFFERS:
        return ReadOnly ? Ext4DiskComplete(Irp, STATUS_SUCCESS, 0) : Ext4FileQueue(Disk, Irp);

    case IRP_MJ_DEVICE_CONTROL:
    case IRP_MJ_INTERNAL_DEVICE_CONTROL:
        if (Sp->Parameters.DeviceIoControl.IoControlCode == IOCTL_STORAGE_MANAGE_DATA_SET_ATTRIBUTES) {
            NTSTATUS Status = Ext4FileCheckTrim(Disk, Irp);
            return NT_SUCCESS(Status) ? Ext4FileQueue(Disk, Irp) : Ext4DiskComplete(Irp, Status, 0);
        }
        return Ext4DiskIoctl(Irp, &Disk->Name, FILE_DISK_UNIQUE_PREFIX, Disk->Id, Disk->Size, ReadOnly);

    case IRP_MJ_PNP: {
        /* no PnP node: whatever the file system above forwards (target device
           relations for a notification) stays unanswered */
        NTSTATUS Status = Irp->IoStatus.Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    default:
        return Ext4DiskComplete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

/* ---------------------------------------------------------------- life cycle */

static VOID
Ext4FileStopWorkers(IN PEXT4_FILE_DISK Disk)
{
    ULONG i;

    for (i = 0; i < Disk->Workers; i++) {
        KeInsertQueue(&Disk->Queue, &Disk->Worker[i].Stop);
    }
    for (i = 0; i < Disk->Workers; i++) {
        KeWaitForSingleObject(Disk->Worker[i].Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Disk->Worker[i].Thread);
    }
    Disk->Workers = 0;
    KeRundownQueue(&Disk->Queue);
    for (i = 0; i < FILE_DISK_WORKERS; i++) {
        if (Disk->Worker[i].Event != NULL) {
            ZwClose(Disk->Worker[i].Event);
            Disk->Worker[i].Event = NULL;
        }
    }
}

static NTSTATUS
Ext4FileStartWorkers(IN PEXT4_FILE_DISK Disk)
{
    ULONG               Count = min(KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS), FILE_DISK_WORKERS);
    OBJECT_ATTRIBUTES   Attributes;
    NTSTATUS            Status = STATUS_SUCCESS;
    ULONG               i;

    InitializeObjectAttributes(&Attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    for (i = 0; i < Count; i++) {
        PEXT4_FILE_WORKER   Worker = &Disk->Worker[i];
        HANDLE              Handle;

        Worker->Disk = Disk;
        Status = ZwCreateEvent(&Worker->Event, EVENT_ALL_ACCESS, &Attributes, SynchronizationEvent, FALSE);
        if (!NT_SUCCESS(Status)) {
            Worker->Event = NULL;
            break;
        }
        Status = PsCreateSystemThread(&Handle, THREAD_ALL_ACCESS, &Attributes, NULL, NULL,
                                      Ext4FileWorkerThread, Worker);
        if (!NT_SUCCESS(Status)) {
            break;
        }
        Status = ObReferenceObjectByHandle(Handle, SYNCHRONIZE, *PsThreadType, KernelMode,
                                           (PVOID *)&Worker->Thread, NULL);
        ZwClose(Handle);
        ASSERT(NT_SUCCESS(Status));     /* a handle just got, to a thread just made */
        Disk->Workers++;
    }
    return Status;
}

/*
 * The file, with what the device needs to know of it. Opened on the caller's
 * thread, with its rights: \??\ is its name space, and what it may not open
 * stays closed. Writers are shut out for as long as it is mounted; readers
 * are not - fsutil sparse queryrange must be able to look.
 */
static NTSTATUS
Ext4FileOpenBacking(IN PEXT4_FILE_OPEN In, OUT PHANDLE File, OUT PEXT4_FILE_DISK Disk)
{
    BOOLEAN                     ReadOnly = IsFlagOn(In->Flags, EXT4_CRYPT_READ_ONLY);
    UNICODE_STRING              Path;
    OBJECT_ATTRIBUTES           Attributes;
    IO_STATUS_BLOCK             Iosb;
    FILE_STANDARD_INFORMATION   Standard = { 0 };
    FILE_BASIC_INFORMATION      Basic = { 0 };
    FILE_INTERNAL_INFORMATION   Internal = { 0 };
    FILE_FS_SIZE_INFORMATION    FsSize = { 0 };
    FILE_FS_VOLUME_INFORMATION  FsVolume = { 0 };
    PFILE_OBJECT                Object;
    PDEVICE_OBJECT              Base;
    NTSTATUS                    Status;

    RtlInitUnicodeString(&Path, In->Path);
    InitializeObjectAttributes(&Attributes, &Path,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE | OBJ_FORCE_ACCESS_CHECK, NULL, NULL);
    Status = ZwCreateFile(File, FILE_READ_DATA | FILE_READ_ATTRIBUTES | (ReadOnly ? 0 : FILE_WRITE_DATA),
                          &Attributes, &Iosb, NULL, 0, FILE_SHARE_READ, FILE_OPEN,
                          FILE_NON_DIRECTORY_FILE | FILE_NO_INTERMEDIATE_BUFFERING, NULL, 0);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    /* never a file on a volume of ours */
    Status = ObReferenceObjectByHandle(*File, 0, *IoFileObjectType, KernelMode, (PVOID *)&Object, NULL);
    if (NT_SUCCESS(Status)) {
        Base = IoGetBaseFileSystemDeviceObject(Object);
        if (Base->DriverObject == Ext2Global->DiskdevObject->DriverObject) {
            Status = STATUS_INVALID_DEVICE_REQUEST;
        }
        Disk->AlignMask = Object->DeviceObject->AlignmentRequirement;
        ObDereferenceObject(Object);
    }

    if (NT_SUCCESS(Status)) {
        Status = ZwQueryInformationFile(*File, &Iosb, &Standard, sizeof(Standard), FileStandardInformation);
    }
    if (NT_SUCCESS(Status)) {
        Status = ZwQueryInformationFile(*File, &Iosb, &Basic, sizeof(Basic), FileBasicInformation);
    }
    if (NT_SUCCESS(Status)) {
        Status = ZwQueryInformationFile(*File, &Iosb, &Internal, sizeof(Internal), FileInternalInformation);
    }
    if (NT_SUCCESS(Status)) {
        Status = ZwQueryVolumeInformationFile(*File, &Iosb, &FsSize, sizeof(FsSize), FileFsSizeInformation);
    }
    if (NT_SUCCESS(Status)) {
        /* the label does not fit, which is all right: the serial is in the part that does */
        Status = ZwQueryVolumeInformationFile(*File, &Iosb, &FsVolume, sizeof(FsVolume), FileFsVolumeInformation);
        if (Status == STATUS_BUFFER_OVERFLOW) {
            Status = STATUS_SUCCESS;
        }
    }
    if (NT_SUCCESS(Status)) {
        Disk->SectorSize = max(FsSize.BytesPerSector, EXT4_SECTOR);
        Disk->Size = (ULONGLONG)Standard.EndOfFile.QuadPart & ~((ULONGLONG)Disk->SectorSize - 1);
        Disk->Sparse = IsFlagOn(Basic.FileAttributes, FILE_ATTRIBUTE_SPARSE_FILE);
        if (Disk->Size == 0 || (Disk->SectorSize & (Disk->SectorSize - 1)) != 0) {
            Status = STATUS_INVALID_PARAMETER;
        }
        /* the same file is the same volume at every mount: the mount manager
           remembers its letter */
        RtlStringCbPrintfA(Disk->Id, sizeof(Disk->Id), "%08x-%016I64x", FsVolume.VolumeSerialNumber,
                           Internal.IndexNumber.QuadPart);
    }
    if (!NT_SUCCESS(Status)) {
        ZwClose(*File);
        *File = NULL;
    }
    return Status;
}

NTSTATUS
Ext4FileOpen(IN OUT PEXT4_FILE_OPEN In)
{
    PEXT4_FILE_DISK Disk;
    PDEVICE_OBJECT  Device;
    PLIST_ENTRY     List;
    UNICODE_STRING  Name;
    WCHAR           NameBuffer[RTL_FIELD_SIZE(EXT4_FILE_DISK, NameBuffer) / sizeof(WCHAR)];
    ULONG           Count = 0;
    NTSTATUS        Status;

    Ext4FileInitialize();
    if (In->Version != EXT4_CRYPT_VERSION || (In->Flags & ~EXT4_CRYPT_READ_ONLY) != 0 ||
        wcsnlen(In->Path, EXT4_FILE_PATH_CHARS) >= EXT4_FILE_PATH_CHARS) {
        return STATUS_INVALID_PARAMETER;
    }
    ExAcquireFastMutex(&Ext4FileListLock);
    for (List = Ext4FileList.Flink; List != &Ext4FileList; List = List->Flink) {
        Count++;
    }
    ExReleaseFastMutex(&Ext4FileListLock);
    if (Count >= EXT4_FILE_MAX_DISKS) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    In->Index = (ULONG)InterlockedIncrement((PLONG)&Ext4FileNext) - 1;
    RtlStringCbPrintfW(NameBuffer, sizeof(NameBuffer), EXT4_FILE_DEVICE_PREFIX L"%u", In->Index);
    RtlInitUnicodeString(&Name, NameBuffer);
    Status = IoCreateDeviceSecure(Ext2Global->DiskdevObject->DriverObject, sizeof(EXT4_FILE_DISK), &Name,
                                  FILE_DEVICE_DISK, 0, FALSE, &SDDL_DEVOBJ_SYS_ALL_ADM_ALL, NULL, &Device);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Disk = (PEXT4_FILE_DISK)Device->DeviceExtension;
    RtlZeroMemory(Disk, sizeof(EXT4_FILE_DISK));
    Status = Ext4FileOpenBacking(In, &Disk->File, Disk);
    if (!NT_SUCCESS(Status)) {
        IoDeleteDevice(Device);
        return Status;
    }
    Disk->Identifier.Type = EXT4FIL;
    Disk->Identifier.Size = sizeof(EXT4_FILE_DISK);
    Disk->Self = Device;
    Disk->Number = In->Index;
    Disk->Flags = In->Flags;
    RtlStringCbCopyW(Disk->NameBuffer, sizeof(Disk->NameBuffer), NameBuffer);
    RtlInitUnicodeString(&Disk->Name, Disk->NameBuffer);
    RtlStringCbCopyW(Disk->Path, sizeof(Disk->Path), In->Path);
    KeInitializeQueue(&Disk->Queue, 0);
    ExInitializeRundownProtection(&Disk->Rundown);
    ExInitializeResourceLite(&Disk->Partial);

    Status = Ext4FileStartWorkers(Disk);
    if (!NT_SUCCESS(Status)) {
        Ext4FileStopWorkers(Disk);
        ExDeleteResourceLite(&Disk->Partial);
        ZwClose(Disk->File);
        IoDeleteDevice(Device);
        return Status;
    }

    SetFlag(Device->Flags, DO_DIRECT_IO);
    Device->AlignmentRequirement = Disk->AlignMask;
    Device->SectorSize = EXT4_SECTOR;           /* 512e, see IOCTL_DISK_GET_DRIVE_GEOMETRY */
    ClearFlag(Device->Flags, DO_DEVICE_INITIALIZING);

    ExAcquireFastMutex(&Ext4FileListLock);
    InsertTailList(&Ext4FileList, &Disk->Link);
    ExReleaseFastMutex(&Ext4FileListLock);

    /* links from the mount manager, the letter asked for, and a first open on
       a worker: that is when the file system mounts */
    Ext4ForgetDevice(&Disk->Name);
    Status = Ext4AnnounceDevice(&Disk->Name);
    if (!NT_SUCCESS(Status)) {
        DbgPrint("ext4: the mount manager did not take %wZ (%xh)\n", &Disk->Name, Status);
    }
    if (In->Letter) {
        Ext4SetDeviceLetter(&Disk->Name, In->Letter);
    }
    Ext2QueueVolumeProbe(&Disk->Name);

    In->Size = Disk->Size;
    In->Sparse = Disk->Sparse;
    return STATUS_SUCCESS;
}

static NTSTATUS
Ext4FileCloseDisk(IN PEXT4_FILE_DISK Disk, IN BOOLEAN Force)
{
    IO_STATUS_BLOCK Iosb;
    NTSTATUS        Status;

    Status = Ext4DismountDevice(Disk->Self, &Disk->Name, Force);
    if (!NT_SUCCESS(Status) && !Force) {
        return Status;
    }
    Ext4ForgetDevice(&Disk->Name);

    /* no new I/O; the I/O in flight finishes */
    InterlockedExchange(&Disk->Closing, TRUE);
    ExWaitForRundownProtectionRelease(&Disk->Rundown);
    Ext4FileStopWorkers(Disk);

    /* what the file's own volume holds of it reaches its disk, and it is
       free for anybody once more */
    if (!IsFlagOn(Disk->Flags, EXT4_CRYPT_READ_ONLY)) {
        (void)ZwFlushBuffersFile(Disk->File, &Iosb);
    }
    ZwClose(Disk->File);
    Disk->File = NULL;
    ExDeleteResourceLite(&Disk->Partial);

    ExAcquireFastMutex(&Ext4FileListLock);
    RemoveEntryList(&Disk->Link);
    ExReleaseFastMutex(&Ext4FileListLock);

    DbgPrint("ext4: %wZ closed (%ws)\n", &Disk->Name, Disk->Path);

    /* a file system still being torn down keeps the device object (not the
       device) until it lets go; Closing refuses its I/O */
    IoDeleteDevice(Disk->Self);
    return STATUS_SUCCESS;
}

NTSTATUS
Ext4FileClose(IN PEXT4_FILE_CLOSE In)
{
    PEXT4_FILE_DISK Found = NULL;
    PLIST_ENTRY     List;

    Ext4FileInitialize();
    ExAcquireFastMutex(&Ext4FileListLock);
    for (List = Ext4FileList.Flink; List != &Ext4FileList; List = List->Flink) {
        PEXT4_FILE_DISK Disk = CONTAINING_RECORD(List, EXT4_FILE_DISK, Link);
        if (Disk->Number == In->Index) {
            Found = Disk;
            break;
        }
    }
    ExReleaseFastMutex(&Ext4FileListLock);
    return Found ? Ext4FileCloseDisk(Found, IsFlagOn(In->Flags, EXT4_CRYPT_FORCE)) : STATUS_NOT_FOUND;
}

NTSTATUS
Ext4FileQuery(OUT PEXT4_FILE_QUERY Out)
{
    PLIST_ENTRY List;

    Ext4FileInitialize();
    Out->Count = 0;
    ExAcquireFastMutex(&Ext4FileListLock);
    for (List = Ext4FileList.Flink;
         List != &Ext4FileList && Out->Count < EXT4_FILE_MAX_DISKS;
         List = List->Flink) {
        PEXT4_FILE_DISK     Disk = CONTAINING_RECORD(List, EXT4_FILE_DISK, Link);
        PEXT4_FILE_ENTRY    Entry = &Out->Entries[Out->Count++];

        RtlZeroMemory(Entry, sizeof(*Entry));
        Entry->Index = Disk->Number;
        Entry->Flags = Disk->Flags;
        Entry->Sparse = Disk->Sparse;
        Entry->Size = Disk->Size;
        RtlStringCbCopyW(Entry->Path, sizeof(Entry->Path), Disk->Path);
    }
    ExReleaseFastMutex(&Ext4FileListLock);
    return STATUS_SUCCESS;
}

/* driver unload, after the file systems have let go: every file goes */
VOID
Ext4FileTeardownAll(VOID)
{
    if (!Ext4FileReady) {
        return;
    }
    for (;;) {
        PEXT4_FILE_DISK Disk = NULL;

        ExAcquireFastMutex(&Ext4FileListLock);
        if (!IsListEmpty(&Ext4FileList)) {
            Disk = CONTAINING_RECORD(Ext4FileList.Flink, EXT4_FILE_DISK, Link);
        }
        ExReleaseFastMutex(&Ext4FileListLock);
        if (Disk == NULL) {
            break;
        }
        (void)Ext4FileCloseDisk(Disk, TRUE);
    }
}
