/**
 * crypt.c - LUKS volumes: a disk device that en- and decrypts a partition.
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
 * I/O. A write is never encrypted in place - the caller's pages are the
 * cache. An aligned write is encrypted in the caller's own thread, from its
 * pages into a buffer of its own, and sent on without waiting: the disk's
 * completion finishes it (no thread switch on the way, see
 * Ext4CryptWriteInline). Reads, and whatever the inline path cannot take,
 * go to a few system threads with key schedules and bounce buffers
 * allocated up front, so a paging write always makes progress; a long
 * aligned request is cut into pieces the threads take in parallel. A read
 * of whole sectors lands in the caller's pages and is decrypted there. The
 * device shows 512-byte sectors (512e): with LUKS2 sectors of 4 KiB an
 * access to part of one is a read-modify-write under a per-device lock.
 * Lower I/O uses IRPs of our own with a completion routine, never thread-
 * queued synchronous requests, so nothing depends on APC delivery.
 *
 * Locking (IOCTL_APP_CRYPT_LOCK, and every device at driver unload):
 * dismount whatever file system is on the device, drop the mount manager's
 * links, refuse new I/O and wait for the I/O in flight (rundown), stop the
 * workers, destroy the keys, delete the device.
 */

#include "ext4fs.h"
#include <bcrypt.h>
#include <mountdev.h>
#include <mountmgr.h>
#include <ntddvol.h>
#include <ntstrsafe.h>
#include <wdmsec.h>
#include "ext4crypt.h"
#include "ext4xts.h"

#define CRYPT_TAG               'YC4E'

#define EXT4_CRYPT_WORKERS      16                  /* threads per device, at most: two per
                                                       processor - a reader waits on the disk */
#define EXT4_CRYPT_BOUNCE       (256 * 1024)        /* per thread: largest piece written at once */
#define EXT4_CRYPT_INLINE_MAX   32                  /* key contexts for writes in the caller's thread */
#define EXT4_CRYPT_INLINE_LIMIT (1024 * 1024)       /* longer writes go to the workers in pieces */
#define EXT4_SECTOR             512                 /* the unit every request is aligned to */
#define EXT4_SECTOR_SHIFT       9
#define EXT4_PARTITION_LINUX    0x83                /* what the device reports as its type */
#define EXT4_CRYPT_UNIQUE_PREFIX "EXT4-LUKS-"
#define EXT4_PREFIX_CHARS       32                  /* room for any unique id prefix */
#define EXT4_UNIQUE_ID_CHARS    (EXT4_PREFIX_CHARS + EXT4_CRYPT_UUID_CHARS)
#define EXT4_LINK_CHARS         128                 /* \DosDevices\X:, \??\Volume{GUID} */
#define EXT4_POINTS_REPLY       1024                /* DELETE_POINTS echoes what it deleted */
#define EXT4_POINTS_FIRST       (16 * 1024)         /* QUERY_POINTS, first guess of its size */
#define EXT4_POINTS_TRIES       4                   /* the list may grow between two asks */

typedef struct _EXT4_CRYPT_DEVICE EXT4_CRYPT_DEVICE, *PEXT4_CRYPT_DEVICE;

typedef struct _EXT4_CRYPT_WORKER {
    PEXT4_CRYPT_DEVICE  Crypt;
    PKTHREAD            Thread;
    EXT4_XTS            Xts;                /* own key handles: nothing shared between threads */
    PUCHAR              Bounce;
    PMDL                BounceMdl;          /* describes all of Bounce */
    PMDL                PartMdl;            /* the part of it one lower request uses */
    PMDL                UserMdl;            /* a piece of the caller's buffer */
    PUCHAR              Scratch;            /* the tweaks of one sector */
    LIST_ENTRY          Stop;               /* queued to end the thread */
} EXT4_CRYPT_WORKER, *PEXT4_CRYPT_WORKER;

/*
 * The unit of work: a piece of an IRP. A request up to one bounce buffer
 * long, or one whose pieces could share an encryption sector, is a single
 * piece that lives in the IRP's own DriverContext (four pointers: exactly
 * this structure) - nothing is allocated, so a paging write always makes
 * progress. A longer aligned one is cut into pieces of EXT4_CRYPT_BOUNCE
 * held by one allocation; the workers take them in parallel and the one
 * that finishes the last piece completes the IRP.
 */
typedef struct _EXT4_CRYPT_PIECE {
    LIST_ENTRY          Entry;              /* in the device's queue */
    struct _EXT4_CRYPT_SPLIT *Split;        /* NULL: the whole IRP, in its DriverContext */
    ULONG               Offset;             /* into the IRP's buffer */
    ULONG               Length;
} EXT4_CRYPT_PIECE, *PEXT4_CRYPT_PIECE;

C_ASSERT(sizeof(EXT4_CRYPT_PIECE) <= sizeof(((PIRP)0)->Tail.Overlay.DriverContext));

typedef struct _EXT4_CRYPT_SPLIT {
    PIRP                Irp;
    volatile LONG       Pending;            /* pieces not finished */
    volatile LONG       Status;             /* the first failure */
    EXT4_CRYPT_PIECE    Piece[ANYSIZE_ARRAY];
} EXT4_CRYPT_SPLIT, *PEXT4_CRYPT_SPLIT;

struct _EXT4_CRYPT_DEVICE {
    EXT2_IDENTIFIER     Identifier;         /* EXT4CRY */
    LIST_ENTRY          Link;               /* Ext4CryptList */
    PDEVICE_OBJECT      Self;
    PDEVICE_OBJECT      Lower;              /* top of the partition's stack, referenced */
    PFILE_OBJECT        LowerFile;          /* our open of the partition */
    ULONG               Number;             /* \Device\Ext4Crypt<Number>, never reused */
    ULONG               Flags;              /* EXT4_CRYPT_READ_ONLY, _IV_LARGE_SECTORS */
    ULONG               Cipher;
    ULONG               KeyBytes;
    ULONG               SectorSize;
    ULONG               SectorShift;
    ULONGLONG           Offset;             /* of the segment on the partition, bytes */
    ULONGLONG           Size;               /* of the segment, bytes */
    ULONGLONG           IvOffset;
    KQUEUE              Queue;              /* IRPs for the workers */
    ULONG               Workers;
    EXT4_CRYPT_WORKER   Worker[EXT4_CRYPT_WORKERS];
    ERESOURCE           Rmw;                /* partial-sector writes: exclusive; whole: shared */
    EX_RUNDOWN_REF      Rundown;            /* I/O in flight */
    volatile LONG       InlineBusy;         /* bit n set: Inline[n] in use */
    ULONG               InlineCount;
    EXT4_CRYPT_WORKER   Inline[EXT4_CRYPT_INLINE_MAX];  /* Xts and Scratch only */
    volatile LONG       Closing;
    UNICODE_STRING      Name;
    WCHAR               NameBuffer[48];
    WCHAR               Source[EXT4_CRYPT_DEVICE_CHARS];
    CHAR                Uuid[EXT4_CRYPT_UUID_CHARS];
};

static LIST_ENTRY           Ext4CryptList;
static FAST_MUTEX           Ext4CryptListLock;      /* the list only, never across I/O */
static KMUTEX               Ext4CryptControlLock;   /* one unlock or lock at a time */
static BCRYPT_ALG_HANDLE    Ext4AesEcb;
static ULONG                Ext4CryptNext;

static KSTART_ROUTINE           Ext4CryptWorkerThread;
static IO_COMPLETION_ROUTINE    Ext4CryptLowerDone;

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

NTSTATUS
Ext4DiskComplete(IN PIRP Irp, IN NTSTATUS Status, IN ULONG_PTR Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_DISK_INCREMENT);
    return Status;
}

/* ---------------------------------------------------------------- lower I/O */

static NTSTATUS
Ext4CryptLowerDone(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp, IN PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;     /* the IRP is ours, freed by the sender */
}

