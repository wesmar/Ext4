/**
 * LvmVolume.c - LVM logical volumes inside an unlocked LUKS volume, read-only.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Qubes OS, and many Linux installations with full-disk encryption, put LVM
 * inside LUKS: the root and every qube's disks are thin volumes in a pool.
 * ext4ctl reads the LVM text metadata and the dm-thin mapping btree in user
 * mode and sends the volume as a table of runs over the LUKS device (a run
 * is either a piece of the device or a hole that reads as zeros). This
 * module only remaps:
 *
 *     ext4 volume (read-only)
 *         \Device\Ext4Lv<n>         run table, binary search per request
 *             \Device\Ext4Crypt<m>  (LuksVolume.c)
 *
 * Read-only by construction - writes are refused and the device reports
 * itself write-protected, so the file system mounts read-only: a write to a
 * thin volume would have to allocate chunks and update the pool metadata,
 * which is dm-thin's job, and a table taken at open time would go stale.
 *
 * A read is cut at run boundaries; each piece is an IRP of its own to the
 * LUKS device over a partial MDL of the caller's buffer, all sent at once,
 * and the last one to complete completes the caller's request. Holes are
 * zero-filled in place. Nothing here waits, so it all runs at the caller's
 * IRQL.
 *
 * Opening, closing and querying run under the LUKS control lock (LuksVolume.c):
 * a LUKS device is locked only after the volumes read through it.
 */

#include "ext4fs.h"
#include <wdmsec.h>
#include <ntstrsafe.h>
#include "ext4crypt.h"

#define LV_TAG                  'VL4E'
#define LV_UNIQUE_PREFIX        "EXT4-LV-"

typedef struct _EXT4_LV_DEVICE {
    EXT2_IDENTIFIER     Identifier;         /* EXT4LVD */
    LIST_ENTRY          Link;
    PDEVICE_OBJECT      Self;
    PDEVICE_OBJECT      Lower;              /* the LUKS device, referenced */
    ULONG               Number;             /* \Device\Ext4Lv<Number>, never reused */
    ULONG               Crypt;              /* \Device\Ext4Crypt<Crypt> */
    ULONGLONG           Size;
    ULONG               Runs;
    PEXT4_LV_RUN        Run;                /* sorted, contiguous from 0 to Size */
    EX_RUNDOWN_REF      Rundown;            /* reads in flight */
    volatile LONG       Closing;
    UNICODE_STRING      Name;
    WCHAR               NameBuffer[48];
    CHAR                Uuid[EXT4_CRYPT_UUID_CHARS];
    CHAR                VolumeName[EXT4_LV_NAME_CHARS];
} EXT4_LV_DEVICE, *PEXT4_LV_DEVICE;

/* one read of the file system above, while its pieces are out */
typedef struct _EXT4_LV_READ {
    PEXT4_LV_DEVICE     Lv;
    PIRP                Irp;
    volatile LONG       Pending;            /* pieces out, plus one while they are sent */
    volatile LONG       Status;             /* the first failure */
} EXT4_LV_READ, *PEXT4_LV_READ;

static LIST_ENTRY       Ext4LvList;
static FAST_MUTEX       Ext4LvListLock;
static ULONG            Ext4LvNext;
static BOOLEAN          Ext4LvReady;

static IO_COMPLETION_ROUTINE Ext4LvPieceDone;

static VOID
Ext4LvInitialize(VOID)
{
    if (!Ext4LvReady) {
        InitializeListHead(&Ext4LvList);
        ExInitializeFastMutex(&Ext4LvListLock);
        Ext4LvReady = TRUE;
    }
}

BOOLEAN
Ext4IsLvDevice(IN PDEVICE_OBJECT DeviceObject)
{
    PEXT4_LV_DEVICE Lv;

    if (DeviceObject == NULL || IsExt2FsDevice(DeviceObject)) {
        return FALSE;
    }
    Lv = (PEXT4_LV_DEVICE)DeviceObject->DeviceExtension;
    return Lv != NULL &&
           Lv->Identifier.Type == EXT4LVD &&
           Lv->Identifier.Size == sizeof(EXT4_LV_DEVICE);
}

/* ---------------------------------------------------------------- reads */

