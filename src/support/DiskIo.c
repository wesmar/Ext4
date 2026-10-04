/**
 * DiskIo.c - synchronous and asynchronous I/O to the underlying disk.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"

NTSTATUS
Ext2ReadWriteBlockSyncCompletionRoutine (
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Context    );

NTSTATUS
Ext2ReadWriteBlockAsyncCompletionRoutine (
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Context    );

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2LockUserBuffer)
#pragma alloc_text(PAGE, Ext2ReadSync)
#pragma alloc_text(PAGE, Ext2ReadDisk)
#pragma alloc_text(PAGE, Ext2DiskIoControl)
#pragma alloc_text(PAGE, Ext2DiskShutDown)
#endif

PMDL
Ext2CreateMdl (
    IN PVOID Buffer,
    IN ULONG Length,
    IN LOCK_OPERATION op
)
{
    NTSTATUS Status;
    PMDL Mdl = NULL;

    ASSERT (Buffer != NULL);
    Mdl = IoAllocateMdl (Buffer, Length, FALSE, FALSE, NULL);
    if (Mdl == NULL) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
    } else {
        __try {
            /* valid for pageable and non-paged memory alike (the one caller
               maps views of the volume stream, which are pageable) */
            MmProbeAndLockPages(Mdl, KernelMode, op);
            Status = STATUS_SUCCESS;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            IoFreeMdl (Mdl);
            Mdl = NULL;
            Status = STATUS_INVALID_USER_BUFFER;
        }
    }
    return Mdl;
}

VOID
Ext2DestroyMdl (IN PMDL Mdl)
{
    ASSERT (Mdl != NULL);
    while (Mdl) {
        PMDL Next;
        Next = Mdl->Next;
        Mdl->Next = NULL;
        if (IsFlagOn(Mdl->MdlFlags, MDL_PAGES_LOCKED)) {
            MmUnlockPages (Mdl);
        }
        IoFreeMdl (Mdl);
        Mdl = Next;
    }
}

