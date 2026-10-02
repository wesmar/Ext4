/**
 * crypt_io.c - LUKS I/O: lower requests, XTS sectors, the worker threads, inline writes.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * A write is never encrypted in place - the caller's pages are the cache.
 * An aligned write is encrypted in the caller's own thread, from its pages
 * into a buffer of its own, and sent on without waiting: the disk's
 * completion finishes it (Ext4CryptWriteInline). Reads, and whatever the
 * inline path cannot take, go to system threads with key schedules and
 * bounce buffers allocated up front, so a paging write always makes
 * progress; a long aligned request is cut into pieces the threads take in
 * parallel. Lower I/O uses IRPs of our own with a completion routine, never
 * thread-queued synchronous requests, so nothing depends on APC delivery.
 */

#include "ext4fs.h"
#include "crypt_internal.h"

static KSTART_ROUTINE           Ext4CryptWorkerThread;
static IO_COMPLETION_ROUTINE    Ext4CryptLowerDone;

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
NTSTATUS
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

VOID
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
VOID
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

/* key contexts for inline writes: one for each writer running and one for
   each preempted while it holds one - EXT4_CRYPT_PER_CPU per processor -
   bounded by the bits of InlineBusy; a writer finding none free hands its
   piece to the workers */
NTSTATUS
Ext4CryptStartInline(IN PEXT4_CRYPT_DEVICE Crypt, IN const UCHAR *Key)
{
    ULONG       Count = (ULONG)min(KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS) *
                                   EXT4_CRYPT_PER_CPU, EXT4_CRYPT_INLINE_MAX);
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

NTSTATUS
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