/*
 * One synchronous request to the partition. Offset is relative to the
 * partition; Mdl (reads and writes) describes exactly Length bytes.
 * StackFlags: SL_WRITE_THROUGH of the caller's write, passed on - the
 * journal's commit block relies on it.
 */
static NTSTATUS
Ext4CryptLowerIo(IN PEXT4_CRYPT_DEVICE Crypt, IN UCHAR Major, IN ULONGLONG Offset,
                 IN PMDL Mdl, IN ULONG Length, IN UCHAR StackFlags)
{
    PIRP                Irp;
    PIO_STACK_LOCATION  Sp;
    KEVENT              Event;
    NTSTATUS            Status;

    Irp = IoAllocateIrp(Crypt->Lower->StackSize, FALSE);
    if (Irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    Irp->RequestorMode = KernelMode;
    Irp->MdlAddress = Mdl;
    Irp->UserBuffer = Mdl ? MmGetMdlVirtualAddress(Mdl) : NULL;
    Irp->Flags = IRP_NOCACHE;

    Sp = IoGetNextIrpStackLocation(Irp);
    Sp->MajorFunction = Major;
    Sp->Flags = SL_OVERRIDE_VERIFY_VOLUME | StackFlags;
    if (Major == IRP_MJ_READ || Major == IRP_MJ_WRITE) {
        Sp->Parameters.Read.Length = Length;
        Sp->Parameters.Read.ByteOffset.QuadPart = (LONGLONG)Offset;
    }

    IoSetCompletionRoutine(Irp, Ext4CryptLowerDone, &Event, TRUE, TRUE, TRUE);
    (void)IoCallDriver(Crypt->Lower, Irp);
    KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);

    Status = Irp->IoStatus.Status;
    if (NT_SUCCESS(Status) && Length != 0 && Irp->IoStatus.Information != Length) {
        Status = STATUS_UNEXPECTED_IO_ERROR;
    }
    IoFreeIrp(Irp);
    return Status;
}

