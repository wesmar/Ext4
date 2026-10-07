/**
 * VolumeWrite.c - IRP_MJ_WRITE on the volume stream: dirty extent gated metadata writes.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "write_internal.h"

/* one write to the volume: the metadata stream's, or a volume handle's */
typedef struct _EXT2_VOLUME_WRITE {
    PIRP            Irp;            /* NULL once the write went asynchronous */
    PFILE_OBJECT    FileObject;
    PEXT2_CCB       Ccb;            /* a volume handle's; NULL for the stream */
    LARGE_INTEGER   ByteOffset;
    ULONG           Length;
    BOOLEAN         PagingIo;
    BOOLEAN         Nocache;
    BOOLEAN         SynchronousIo;
    BOOLEAN         Deferred;       /* CcDeferWrite owns the request */
    BOOLEAN         Done;           /* finished before writing anything */
} EXT2_VOLUME_WRITE, *PEXT2_VOLUME_WRITE;

static VOID
Ext2VolumeWriteParameters(IN PEXT2_IRP_CONTEXT IrpContext, OUT PEXT2_VOLUME_WRITE V)
{
    PIO_STACK_LOCATION IrpSp;

    RtlZeroMemory(V, sizeof(EXT2_VOLUME_WRITE));
    V->Irp = IrpContext->Irp;
    V->FileObject = IrpContext->FileObject;
    V->Ccb = (PEXT2_CCB) V->FileObject->FsContext2;

    IrpSp = IoGetCurrentIrpStackLocation(V->Irp);
    V->Length = IrpSp->Parameters.Write.Length;
    V->ByteOffset = IrpSp->Parameters.Write.ByteOffset;

    /* a volume handle never goes through the cache */
    V->PagingIo = IsFlagOn(V->Irp->Flags, IRP_PAGING_IO);
    V->Nocache = IsFlagOn(V->Irp->Flags, IRP_NOCACHE) || V->Ccb != NULL;
    V->SynchronousIo = IsFlagOn(V->FileObject->Flags, FO_SYNCHRONOUS_IO);
    if (V->PagingIo) {
        ASSERT(V->Nocache);
    }
}

/* what makes a volume write wrong, or finished, before anything is done */
static NTSTATUS
Ext2VolumeWriteCheck(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN OUT PEXT2_VOLUME_WRITE V)
{
    if (V->Length == 0) {
        V->Irp->IoStatus.Information = 0;
        V->Done = TRUE;
        return STATUS_SUCCESS;
    }

    if (V->Nocache &&
        ((V->ByteOffset.LowPart & (SECTOR_SIZE - 1)) || (V->Length & (SECTOR_SIZE - 1)))) {
        return STATUS_INVALID_PARAMETER;
    }

    if (FlagOn(IrpContext->MinorFunction, IRP_MN_DPC)) {
        ClearFlag(IrpContext->MinorFunction, IRP_MN_DPC);
        return STATUS_PENDING;
    }

    /* the stream is the partition: nothing past it, and a write is cut at
       its end (an extended DASD handle could go no further either) */
    if (V->ByteOffset.QuadPart >= Vcb->Header.FileSize.QuadPart) {
        V->Irp->IoStatus.Information = 0;
        V->Done = TRUE;
        return STATUS_END_OF_FILE;
    }
    if (V->ByteOffset.QuadPart + V->Length > Vcb->Header.FileSize.QuadPart) {
        V->Length = (ULONG)(Vcb->Header.FileSize.QuadPart - V->ByteOffset.QuadPart);
    }
    return STATUS_SUCCESS;
}

/*
 * The cache and the disk agree before a write that goes past the cache. A
 * volume handle's first write purges what the file system has cached (a
 * locked volume has nothing of it left to purge). A non-cached write of the
 * stream writes back and purges the range it covers.
 */
static NTSTATUS
Ext2VolumeWriteCoherent(IN PEXT2_VCB Vcb, IN OUT PEXT2_VOLUME_WRITE V)
{
    NTSTATUS Status;

    if (V->Ccb != NULL && !V->PagingIo) {
        if (!FlagOn(V->Ccb->Flags, CCB_VOLUME_DASD_PURGE)) {
            if (!FlagOn(Vcb->Flags, VCB_VOLUME_LOCKED)) {
                /* best effort, as NTFS: what cannot be purged is in use */
                (void)Ext2PurgeVolume(Vcb, TRUE);
            }
            SetLongFlag(V->Ccb->Flags, CCB_VOLUME_DASD_PURGE);
        }
        return STATUS_SUCCESS;
    }

    if (!V->Nocache || V->PagingIo || Vcb->SectionObject.DataSectionObject == NULL) {
        return STATUS_SUCCESS;
    }

    ExAcquireResourceExclusiveLite(&Vcb->MainResource, TRUE);
    Status = Ext2FlushPurgeRange(&Vcb->SectionObject, &Vcb->PagingIoResource,
                                 &V->ByteOffset, V->Length, &V->Irp->IoStatus);
    ExReleaseResourceLite(&Vcb->MainResource);
    return Status;
}