NTSTATUS
Ext2LockUserBuffer (IN PIRP     Irp,
                    IN ULONG            Length,
                    IN LOCK_OPERATION   Operation)
{
    NTSTATUS Status;
    ASSERT(Irp != NULL);

    /* nothing to lock: no MDL describes an empty buffer */
    if (Irp->MdlAddress != NULL || Length == 0) {
        return STATUS_SUCCESS;
    }

    IoAllocateMdl(Irp->UserBuffer, Length, FALSE, FALSE, Irp);
    if (Irp->MdlAddress == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    __try {

        MmProbeAndLockPages(Irp->MdlAddress, Irp->RequestorMode, Operation);
        Status = STATUS_SUCCESS;

    } __except (EXCEPTION_EXECUTE_HANDLER) {

        IoFreeMdl(Irp->MdlAddress);
        Irp->MdlAddress = NULL;
        Status = STATUS_INVALID_USER_BUFFER;
    }

    return Status;
}

PVOID
Ext2GetUserBuffer (IN PIRP Irp )
{
    ASSERT(Irp != NULL);

    if (Irp->MdlAddress) {

        return MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
    } else {

        return Irp->UserBuffer;
    }
}

NTSTATUS
Ext2ReadWriteBlockSyncCompletionRoutine (
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Context    )
{
    UNREFERENCED_PARAMETER(DeviceObject);
    PEXT2_RW_CONTEXT pContext = (PEXT2_RW_CONTEXT)Context;

    if (Irp != pContext->MasterIrp) {

        if (!NT_SUCCESS(Irp->IoStatus.Status)) {
            pContext->MasterIrp->IoStatus = Irp->IoStatus;
        }

        IoFreeMdl(Irp->MdlAddress);
        IoFreeIrp(Irp );
    }

    if (InterlockedDecrement(&pContext->Blocks) == 0) {

        pContext->MasterIrp->IoStatus.Information = 0;
        if (NT_SUCCESS(pContext->MasterIrp->IoStatus.Status)) {

            pContext->MasterIrp->IoStatus.Information =
                pContext->Length;
        }

        KeSetEvent(&pContext->Event, 0, FALSE);
    }

    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS
Ext2ReadWriteBlockAsyncCompletionRoutine (
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Context
)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    PEXT2_RW_CONTEXT pContext = (PEXT2_RW_CONTEXT)Context;
    PIO_STACK_LOCATION iosp;

    ASSERT(FALSE == pContext->Wait);

    if (Irp != pContext->MasterIrp && !NT_SUCCESS(Irp->IoStatus.Status)) {
        pContext->MasterIrp->IoStatus = Irp->IoStatus;
    }

    if (InterlockedDecrement(&pContext->Blocks) == 0) {

        if (NT_SUCCESS(pContext->MasterIrp->IoStatus.Status)) {

            /* set written bytes to status information */
            pContext->MasterIrp->IoStatus.Information = pContext->Length;

            if (pContext->FileObject != NULL && !IsFlagOn(pContext->MasterIrp->Flags, IRP_PAGING_IO)) {

                /* modify FileObject flags, skip this for volume direct access */
                SetFlag( pContext->FileObject->Flags,
                         IsFlagOn(pContext->Flags, EXT2_RW_CONTEXT_WRITE) ?
                         FO_FILE_MODIFIED : FO_FILE_FAST_IO_READ);

                /* update Current Byteoffset */
                if (IsFlagOn(pContext->FileObject->Flags, FO_SYNCHRONOUS_IO)) {
                    iosp = IoGetCurrentIrpStackLocation(pContext->MasterIrp);
                    pContext->FileObject->CurrentByteOffset.QuadPart =
                        iosp->Parameters.Read.ByteOffset.QuadPart +  pContext->Length;
                }
            }

        } else {

            pContext->MasterIrp->IoStatus.Information = 0;
        }

        /* release the locked resource acquired by the caller */
        if (pContext->Resource) {
            ExReleaseResourceForThreadLite(pContext->Resource, pContext->ThreadId);
        }

        Ext2FreePool(pContext, EXT2_RWC_MAGIC);
        DEC_MEM_COUNT(PS_RW_CONTEXT, pContext, sizeof(EXT2_RW_CONTEXT));
    }

    return STATUS_SUCCESS;
}

NTSTATUS
Ext2ReadWriteBlocks(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_EXTENT         Chain,
    IN ULONG                Length
    )
{
    PIRP                Irp;
    PIRP                MasterIrp = IrpContext->Irp;
    PIO_STACK_LOCATION  IrpSp;
    PMDL                Mdl;
    PEXT2_RW_CONTEXT    pContext = NULL;
    PEXT2_EXTENT        Extent;
    NTSTATUS            Status = STATUS_SUCCESS;
    BOOLEAN             bMasterCompleted = FALSE;
    BOOLEAN             bBugCheck = FALSE;

    ASSERT(MasterIrp);

    __try {

        pContext = Ext2AllocatePool(NonPagedPool, sizeof(EXT2_RW_CONTEXT), EXT2_RWC_MAGIC);

        if (!pContext) {
            DEBUG(DL_ERR, ( "Ext2ReadWriteBlocks: failed to allocate pContext.\n"));
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        INC_MEM_COUNT(PS_RW_CONTEXT, pContext, sizeof(EXT2_RW_CONTEXT));
        RtlZeroMemory(pContext, sizeof(EXT2_RW_CONTEXT));
        pContext->Wait = Ext2CanIWait();
        pContext->MasterIrp = MasterIrp;
        pContext->Length = Length;

        if (IrpContext->MajorFunction == IRP_MJ_WRITE) {
            SetFlag(pContext->Flags, EXT2_RW_CONTEXT_WRITE);
        }

        if (pContext->Wait) {

            KeInitializeEvent(&(pContext->Event), NotificationEvent, FALSE);

        } else if (IrpContext->Fcb->Identifier.Type == EXT2FCB) {

            if (IsFlagOn(MasterIrp->Flags, IRP_PAGING_IO)) {
                pContext->Resource = &IrpContext->Fcb->PagingIoResource;
            } else {
                pContext->Resource = &IrpContext->Fcb->MainResource;
            }

            pContext->FileObject = IrpContext->FileObject;
            pContext->ThreadId = ExGetCurrentResourceThread();
        }


        if (NULL == Chain->Next && 0 == Chain->Offset) {

            /* we get only 1 extent to dispatch, then don't bother allocating new irps */

            /* setup the Stack location to do a read from the disk driver. */
            IrpSp = IoGetNextIrpStackLocation(MasterIrp);
            IrpSp->MajorFunction = IrpContext->MajorFunction;
            IrpSp->Parameters.Read.Length = Chain->Length;
            IrpSp->Parameters.Read.ByteOffset.QuadPart = Chain->Lba;
            if (IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WRITE_THROUGH)) {
                SetFlag(IrpSp->Flags, SL_WRITE_THROUGH);
            }
            if (IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_VERIFY_READ)) {
                SetFlag(IrpSp->Flags, SL_OVERRIDE_VERIFY_VOLUME);
            }

            IoSetCompletionRoutine(
                    MasterIrp,
                    Ext2CanIWait() ?
                    Ext2ReadWriteBlockSyncCompletionRoutine :
                    Ext2ReadWriteBlockAsyncCompletionRoutine,
                    (PVOID) pContext,
                    TRUE,
                    TRUE,
                    TRUE );

            /* intialize context block */
            Chain->Irp = MasterIrp;
            pContext->Blocks = 1;

        } else {

            for (Extent = Chain; Extent != NULL; Extent = Extent->Next) {

                Irp = IoMakeAssociatedIrp(
                          MasterIrp,
                          (CCHAR)(Vcb->TargetDeviceObject->StackSize + 1) );

                if (!Irp) {
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    __leave;
                }

                Mdl = IoAllocateMdl( (PCHAR)MasterIrp->UserBuffer +
                                     Extent->Offset,
                                     Extent->Length,
                                     FALSE,
                                     FALSE,
                                     Irp );

                if (!Mdl)  {
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    __leave;
                }

                IoBuildPartialMdl( MasterIrp->MdlAddress,
                                   Mdl,
                                   (PCHAR)MasterIrp->UserBuffer+Extent->Offset,
                                   Extent->Length );

                IoSetNextIrpStackLocation(Irp);
                IrpSp = IoGetCurrentIrpStackLocation(Irp);

                IrpSp->MajorFunction = IrpContext->MajorFunction;
                IrpSp->Parameters.Read.Length = Extent->Length;
                IrpSp->Parameters.Read.ByteOffset.QuadPart = Extent->Lba;

                IoSetCompletionRoutine(
                    Irp,
                    Ext2CanIWait() ?
                    Ext2ReadWriteBlockSyncCompletionRoutine :
                    Ext2ReadWriteBlockAsyncCompletionRoutine,
                    (PVOID) pContext,
                    TRUE,
                    TRUE,
                    TRUE );

                IrpSp = IoGetNextIrpStackLocation(Irp);

                IrpSp->MajorFunction = IrpContext->MajorFunction;
                IrpSp->Parameters.Read.Length =Extent->Length;
                IrpSp->Parameters.Read.ByteOffset.QuadPart = Extent->Lba;

                /* set write through flag */
                if (IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WRITE_THROUGH)) {
                    SetFlag( IrpSp->Flags, SL_WRITE_THROUGH );
                }

                /* set verify flag */
                if (IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_VERIFY_READ)) {
                    SetFlag(IrpSp->Flags, SL_OVERRIDE_VERIFY_VOLUME);
                }

                Extent->Irp = Irp;
                pContext->Blocks += 1;
            }

            MasterIrp->AssociatedIrp.IrpCount = pContext->Blocks;
            if (Ext2CanIWait()) {
                MasterIrp->AssociatedIrp.IrpCount += 1;
            }
        }
        if (!Ext2CanIWait()) {
            /* mark MasterIrp pending */
            IoMarkIrpPending(pContext->MasterIrp);
        }

        bBugCheck = TRUE;

        for (Extent = Chain; Extent != NULL; Extent = Extent->Next) {
            Status = IoCallDriver ( Vcb->TargetDeviceObject,
                                    Extent->Irp);
            Extent->Irp = NULL;
        }

        if (Ext2CanIWait()) {
            LARGE_INTEGER Timeout;
            Timeout.QuadPart = (LONGLONG)-30 * 10 * 1000 * 1000; /* 30 seconds */
            Status = KeWaitForSingleObject( &(pContext->Event),
                                   Executive, KernelMode, FALSE, &Timeout );
            if (Status == STATUS_TIMEOUT) {
                /* Device likely removed — unblock and report error */
                MasterIrp->IoStatus.Status = STATUS_IO_TIMEOUT;
                MasterIrp->IoStatus.Information = 0;
            }
            KeClearEvent( &(pContext->Event) );
        } else {
            bMasterCompleted = TRUE;
        }

    } __finally {

        for (Extent = Chain; Extent != NULL; Extent = Extent->Next)  {
            if (Extent->Irp != NULL ) {
                if (Extent->Irp->MdlAddress != NULL) {
                    IoFreeMdl(Extent->Irp->MdlAddress );
                }
                IoFreeIrp(Extent->Irp);
            }
        }

        if (IrpContext->ExceptionInProgress) {

            if (bBugCheck) {
                Ext2BugCheck(EXT2_BUGCHK_BLOCK, 0, 0, 0);
            }

        } else {

            if (Ext2CanIWait()) {
                if (MasterIrp) {
                    Status = MasterIrp->IoStatus.Status;
                }
                if (pContext) {
                    Ext2FreePool(pContext, EXT2_RWC_MAGIC);
                    DEC_MEM_COUNT(PS_RW_CONTEXT, pContext, sizeof(EXT2_RW_CONTEXT));
                }
            } else {
                if (bMasterCompleted) {
                    IrpContext->Irp = NULL;
                    Status = STATUS_PENDING;
                }
            }
        }
    }

    return Status;
}