/* A device control to the partition, buffered, synchronous. */
static NTSTATUS
Ext4CryptLowerIoctl(IN PDEVICE_OBJECT Lower, IN ULONG Code, OUT PVOID Out, IN ULONG OutLength)
{
    KEVENT          Event;
    IO_STATUS_BLOCK IoStatus;
    PIRP            Irp;
    NTSTATUS        Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoBuildDeviceIoControlRequest(Code, Lower, NULL, 0, Out, OutLength,
                                        FALSE, &Event, &IoStatus);
    if (Irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Status = IoCallDriver(Lower, Irp);
    if (Status == STATUS_PENDING) {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }
    return Status;
}

/* ---------------------------------------------------------------- sectors */

/*
 * En- or decrypt whole sectors into Buffer, from In (NULL: in place). Byte:
 * position of Buffer on the device, a multiple of the sector size.
 */
static NTSTATUS
Ext4CryptSectors(IN PEXT4_CRYPT_DEVICE Crypt, IN PEXT4_CRYPT_WORKER Worker,
                 IN BOOLEAN Encrypt, IN ULONGLONG Byte, IN PUCHAR Buffer,
                 IN const UCHAR *In, IN ULONG Length)
{
    ULONGLONG   Sector = Byte >> Crypt->SectorShift;
    ULONG       Done;
    NTSTATUS    Status = STATUS_SUCCESS;

    for (Done = 0; Done < Length; Done += Crypt->SectorSize, Sector++) {

        /* plain64: the sector number, counted in 512-byte units unless
           the header says large sectors count themselves (LUKS2) */
        ULONGLONG Iv = IsFlagOn(Crypt->Flags, EXT4_CRYPT_IV_LARGE_SECTORS) ?
                       Sector : Sector << (Crypt->SectorShift - EXT4_SECTOR_SHIFT);
        Iv += Crypt->IvOffset;
        if (Crypt->Cipher == EXT4_CIPHER_AES_XTS_PLAIN) {
            Iv &= MAXULONG;
        }

        Status = Ext4XtsUnitCopy(&Worker->Xts, Encrypt, Iv, Buffer + Done,
                                 (In ? In : Buffer) + Done, Crypt->SectorSize, Worker->Scratch);
        if (!NT_SUCCESS(Status)) {
            break;
        }
    }
    return Status;
}

/*
 * Read or write Length bytes at Byte of the device through the bounce
 * buffer: the covering sectors are read (unless a write replaces them
 * whole), decrypted, merged with the caller's bytes, encrypted, written.
 */
static NTSTATUS
Ext4CryptBounced(IN PEXT4_CRYPT_DEVICE Crypt, IN PEXT4_CRYPT_WORKER Worker,
                 IN BOOLEAN Write, IN ULONGLONG Byte, IN PUCHAR User, IN ULONG Length,
                 IN UCHAR StackFlags)
{
    ULONG       Mask = Crypt->SectorSize - 1;
    ULONG       Done = 0;
    NTSTATUS    Status = STATUS_SUCCESS;

    while (Done < Length && NT_SUCCESS(Status)) {

        ULONGLONG   Pos = Byte + Done;
        ULONGLONG   First = Pos & ~(ULONGLONG)Mask;
        ULONG       Head = (ULONG)(Pos - First);
        ULONG       Chunk = min(Length - Done, EXT4_CRYPT_BOUNCE - Head);
        ULONG       Span = (Head + Chunk + Mask) & ~Mask;
        BOOLEAN     Partial = (Head != 0) || (Span != Head + Chunk);
        BOOLEAN     Locked = FALSE;

        IoBuildPartialMdl(Worker->BounceMdl, Worker->PartMdl, Worker->Bounce, Span);

        if (Write) {
            /* A partial write reads its sectors, merges and writes them
               back: nothing else may write them in between, or the merge
               puts their old contents back. Partial writes take the lock
               exclusively, whole-sector writes shared - they still run
               side by side, only never inside a merge. */
            KeEnterCriticalRegion();
            if (Partial) {
                ExAcquireResourceExclusiveLite(&Crypt->Rmw, TRUE);
            } else {
                ExAcquireResourceSharedLite(&Crypt->Rmw, TRUE);
            }
            Locked = TRUE;
        }

        if (!Write || Partial) {
            Status = Ext4CryptLowerIo(Crypt, IRP_MJ_READ, Crypt->Offset + First,
                                      Worker->PartMdl, Span, 0);
            if (NT_SUCCESS(Status)) {
                Status = Ext4CryptSectors(Crypt, Worker, FALSE, First, Worker->Bounce, NULL, Span);
            }
        }

        if (NT_SUCCESS(Status)) {
            if (Write && !Partial) {
                /* whole sectors: encrypted from the caller's pages straight
                   into the bounce, the whitening pass doing the copy */
                Status = Ext4CryptSectors(Crypt, Worker, TRUE, First, Worker->Bounce,
                                          User + Done, Span);
                if (NT_SUCCESS(Status)) {
                    Status = Ext4CryptLowerIo(Crypt, IRP_MJ_WRITE, Crypt->Offset + First,
                                              Worker->PartMdl, Span, StackFlags);
                }
            } else if (Write) {
                RtlCopyMemory(Worker->Bounce + Head, User + Done, Chunk);
                Status = Ext4CryptSectors(Crypt, Worker, TRUE, First, Worker->Bounce, NULL, Span);
                if (NT_SUCCESS(Status)) {
                    Status = Ext4CryptLowerIo(Crypt, IRP_MJ_WRITE, Crypt->Offset + First,
                                              Worker->PartMdl, Span, StackFlags);
                }
            } else {
                RtlCopyMemory(User + Done, Worker->Bounce + Head, Chunk);
            }
        }

        if (Locked) {
            ExReleaseResourceLite(&Crypt->Rmw);
            KeLeaveCriticalRegion();
        }
        MmPrepareMdlForReuse(Worker->PartMdl);

        /* no plaintext stays behind in the bounce: a write leaves
           ciphertext there (encrypted in place), a read or a failed
           write the decrypted sectors */
        if (!Write || !NT_SUCCESS(Status)) {
            RtlSecureZeroMemory(Worker->Bounce, Span);
        }

        Done += Chunk;
    }
    return Status;
}

/* one piece: Offset/Length within the IRP's buffer */
static NTSTATUS
Ext4CryptTransfer(IN PEXT4_CRYPT_DEVICE Crypt, IN PEXT4_CRYPT_WORKER Worker, IN PIRP Irp,
                  IN ULONG Offset, IN ULONG Length)
{
    PIO_STACK_LOCATION  IrpSp = IoGetCurrentIrpStackLocation(Irp);
    BOOLEAN             Write = (IrpSp->MajorFunction == IRP_MJ_WRITE);
    ULONGLONG           Byte = (ULONGLONG)IrpSp->Parameters.Read.ByteOffset.QuadPart + Offset;
    PUCHAR              User;
    NTSTATUS            Status;

    User = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, HighPagePriority | MdlMappingNoExecute);
    if (User == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    User += Offset;

    /* a read of whole sectors: straight into the caller's pages, which
       are then decrypted where they are */
    if (!Write && ((Byte | Length) & (Crypt->SectorSize - 1)) == 0) {
        PMDL Mdl = Irp->MdlAddress;
        if (Length != IrpSp->Parameters.Read.Length) {
            IoBuildPartialMdl(Irp->MdlAddress, Worker->UserMdl,
                              (PUCHAR)MmGetMdlVirtualAddress(Irp->MdlAddress) + Offset, Length);
            Mdl = Worker->UserMdl;
        }
        Status = Ext4CryptLowerIo(Crypt, IRP_MJ_READ, Crypt->Offset + Byte, Mdl, Length, 0);
        if (Mdl == Worker->UserMdl) {
            MmPrepareMdlForReuse(Worker->UserMdl);
        }
        if (NT_SUCCESS(Status)) {
            Status = Ext4CryptSectors(Crypt, Worker, FALSE, Byte, User, NULL, Length);
        }
        return Status;
    }

    return Ext4CryptBounced(Crypt, Worker, Write, Byte, User, Length,
                            (UCHAR)(IrpSp->Flags & SL_WRITE_THROUGH));
}

static VOID
Ext4CryptFinish(IN PEXT4_CRYPT_DEVICE Crypt, IN PIRP Irp, IN NTSTATUS Status)
{
    PIO_STACK_LOCATION Sp = IoGetCurrentIrpStackLocation(Irp);

    if (!NT_SUCCESS(Status)) {
        DbgPrint("ext4: %wZ %s of %lu bytes at %I64u failed (%xh)\n", &Crypt->Name,
                 Sp->MajorFunction == IRP_MJ_WRITE ? "write" : "read",
                 Sp->Parameters.Read.Length, Sp->Parameters.Read.ByteOffset.QuadPart, Status);
    }
    Ext4DiskComplete(Irp, Status, NT_SUCCESS(Status) ? Sp->Parameters.Read.Length : 0);
    ExReleaseRundownProtection(&Crypt->Rundown);
}

static VOID
Ext4CryptWorkerThread(IN PVOID Context)
{
    PEXT4_CRYPT_WORKER  Worker = (PEXT4_CRYPT_WORKER)Context;
    PEXT4_CRYPT_DEVICE  Crypt = Worker->Crypt;

    for (;;) {
        PLIST_ENTRY         Entry = KeRemoveQueue(&Crypt->Queue, KernelMode, NULL);
        PEXT4_CRYPT_PIECE   Piece;
        PEXT4_CRYPT_SPLIT   Split;
        PIRP                Irp;
        NTSTATUS            Status;
        ULONG               i;
        BOOLEAN             Stop = FALSE;

        for (i = 0; i < Crypt->Workers; i++) {
            if (Entry == &Crypt->Worker[i].Stop) {
                Stop = TRUE;
            }
        }
        if (Stop) {
            break;
        }

        Piece = CONTAINING_RECORD(Entry, EXT4_CRYPT_PIECE, Entry);
        Split = Piece->Split;
        Irp = Split ? Split->Irp :
              CONTAINING_RECORD((PVOID *)Piece, IRP, Tail.Overlay.DriverContext[0]);

        Status = Ext4CryptTransfer(Crypt, Worker, Irp, Piece->Offset, Piece->Length);

        if (Split == NULL) {
            Ext4CryptFinish(Crypt, Irp, Status);
            continue;
        }
        if (!NT_SUCCESS(Status)) {
            InterlockedCompareExchange(&Split->Status, Status, STATUS_SUCCESS);
        }
        if (InterlockedDecrement(&Split->Pending) == 0) {
            Status = Split->Status;
            Ext2FreePool(Split, CRYPT_TAG);
            Ext4CryptFinish(Crypt, Irp, Status);
        }
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

/*
 * Queue a read or write: in pieces when it is long and every piece starts
 * and ends on an encryption sector, whole otherwise (see EXT4_CRYPT_PIECE).
 */
static VOID
Ext4CryptQueue(IN PEXT4_CRYPT_DEVICE Crypt, IN PIRP Irp, IN ULONGLONG Byte, IN ULONG Length)
{
    PEXT4_CRYPT_SPLIT   Split = NULL;
    PEXT4_CRYPT_PIECE   Piece;
    ULONG               Pieces = (ULONG)(((ULONGLONG)Length + EXT4_CRYPT_BOUNCE - 1) / EXT4_CRYPT_BOUNCE);
    ULONG               i;

    if (Pieces > 1 && ((Byte | Length) & (Crypt->SectorSize - 1)) == 0) {
        Split = Ext2AllocatePool(NonPagedPoolNx,
                                 FIELD_OFFSET(EXT4_CRYPT_SPLIT, Piece) + Pieces * sizeof(EXT4_CRYPT_PIECE),
                                 CRYPT_TAG);
    }

    if (Split == NULL) {
        Piece = (PEXT4_CRYPT_PIECE)Irp->Tail.Overlay.DriverContext;
        Piece->Split = NULL;
        Piece->Offset = 0;
        Piece->Length = Length;
        KeInsertQueue(&Crypt->Queue, &Piece->Entry);
        return;
    }

    Split->Irp = Irp;
    Split->Pending = (LONG)Pieces;
    Split->Status = STATUS_SUCCESS;
    for (i = 0; i < Pieces; i++) {
        Piece = &Split->Piece[i];
        Piece->Split = Split;
        Piece->Offset = i * EXT4_CRYPT_BOUNCE;
        Piece->Length = min(EXT4_CRYPT_BOUNCE, Length - Piece->Offset);
    }
    /* queued after all are set up: the last one may complete at once */
    for (i = 0; i < Pieces; i++) {
        KeInsertQueue(&Crypt->Queue, &Split->Piece[i].Entry);
    }
}

/*
 * Writes in the caller's thread. A write the file system waits for used to
 * cost two thread switches (to a worker, and back to it from the disk's
 * completion) - on a virtual machine each a cross-processor wake-up, and
 * together three quarters of the time a write took. An aligned write up
 * to EXT4_CRYPT_INLINE_LIMIT is encrypted right here into a buffer of its
 * own and sent on without waiting; the disk's completion finishes it. Key
 * contexts are claimed from a bitmap, no lock. Whatever cannot be had at
 * once (a context, the memory, a low enough IRQL) sends the write to the
 * workers instead, whose buffers are allocated up front: a paging write
 * always makes progress.
 */
typedef struct _EXT4_CRYPT_WRITE {
    PEXT4_CRYPT_DEVICE  Crypt;
    PIRP                Irp;                /* the file system's */
    PMDL                Mdl;
    PUCHAR              Data;               /* the ciphertext */
    ULONG               Length;
} EXT4_CRYPT_WRITE, *PEXT4_CRYPT_WRITE;

static IO_COMPLETION_ROUTINE Ext4CryptWriteDone;

static LONG
Ext4CryptClaim(IN PEXT4_CRYPT_DEVICE Crypt)
{
    ULONG i;

    for (i = 0; i < Crypt->InlineCount; i++) {
        if (!InterlockedBitTestAndSet(&Crypt->InlineBusy, (LONG)i)) {
            return (LONG)i;
        }
    }
    return -1;
}

static VOID
Ext4CryptFreeWrite(IN PEXT4_CRYPT_WRITE W)
{
    if (W->Mdl) {
        IoFreeMdl(W->Mdl);
    }
    if (W->Data) {
        Ext2FreePool(W->Data, CRYPT_TAG);
    }
    Ext2FreePool(W, CRYPT_TAG);
}

static NTSTATUS
Ext4CryptWriteDone(IN PDEVICE_OBJECT DeviceObject, IN PIRP Lower, IN PVOID Context)
{
    PEXT4_CRYPT_WRITE   W = (PEXT4_CRYPT_WRITE)Context;
    PEXT4_CRYPT_DEVICE  Crypt = W->Crypt;
    PIRP                Irp = W->Irp;
    NTSTATUS            Status = Lower->IoStatus.Status;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (NT_SUCCESS(Status) && Lower->IoStatus.Information != W->Length) {
        Status = STATUS_UNEXPECTED_IO_ERROR;
    }
    Ext4CryptFreeWrite(W);
    IoFreeIrp(Lower);
    Ext4CryptFinish(Crypt, Irp, Status);
    return STATUS_MORE_PROCESSING_REQUIRED;     /* the lower IRP is freed */
}

/* TRUE: the write is under way (or already failed and completed) */
static BOOLEAN
Ext4CryptWriteInline(IN PEXT4_CRYPT_DEVICE Crypt, IN PIRP Irp, IN ULONGLONG Byte, IN ULONG Length)
{
    PIO_STACK_LOCATION  IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PIO_STACK_LOCATION  Sp;
    PEXT4_CRYPT_WRITE   W;
    PUCHAR              User;
    PIRP                Lower;
    LONG                Slot;
    NTSTATUS            Status;

    if (((Byte | Length) & (Crypt->SectorSize - 1)) != 0 ||
        Length > EXT4_CRYPT_INLINE_LIMIT || KeGetCurrentIrql() > APC_LEVEL) {
        return FALSE;
    }
    User = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, HighPagePriority | MdlMappingNoExecute);
    if (User == NULL) {
        return FALSE;
    }

    W = Ext2AllocatePool(NonPagedPoolNx, sizeof(EXT4_CRYPT_WRITE), CRYPT_TAG);
    if (W == NULL) {
        return FALSE;
    }
    RtlZeroMemory(W, sizeof(*W));
    W->Crypt = Crypt;
    W->Irp = Irp;
    W->Length = Length;
    W->Data = Ext2AllocatePool(NonPagedPoolNx, Length, CRYPT_TAG);
    W->Mdl = W->Data ? IoAllocateMdl(W->Data, Length, FALSE, FALSE, NULL) : NULL;
    Lower = W->Mdl ? IoAllocateIrp(Crypt->Lower->StackSize, FALSE) : NULL;
    if (Lower == NULL) {
        Ext4CryptFreeWrite(W);
        return FALSE;
    }
    Slot = Ext4CryptClaim(Crypt);
    if (Slot < 0) {
        IoFreeIrp(Lower);
        Ext4CryptFreeWrite(W);
        return FALSE;
    }
    MmBuildMdlForNonPagedPool(W->Mdl);

    Status = Ext4CryptSectors(Crypt, &Crypt->Inline[Slot], TRUE, Byte, W->Data, User, Length);
    InterlockedBitTestAndReset(&Crypt->InlineBusy, Slot);
    if (!NT_SUCCESS(Status)) {
        IoFreeIrp(Lower);
        Ext4CryptFreeWrite(W);
        Ext4CryptFinish(Crypt, Irp, Status);
        return TRUE;
    }

    Lower->Tail.Overlay.Thread = PsGetCurrentThread();
    Lower->RequestorMode = KernelMode;
    Lower->MdlAddress = W->Mdl;
    Lower->UserBuffer = W->Data;
    Lower->Flags = IRP_NOCACHE;
    Sp = IoGetNextIrpStackLocation(Lower);
    Sp->MajorFunction = IRP_MJ_WRITE;
    Sp->Flags = SL_OVERRIDE_VERIFY_VOLUME | (IrpSp->Flags & SL_WRITE_THROUGH);
    Sp->Parameters.Write.Length = Length;
    Sp->Parameters.Write.ByteOffset.QuadPart = (LONGLONG)(Crypt->Offset + Byte);
    IoSetCompletionRoutine(Lower, Ext4CryptWriteDone, W, TRUE, TRUE, TRUE);
    (void)IoCallDriver(Crypt->Lower, Lower);
    return TRUE;
}

/* ---------------------------------------------------------------- dispatch */

static NTSTATUS
Ext4DiskCopyOut(IN PIRP Irp, IN const VOID *Data, IN ULONG Length)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);

    if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength < Length) {
        return Ext4DiskComplete(Irp, STATUS_BUFFER_TOO_SMALL, 0);
    }
    RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, Data, Length);
    return Ext4DiskComplete(Irp, STATUS_SUCCESS, Length);
}

