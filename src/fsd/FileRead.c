/**
 * FileRead.c - IRP_MJ_READ: cached, paging and volume reads.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"

NTSTATUS
Ext2ReadComplete (IN PEXT2_IRP_CONTEXT IrpContext);

NTSTATUS
Ext2ReadFile (IN PEXT2_IRP_CONTEXT IrpContext);

NTSTATUS
Ext2ReadVolume (IN PEXT2_IRP_CONTEXT IrpContext);

#define SafeZeroMemory(AT,BYTE_COUNT) {                                 \
    __try {                                                             \
        if (AT)                                                         \
            RtlZeroMemory((AT), (BYTE_COUNT));                          \
    } __except(EXCEPTION_EXECUTE_HANDLER) {                             \
         Ext2RaiseStatus( IrpContext, STATUS_INVALID_USER_BUFFER );     \
    }                                                                   \
}

NTSTATUS
Ext2CompleteIrpContext (
    IN PEXT2_IRP_CONTEXT IrpContext,
    IN NTSTATUS Status )
{
    PIRP    Irp = NULL;
    BOOLEAN bPrint;

    Irp = IrpContext->Irp;

    if (Irp != NULL) {

        if (NT_ERROR(Status)) {
            Irp->IoStatus.Information = 0;
        }

        Irp->IoStatus.Status = Status;
        bPrint = !IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_REQUEUED);

        Ext2CompleteRequest(
            Irp, bPrint, (CCHAR)(NT_SUCCESS(Status)?
                                 IO_DISK_INCREMENT : IO_NO_INCREMENT) );

        IrpContext->Irp = NULL;
    }

    Ext2FreeIrpContext(IrpContext);

    return Status;
}

/* one read of the volume: the metadata stream's, or a volume handle's */
typedef struct _EXT2_VOLUME_READ {
    PIRP            Irp;            /* NULL once the read went asynchronous */
    PFILE_OBJECT    FileObject;
    PEXT2_CCB       Ccb;            /* a volume handle's; NULL for the stream */
    LARGE_INTEGER   ByteOffset;
    ULONG           Length;
    BOOLEAN         PagingIo;
    BOOLEAN         Nocache;
    BOOLEAN         SynchronousIo;
    BOOLEAN         Done;           /* finished before reading anything */
} EXT2_VOLUME_READ, *PEXT2_VOLUME_READ;

static VOID
Ext2VolumeReadParameters(IN PEXT2_IRP_CONTEXT IrpContext, OUT PEXT2_VOLUME_READ V)
{
    PIO_STACK_LOCATION IrpSp;

    RtlZeroMemory(V, sizeof(EXT2_VOLUME_READ));
    V->Irp = IrpContext->Irp;
    V->FileObject = IrpContext->FileObject;
    V->Ccb = (PEXT2_CCB) V->FileObject->FsContext2;

    IrpSp = IoGetCurrentIrpStackLocation(V->Irp);
    V->Length = IrpSp->Parameters.Read.Length;
    V->ByteOffset = IrpSp->Parameters.Read.ByteOffset;

    /* a volume handle never goes through the cache */
    V->PagingIo = IsFlagOn(V->Irp->Flags, IRP_PAGING_IO);
    V->Nocache = IsFlagOn(V->Irp->Flags, IRP_NOCACHE) || V->Ccb != NULL;
    V->SynchronousIo = IsFlagOn(V->FileObject->Flags, FO_SYNCHRONOUS_IO);
    if (V->PagingIo) {
        ASSERT(V->Nocache);
    }
}