NTSTATUS
Ext2ReadSync(
    IN PEXT2_VCB        Vcb,
    IN ULONGLONG        Offset,
    IN ULONG            Length,
    OUT PVOID           Buffer
)
{
    PKEVENT         Event = NULL;

    PIRP            Irp;
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS        Status = STATUS_INSUFFICIENT_RESOURCES;


    ASSERT(Vcb != NULL);
    ASSERT(Vcb->TargetDeviceObject != NULL);
    ASSERT(Buffer != NULL);

    __try {

        Event = Ext2AllocatePool(NonPagedPool, sizeof(KEVENT), 'EK2E');

        if (NULL == Event) {
            DEBUG(DL_ERR, ( "Ext2ReadSync: failed to allocate Event.\n"));
            __leave;
        }

        INC_MEM_COUNT(PS_DISK_EVENT, Event, sizeof(KEVENT));

        KeInitializeEvent(Event, NotificationEvent, FALSE);

        Irp = IoBuildSynchronousFsdRequest(
                  IRP_MJ_READ,
                  Vcb->TargetDeviceObject,
                  Buffer,
                  Length,
                  (PLARGE_INTEGER)(&Offset),
                  Event,
                  &IoStatus
              );

        if (!Irp) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        Status = IoCallDriver(Vcb->TargetDeviceObject, Irp);

        if (Status == STATUS_PENDING) {
            LARGE_INTEGER Timeout;
            Timeout.QuadPart = (LONGLONG)-30 * 10 * 1000 * 1000; /* 30 seconds */
            Status = KeWaitForSingleObject(
                Event,
                Suspended,
                KernelMode,
                FALSE,
                &Timeout
            );

            if (Status == STATUS_TIMEOUT) {
                IoCancelIrp(Irp);
                KeWaitForSingleObject(Event, Executive, KernelMode, FALSE, NULL);
                Status = STATUS_IO_TIMEOUT;
            } else {
                Status = IoStatus.Status;
            }
        }

    } __finally {

        if (Event) {
            Ext2FreePool(Event, 'EK2E');
            DEC_MEM_COUNT(PS_DISK_EVENT, Event, sizeof(KEVENT));
        }
    }

    return Status;
}