/*
 * MOUNTDEV_NAME and MOUNTDEV_UNIQUE_ID share their layout (a USHORT length
 * and the bytes): a buffer too short for the bytes gets the length and
 * STATUS_BUFFER_OVERFLOW, so that the mount manager can ask again.
 */
static NTSTATUS
Ext4DiskVariable(IN PIRP Irp, IN const VOID *Data, IN USHORT Length)
{
    PIO_STACK_LOCATION  IrpSp = IoGetCurrentIrpStackLocation(Irp);
    ULONG               Out = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
    PMOUNTDEV_NAME      Name = (PMOUNTDEV_NAME)Irp->AssociatedIrp.SystemBuffer;
    ULONG               Needed = FIELD_OFFSET(MOUNTDEV_NAME, Name) + Length;

    if (Out < sizeof(MOUNTDEV_NAME)) {
        return Ext4DiskComplete(Irp, STATUS_INVALID_PARAMETER, 0);
    }
    Name->NameLength = Length;
    if (Out < Needed) {
        return Ext4DiskComplete(Irp, STATUS_BUFFER_OVERFLOW, sizeof(MOUNTDEV_NAME));
    }
    RtlCopyMemory(Name->Name, Data, Length);
    return Ext4DiskComplete(Irp, STATUS_SUCCESS, Needed);
}

/*
 * The device controls of the driver's own disk devices (LUKS and LVM):
 * what a file system and the mount manager ask a disk. Name: the NT device
 * name; the unique id, UniquePrefix and Uuid, stays the same for the same
 * volume at every unlock, so the mount manager remembers its letter.
 */