/* what makes a volume read wrong, or finished, before anything is done */
static NTSTATUS
Ext2VolumeReadCheck(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN OUT PEXT2_VOLUME_READ V)
{
    V->Irp->IoStatus.Information = 0;

    if (V->Length == 0) {
        V->Done = TRUE;
        return STATUS_SUCCESS;
    }

    /* whole sectors past the cache, as a write takes them (a read of part
       of one used to be cut to nothing, and sent to the disk so) */
    if (V->Nocache &&
        ((V->ByteOffset.LowPart & (SECTOR_SIZE - 1)) || (V->Length & (SECTOR_SIZE - 1)))) {
        return STATUS_INVALID_PARAMETER;
    }

    if (FlagOn(IrpContext->MinorFunction, IRP_MN_DPC)) {
        ClearFlag(IrpContext->MinorFunction, IRP_MN_DPC);
        return STATUS_PENDING;
    }

    /* the stream is the partition: nothing past it, and a read is cut at
       its end */
    if (V->ByteOffset.QuadPart >= Vcb->Header.FileSize.QuadPart) {
        V->Done = TRUE;
        return STATUS_END_OF_FILE;
    }
    if (V->ByteOffset.QuadPart + V->Length > Vcb->Header.FileSize.QuadPart) {
        V->Length = (ULONG)(Vcb->Header.FileSize.QuadPart - V->ByteOffset.QuadPart);
    }
    return STATUS_SUCCESS;
}

/* a volume handle's first read: what the file system has cached goes to
   the disk first, so that the handle reads it (a locked volume has none) */
static NTSTATUS
Ext2VolumeReadCoherent(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN PEXT2_VOLUME_READ V)
{
    if (V->Ccb == NULL || V->PagingIo || FlagOn(V->Ccb->Flags, CCB_VOLUME_DASD_PURGE)) {
        return STATUS_SUCCESS;
    }

    if (!ExAcquireResourceExclusiveLite(&Vcb->MainResource,
                                        IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT))) {
        return STATUS_PENDING;
    }
    if (!FlagOn(Vcb->Flags, VCB_VOLUME_LOCKED)) {
        /* best effort: what cannot be written now is read as the disk has it */
        (void)Ext2FlushVolume(IrpContext, Vcb, FALSE);
    }
    SetLongFlag(V->Ccb->Flags, CCB_VOLUME_DASD_PURGE);
    ExReleaseResourceLite(&Vcb->MainResource);
    return STATUS_SUCCESS;
}

/* a cached read of the stream */
static NTSTATUS
Ext2VolumeReadCached(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN PEXT2_VOLUME_READ V)
{
    PUCHAR Buffer;

    if (IsFlagOn(IrpContext->MinorFunction, IRP_MN_MDL)) {
        CcMdlRead(Vcb->Volume, &V->ByteOffset, V->Length, &V->Irp->MdlAddress, &V->Irp->IoStatus);
        return V->Irp->IoStatus.Status;
    }

    Buffer = Ext2GetUserBuffer(V->Irp);
    if (Buffer == NULL) {
        return STATUS_INVALID_USER_BUFFER;
    }
    if (!CcCopyRead(Vcb->Volume, &V->ByteOffset, V->Length, Ext2CanIWait(), Buffer, &V->Irp->IoStatus)) {
        return STATUS_PENDING;
    }
    return V->Irp->IoStatus.Status;
}

/* a read past the cache, straight from the disk */
static NTSTATUS
Ext2VolumeReadDirect(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN OUT PEXT2_VOLUME_READ V)
{
    EXT2_EXTENT Run = { 0 };
    NTSTATUS    Status;

    Status = Ext2LockUserBuffer(V->Irp, V->Length, IoWriteAccess);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Run.Lba = V->ByteOffset.QuadPart;
    Run.Length = V->Length;
    Status = Ext2ReadWriteBlocks(IrpContext, Vcb, &Run, V->Length);

    /* gone asynchronous: the IRP is its completion routine's now */
    V->Irp = IrpContext->Irp;
    return Status;
}

static NTSTATUS
Ext2VolumeReadFinish(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VOLUME_READ V, IN NTSTATUS Status)
{
    if (IrpContext->ExceptionInProgress) {
        return Status;
    }

    if (V->Irp == NULL) {
        Ext2FreeIrpContext(IrpContext);
        return Status;
    }

    /* a request that could not wait goes to a worker, which can (it was
       completed as STATUS_PENDING when that had happened once already) */
    if (Status == STATUS_PENDING || Status == STATUS_CANT_WAIT) {
        Status = Ext2LockUserBuffer(V->Irp, V->Length, IoWriteAccess);
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
        V->FileObject->Flags |= FO_FILE_FAST_IO_READ;
    }

    Ext2CompleteIrpContext(IrpContext, Status);
    return Status;
}