NTSTATUS
Ext2ReadDisk(
    IN PEXT2_VCB   Vcb,
    IN ULONGLONG   Offset,
    IN ULONG       Size,
    IN PVOID       Buffer,
    IN BOOLEAN     bVerify )
{
    UNREFERENCED_PARAMETER(bVerify);
    NTSTATUS    Status;
    PUCHAR      Buf;
    ULONG       Length;
    ULONGLONG   Lba;

    Lba = Offset & (~((ULONGLONG)SECTOR_SIZE - 1));
    Length = (ULONG)(Size + Offset + SECTOR_SIZE - 1 - Lba) &
             (~((ULONG)SECTOR_SIZE - 1));

    Buf = Ext2AllocatePool(PagedPool, Length, EXT2_DATA_MAGIC);
    if (!Buf) {
        DEBUG(DL_ERR, ( "Ext2ReadDisk: failed to allocate Buffer.\n"));
        Status = STATUS_INSUFFICIENT_RESOURCES;

        goto errorout;
    }
    INC_MEM_COUNT(PS_DISK_BUFFER, Buf, Length);

    Status = Ext2ReadSync(  Vcb,
                            Lba,
                            Length,
                            Buf );

    if (!NT_SUCCESS(Status)) {
        DEBUG(DL_ERR, ("Ext2ReadDisk: disk i/o error: %xh.\n", Status));
        goto errorout;
    }

    RtlCopyMemory(Buffer, &Buf[Offset - Lba], Size);

errorout:

    if (Buf) {
        Ext2FreePool(Buf, EXT2_DATA_MAGIC);
        DEC_MEM_COUNT(PS_DISK_BUFFER, Buf, Length);
    }

    return Status;
}