NTSTATUS
Ext4DiskIoctl(IN PIRP Irp, IN PCUNICODE_STRING Name, IN PCSTR UniquePrefix, IN PCSTR Uuid,
              IN ULONGLONG Size, IN BOOLEAN ReadOnly)
{
    PIO_STACK_LOCATION  IrpSp = IoGetCurrentIrpStackLocation(Irp);
    ULONG               Code = IrpSp->Parameters.DeviceIoControl.IoControlCode;

    switch (Code) {

    case IOCTL_DISK_GET_DRIVE_GEOMETRY:
    case IOCTL_DISK_GET_DRIVE_GEOMETRY_EX: {
        DISK_GEOMETRY_EX Geometry;

        /* 512-byte logical sectors whatever the encryption sector (512e):
           the device takes any 512-aligned request (a part of a larger
           encryption sector is read, merged and rewritten), and file
           systems written for 512-byte sectors - ext4.sys among them -
           round paging I/O up to the sector size they are given, past
           the end of the caller's buffer at 4096. One sector per track:
           every sector is addressable, whatever the size. */
        RtlZeroMemory(&Geometry, sizeof(Geometry));
        Geometry.Geometry.Cylinders.QuadPart = (LONGLONG)(Size >> EXT4_SECTOR_SHIFT);
        Geometry.Geometry.MediaType = FixedMedia;
        Geometry.Geometry.TracksPerCylinder = 1;
        Geometry.Geometry.SectorsPerTrack = 1;
        Geometry.Geometry.BytesPerSector = EXT4_SECTOR;
        Geometry.DiskSize.QuadPart = (LONGLONG)Size;
        return Ext4DiskCopyOut(Irp, &Geometry, Code == IOCTL_DISK_GET_DRIVE_GEOMETRY ?
                               sizeof(DISK_GEOMETRY) : FIELD_OFFSET(DISK_GEOMETRY_EX, Data));
    }

    case IOCTL_DISK_GET_LENGTH_INFO: {
        GET_LENGTH_INFORMATION Info;
        Info.Length.QuadPart = (LONGLONG)Size;
        return Ext4DiskCopyOut(Irp, &Info, sizeof(Info));
    }

    case IOCTL_DISK_GET_PARTITION_INFO: {
        PARTITION_INFORMATION Info;
        RtlZeroMemory(&Info, sizeof(Info));
        Info.PartitionLength.QuadPart = (LONGLONG)Size;
        Info.PartitionNumber = 1;
        Info.PartitionType = EXT4_PARTITION_LINUX;
        Info.RecognizedPartition = TRUE;
        return Ext4DiskCopyOut(Irp, &Info, sizeof(Info));
    }

    case IOCTL_DISK_GET_PARTITION_INFO_EX: {
        PARTITION_INFORMATION_EX Info;
        RtlZeroMemory(&Info, sizeof(Info));
        Info.PartitionStyle = PARTITION_STYLE_MBR;
        Info.PartitionLength.QuadPart = (LONGLONG)Size;
        Info.PartitionNumber = 1;
        Info.Mbr.PartitionType = EXT4_PARTITION_LINUX;
        Info.Mbr.RecognizedPartition = TRUE;
        return Ext4DiskCopyOut(Irp, &Info, sizeof(Info));
    }

    case IOCTL_DISK_IS_WRITABLE:
        return Ext4DiskComplete(Irp, ReadOnly ? STATUS_MEDIA_WRITE_PROTECTED : STATUS_SUCCESS, 0);

    case IOCTL_DISK_CHECK_VERIFY:
    case IOCTL_STORAGE_CHECK_VERIFY:
    case IOCTL_STORAGE_CHECK_VERIFY2:
        if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(ULONG)) {
            *(PULONG)Irp->AssociatedIrp.SystemBuffer = 0;       /* media change count */
            return Ext4DiskComplete(Irp, STATUS_SUCCESS, sizeof(ULONG));
        }
        return Ext4DiskComplete(Irp, STATUS_SUCCESS, 0);

    case IOCTL_DISK_MEDIA_REMOVAL:
    case IOCTL_STORAGE_MEDIA_REMOVAL:
    case IOCTL_VOLUME_ONLINE:
    case IOCTL_MOUNTDEV_LINK_CREATED:
    case IOCTL_MOUNTDEV_LINK_DELETED:
        return Ext4DiskComplete(Irp, STATUS_SUCCESS, 0);

    case IOCTL_STORAGE_GET_HOTPLUG_INFO: {
        STORAGE_HOTPLUG_INFO Info;
        RtlZeroMemory(&Info, sizeof(Info));
        Info.Size = sizeof(Info);
        return Ext4DiskCopyOut(Irp, &Info, sizeof(Info));
    }

    case IOCTL_MOUNTDEV_QUERY_DEVICE_NAME:
        return Ext4DiskVariable(Irp, Name->Buffer, Name->Length);

    case IOCTL_MOUNTDEV_QUERY_UNIQUE_ID: {
        CHAR    Id[EXT4_UNIQUE_ID_CHARS];
        size_t  Length = 0;
        RtlStringCbPrintfA(Id, sizeof(Id), "%s%s", UniquePrefix, Uuid);
        RtlStringCbLengthA(Id, sizeof(Id), &Length);
        return Ext4DiskVariable(Irp, Id, (USHORT)Length);
    }

    default:
        /* SUGGESTED_LINK_NAME, STABLE_GUID, UNIQUE_ID_CHANGE_NOTIFY, disk
           extents, storage properties: nothing to say about them */
        return Ext4DiskComplete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

static NTSTATUS
Ext4CryptDeviceControl(IN PEXT4_CRYPT_DEVICE Crypt, IN PIRP Irp)
{
    /* the LUKS UUID: the same container is the same volume at every unlock */
    return Ext4DiskIoctl(Irp, &Crypt->Name, EXT4_CRYPT_UNIQUE_PREFIX, Crypt->Uuid, Crypt->Size,
                         IsFlagOn(Crypt->Flags, EXT4_CRYPT_READ_ONLY));
}

/*
 * Every IRP sent to a crypt device. The file system on top reaches it
 * through Ext2BuildRequest, which hands such IRPs over before any of the
 * file system's own machinery runs.
 */
NTSTATUS
Ext4CryptDispatch(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp)
{
    PEXT4_CRYPT_DEVICE  Crypt = (PEXT4_CRYPT_DEVICE)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION  IrpSp = IoGetCurrentIrpStackLocation(Irp);

    switch (IrpSp->MajorFunction) {

    case IRP_MJ_CREATE:
        /* the device itself only: files live on the file system above */
        if (IrpSp->FileObject && IrpSp->FileObject->FileName.Length != 0) {
            return Ext4DiskComplete(Irp, STATUS_OBJECT_PATH_NOT_FOUND, 0);
        }
        return Ext4DiskComplete(Irp, Crypt->Closing ? STATUS_DEVICE_DOES_NOT_EXIST :
                                                       STATUS_SUCCESS, 0);

    case IRP_MJ_CLOSE:
    case IRP_MJ_CLEANUP:
        return Ext4DiskComplete(Irp, STATUS_SUCCESS, 0);

    case IRP_MJ_READ:
    case IRP_MJ_WRITE: {
        ULONGLONG   Byte = (ULONGLONG)IrpSp->Parameters.Read.ByteOffset.QuadPart;
        ULONG       Length = IrpSp->Parameters.Read.Length;

        if (Crypt->Closing) {
            return Ext4DiskComplete(Irp, STATUS_DEVICE_DOES_NOT_EXIST, 0);
        }
        if (IrpSp->MajorFunction == IRP_MJ_WRITE &&
            IsFlagOn(Crypt->Flags, EXT4_CRYPT_READ_ONLY)) {
            return Ext4DiskComplete(Irp, STATUS_MEDIA_WRITE_PROTECTED, 0);
        }
        if (Length == 0) {
            return Ext4DiskComplete(Irp, STATUS_SUCCESS, 0);
        }
        if (IrpSp->Parameters.Read.ByteOffset.QuadPart < 0 ||
            ((Byte | Length) & (EXT4_SECTOR - 1)) != 0 ||
            Irp->MdlAddress == NULL) {
            return Ext4DiskComplete(Irp, STATUS_INVALID_PARAMETER, 0);
        }
        if (Byte >= Crypt->Size || Length > Crypt->Size - Byte) {
            return Ext4DiskComplete(Irp, IrpSp->MajorFunction == IRP_MJ_READ ?
                                     STATUS_END_OF_FILE : STATUS_DISK_FULL, 0);
        }
        if (!ExAcquireRundownProtection(&Crypt->Rundown)) {
            return Ext4DiskComplete(Irp, STATUS_DEVICE_DOES_NOT_EXIST, 0);
        }
        IoMarkIrpPending(Irp);
        if (IrpSp->MajorFunction == IRP_MJ_WRITE &&
            Ext4CryptWriteInline(Crypt, Irp, Byte, Length)) {
            return STATUS_PENDING;
        }
        Ext4CryptQueue(Crypt, Irp, Byte, Length);
        return STATUS_PENDING;
    }

    case IRP_MJ_FLUSH_BUFFERS: {
        NTSTATUS Status;
        if (!ExAcquireRundownProtection(&Crypt->Rundown)) {
            return Ext4DiskComplete(Irp, STATUS_DEVICE_DOES_NOT_EXIST, 0);
        }
        Status = Ext4CryptLowerIo(Crypt, IRP_MJ_FLUSH_BUFFERS, 0, NULL, 0, 0);
        ExReleaseRundownProtection(&Crypt->Rundown);
        return Ext4DiskComplete(Irp, Status, 0);
    }

    case IRP_MJ_DEVICE_CONTROL:
    case IRP_MJ_INTERNAL_DEVICE_CONTROL:
        return Ext4CryptDeviceControl(Crypt, Irp);

    case IRP_MJ_PNP: {
        /* no PnP node: whatever the file system above forwards (target
           device relations for a notification) stays unanswered */
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
Ext4CryptFreeWorkers(IN PEXT4_CRYPT_DEVICE Crypt)
{
    ULONG i;

    for (i = 0; i < EXT4_CRYPT_INLINE_MAX; i++) {
        Ext4XtsFree(&Crypt->Inline[i].Xts);
        if (Crypt->Inline[i].Scratch) {
            RtlSecureZeroMemory(Crypt->Inline[i].Scratch, EXT4_XTS_MAX_UNIT);
            Ext2FreePool(Crypt->Inline[i].Scratch, CRYPT_TAG);
            Crypt->Inline[i].Scratch = NULL;
        }
    }
    Crypt->InlineCount = 0;

    for (i = 0; i < EXT4_CRYPT_WORKERS; i++) {
        PEXT4_CRYPT_WORKER Worker = &Crypt->Worker[i];

        Ext4XtsFree(&Worker->Xts);
        if (Worker->PartMdl) {
            IoFreeMdl(Worker->PartMdl);
            Worker->PartMdl = NULL;
        }
        if (Worker->UserMdl) {
            IoFreeMdl(Worker->UserMdl);
            Worker->UserMdl = NULL;
        }
        if (Worker->BounceMdl) {
            IoFreeMdl(Worker->BounceMdl);
            Worker->BounceMdl = NULL;
        }
        if (Worker->Bounce) {
            RtlSecureZeroMemory(Worker->Bounce, EXT4_CRYPT_BOUNCE);
            Ext2FreePool(Worker->Bounce, CRYPT_TAG);
            Worker->Bounce = NULL;
        }
        if (Worker->Scratch) {
            RtlSecureZeroMemory(Worker->Scratch, EXT4_XTS_MAX_UNIT);
            Ext2FreePool(Worker->Scratch, CRYPT_TAG);
            Worker->Scratch = NULL;
        }
    }
}

/* Stop the threads started so far: one stop entry each, then wait for all. */
static VOID
Ext4CryptStopWorkers(IN PEXT4_CRYPT_DEVICE Crypt)
{
    ULONG i;

    for (i = 0; i < Crypt->Workers; i++) {
        KeInsertQueue(&Crypt->Queue, &Crypt->Worker[i].Stop);
    }
    for (i = 0; i < Crypt->Workers; i++) {
        KeWaitForSingleObject(Crypt->Worker[i].Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Crypt->Worker[i].Thread);
        Crypt->Worker[i].Thread = NULL;
    }
    Crypt->Workers = 0;
}

/* key contexts for inline writes: two per processor, at most 32 */
static NTSTATUS
Ext4CryptStartInline(IN PEXT4_CRYPT_DEVICE Crypt, IN const UCHAR *Key)
{
    ULONG       Count = min(2 * KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS),
                            EXT4_CRYPT_INLINE_MAX);
    ULONG       i;
    NTSTATUS    Status = STATUS_SUCCESS;

    for (i = 0; i < Count; i++) {
        Status = Ext4XtsInit(&Crypt->Inline[i].Xts, Ext4AesEcb, Key, Crypt->KeyBytes);
        if (!NT_SUCCESS(Status)) {
            break;
        }
        Crypt->Inline[i].Scratch = Ext2AllocatePool(NonPagedPoolNx, EXT4_XTS_MAX_UNIT, CRYPT_TAG);
        if (Crypt->Inline[i].Scratch == NULL) {
            Ext4XtsFree(&Crypt->Inline[i].Xts);
            Status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        Crypt->InlineCount++;
    }
    return Status;
}

static NTSTATUS
Ext4CryptStartWorkers(IN PEXT4_CRYPT_DEVICE Crypt, IN const UCHAR *Key, IN ULONG Count)
{
    ULONG       i;
    NTSTATUS    Status = STATUS_SUCCESS;

    for (i = 0; i < Count; i++) {
        PEXT4_CRYPT_WORKER  Worker = &Crypt->Worker[i];
        HANDLE              Handle;

        Worker->Crypt = Crypt;
        Status = Ext4XtsInit(&Worker->Xts, Ext4AesEcb, Key, Crypt->KeyBytes);
        if (!NT_SUCCESS(Status)) {
            break;
        }
        Worker->Bounce = Ext2AllocatePool(NonPagedPoolNx, EXT4_CRYPT_BOUNCE, CRYPT_TAG);
        Worker->Scratch = Ext2AllocatePool(NonPagedPoolNx, EXT4_XTS_MAX_UNIT, CRYPT_TAG);
        if (Worker->Bounce == NULL || Worker->Scratch == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        Worker->BounceMdl = IoAllocateMdl(Worker->Bounce, EXT4_CRYPT_BOUNCE, FALSE, FALSE, NULL);
        Worker->PartMdl = IoAllocateMdl(Worker->Bounce, EXT4_CRYPT_BOUNCE, FALSE, FALSE, NULL);
        /* room for a piece of the caller's buffer at any page offset */
        Worker->UserMdl = IoAllocateMdl(NULL, EXT4_CRYPT_BOUNCE + PAGE_SIZE, FALSE, FALSE, NULL);
        if (Worker->BounceMdl == NULL || Worker->PartMdl == NULL || Worker->UserMdl == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        MmBuildMdlForNonPagedPool(Worker->BounceMdl);

        Status = PsCreateSystemThread(&Handle, THREAD_ALL_ACCESS, NULL, NULL, NULL,
                                      Ext4CryptWorkerThread, Worker);
        if (!NT_SUCCESS(Status)) {
            break;
        }
        Status = ObReferenceObjectByHandle(Handle, SYNCHRONIZE, *PsThreadType, KernelMode,
                                           (PVOID *)&Worker->Thread, NULL);
        ZwClose(Handle);
        /* a handle we just got to a thread we just made: this cannot fail,
           and the thread is waiting on the queue either way */
        ASSERT(NT_SUCCESS(Status));
        Crypt->Workers++;
    }
    return Status;
}

/* Is a file system mounted on the device (ours, RAW, any other)? */
static BOOLEAN
Ext4DeviceMounted(IN PDEVICE_OBJECT Device)
{
    KIRQL   Irql;
    BOOLEAN Mounted;

    IoAcquireVpbSpinLock(&Irql);
    Mounted = Device->Vpb != NULL && IsFlagOn(Device->Vpb->Flags, VPB_MOUNTED);
    IoReleaseVpbSpinLock(Irql);
    return Mounted;
}

/*
 * Take the file system off a device the way mountvol /P does: open the
 * volume, lock it (every other handle must be gone, unless Force), dismount.
 */
NTSTATUS
Ext4DismountDevice(IN PDEVICE_OBJECT Device, IN PCUNICODE_STRING Name, IN BOOLEAN Force)
{
    OBJECT_ATTRIBUTES   Attributes;
    IO_STATUS_BLOCK     IoStatus;
    HANDLE              Handle;
    NTSTATUS            Status;

    if (!Ext4DeviceMounted(Device)) {
        return STATUS_SUCCESS;
    }

    InitializeObjectAttributes(&Attributes, (PUNICODE_STRING)Name,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwCreateFile(&Handle, FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE,
                          &Attributes, &IoStatus, NULL, 0,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    if (Status == STATUS_MEDIA_WRITE_PROTECTED || Status == STATUS_ACCESS_DENIED) {
        /* a read-only volume: reading is enough to lock and dismount it */
        Status = ZwCreateFile(&Handle, FILE_READ_DATA | SYNCHRONIZE,
                              &Attributes, &IoStatus, NULL, 0,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    }
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Status = ZwFsControlFile(Handle, NULL, NULL, NULL, &IoStatus,
                             FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0);
    if (!NT_SUCCESS(Status) && !Force) {
        ZwClose(Handle);
        return STATUS_DEVICE_BUSY;
    }

    Status = ZwFsControlFile(Handle, NULL, NULL, NULL, &IoStatus,
                             FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0);
    ZwClose(Handle);
    return Status;
}

/* Tell the mount manager a device came: it gives it its links. */
NTSTATUS
Ext4AnnounceDevice(IN PCUNICODE_STRING Name)
{
    UCHAR                   Buffer[sizeof(MOUNTMGR_TARGET_NAME) + 64 * sizeof(WCHAR)];
    PMOUNTMGR_TARGET_NAME   Target = (PMOUNTMGR_TARGET_NAME)Buffer;

    if (Name->Length > sizeof(Buffer) - sizeof(MOUNTMGR_TARGET_NAME)) {
        return STATUS_NAME_TOO_LONG;
    }
    Target->DeviceNameLength = Name->Length;
    RtlCopyMemory(Target->DeviceName, Name->Buffer, Name->Length);
    return Ext2MountMgrIoctl(IOCTL_MOUNTMGR_VOLUME_ARRIVAL_NOTIFICATION, Target,
                             FIELD_OFFSET(MOUNTMGR_TARGET_NAME, DeviceName) + Name->Length,
                             NULL, 0, NULL);
}

/*
 * Remove one link the mount manager made (a drive letter, a volume name)
 * and its record of it. The mount manager drops the record even when the
 * link object itself is gone already and it answers STATUS_OBJECT_NAME_NOT_FOUND.
 */
NTSTATUS
Ext4DeleteDevicePoints(IN PCUNICODE_STRING Link)
{
    UCHAR                   In[sizeof(MOUNTMGR_MOUNT_POINT) + EXT4_LINK_CHARS * sizeof(WCHAR)];
    UCHAR                   Out[sizeof(MOUNTMGR_MOUNT_POINTS) + EXT4_POINTS_REPLY];
    PMOUNTMGR_MOUNT_POINT   Point = (PMOUNTMGR_MOUNT_POINT)In;

    if (Link->Length > EXT4_LINK_CHARS * sizeof(WCHAR)) {
        return STATUS_NAME_TOO_LONG;
    }
    RtlZeroMemory(In, sizeof(In));
    Point->SymbolicLinkNameOffset = sizeof(MOUNTMGR_MOUNT_POINT);
    Point->SymbolicLinkNameLength = Link->Length;
    RtlCopyMemory(In + sizeof(MOUNTMGR_MOUNT_POINT), Link->Buffer, Link->Length);
    return Ext2MountMgrIoctl(IOCTL_MOUNTMGR_DELETE_POINTS, In,
                             sizeof(MOUNTMGR_MOUNT_POINT) + Link->Length,
                             Out, sizeof(Out), NULL);
}

/*
 * Make the mount manager forget a device name: every link it holds for it
 * goes, one by one. Found in the list of all its points, never by asking
 * for the device name - for that the mount manager opens the device, and
 * while the driver unloads the I/O manager fails every open of its devices.
 * Named devices have no PnP removal the mount manager would hear of, so a
 * record it keeps outlives the device; the next device of the same name
 * (numbers start at 0 again after a restart of the driver) then counts as
 * known and gets no drive letter. Called when a device goes, and before a
 * new one is announced, for the records an earlier instance left behind.
 */
VOID
Ext4ForgetDevice(IN PCUNICODE_STRING DeviceName)
{
    MOUNTMGR_MOUNT_POINT    All;
    PMOUNTMGR_MOUNT_POINTS  Points = NULL;
    ULONG                   Size = EXT4_POINTS_FIRST, Try, i;
    NTSTATUS                Status = STATUS_BUFFER_OVERFLOW;

    PAGED_CODE();

    RtlZeroMemory(&All, sizeof(All));
    for (Try = 0; Try < EXT4_POINTS_TRIES && Status == STATUS_BUFFER_OVERFLOW; Try++) {
        if (Points) {
            Size = max(Points->Size, Size * 2);
            Ext2FreePool(Points, CRYPT_TAG);
        }
        Points = Ext2AllocatePool(PagedPool, Size, CRYPT_TAG);
        if (Points == NULL) {
            return;
        }
        Status = Ext2MountMgrIoctl(IOCTL_MOUNTMGR_QUERY_POINTS, &All, sizeof(All),
                                   Points, Size, NULL);
    }
    if (!NT_SUCCESS(Status)) {
        Ext2FreePool(Points, CRYPT_TAG);
        return;
    }

    for (i = 0; i < Points->NumberOfMountPoints; i++) {
        PMOUNTMGR_MOUNT_POINT   P = &Points->MountPoints[i];
        UNICODE_STRING          Device, Link;

        if (P->DeviceNameLength == 0 || P->SymbolicLinkNameLength == 0 ||
            (ULONGLONG)P->DeviceNameOffset + P->DeviceNameLength > Size ||
            (ULONGLONG)P->SymbolicLinkNameOffset + P->SymbolicLinkNameLength > Size) {
            continue;
        }
        Device.Buffer = (PWCH)((PUCHAR)Points + P->DeviceNameOffset);
        Device.Length = Device.MaximumLength = P->DeviceNameLength;
        if (!RtlEqualUnicodeString(&Device, DeviceName, TRUE)) {
            continue;
        }
        Link.Buffer = (PWCH)((PUCHAR)Points + P->SymbolicLinkNameOffset);
        Link.Length = Link.MaximumLength = P->SymbolicLinkNameLength;
        Ext4DeleteDevicePoints(&Link);
    }
    Ext2FreePool(Points, CRYPT_TAG);
}

/*
 * Give a device the letter asked for, if it is free. The mount manager may
 * have assigned one itself on arrival; that one is replaced.
 */
VOID
Ext4SetDeviceLetter(IN PCUNICODE_STRING Name, IN WCHAR Wanted)
{
    WCHAR   Current = 0;

    if (Wanted >= L'a' && Wanted <= L'z') {
        Wanted = (WCHAR)(Wanted - L'a' + L'A');
    }
    if (Wanted < L'A' || Wanted > L'Z') {
        return;
    }
    if (Ext2VolumeHasLetter((PUNICODE_STRING)Name, &Current) && Current == Wanted) {
        return;
    }
    if (Ext2LetterInUse(Wanted)) {
        DbgPrint("ext4: %c: is taken, %wZ keeps the letter it has\n", Wanted, Name);
        return;
    }
    if (Current) {
        WCHAR           LinkBuffer[16];
        UNICODE_STRING  Link;
        RtlStringCbPrintfW(LinkBuffer, sizeof(LinkBuffer), L"\\DosDevices\\%c:", Current);
        RtlInitUnicodeString(&Link, LinkBuffer);
        Ext4DeleteDevicePoints(&Link);
    }
    Ext2CreateLetter(Wanted, (PUNICODE_STRING)Name);
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

static NTSTATUS
Ext4CryptUnlock(IN PEXT4_CRYPT_UNLOCK In)
{
    PEXT4_CRYPT_DEVICE  Crypt = NULL;
    PDEVICE_OBJECT      Device = NULL;
    PFILE_OBJECT        LowerFile = NULL;
    PDEVICE_OBJECT      Lower = NULL;
    PDEVICE_OBJECT      Top;
    UNICODE_STRING      Source;
    UNICODE_STRING      Name;
    WCHAR               NameBuffer[48];
    GET_LENGTH_INFORMATION Length;
    ULONGLONG           Size;
    PLIST_ENTRY         List;
    ULONG               Count = 0, Workers, Number;
    NTSTATUS            Status;

    /* the request, field by field */
    if (In->Version != EXT4_CRYPT_VERSION ||
        (In->Cipher != EXT4_CIPHER_AES_XTS_PLAIN64 && In->Cipher != EXT4_CIPHER_AES_XTS_PLAIN) ||
        (In->KeyBytes != 32 && In->KeyBytes != 64) ||
        In->SectorSize < EXT4_SECTOR || In->SectorSize > EXT4_XTS_MAX_UNIT ||
        (In->SectorSize & (In->SectorSize - 1)) != 0 ||
        (In->PayloadOffset & (EXT4_SECTOR - 1)) != 0 ||
        (In->Flags & ~(EXT4_CRYPT_READ_ONLY | EXT4_CRYPT_IV_LARGE_SECTORS)) != 0 ||
        !Ext4CryptTerminated(In->Device, EXT4_CRYPT_DEVICE_CHARS) ||
        strnlen(In->Uuid, EXT4_CRYPT_UUID_CHARS) >= EXT4_CRYPT_UUID_CHARS ||
        _wcsnicmp(In->Device, L"\\Device\\", 8) != 0) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlInitUnicodeString(&Source, In->Device);

    Status = Ext4CryptOpenAes();
    if (!NT_SUCCESS(Status)) {
        DbgPrint("ext4: no AES from CNG (%xh)\n", Status);
        return Status;
    }

    /* one device per partition */
    ExAcquireFastMutex(&Ext4CryptListLock);
    for (List = Ext4CryptList.Flink; List != &Ext4CryptList; List = List->Flink) {
        PEXT4_CRYPT_DEVICE Other = CONTAINING_RECORD(List, EXT4_CRYPT_DEVICE, Link);
        if (_wcsicmp(Other->Source, In->Device) == 0) {
            Status = STATUS_OBJECT_NAME_COLLISION;
        }
        Count++;
    }
    ExReleaseFastMutex(&Ext4CryptListLock);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    if (Count >= EXT4_CRYPT_MAX_VOLUMES) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* the partition: an attributes-only open, which mounts nothing on it;
       the requests go to the top of its own stack */
    Status = IoGetDeviceObjectPointer(&Source, FILE_READ_ATTRIBUTES, &LowerFile, &Top);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Lower = IoGetAttachedDeviceReference(LowerFile->DeviceObject);

    Status = Ext4CryptLowerIoctl(Lower, IOCTL_DISK_GET_LENGTH_INFO, &Length, sizeof(Length));
    if (!NT_SUCCESS(Status)) {
        goto fail;
    }
    if (In->PayloadOffset >= (ULONGLONG)Length.Length.QuadPart) {
        Status = STATUS_INVALID_PARAMETER;
        goto fail;
    }
    Size = (ULONGLONG)Length.Length.QuadPart - In->PayloadOffset;
    if (In->PayloadSize != 0) {
        if (In->PayloadSize > Size) {
            Status = STATUS_INVALID_PARAMETER;
            goto fail;
        }
        Size = In->PayloadSize;
    }
    Size &= ~(ULONGLONG)(In->SectorSize - 1);
    if (Size == 0) {
        Status = STATUS_INVALID_PARAMETER;
        goto fail;
    }

    /* the disk device; raw access to the plaintext for SYSTEM and
       administrators only. Not FILE_DEVICE_SECURE_OPEN: files on the
       volume are the file system's to check. */
    Number = (ULONG)InterlockedIncrement((PLONG)&Ext4CryptNext) - 1;
    RtlStringCbPrintfW(NameBuffer, sizeof(NameBuffer), EXT4_CRYPT_DEVICE_PREFIX L"%u", Number);
    RtlInitUnicodeString(&Name, NameBuffer);
    Status = IoCreateDeviceSecure(Ext2Global->DiskdevObject->DriverObject,
                                  sizeof(EXT4_CRYPT_DEVICE), &Name, FILE_DEVICE_DISK, 0,
                                  FALSE, &SDDL_DEVOBJ_SYS_ALL_ADM_ALL, NULL, &Device);
    if (!NT_SUCCESS(Status)) {
        goto fail;
    }

    Crypt = (PEXT4_CRYPT_DEVICE)Device->DeviceExtension;
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
    RtlStringCbCopyW(Crypt->NameBuffer, sizeof(Crypt->NameBuffer), NameBuffer);
    RtlInitUnicodeString(&Crypt->Name, Crypt->NameBuffer);
    RtlStringCbCopyW(Crypt->Source, sizeof(Crypt->Source), In->Device);
    RtlStringCbCopyA(Crypt->Uuid, sizeof(Crypt->Uuid), In->Uuid);
    KeInitializeQueue(&Crypt->Queue, 0);
    ExInitializeRundownProtection(&Crypt->Rundown);
    ExInitializeResourceLite(&Crypt->Rmw);

    Workers = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    Status = Ext4CryptStartWorkers(Crypt, In->Key, min(2 * Workers, EXT4_CRYPT_WORKERS));
    if (NT_SUCCESS(Status)) {
        Status = Ext4CryptStartInline(Crypt, In->Key);
    }
    if (!NT_SUCCESS(Status)) {
        Ext4CryptStopWorkers(Crypt);
        KeRundownQueue(&Crypt->Queue);
        Ext4CryptFreeWorkers(Crypt);
        ExDeleteResourceLite(&Crypt->Rmw);
        IoDeleteDevice(Device);
        goto fail;
    }

    SetFlag(Device->Flags, DO_DIRECT_IO);
    Device->AlignmentRequirement = Lower->AlignmentRequirement;
    Device->SectorSize = EXT4_SECTOR;          /* 512e, see IOCTL_DISK_GET_DRIVE_GEOMETRY */
    ClearFlag(Device->Flags, DO_DEVICE_INITIALIZING);

    ExAcquireFastMutex(&Ext4CryptListLock);
    InsertTailList(&Ext4CryptList, &Crypt->Link);
    ExReleaseFastMutex(&Ext4CryptListLock);

    DbgPrint("ext4: %ws unlocked as %wZ (%I64u bytes from %I64u, %u-byte sectors)\n",
             In->Device, &Crypt->Name, Size, In->PayloadOffset, In->SectorSize);

    /* links from the mount manager, the letter asked for, then a first
       open on a worker: that is when the file system mounts */
    Ext4ForgetDevice(&Crypt->Name);
    Status = Ext4AnnounceDevice(&Crypt->Name);
    if (!NT_SUCCESS(Status)) {
        DbgPrint("ext4: the mount manager did not take %wZ (%xh)\n", &Crypt->Name, Status);
    }
    if (In->Letter) {
        Ext4SetDeviceLetter(&Crypt->Name, In->Letter);
    }
    Ext2QueueVolumeProbe(&Crypt->Name);

    In->Index = Number;
    return STATUS_SUCCESS;

fail:
    if (Lower) {
        ObDereferenceObject(Lower);
    }
    if (LowerFile) {
        ObDereferenceObject(LowerFile);
    }
    return Status;
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