NTSTATUS
Ext2ReadVolume (IN PEXT2_IRP_CONTEXT IrpContext)
{
    PEXT2_VCB           Vcb;
    EXT2_VOLUME_READ    V;
    NTSTATUS            Status = STATUS_UNSUCCESSFUL;

    ASSERT(IrpContext);
    ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
           (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

    Vcb = (PEXT2_VCB) IrpContext->DeviceObject->DeviceExtension;
    ASSERT(Vcb != NULL);
    ASSERT((Vcb->Identifier.Type == EXT2VCB) &&
           (Vcb->Identifier.Size == sizeof(EXT2_VCB)));

    Ext2VolumeReadParameters(IrpContext, &V);

    __try {

        if (V.FileObject->FsContext != (PVOID)Vcb) {
            Status = STATUS_INVALID_DEVICE_REQUEST;
            __leave;
        }

        Status = Ext2VolumeReadCheck(IrpContext, Vcb, &V);
        if (Status != STATUS_SUCCESS || V.Done) {
            __leave;
        }

        Status = Ext2VolumeReadCoherent(IrpContext, Vcb, &V);
        if (Status != STATUS_SUCCESS) {
            __leave;
        }

        if (V.Nocache) {
            Status = Ext2VolumeReadDirect(IrpContext, Vcb, &V);
        } else {
            Status = Ext2VolumeReadCached(IrpContext, Vcb, &V);
        }

    } __finally {

        Status = Ext2VolumeReadFinish(IrpContext, &V, Status);
    }

    return Status;
}

NTSTATUS
Ext2ReadInode (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_MCB            Mcb,
    IN ULONGLONG            Offset,
    IN PVOID                Buffer,
    IN ULONG                Size,
    IN BOOLEAN              bDirectIo,
    OUT PULONG              BytesRead
)
{
    PEXT2_EXTENT    Chain = NULL;
    PEXT2_EXTENT    Extent = NULL, Prev = NULL;

    IO_STATUS_BLOCK IoStatus;
    NTSTATUS        Status = STATUS_UNSUCCESSFUL;
    ULONG           RealSize ;

    if (BytesRead) {
        *BytesRead = 0;
    }

    __try {

        Ext2ReferMcb(Mcb);

        ASSERT((Mcb->Identifier.Type == EXT2MCB) &&
               (Mcb->Identifier.Size == sizeof(EXT2_MCB)));

        if ((Mcb->Identifier.Type != EXT2MCB) ||
                (Mcb->Identifier.Size != sizeof(EXT2_MCB))) {
            __leave;
        }

        if (Buffer == NULL && IrpContext != NULL)
            Buffer = Ext2GetUserBuffer(IrpContext->Irp);


        /* handle fast symlinks */
        if (ext4_inode_is_fast_symlink(Mcb->Inode)) {

            PUCHAR Data = (PUCHAR) (&Mcb->Inode->i_block[0]);
            if (!Buffer) {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                __leave;
            }

            if (Offset < EXT2_LINKLEN_IN_INODE) {
                if ((ULONG)Offset + Size >= EXT2_LINKLEN_IN_INODE)
                    Size = EXT2_LINKLEN_IN_INODE - (ULONG)Offset - 1;
                RtlCopyMemory(Buffer, Data + (ULONG)Offset, Size);
                Status = STATUS_SUCCESS;
            } else {
                Status = STATUS_END_OF_FILE;
            }
            __leave;
        }

        /* inline data: the bytes, or a directory's view, from the inode */
        if (Ext4IsInline(Mcb->Inode)) {
            if (!Buffer) {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                __leave;
            }
            Status = Ext4InlineRead(IrpContext, Vcb, Mcb, Offset, Buffer, Size, BytesRead);
            __leave;
        }

        /* Build the scatterred block ranges to be read */

        if (bDirectIo) {
            /* whole sectors to the disk (it used to align to 511) */
            RealSize = CEILING_ALIGNED(ULONG, Size, SECTOR_SIZE);
        } else {
            RealSize = Size;
        }

        Status = Ext2BuildExtents(
                     IrpContext,
                     Vcb,
                     Mcb,
                     Offset,
                     RealSize,
                     FALSE,
                     &Chain
                 );

        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        if (Chain == NULL) {
            SafeZeroMemory((PCHAR)Buffer, Size);
            Status = STATUS_SUCCESS;
            __leave;
        }

        /* for sparse file, we need zero the gaps */
        for (Extent = Chain; Buffer != NULL && Extent != NULL; Extent = Extent->Next) {
            if (NULL == Prev) {
                ASSERT(Extent == Chain);
                if (Extent->Offset) {
                    SafeZeroMemory((PCHAR)Buffer, Extent->Offset);
                }
            } else if (Extent->Offset > (Prev->Offset + Prev->Length)) {
                SafeZeroMemory((PCHAR)Buffer + Prev->Offset + Prev->Length,
                               Extent->Offset - Prev->Offset - Prev->Length);
            }
            if (NULL == Extent->Next) {
                if (Extent->Offset + Extent->Length < Size) {
                    SafeZeroMemory((PCHAR)Buffer + Extent->Offset + Extent->Length,
                                   Size - Extent->Offset - Extent->Length);
                }
            }
            Prev = Extent;
        }

        if (bDirectIo) {

            ASSERT(IrpContext != NULL);

            /* Offset should be SECTOR_SIZE aligned ... */
            Status = Ext2ReadWriteBlocks(
                         IrpContext,
                         Vcb,
                         Chain,
                         Size
                     );
        } else {

            for (Extent = Chain; Extent != NULL; Extent = Extent->Next) {

                CcCopyRead(
                    Vcb->Volume,
                    (PLARGE_INTEGER)(&(Extent->Lba)),
                    Extent->Length,
                    TRUE,
                    (PVOID)((PUCHAR)Buffer + Extent->Offset),
                    &IoStatus
                );
                Status = IoStatus.Status;

                if (!NT_SUCCESS(Status)) {
                    break;
                }
            }
        }

    } __finally {

        if (Chain) {
            Ext2DestroyExtentChain(Chain);
        }

        Ext2DerefMcb(Mcb);
    }

    if (NT_SUCCESS(Status)) {
        if (BytesRead)
            *BytesRead = Size;
    }

    return Status;
}

/* one read of a file, as Ext2ReadFile carries it from step to step */
typedef struct _EXT2_READ {
    PIRP            Irp;            /* NULL once the read went asynchronous */
    PFILE_OBJECT    FileObject;
    PEXT2_FCB       Fcb;
    PEXT2_CCB       Ccb;
    LARGE_INTEGER   ByteOffset;
    ULONG           Length;         /* bytes asked for */
    ULONG           ReturnedLength; /* bytes there are before the end of file */
    BOOLEAN         PagingIo;
    BOOLEAN         Nocache;
    BOOLEAN         SynchronousIo;
    BOOLEAN         MainResourceAcquired;
    BOOLEAN         PagingIoResourceAcquired;
    BOOLEAN         OpPostIrp;      /* the oplock package owns the request */
    BOOLEAN         Done;           /* finished before reading anything */
} EXT2_READ, *PEXT2_READ;

static VOID
Ext2ReadParameters(IN PEXT2_IRP_CONTEXT IrpContext, OUT PEXT2_READ R)
{
    PIO_STACK_LOCATION IrpSp;

    RtlZeroMemory(R, sizeof(EXT2_READ));
    R->Irp = IrpContext->Irp;
    R->FileObject = IrpContext->FileObject;
    R->Fcb = (PEXT2_FCB) R->FileObject->FsContext;
    R->Ccb = (PEXT2_CCB) R->FileObject->FsContext2;
    ASSERT(R->Fcb);
    ASSERT((R->Fcb->Identifier.Type == EXT2FCB) &&
           (R->Fcb->Identifier.Size == sizeof(EXT2_FCB)));

    IrpSp = IoGetCurrentIrpStackLocation(R->Irp);
    R->Length = IrpSp->Parameters.Read.Length;
    R->ByteOffset = IrpSp->Parameters.Read.ByteOffset;

    R->PagingIo = IsFlagOn(R->Irp->Flags, IRP_PAGING_IO);
    R->Nocache = IsFlagOn(R->Irp->Flags, IRP_NOCACHE);
    R->SynchronousIo = IsFlagOn(R->FileObject->Flags, FO_SYNCHRONOUS_IO);
    if (R->PagingIo) {
        ASSERT(R->Nocache);
    }
}

/* a read finished before it starts; Information is what the caller is told */
static NTSTATUS
Ext2ReadDone(IN OUT PEXT2_READ R, IN ULONG_PTR Information, IN NTSTATUS Status)
{
    R->Irp->IoStatus.Information = Information;
    R->Done = TRUE;
    return Status;
}

/* what makes a read wrong, or finished, before anything is locked */
static NTSTATUS
Ext2ReadCheck(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN OUT PEXT2_READ R)
{
    PEXT2_FCB Fcb = R->Fcb;

    if (IsSpecialFile(Fcb) || IsInodeSymLink(Fcb->Inode)) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    if ((IsSymLink(Fcb) && Ext2IsLinkDangling(Vcb, Fcb->Mcb)) || IsInodeDeleted(Fcb)) {
        return STATUS_FILE_DELETED;
    }

    /* the offset first: even a read of nothing leaves the file pointer
       after it (Ext2ReadFinish), and the special offset is no place */
    if (R->ByteOffset.LowPart == FILE_USE_FILE_POINTER_POSITION &&
        R->ByteOffset.HighPart == -1) {
        R->ByteOffset = R->FileObject->CurrentByteOffset;
    }

    if (R->Length == 0) {
        return Ext2ReadDone(R, 0, STATUS_SUCCESS);
    }

    if (R->Nocache &&
        ((R->ByteOffset.LowPart & (SECTOR_SIZE - 1)) || (R->Length & (SECTOR_SIZE - 1)))) {
        return STATUS_INVALID_PARAMETER;
    }

    if (FlagOn(IrpContext->MinorFunction, IRP_MN_DPC)) {
        ClearFlag(IrpContext->MinorFunction, IRP_MN_DPC);
        return STATUS_PENDING;
    }

    /* a directory's blocks are read through the volume, never cached as a
       file's (it used to fail as STATUS_UNSUCCESSFUL, after the locks) */
    if (IsDirectory(Fcb) && !R->Nocache) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    return STATUS_SUCCESS;
}

/*
 * The locks of a read. A paging read holds PagingIoResource shared, which
 * keeps the file from shrinking under it. A caller's read holds
 * MainResource shared and passes the byte range locks; a non-cached one of
 * a file that is also cached first writes back and purges what the cache
 * holds, under MainResource exclusive, so that the disk is what it reads.
 */
static NTSTATUS
Ext2ReadLock(IN PEXT2_IRP_CONTEXT IrpContext, IN OUT PEXT2_READ R)
{
    PEXT2_FCB   Fcb = R->Fcb;
    BOOLEAN     Wait = IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT);

    if (R->PagingIo) {
        if (!ExAcquireResourceSharedLite(&Fcb->PagingIoResource, Wait)) {
            return STATUS_PENDING;
        }
        R->PagingIoResourceAcquired = TRUE;
        return STATUS_SUCCESS;
    }

    if (R->Nocache && R->Ccb != NULL && Fcb->SectionObject.DataSectionObject) {

        if (!ExAcquireResourceExclusiveLite(&Fcb->MainResource, Wait)) {
            return STATUS_PENDING;
        }
        R->MainResourceAcquired = TRUE;

        CcFlushCache(&Fcb->SectionObject, &R->ByteOffset, R->Length, &R->Irp->IoStatus);
        if (!NT_SUCCESS(R->Irp->IoStatus.Status)) {
            return R->Irp->IoStatus.Status;     /* it used to fail as STATUS_UNSUCCESSFUL */
        }
        ClearLongFlag(Fcb->Flags, FCB_FILE_MODIFIED);

        /* the paging I/O in flight first */
        ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
        ExReleaseResourceLite(&Fcb->PagingIoResource);
        CcPurgeCacheSection(&Fcb->SectionObject, NULL, 0, FALSE);

        ExConvertExclusiveToSharedLite(&Fcb->MainResource);

    } else {

        if (!ExAcquireResourceSharedLite(&Fcb->MainResource, Wait)) {
            return STATUS_PENDING;
        }
        R->MainResourceAcquired = TRUE;
    }

    if (!FsRtlCheckLockForReadAccess(&Fcb->FileLockAnchor, R->Irp)) {
        return STATUS_FILE_LOCK_CONFLICT;
    }
    return STATUS_SUCCESS;
}