/*
 * Ext2WriteDiskSync: synchronous, uncached write straight to the target
 * device. Used by the journal engine for log blocks, the journal superblock
 * and the fs superblock markers, which must never go through the volume
 * cache (their ordering relative to other writes is what makes the journal
 * work). Offset and Length must be sector aligned; Buffer must be non-paged.
 */
NTSTATUS
Ext2WriteDiskSync(
    IN PEXT2_VCB       Vcb,
    IN ULONGLONG       Offset,
    IN ULONG           Length,
    IN PVOID           Buffer,
    IN BOOLEAN         WriteThrough )
{
    KEVENT              Event;
    PIRP                Irp;
    PIO_STACK_LOCATION  IrpSp;
    IO_STATUS_BLOCK     IoStatus;
    NTSTATUS            Status;

    ASSERT(Vcb != NULL);
    ASSERT(Vcb->TargetDeviceObject != NULL);
    ASSERT(Buffer != NULL);

    if ((Offset & (SECTOR_SIZE - 1)) || (Length & (SECTOR_SIZE - 1))) {
        return STATUS_INVALID_PARAMETER;
    }

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    Irp = IoBuildSynchronousFsdRequest(
              IRP_MJ_WRITE,
              Vcb->TargetDeviceObject,
              Buffer,
              Length,
              (PLARGE_INTEGER)(&Offset),
              &Event,
              &IoStatus
          );

    if (!Irp) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    IrpSp = IoGetNextIrpStackLocation(Irp);
    if (WriteThrough) {
        SetFlag(IrpSp->Flags, SL_WRITE_THROUGH);
    }
    SetFlag(Irp->Flags, IRP_NOCACHE);

    Status = IoCallDriver(Vcb->TargetDeviceObject, Irp);

    if (Status == STATUS_PENDING) {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }

    if (NT_SUCCESS(Status) && IoStatus.Information != Length) {
        Status = STATUS_UNEXPECTED_IO_ERROR;
    }

    return Status;
}

/*
 * Ext2FlushDisk: ask the storage stack to flush its write cache so that
 * everything written so far is durable. This is the barrier between the
 * journal data blocks and the commit block.
 */