/* A cached write of the stream. Its paging write writes registered ranges
   only: the range is registered first - one the cache held unregistered
   would never reach the disk. */
static NTSTATUS
Ext2VolumeWriteCached(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN OUT PEXT2_VOLUME_WRITE V)
{
    PUCHAR Buffer;

    if (FlagOn(IrpContext->MinorFunction, IRP_MN_MDL)) {

        CcPrepareMdlWrite(Vcb->Volume, &V->ByteOffset, V->Length,
                          &V->Irp->MdlAddress, &V->Irp->IoStatus);

    } else {

        Buffer = Ext2GetUserBuffer(V->Irp);
        if (Buffer == NULL) {
            return STATUS_INVALID_USER_BUFFER;
        }
        if (!Ext2AddVcbExtent(Vcb, V->ByteOffset.QuadPart, (LONGLONG)V->Length)) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        CcCopyWrite(Vcb->Volume, &V->ByteOffset, V->Length, TRUE, Buffer);
    }

    if (NT_SUCCESS(V->Irp->IoStatus.Status)) {
        V->Irp->IoStatus.Information = V->Length;
    }
    return V->Irp->IoStatus.Status;
}

/*
 * The registered ranges of [Offset, Offset + Length) as a chain of runs to
 * write: what the file system wrote through the cache. A sector nobody
 * registered is left as it is on the disk.
 */