/* a cached read: from the cache manager, which reads ahead */
static NTSTATUS
Ext2ReadCached(IN PEXT2_IRP_CONTEXT IrpContext, IN OUT PEXT2_READ R)
{
    PEXT2_FCB       Fcb = R->Fcb;
    PFILE_OBJECT    FileObject = R->FileObject;
    PUCHAR          Buffer;

    if (FileObject->PrivateCacheMap == NULL) {
        CcInitializeCacheMap(FileObject,
                             (PCC_FILE_SIZES)(&Fcb->Header.AllocationSize),
                             FALSE,
                             &Ext2Global->CacheManagerCallbacks,
                             Fcb);
        CcSetReadAheadGranularity(FileObject, READ_AHEAD_GRANULARITY);
    }

    if (FlagOn(IrpContext->MinorFunction, IRP_MN_MDL)) {
        CcMdlRead(FileObject, &R->ByteOffset, R->ReturnedLength,
                  &R->Irp->MdlAddress, &R->Irp->IoStatus);
    } else {
        Buffer = Ext2GetUserBuffer(R->Irp);
        if (Buffer == NULL) {
            return STATUS_INVALID_USER_BUFFER;
        }
        CcCopyRead(FileObject, &R->ByteOffset, R->ReturnedLength, TRUE, Buffer, &R->Irp->IoStatus);
    }

    if (NT_SUCCESS(R->Irp->IoStatus.Status)) {
        R->Irp->IoStatus.Information = R->ReturnedLength;
    }
    return R->Irp->IoStatus.Status;
}