NTSTATUS
Ext2FlushDisk(
    IN PEXT2_VCB       Vcb )
{
    KEVENT              Event;
    PIRP                Irp;
    IO_STATUS_BLOCK     IoStatus;
    NTSTATUS            Status;
    LONG64              Number;

    ASSERT(Vcb != NULL);
    ASSERT(Vcb->TargetDeviceObject != NULL);

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    /* numbered before it is sent: it covers every write completed by now */
    Number = InterlockedIncrement64(&Vcb->FlushIssued);

    Irp = IoBuildSynchronousFsdRequest(
              IRP_MJ_FLUSH_BUFFERS,
              Vcb->TargetDeviceObject,
              NULL,
              0,
              NULL,
              &Event,
              &IoStatus
          );

    if (!Irp) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = IoCallDriver(Vcb->TargetDeviceObject, Irp);

    if (Status == STATUS_PENDING) {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }

    /* a device without a write cache says "not supported": that is fine */
    if (Status == STATUS_INVALID_DEVICE_REQUEST || Status == STATUS_NOT_SUPPORTED) {
        Status = STATUS_SUCCESS;
    }

    if (NT_SUCCESS(Status)) {
        LONG64 Covered = Vcb->FlushCovered;
        while (Covered < Number) {
            LONG64 Seen = InterlockedCompareExchange64(&Vcb->FlushCovered, Number, Covered);
            if (Seen == Covered) {
                break;
            }
            Covered = Seen;
        }
    }

    return Status;
}

LONG64
Ext2FlushMark(
    IN PEXT2_VCB       Vcb )
{
    return InterlockedCompareExchange64(&Vcb->FlushIssued, 0, 0);
}

BOOLEAN
Ext2FlushedSince(
    IN PEXT2_VCB       Vcb,
    IN LONG64          Mark )
{
    return InterlockedCompareExchange64(&Vcb->FlushCovered, 0, 0) > Mark;
}

NTSTATUS
Ext2DiskIoControl (
    IN PDEVICE_OBJECT   DeviceObject,
    IN ULONG            IoctlCode,
    IN PVOID            InputBuffer,
    IN ULONG            InputBufferSize,
    IN OUT PVOID        OutputBuffer,
    IN OUT PULONG       OutputBufferSize)
{
    ULONG           OutBufferSize = 0;
    KEVENT          Event;
    PIRP            Irp;
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS        Status;

    ASSERT(DeviceObject != NULL);

    if (OutputBufferSize)
    {
        OutBufferSize = *OutputBufferSize;
    }

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    Irp = IoBuildDeviceIoControlRequest(
              IoctlCode,
              DeviceObject,
              InputBuffer,
              InputBufferSize,
              OutputBuffer,
              OutBufferSize,
              FALSE,
              &Event,
              &IoStatus
          );

    if (Irp == NULL) {
        DEBUG(DL_ERR, ( "Ext2DiskIoControl: failed to build Irp!\n"));
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = IoCallDriver(DeviceObject, Irp);

    if (Status == STATUS_PENDING)  {
        LARGE_INTEGER Timeout;
        Timeout.QuadPart = (LONGLONG)-30 * 10 * 1000 * 1000; /* 30 seconds */
        Status = KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, &Timeout);
        if (Status == STATUS_TIMEOUT) {
            IoCancelIrp(Irp);
            KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
            Status = STATUS_IO_TIMEOUT;
        } else {
            Status = IoStatus.Status;
        }
    }

    if (OutputBufferSize) {
        *OutputBufferSize = (ULONG)(IoStatus.Information);
    }

    return Status;
}

NTSTATUS
Ext2DiskShutDown(PEXT2_VCB Vcb)
{
    PIRP                Irp;
    KEVENT              Event;

    NTSTATUS            Status;
    IO_STATUS_BLOCK     IoStatus;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_SHUTDOWN,
                                       Vcb->TargetDeviceObject,
                                       NULL,
                                       0,
                                       NULL,
                                       &Event,
                                       &IoStatus);

    if (Irp) {
        Status = IoCallDriver(Vcb->TargetDeviceObject, Irp);

        if (Status == STATUS_PENDING) {
            LARGE_INTEGER Timeout;
            Timeout.QuadPart = (LONGLONG)-30 * 10 * 1000 * 1000; /* 30 seconds */
            Status = KeWaitForSingleObject(&Event,
                                  Executive,
                                  KernelMode,
                                  FALSE,
                                  &Timeout);

            if (Status == STATUS_TIMEOUT) {
                IoCancelIrp(Irp);
                KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
                Status = STATUS_IO_TIMEOUT;
            } else {
                Status = IoStatus.Status;
            }
        }
    } else  {
        Status = IoStatus.Status;
    }

    return Status;
}