static VOID
Ext4LvReadDrop(IN PEXT4_LV_READ R)
{
    if (InterlockedDecrement(&R->Pending) == 0) {
        PEXT4_LV_DEVICE Lv = R->Lv;
        PIRP            Irp = R->Irp;
        NTSTATUS        Status = R->Status;

        Ext2FreePool(R, LV_TAG);
        Ext4DiskComplete(Irp, Status, NT_SUCCESS(Status) ?
                         IoGetCurrentIrpStackLocation(Irp)->Parameters.Read.Length : 0);
        ExReleaseRundownProtection(&Lv->Rundown);
    }
}

static NTSTATUS
Ext4LvPieceDone(IN PDEVICE_OBJECT DeviceObject, IN PIRP Piece, IN PVOID Context)
{
    PEXT4_LV_READ   R = (PEXT4_LV_READ)Context;
    NTSTATUS        Status = Piece->IoStatus.Status;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!NT_SUCCESS(Status)) {
        InterlockedCompareExchange(&R->Status, Status, STATUS_SUCCESS);
    }
    IoFreeMdl(Piece->MdlAddress);
    IoFreeIrp(Piece);
    Ext4LvReadDrop(R);
    return STATUS_MORE_PROCESSING_REQUIRED;     /* the piece is freed */
}

/* the run holding Byte (runs are contiguous from 0 to Size) */
static ULONG
Ext4LvFindRun(IN PEXT4_LV_DEVICE Lv, IN ULONGLONG Byte)
{
    ULONG Low = 0, High = Lv->Runs - 1;

    while (Low < High) {
        ULONG Mid = Low + (High - Low + 1) / 2;
        if (Lv->Run[Mid].Start <= Byte) {
            Low = Mid;
        } else {
            High = Mid - 1;
        }
    }
    return Low;
}