/* A non-cached read: straight from the blocks. What lies past the valid
   data length reads as zeros, without touching the disk (ext4 keeps no
   valid data length; blocks there may hold anything). */
static NTSTATUS
Ext2ReadNoncached(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN OUT PEXT2_READ R)
{
    PEXT2_FCB   Fcb = R->Fcb;
    LONGLONG    Valid = Fcb->Header.ValidDataLength.QuadPart;
    ULONG       BytesRead = R->ReturnedLength;
    PUCHAR      SystemVA = Ext2GetUserBuffer(R->Irp);
    NTSTATUS    Status;

    if (R->ByteOffset.QuadPart + BytesRead > Valid) {
        if (R->ByteOffset.QuadPart >= Valid) {
            SafeZeroMemory(SystemVA, R->Length);
            R->Irp->IoStatus.Information = R->ReturnedLength;
            return STATUS_SUCCESS;
        }
        BytesRead = (ULONG)(Valid - R->ByteOffset.QuadPart);
        if (SystemVA) {
            SafeZeroMemory(SystemVA + BytesRead, R->Length - BytesRead);
        }
    }

    Status = Ext2LockUserBuffer(R->Irp, BytesRead, IoReadAccess);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Status = Ext2ReadInode(IrpContext, Vcb, Fcb->Mcb, R->ByteOffset.QuadPart,
                           NULL, BytesRead, TRUE, NULL);
    if (Status == STATUS_PENDING) {
        /* gone asynchronous: the IRP, and the resource it was read under,
           are its completion routine's now (Ext2ReadWriteBlocks) */
        R->Irp = IrpContext->Irp = NULL;
    } else if (NT_SUCCESS(Status)) {
        R->Irp->IoStatus.Information = R->ReturnedLength;
    }
    return Status;
}