static NTSTATUS
Ext2VolumeDirtyChain(IN PEXT2_VCB Vcb, IN LONGLONG Offset, IN ULONG Length, OUT PEXT2_EXTENT *Chain)
{
    LONGLONG        DirtyStart;
    LONGLONG        DirtyLba = Offset;
    LONGLONG        DirtyLength;
    LONGLONG        RemainLength = (LONGLONG)Length;
    PEXT2_EXTENT    Extent;
    PEXT2_EXTENT    List = NULL;

    *Chain = NULL;
    ASSERT(Length >= SECTOR_SIZE);

    while (RemainLength > 0) {

        DirtyStart = DirtyLba;
        ASSERT(DirtyStart >= Offset);
        ASSERT(DirtyStart <= Offset + Length);

        if (!Ext2LookupVcbExtent(Vcb, DirtyStart, &DirtyLba, &DirtyLength)) {
            /* not registered: the next sector */
            if (RemainLength > SECTOR_SIZE) {
                DirtyLba = DirtyStart + SECTOR_SIZE;
                RemainLength -= SECTOR_SIZE;
            } else {
                RemainLength = 0;
            }
            continue;
        }

        if (DirtyLba == -1) {
            /* a gap DirtyLength long: past it */
            DirtyLba = DirtyStart + DirtyLength;
            RemainLength = Offset + Length > DirtyLba ? Offset + Length - DirtyLba : 0;
            continue;
        }

        ASSERT(DirtyLba <= DirtyStart);
        Extent = Ext2AllocateExtent();
        if (Extent == NULL) {
            DEBUG(DL_ERR, ("Ext2VolumeDirtyChain: failed to allocate Extent\n"));
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        Extent->Irp = NULL;
        Extent->Lba = DirtyStart;
        Extent->Offset = (ULONG)(DirtyStart + Length - RemainLength - DirtyLba);
        ASSERT(Extent->Offset <= Length);

        if (DirtyLba + DirtyLength >= DirtyStart + RemainLength) {
            Extent->Length = (ULONG)(DirtyLba + RemainLength - DirtyStart);
            RemainLength = 0;
        } else {
            Extent->Length = (ULONG)(DirtyLength + DirtyLba - DirtyStart);
            RemainLength -= Extent->Length;
        }
        ASSERT(Extent->Length >= SECTOR_SIZE && Extent->Length <= Length);
        DirtyLba = DirtyStart + Extent->Length;

        if (List) {
            List->Next = Extent;
        } else {
            *Chain = Extent;
        }
        List = Extent;
    }
    return STATUS_SUCCESS;
}

/* A paging write of the stream: the lazy writer's or the mapped page
   writer's pages, of which the registered ranges reach the disk. */
static NTSTATUS
Ext2VolumeWritePaging(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN OUT PEXT2_VOLUME_WRITE V,
                      OUT PEXT2_EXTENT *Chain)
{
    PEXT2_EXTENT    Extent;
    NTSTATUS        Status;

    V->Length &= ~((ULONG)SECTOR_SIZE - 1);

    Status = Ext2LockUserBuffer(V->Irp, V->Length, IoReadAccess);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Status = Ext2VolumeDirtyChain(Vcb, V->ByteOffset.QuadPart, V->Length, Chain);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    if (*Chain == NULL) {
        V->Irp->IoStatus.Information = V->Length;
        return STATUS_SUCCESS;
    }

    Status = Ext2ReadWriteBlocks(IrpContext, Vcb, *Chain, V->Length);

    /* gone asynchronous: the IRP is its completion routine's now */
    V->Irp = IrpContext->Irp;

    /* Written: the ranges are clean. Still in flight, they stay registered
       - a write that then fails is retried by the memory manager and has to
       find them; they used to be dropped on STATUS_PENDING, and such a page
       never reached the disk. A range left registered is only written
       again, unchanged. */
    if (Status == STATUS_SUCCESS) {
        for (Extent = *Chain; Extent != NULL; Extent = Extent->Next) {
            (void)Ext2RemoveVcbExtent(Vcb, Extent->Lba, Extent->Length);
        }
    }
    return Status;
}

/* a volume handle's write past the cache, straight to the disk */
static NTSTATUS
Ext2VolumeWriteDirect(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN OUT PEXT2_VOLUME_WRITE V)
{
    EXT2_EXTENT Run = { 0 };
    NTSTATUS    Status;

    V->Length &= ~((ULONG)SECTOR_SIZE - 1);

    /* the buffer is read from: a read-only one is good enough (it used to
       be locked for write access, and refused) */
    Status = Ext2LockUserBuffer(V->Irp, V->Length, IoReadAccess);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Run.Lba = V->ByteOffset.QuadPart;
    Run.Length = V->Length;
    Status = Ext2ReadWriteBlocks(IrpContext, Vcb, &Run, V->Length);

    /* gone asynchronous, the IRP is its completion routine's - not to be
       written to any more (it used to be, after the call) */
    V->Irp = IrpContext->Irp;
    if (Status == STATUS_SUCCESS) {
        V->Irp->IoStatus.Information = V->Length;
    }
    return Status;
}

static NTSTATUS
Ext2VolumeWriteFinish(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VOLUME_WRITE V, IN NTSTATUS Status)
{
    if (IrpContext->ExceptionInProgress) {
        return Status;
    }

    if (V->Irp == NULL) {
        Ext2FreeIrpContext(IrpContext);
        return Status;
    }

    if (Status == STATUS_PENDING || Status == STATUS_CANT_WAIT) {
        if (V->Deferred) {
            return Status;
        }
        Status = Ext2LockUserBuffer(V->Irp, V->Length, IoReadAccess);
        if (NT_SUCCESS(Status)) {
            return Ext2QueueRequest(IrpContext);
        }
        Ext2CompleteIrpContext(IrpContext, Status);
        return Status;
    }

    if (NT_SUCCESS(Status) && !V->PagingIo) {
        if (V->SynchronousIo) {
            V->FileObject->CurrentByteOffset.QuadPart =
                V->ByteOffset.QuadPart + V->Irp->IoStatus.Information;
        }
        SetFlag(V->FileObject->Flags, FO_FILE_MODIFIED);
    }

    Ext2CompleteIrpContext(IrpContext, Status);
    return Status;
}

NTSTATUS
Ext2WriteVolume (IN PEXT2_IRP_CONTEXT IrpContext)
{
    PEXT2_VCB           Vcb;
    EXT2_VOLUME_WRITE   V;
    PEXT2_EXTENT        Chain = NULL;
    NTSTATUS            Status = STATUS_UNSUCCESSFUL;

    ASSERT(IrpContext);
    ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
           (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

    Vcb = (PEXT2_VCB) IrpContext->DeviceObject->DeviceExtension;
    ASSERT(Vcb != NULL);
    ASSERT((Vcb->Identifier.Type == EXT2VCB) &&
           (Vcb->Identifier.Size == sizeof(EXT2_VCB)));

    Ext2VolumeWriteParameters(IrpContext, &V);

    DEBUG(DL_INF, ("Ext2WriteVolume: Off=%I64xh Len=%xh Paging=%xh Nocache=%xh\n",
                   V.ByteOffset.QuadPart, V.Length, V.PagingIo, V.Nocache));

    __try {

        if (V.FileObject->FsContext != (PVOID)Vcb) {
            Status = STATUS_INVALID_DEVICE_REQUEST;
            __leave;
        }

        Status = Ext2VolumeWriteCheck(IrpContext, Vcb, &V);
        if (Status != STATUS_SUCCESS || V.Done) {
            __leave;
        }

        if (!V.Nocache) {
            Status = Ext2ThrottleWrite(IrpContext, V.FileObject, V.Length);
            if (Status != STATUS_SUCCESS) {
                V.Deferred = Status == STATUS_PENDING;
                __leave;
            }
        }

        Status = Ext2VolumeWriteCoherent(Vcb, &V);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        if (!V.Nocache) {
            Status = Ext2VolumeWriteCached(IrpContext, Vcb, &V);
        } else if (V.PagingIo) {
            Status = Ext2VolumeWritePaging(IrpContext, Vcb, &V, &Chain);
        } else {
            Status = Ext2VolumeWriteDirect(IrpContext, Vcb, &V);
        }

    } __finally {

        Status = Ext2VolumeWriteFinish(IrpContext, &V, Status);
        if (Chain) {
            Ext2DestroyExtentChain(Chain);
        }
    }

    return Status;
}