static NTSTATUS
Ext4LvRead(IN PEXT4_LV_DEVICE Lv, IN PIRP Irp, IN ULONGLONG Byte, IN ULONG Length)
{
    PEXT4_LV_READ   R;
    PUCHAR          Va = (PUCHAR)MmGetMdlVirtualAddress(Irp->MdlAddress);
    PUCHAR          User = NULL;
    ULONG           Done = 0;
    ULONG           i;

    R = Ext2AllocatePool(NonPagedPoolNx, sizeof(EXT4_LV_READ), LV_TAG);
    if (R == NULL) {
        ExReleaseRundownProtection(&Lv->Rundown);
        return Ext4DiskComplete(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    }
    R->Lv = Lv;
    R->Irp = Irp;
    R->Pending = 1;
    R->Status = STATUS_SUCCESS;
    IoMarkIrpPending(Irp);

    for (i = Ext4LvFindRun(Lv, Byte); Done < Length && i < Lv->Runs; i++) {
        PEXT4_LV_RUN    Run = &Lv->Run[i];
        ULONGLONG       Within = Byte + Done - Run->Start;
        ULONG           n = (ULONG)min(Run->Length - Within, (ULONGLONG)(Length - Done));

        if (Run->Target == EXT4_LV_HOLE) {
            /* never written in the pool: zeros, as dm-thin returns */
            if (User == NULL) {
                User = MmGetSystemAddressForMdlSafe(Irp->MdlAddress,
                                                    HighPagePriority | MdlMappingNoExecute);
            }
            if (User == NULL) {
                InterlockedCompareExchange(&R->Status, STATUS_INSUFFICIENT_RESOURCES, STATUS_SUCCESS);
                break;
            }
            RtlZeroMemory(User + Done, n);
        } else {
            PIRP                Piece = IoAllocateIrp(Lv->Lower->StackSize, FALSE);
            PMDL                Mdl = Piece ? IoAllocateMdl(Va + Done, n, FALSE, FALSE, NULL) : NULL;
            PIO_STACK_LOCATION  Sp;

            if (Mdl == NULL) {
                if (Piece) {
                    IoFreeIrp(Piece);
                }
                InterlockedCompareExchange(&R->Status, STATUS_INSUFFICIENT_RESOURCES, STATUS_SUCCESS);
                break;
            }
            IoBuildPartialMdl(Irp->MdlAddress, Mdl, Va + Done, n);
            Piece->MdlAddress = Mdl;
            Piece->UserBuffer = Va + Done;
            Piece->Flags = IRP_NOCACHE;
            Piece->RequestorMode = KernelMode;
            Piece->Tail.Overlay.Thread = PsGetCurrentThread();
            Sp = IoGetNextIrpStackLocation(Piece);
            Sp->MajorFunction = IRP_MJ_READ;
            Sp->Flags = SL_OVERRIDE_VERIFY_VOLUME;
            Sp->Parameters.Read.Length = n;
            Sp->Parameters.Read.ByteOffset.QuadPart = (LONGLONG)(Run->Target + Within);
            IoSetCompletionRoutine(Piece, Ext4LvPieceDone, R, TRUE, TRUE, TRUE);
            InterlockedIncrement(&R->Pending);
            (void)IoCallDriver(Lv->Lower, Piece);
        }
        Done += n;
    }
    if (Done < Length) {
        InterlockedCompareExchange(&R->Status, STATUS_INSUFFICIENT_RESOURCES, STATUS_SUCCESS);
    }

    Ext4LvReadDrop(R);                      /* the one held while sending */
    return STATUS_PENDING;
}

NTSTATUS
Ext4LvDispatch(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp)
{
    PEXT4_LV_DEVICE     Lv = (PEXT4_LV_DEVICE)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION  IrpSp = IoGetCurrentIrpStackLocation(Irp);

    switch (IrpSp->MajorFunction) {

    case IRP_MJ_CREATE:
        if (IrpSp->FileObject && IrpSp->FileObject->FileName.Length != 0) {
            return Ext4DiskComplete(Irp, STATUS_OBJECT_PATH_NOT_FOUND, 0);
        }
        return Ext4DiskComplete(Irp, Lv->Closing ? STATUS_DEVICE_DOES_NOT_EXIST :
                                                   STATUS_SUCCESS, 0);

    case IRP_MJ_CLOSE:
    case IRP_MJ_CLEANUP:
    case IRP_MJ_FLUSH_BUFFERS:
        return Ext4DiskComplete(Irp, STATUS_SUCCESS, 0);

    case IRP_MJ_WRITE:
        return Ext4DiskComplete(Irp, STATUS_MEDIA_WRITE_PROTECTED, 0);

    case IRP_MJ_READ: {
        ULONGLONG   Byte = (ULONGLONG)IrpSp->Parameters.Read.ByteOffset.QuadPart;
        ULONG       Length = IrpSp->Parameters.Read.Length;

        if (Lv->Closing) {
            return Ext4DiskComplete(Irp, STATUS_DEVICE_DOES_NOT_EXIST, 0);
        }
        if (Length == 0) {
            return Ext4DiskComplete(Irp, STATUS_SUCCESS, 0);
        }
        if (IrpSp->Parameters.Read.ByteOffset.QuadPart < 0 ||
            ((Byte | Length) & (EXT4_SECTOR - 1)) != 0 || Irp->MdlAddress == NULL) {
            return Ext4DiskComplete(Irp, STATUS_INVALID_PARAMETER, 0);
        }
        if (Byte >= Lv->Size || Length > Lv->Size - Byte) {
            return Ext4DiskComplete(Irp, STATUS_END_OF_FILE, 0);
        }
        if (!ExAcquireRundownProtection(&Lv->Rundown)) {
            return Ext4DiskComplete(Irp, STATUS_DEVICE_DOES_NOT_EXIST, 0);
        }
        return Ext4LvRead(Lv, Irp, Byte, Length);
    }

    case IRP_MJ_DEVICE_CONTROL:
    case IRP_MJ_INTERNAL_DEVICE_CONTROL:
        return Ext4DiskIoctl(Irp, &Lv->Name, LV_UNIQUE_PREFIX, Lv->Uuid, Lv->Size, TRUE);

    case IRP_MJ_PNP: {
        NTSTATUS Status = Irp->IoStatus.Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }

    default:
        return Ext4DiskComplete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

/* ---------------------------------------------------------------- life cycle */

static PEXT4_LV_DEVICE
Ext4LvFind(IN ULONG Number)
{
    PLIST_ENTRY     List;
    PEXT4_LV_DEVICE Found = NULL;

    ExAcquireFastMutex(&Ext4LvListLock);
    for (List = Ext4LvList.Flink; List != &Ext4LvList; List = List->Flink) {
        PEXT4_LV_DEVICE Lv = CONTAINING_RECORD(List, EXT4_LV_DEVICE, Link);
        if (Lv->Number == Number) {
            Found = Lv;
            break;
        }
    }
    ExReleaseFastMutex(&Ext4LvListLock);
    return Found;
}

static NTSTATUS
Ext4LvCloseDevice(IN PEXT4_LV_DEVICE Lv, IN BOOLEAN Force)
{
    NTSTATUS Status = Ext4DismountDevice(Lv->Self, &Lv->Name, Force);

    if (!NT_SUCCESS(Status) && !Force) {
        return Status;
    }
    Ext4ForgetDevice(&Lv->Name);

    InterlockedExchange(&Lv->Closing, TRUE);
    ExWaitForRundownProtectionRelease(&Lv->Rundown);

    ExAcquireFastMutex(&Ext4LvListLock);
    RemoveEntryList(&Lv->Link);
    ExReleaseFastMutex(&Ext4LvListLock);

    DbgPrint("ext4: %wZ closed (%s)\n", &Lv->Name, Lv->VolumeName);

    ObDereferenceObject(Lv->Lower);
    Lv->Lower = NULL;
    Ext2FreePool(Lv->Run, LV_TAG);
    Lv->Run = NULL;
    Lv->Runs = 0;
    IoDeleteDevice(Lv->Self);
    return STATUS_SUCCESS;
}

/*
 * The table from ext4ctl, checked entry by entry: it starts at 0, runs are
 * contiguous, sector-aligned and end at Size, targets lie on the device.
 */
static NTSTATUS
Ext4LvCheckRuns(IN const EXT4_LV_OPEN *In, IN ULONGLONG LowerSize)
{
    ULONGLONG   Next = 0;
    ULONG       i;

    for (i = 0; i < In->Runs; i++) {
        const EXT4_LV_RUN *Run = &In->Run[i];

        if (Run->Start != Next || Run->Length == 0 ||
            ((Run->Start | Run->Length) & (EXT4_SECTOR - 1)) != 0 ||
            Run->Length > In->Size - Run->Start) {
            return STATUS_INVALID_PARAMETER;
        }
        if (Run->Target != EXT4_LV_HOLE &&
            ((Run->Target & (EXT4_SECTOR - 1)) != 0 || Run->Target >= LowerSize ||
             Run->Length > LowerSize - Run->Target)) {
            return STATUS_INVALID_PARAMETER;
        }
        Next = Run->Start + Run->Length;
    }
    return Next == In->Size ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

/* the request, field by field, InLength included */
static BOOLEAN
Ext4LvRequestValid(IN PEXT4_LV_OPEN In, IN ULONG InLength)
{
    return InLength >= (ULONG)FIELD_OFFSET(EXT4_LV_OPEN, Run) &&
           In->Version == EXT4_CRYPT_VERSION && In->Runs != 0 && In->Runs <= EXT4_LV_MAX_RUNS &&
           InLength >= (SIZE_T)FIELD_OFFSET(EXT4_LV_OPEN, Run) + (SIZE_T)In->Runs * sizeof(EXT4_LV_RUN) &&
           In->Size != 0 && (In->Size & (EXT4_SECTOR - 1)) == 0 &&
           strnlen(In->Uuid, sizeof(In->Uuid)) < sizeof(In->Uuid) &&
           strnlen(In->Name, sizeof(In->Name)) < sizeof(In->Name);
}

/* one device per logical volume, and no more than the table holds */
static NTSTATUS
Ext4LvRoomFor(IN PCSTR Uuid)
{
    PLIST_ENTRY List;
    ULONG       Count = 0;
    NTSTATUS    Status = STATUS_SUCCESS;

    ExAcquireFastMutex(&Ext4LvListLock);
    for (List = Ext4LvList.Flink; List != &Ext4LvList; List = List->Flink) {
        PEXT4_LV_DEVICE Other = CONTAINING_RECORD(List, EXT4_LV_DEVICE, Link);
        if (strcmp(Other->Uuid, Uuid) == 0) {
            Status = STATUS_OBJECT_NAME_COLLISION;
        }
        Count++;
    }
    ExReleaseFastMutex(&Ext4LvListLock);
    if (NT_SUCCESS(Status) && Count >= EXT4_LV_MAX_VOLUMES) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
    }
    return Status;
}

/*
 * The read-only disk device of the volume, its run table copied from the
 * request; for SYSTEM and administrators only. On success the device owns
 * the reference on Lower.
 */
static NTSTATUS
Ext4LvCreateDevice(IN PEXT4_LV_OPEN In, IN PDEVICE_OBJECT Lower, OUT PEXT4_LV_DEVICE *Out)
{
    PEXT4_LV_DEVICE Lv;
    PDEVICE_OBJECT  Device;
    UNICODE_STRING  Name;
    WCHAR           NameBuffer[RTL_FIELD_SIZE(EXT4_LV_DEVICE, NameBuffer) / sizeof(WCHAR)];
    ULONG           Number;
    SIZE_T          TableBytes = (SIZE_T)In->Runs * sizeof(EXT4_LV_RUN);
    NTSTATUS        Status;

    Number = (ULONG)InterlockedIncrement((PLONG)&Ext4LvNext) - 1;
    RtlStringCbPrintfW(NameBuffer, sizeof(NameBuffer), EXT4_LV_DEVICE_PREFIX L"%u", Number);
    RtlInitUnicodeString(&Name, NameBuffer);
    Status = IoCreateDeviceSecure(Ext2Global->DiskdevObject->DriverObject,
                                  sizeof(EXT4_LV_DEVICE), &Name, FILE_DEVICE_DISK,
                                  FILE_READ_ONLY_DEVICE, FALSE,
                                  &SDDL_DEVOBJ_SYS_ALL_ADM_ALL, NULL, &Device);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Lv = (PEXT4_LV_DEVICE)Device->DeviceExtension;
    RtlZeroMemory(Lv, sizeof(EXT4_LV_DEVICE));
    Lv->Run = Ext2AllocatePool(NonPagedPoolNx, TableBytes, LV_TAG);
    if (Lv->Run == NULL) {
        IoDeleteDevice(Device);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(Lv->Run, In->Run, TableBytes);
    Lv->Runs = In->Runs;
    Lv->Identifier.Type = EXT4LVD;
    Lv->Identifier.Size = sizeof(EXT4_LV_DEVICE);
    Lv->Self = Device;
    Lv->Lower = Lower;
    Lv->Number = Number;
    Lv->Crypt = In->Crypt;
    Lv->Size = In->Size;
    ExInitializeRundownProtection(&Lv->Rundown);
    RtlStringCbCopyW(Lv->NameBuffer, sizeof(Lv->NameBuffer), NameBuffer);
    RtlInitUnicodeString(&Lv->Name, Lv->NameBuffer);
    RtlStringCbCopyA(Lv->Uuid, sizeof(Lv->Uuid), In->Uuid);
    RtlStringCbCopyA(Lv->VolumeName, sizeof(Lv->VolumeName), In->Name);

    SetFlag(Device->Flags, DO_DIRECT_IO);
    Device->AlignmentRequirement = Lower->AlignmentRequirement;
    Device->SectorSize = EXT4_SECTOR;
    ClearFlag(Device->Flags, DO_DEVICE_INITIALIZING);
    *Out = Lv;
    return STATUS_SUCCESS;
}

/* listed, then links from the mount manager, the letter asked for, and a
   first open on a worker: that is when the file system mounts */
static VOID
Ext4LvPublish(IN PEXT4_LV_DEVICE Lv, IN WCHAR Letter)
{
    NTSTATUS Status;

    ExAcquireFastMutex(&Ext4LvListLock);
    InsertTailList(&Ext4LvList, &Lv->Link);
    ExReleaseFastMutex(&Ext4LvListLock);

    DbgPrint("ext4: %s opened as %wZ (%I64u bytes in %lu runs, read-only)\n",
             Lv->VolumeName, &Lv->Name, Lv->Size, Lv->Runs);

    Ext4ForgetDevice(&Lv->Name);
    Status = Ext4AnnounceDevice(&Lv->Name);
    if (!NT_SUCCESS(Status)) {
        DbgPrint("ext4: the mount manager did not take %wZ (%xh)\n", &Lv->Name, Status);
    }
    if (Letter) {
        Ext4SetDeviceLetter(&Lv->Name, Letter);
    }
    Ext2QueueVolumeProbe(&Lv->Name);
}

NTSTATUS
Ext4LvOpen(IN OUT PEXT4_LV_OPEN In, IN ULONG InLength)
{
    PEXT4_LV_DEVICE Lv = NULL;
    PDEVICE_OBJECT  Lower;
    ULONGLONG       LowerSize = 0;
    NTSTATUS        Status;

    Ext4LvInitialize();

    if (!Ext4LvRequestValid(In, InLength)) {
        return STATUS_INVALID_PARAMETER;
    }
    Status = Ext4LvRoomFor(In->Uuid);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Lower = Ext4CryptReference(In->Crypt, &LowerSize);
    if (Lower == NULL) {
        return STATUS_NOT_FOUND;
    }
    Status = Ext4LvCheckRuns(In, LowerSize);
    if (NT_SUCCESS(Status)) {
        Status = Ext4LvCreateDevice(In, Lower, &Lv);
    }
    if (!NT_SUCCESS(Status)) {
        ObDereferenceObject(Lower);
        return Status;
    }
    Ext4LvPublish(Lv, In->Letter);
    In->Index = Lv->Number;
    return STATUS_SUCCESS;
}

NTSTATUS
Ext4LvClose(IN PEXT4_LV_CLOSE In)
{
    PEXT4_LV_DEVICE Lv;

    Ext4LvInitialize();
    Lv = Ext4LvFind(In->Index);
    return Lv ? Ext4LvCloseDevice(Lv, IsFlagOn(In->Flags, EXT4_CRYPT_FORCE)) : STATUS_NOT_FOUND;
}

NTSTATUS
Ext4LvQuery(OUT PEXT4_LV_QUERY Out)
{
    PLIST_ENTRY List;

    Ext4LvInitialize();
    Out->Count = 0;
    ExAcquireFastMutex(&Ext4LvListLock);
    for (List = Ext4LvList.Flink;
         List != &Ext4LvList && Out->Count < EXT4_LV_MAX_VOLUMES;
         List = List->Flink) {
        PEXT4_LV_DEVICE Lv = CONTAINING_RECORD(List, EXT4_LV_DEVICE, Link);
        PEXT4_LV_ENTRY  E = &Out->Entries[Out->Count++];

        RtlZeroMemory(E, sizeof(*E));
        E->Index = Lv->Number;
        E->Crypt = Lv->Crypt;
        E->Runs = Lv->Runs;
        E->Size = Lv->Size;
        RtlStringCbCopyA(E->Uuid, sizeof(E->Uuid), Lv->Uuid);
        RtlStringCbCopyA(E->Name, sizeof(E->Name), Lv->VolumeName);
    }
    ExReleaseFastMutex(&Ext4LvListLock);
    return STATUS_SUCCESS;
}

BOOLEAN
Ext4LvUsing(IN ULONG Crypt)
{
    PLIST_ENTRY List;
    BOOLEAN     Using = FALSE;

    Ext4LvInitialize();
    ExAcquireFastMutex(&Ext4LvListLock);
    for (List = Ext4LvList.Flink; List != &Ext4LvList; List = List->Flink) {
        if (CONTAINING_RECORD(List, EXT4_LV_DEVICE, Link)->Crypt == Crypt) {
            Using = TRUE;
            break;
        }
    }
    ExReleaseFastMutex(&Ext4LvListLock);
    return Using;
}

/* every volume read through LUKS device Crypt (or all of them: MAXULONG) */
static VOID
Ext4LvCloseMatching(IN ULONG Crypt)
{
    for (;;) {
        PLIST_ENTRY     List;
        PEXT4_LV_DEVICE Lv = NULL;

        ExAcquireFastMutex(&Ext4LvListLock);
        for (List = Ext4LvList.Flink; List != &Ext4LvList; List = List->Flink) {
            PEXT4_LV_DEVICE Each = CONTAINING_RECORD(List, EXT4_LV_DEVICE, Link);
            if (Crypt == MAXULONG || Each->Crypt == Crypt) {
                Lv = Each;
                break;
            }
        }
        ExReleaseFastMutex(&Ext4LvListLock);
        if (Lv == NULL) {
            break;
        }
        Ext4LvCloseDevice(Lv, TRUE);
    }
}

VOID
Ext4LvCloseOn(IN ULONG Crypt)
{
    Ext4LvInitialize();
    Ext4LvCloseMatching(Crypt);
}

VOID
Ext4LvTeardownAll(VOID)
{
    if (Ext4LvReady) {
        Ext4LvCloseMatching(MAXULONG);
    }
}