static NTSTATUS
Ext2ReadFinish(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_READ R, IN NTSTATUS Status)
{
    if (R->Irp != NULL) {
        if (R->PagingIoResourceAcquired) {
            ExReleaseResourceLite(&R->Fcb->PagingIoResource);
        }
        if (R->MainResourceAcquired) {
            ExReleaseResourceLite(&R->Fcb->MainResource);
        }
    }

    if (R->OpPostIrp || IrpContext->ExceptionInProgress) {
        return Status;
    }

    if (R->Irp == NULL) {
        Ext2FreeIrpContext(IrpContext);
        return Status;
    }

    if (Status == STATUS_PENDING || Status == STATUS_CANT_WAIT) {
        Status = Ext2LockUserBuffer(R->Irp, R->Length, IoWriteAccess);
        if (NT_SUCCESS(Status)) {
            return Ext2QueueRequest(IrpContext);
        }
        Ext2CompleteIrpContext(IrpContext, Status);
        return Status;
    }

    if (NT_SUCCESS(Status) && !R->PagingIo) {
        if (R->SynchronousIo) {
            R->FileObject->CurrentByteOffset.QuadPart =
                R->ByteOffset.QuadPart + R->Irp->IoStatus.Information;
        }
        R->FileObject->Flags |= FO_FILE_FAST_IO_READ;
    }

    Ext2CompleteIrpContext(IrpContext, Status);
    return Status;
}

NTSTATUS
Ext2ReadFile(IN PEXT2_IRP_CONTEXT IrpContext)
{
    PEXT2_VCB   Vcb;
    PEXT2_FCB   Fcb;
    EXT2_READ   R;
    NTSTATUS    Status = STATUS_UNSUCCESSFUL;

    ASSERT(IrpContext);
    ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
           (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

    Vcb = (PEXT2_VCB) IrpContext->DeviceObject->DeviceExtension;
    ASSERT(Vcb != NULL);
    ASSERT((Vcb->Identifier.Type == EXT2VCB) &&
           (Vcb->Identifier.Size == sizeof(EXT2_VCB)));

    Ext2ReadParameters(IrpContext, &R);
    Fcb = R.Fcb;

    DEBUG(DL_INF, ("Ext2ReadFile: reading %wZ Off=%I64xh Len=%xh Paging=%xh Nocache=%xh\n",
                   &Fcb->Mcb->ShortName, R.ByteOffset.QuadPart, R.Length, R.PagingIo, R.Nocache));

    __try {

        Status = Ext2ReadCheck(IrpContext, Vcb, &R);
        if (Status != STATUS_SUCCESS || R.Done) {
            __leave;
        }

        Status = Ext2ReadLock(IrpContext, &R);
        if (Status != STATUS_SUCCESS) {
            __leave;
        }

        /* the end of file, under the locks that keep it */
        if (R.ByteOffset.QuadPart >= Fcb->Header.FileSize.QuadPart) {
            Status = Ext2ReadDone(&R, 0, STATUS_END_OF_FILE);
            __leave;
        }
        R.ReturnedLength = (ULONG)min((LONGLONG)R.Length,
                                      Fcb->Header.FileSize.QuadPart - R.ByteOffset.QuadPart);

        if (!IsDirectory(Fcb) && R.Ccb != NULL) {
            Status = FsRtlCheckOplock(&Fcb->Oplock, R.Irp, IrpContext,
                                      Ext2OplockComplete, Ext2LockIrp);
            if (Status != STATUS_SUCCESS) {
                R.OpPostIrp = TRUE;
                __leave;
            }

            /* Set the flag indicating if Fast I/O is possible */
            Fcb->Header.IsFastIoPossible = Ext2IsFastIoPossible(Fcb);
        }

        if (R.Nocache) {
            Status = Ext2ReadNoncached(IrpContext, Vcb, &R);
        } else {
            Status = Ext2ReadCached(IrpContext, &R);
        }

    } __finally {

        Status = Ext2ReadFinish(IrpContext, &R, Status);
    }

    DEBUG(DL_IO, ("Ext2ReadFile: %wZ fetch at Off=%I64xh Len=%xh Paging=%xh Nocache=%xh Returned=%xh Status=%xh\n",
                  &Fcb->Mcb->ShortName, R.ByteOffset.QuadPart, R.Length, R.PagingIo, R.Nocache,
                  R.ReturnedLength, Status));
    return Status;
}

NTSTATUS
Ext2ReadComplete (IN PEXT2_IRP_CONTEXT IrpContext)
{
    NTSTATUS        Status = STATUS_UNSUCCESSFUL;
    PFILE_OBJECT    FileObject;
    PIRP            Irp;

    __try {

        ASSERT(IrpContext);
        ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
               (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

        FileObject = IrpContext->FileObject;
        Irp = IrpContext->Irp;

        CcMdlReadComplete(FileObject, Irp->MdlAddress);
        Irp->MdlAddress = NULL;
        Status = STATUS_SUCCESS;

    } __finally {

        if (!IrpContext->ExceptionInProgress) {
            Ext2CompleteIrpContext(IrpContext, Status);
        }
    }

    return Status;
}

NTSTATUS
Ext2Read (IN PEXT2_IRP_CONTEXT IrpContext)
{
    NTSTATUS Status = STATUS_UNSUCCESSFUL;
    PEXT2_VCB           Vcb;
    PEXT2_FCBVCB        FcbOrVcb;
    PDEVICE_OBJECT      DeviceObject;
    PFILE_OBJECT        FileObject;
    BOOLEAN bCompleteRequest = FALSE;

    ASSERT(IrpContext);

    ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
           (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

    __try {

        if (FlagOn(IrpContext->MinorFunction, IRP_MN_COMPLETE)) {

            Status =  Ext2ReadComplete(IrpContext);
            bCompleteRequest = FALSE;

        } else {

            DeviceObject = IrpContext->DeviceObject;

            if (IsExt2FsDevice(DeviceObject)) {
                Status = STATUS_INVALID_DEVICE_REQUEST;
                bCompleteRequest = TRUE;
                __leave;
            }

            Vcb = (PEXT2_VCB) DeviceObject->DeviceExtension;
            if (Vcb->Identifier.Type != EXT2VCB ||
                    Vcb->Identifier.Size != sizeof(EXT2_VCB) ) {
                Status = STATUS_INVALID_DEVICE_REQUEST;
                bCompleteRequest = TRUE;

                __leave;
            }

            FileObject = IrpContext->FileObject;

            if (IsFlagOn(Vcb->Flags, VCB_DEVICE_REMOVED)) {
                Status = STATUS_NO_SUCH_DEVICE;
                bCompleteRequest = TRUE;
                __leave;
            }

            if (FlagOn(Vcb->Flags, VCB_VOLUME_LOCKED) &&
                Vcb->LockFile != FileObject ) {
                Status = STATUS_ACCESS_DENIED;
                bCompleteRequest = TRUE;
                __leave;
            }

            FcbOrVcb = (PEXT2_FCBVCB) FileObject->FsContext;

            if (FcbOrVcb->Identifier.Type == EXT2VCB) {

                Status = Ext2ReadVolume(IrpContext);
                bCompleteRequest = FALSE;

            } else if (FcbOrVcb->Identifier.Type == EXT2FCB) {

                if (IsFlagOn(Vcb->Flags, VCB_DISMOUNT_PENDING)) {
                    Status = STATUS_TOO_LATE;
                    bCompleteRequest = TRUE;
                    __leave;
                }

                Status = Ext2ReadFile(IrpContext);
                bCompleteRequest = FALSE;
            } else {
                DEBUG(DL_ERR, ( "Ext2Read: Invalid FileObject (Vcb or Fcb corrupted)\n"));

                Status = STATUS_INVALID_PARAMETER;
                bCompleteRequest = TRUE;
            }
        }

    } __finally {
        if (bCompleteRequest) {
            Ext2CompleteIrpContext(IrpContext, Status);
        }
    }

    return Status;
}
