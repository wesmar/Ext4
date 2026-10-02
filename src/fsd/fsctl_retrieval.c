/**
 * fsctl_retrieval.c - FSCTL retrieval pointers and extent mapping queries.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "fsctl_internal.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2GetRetrievalPointerBase)
#pragma alloc_text(PAGE, Ext2QueryExtentMappings)
#pragma alloc_text(PAGE, Ext2QueryRetrievalPointers)
#pragma alloc_text(PAGE, Ext2GetRetrievalPointers)
#endif

NTSTATUS
Ext2QueryExtentMappings(
    IN PEXT2_IRP_CONTEXT   IrpContext,
    IN PEXT2_VCB           Vcb,
    IN PEXT2_FCB           Fcb,
    IN PLARGE_INTEGER      RequestVbn,
    OUT PLARGE_INTEGER *   pMappedRuns
)
{
    PLARGE_INTEGER      MappedRuns = NULL;
    PLARGE_INTEGER      PartialRuns = NULL;

    PEXT2_EXTENT        Chain = NULL;
    PEXT2_EXTENT        Extent = NULL;

    LONGLONG            Vbn = 0;
    ULONG               Length = 0;
    ULONG               i = 0;

    NTSTATUS            Status = STATUS_SUCCESS;

    __try {

        /* now building all the request extents */
        while (Vbn < RequestVbn->QuadPart) {

            Length = 0x80000000; /* 2g bytes */
            if (RequestVbn->QuadPart < Vbn + Length) {
                Length = (ULONG)(RequestVbn->QuadPart - Vbn);
            }

            /* build extents for sub-range */
            Extent = NULL;
            Status = Ext2BuildExtents(
                         IrpContext,
                         Vcb,
                         Fcb->Mcb,
                         Vbn,
                         Length,
                         FALSE,
                         &Extent);

            if (!NT_SUCCESS(Status)) {
                __leave;
            }

            if (Chain) {
                Ext2JointExtents(Chain, Extent);
            } else {
                Chain = Extent;
            }

            /* allocate extent array */
            PartialRuns = Ext2AllocatePool(
                              NonPagedPool,
                              (Ext2CountExtents(Chain) + 2) *
                              (2 * sizeof(LARGE_INTEGER)),
                              'RE2E');

            if (PartialRuns == NULL) {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                __leave;
            }
            RtlZeroMemory(  PartialRuns,
                            (Ext2CountExtents(Chain) + 2) *
                            (2 * sizeof(LARGE_INTEGER)));

            if (MappedRuns) {
                RtlMoveMemory(PartialRuns,
                              MappedRuns,
                              i * 2 * sizeof(LARGE_INTEGER));
                Ext2FreePool(MappedRuns, 'RE2E');
            }
            MappedRuns = PartialRuns;

            /* walk all the Mcb runs in Extent */
            for (; Extent != NULL; Extent = Extent->Next) {
                MappedRuns[i*2 + 0].QuadPart = Vbn + Extent->Offset;
                MappedRuns[i*2 + 1].QuadPart = Extent->Lba;
                i = i+1;
            }

            Vbn = Vbn + Length;
        }

        *pMappedRuns = MappedRuns;

    } __finally {

        if (!NT_SUCCESS(Status) || Status == STATUS_PENDING) {
            if (MappedRuns) {
                Ext2FreePool(MappedRuns, 'RE2E');
            }
            *pMappedRuns = NULL;
        }

        if (Chain) {
            Ext2DestroyExtentChain(Chain);
        }
    }

    return Status;
}

NTSTATUS
Ext2QueryRetrievalPointers (
    IN PEXT2_IRP_CONTEXT IrpContext
)
{
    PIRP                Irp = NULL;
    PIO_STACK_LOCATION  IrpSp;
    PEXTENDED_IO_STACK_LOCATION EIrpSp;

    PDEVICE_OBJECT      DeviceObject;
    PFILE_OBJECT        FileObject;

    PEXT2_VCB           Vcb = NULL;
    PEXT2_FCB           Fcb = NULL;
    PEXT2_CCB           Ccb = NULL;

    PLARGE_INTEGER      RequestVbn;
    PLARGE_INTEGER *    pMappedRuns;

    ULONG               InputSize;
    ULONG               OutputSize;

    NTSTATUS            Status = STATUS_SUCCESS;

    BOOLEAN FcbResourceAcquired = FALSE;

    __try {

        ASSERT(IrpContext);
        Irp = IrpContext->Irp;
        ASSERT(Irp);

        IrpSp = IoGetCurrentIrpStackLocation(Irp);
        EIrpSp = (PEXTENDED_IO_STACK_LOCATION)IrpSp;
        ASSERT(IrpSp);

        InputSize = EIrpSp->Parameters.FileSystemControl.InputBufferLength;
        OutputSize = EIrpSp->Parameters.FileSystemControl.OutputBufferLength;

        ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
               (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

        DeviceObject = IrpContext->DeviceObject;


        /* This request is not allowed on the main device object */
        if (IsExt2FsDevice(DeviceObject)) {
            Status = STATUS_INVALID_DEVICE_REQUEST;
            __leave;
        }

        Vcb = (PEXT2_VCB) DeviceObject->DeviceExtension;
        ASSERT(Vcb != NULL);
        ASSERT((Vcb->Identifier.Type == EXT2VCB) &&
               (Vcb->Identifier.Size == sizeof(EXT2_VCB)));
        if (!IsMounted(Vcb) || IsFlagOn(Vcb->Flags, VCB_DISMOUNT_PENDING)) {
            Status = STATUS_VOLUME_DISMOUNTED;
            __leave;
        }

        FileObject = IrpContext->FileObject;
        Fcb = (PEXT2_FCB) FileObject->FsContext;

        /* check Fcb is valid or not */
        if (Fcb == NULL || Fcb->Identifier.Type == EXT2VCB) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        ASSERT((Fcb->Identifier.Type == EXT2FCB) &&
               (Fcb->Identifier.Size == sizeof(EXT2_FCB)));
        if (IsInodeDeleted(Fcb)) {
            Status = STATUS_FILE_DELETED;
            __leave;
        }

        Ccb = (PEXT2_CCB) FileObject->FsContext2;
        if (Ccb == NULL) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        ASSERT((Ccb->Identifier.Type == EXT2CCB) &&
               (Ccb->Identifier.Size == sizeof(EXT2_CCB)));

        /* Is requstor in kernel and Fcb a paging file ? */
        if (Irp->RequestorMode != KernelMode ||
                !IsFlagOn(Fcb->Flags, FCB_PAGE_FILE) ||
                InputSize != sizeof(LARGE_INTEGER) ||
                OutputSize != sizeof(PVOID)) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        if (!ExAcquireResourceExclusiveLite (
                    &Fcb->MainResource, Ext2CanIWait())) {
            Status = STATUS_PENDING;
            __leave;
        }
        FcbResourceAcquired = TRUE;

        RequestVbn  = EIrpSp->Parameters.FileSystemControl.Type3InputBuffer;
        pMappedRuns = Irp->UserBuffer;


        /* request size beyonds whole file size */
        if (RequestVbn->QuadPart >= Fcb->Header.AllocationSize.QuadPart) {
            Status = STATUS_END_OF_FILE;
            __leave;
        }

        Status = Ext2QueryExtentMappings(
                     IrpContext,
                     Vcb,
                     Fcb,
                     RequestVbn,
                     pMappedRuns
                 );

    } __finally {

        if (FcbResourceAcquired) {
            ExReleaseResourceLite(&Fcb->MainResource);
        }

        if (!AbnormalTermination()) {
            if (Status == STATUS_PENDING || Status == STATUS_CANT_WAIT) {
                Status = Ext2QueueRequest(IrpContext);
            } else {
                Ext2CompleteIrpContext(IrpContext, Status);
            }
        }
    }

    return Status;
}

/*
 * One run of FSCTL_GET_RETRIEVAL_POINTERS into the caller's buffer. The
 * request is METHOD_NEITHER: the buffer is a user address that another thread
 * may unmap at any moment, so every store sits in its own __try and a fault
 * becomes STATUS_INVALID_USER_BUFFER instead of a bugcheck. A run that
 * continues the previous one (same kind, contiguous on disk) extends it, so
 * the 2 GB chunks the map is built in never show up as separate runs.
 */
static NTSTATUS
Ext2PutRetrievalRun(
    IN PRETRIEVAL_POINTERS_BUFFER RPSB,
    IN ULONG        OutputSize,
    IN OUT PULONG   Count,
    IN OUT PLONGLONG LastLcnEnd,
    IN LONGLONG     StartVcn,
    IN LONGLONG     NextVcn,
    IN LONGLONG     Lcn             /* -1 for a hole */
)
{
    ULONG Needed;

    __try {
        if (*Count && *LastLcnEnd == (Lcn < 0 ? -1 : Lcn) &&
            RPSB->Extents[*Count - 1].NextVcn.QuadPart == StartVcn) {
            RPSB->Extents[*Count - 1].NextVcn.QuadPart = NextVcn;
            if (Lcn >= 0)
                *LastLcnEnd = Lcn + (NextVcn - StartVcn);
            return STATUS_SUCCESS;
        }

        Needed = FIELD_OFFSET(RETRIEVAL_POINTERS_BUFFER, Extents) +
                 (*Count + 1) * sizeof(RPSB->Extents[0]);
        if (Needed > OutputSize)
            return STATUS_BUFFER_OVERFLOW;

        if (*Count == 0)
            RPSB->StartingVcn.QuadPart = StartVcn;
        RPSB->Extents[*Count].NextVcn.QuadPart = NextVcn;
        RPSB->Extents[*Count].Lcn.QuadPart = Lcn;
        RPSB->ExtentCount = *Count + 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return STATUS_INVALID_USER_BUFFER;
    }

    *LastLcnEnd = Lcn < 0 ? -1 : Lcn + (NextVcn - StartVcn);
    (*Count)++;
    return STATUS_SUCCESS;
}

NTSTATUS
Ext2GetRetrievalPointers (
    IN PEXT2_IRP_CONTEXT IrpContext
)
{
    PIRP                Irp = NULL;
    PIO_STACK_LOCATION  IrpSp;
    PEXTENDED_IO_STACK_LOCATION EIrpSp;

    PDEVICE_OBJECT      DeviceObject;
    PFILE_OBJECT        FileObject;

    PEXT2_VCB           Vcb = NULL;
    PEXT2_FCB           Fcb = NULL;
    PEXT2_CCB           Ccb = NULL;

    PSTARTING_VCN_INPUT_BUFFER  SVIB;
    PRETRIEVAL_POINTERS_BUFFER  RPSB;

    PEXT2_EXTENT        Chain = NULL;
    PEXT2_EXTENT        Extent = NULL;

    LONGLONG            Vbn = 0;
    ULONG               Length = 0;
    ULONG               i = 0;

    LONGLONG            StartVcn = 0;
    LONGLONG            EndVcn;
    LONGLONG            LastLcnEnd = -1;
    ULONG               InputSize;
    ULONG               OutputSize;

    NTSTATUS            Status = STATUS_SUCCESS;

    BOOLEAN FcbResourceAcquired = FALSE;

    __try {

        ASSERT(IrpContext);
        Irp = IrpContext->Irp;
        ASSERT(Irp);

        IrpSp = IoGetCurrentIrpStackLocation(Irp);
        EIrpSp = (PEXTENDED_IO_STACK_LOCATION)IrpSp;
        ASSERT(IrpSp);

        InputSize = EIrpSp->Parameters.FileSystemControl.InputBufferLength;
        OutputSize = EIrpSp->Parameters.FileSystemControl.OutputBufferLength;

        ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
               (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

        DeviceObject = IrpContext->DeviceObject;

        /* This request is not allowed on the main device object */
        if (IsExt2FsDevice(DeviceObject)) {
            Status = STATUS_INVALID_DEVICE_REQUEST;
            __leave;
        }

        Vcb = (PEXT2_VCB) DeviceObject->DeviceExtension;
        ASSERT(Vcb != NULL);
        ASSERT((Vcb->Identifier.Type == EXT2VCB) &&
               (Vcb->Identifier.Size == sizeof(EXT2_VCB)));
        if (!IsMounted(Vcb) || IsFlagOn(Vcb->Flags, VCB_DISMOUNT_PENDING)) {
            Status = STATUS_VOLUME_DISMOUNTED;
            __leave;
        }

        FileObject = IrpContext->FileObject;
        Fcb = (PEXT2_FCB) FileObject->FsContext;

        /* check Fcb is valid or not */
        if (Fcb == NULL || Fcb->Identifier.Type == EXT2VCB) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        ASSERT((Fcb->Identifier.Type == EXT2FCB) &&
               (Fcb->Identifier.Size == sizeof(EXT2_FCB)));

        if (IsInodeDeleted(Fcb)) {
            Status = STATUS_FILE_DELETED;
            __leave;
        }

        Ccb = (PEXT2_CCB) FileObject->FsContext2;
        if (Ccb == NULL) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        ASSERT((Ccb->Identifier.Type == EXT2CCB) &&
               (Ccb->Identifier.Size == sizeof(EXT2_CCB)));

        if (InputSize  < sizeof(STARTING_VCN_INPUT_BUFFER) ||
                OutputSize < sizeof(RETRIEVAL_POINTERS_BUFFER) ) {
            Status = STATUS_BUFFER_TOO_SMALL;
            __leave;
        }

        if (!ExAcquireResourceExclusiveLite (
                    &Fcb->MainResource, Ext2CanIWait())) {
            Status = STATUS_PENDING;
            __leave;
        }
        FcbResourceAcquired = TRUE;

        SVIB = (PSTARTING_VCN_INPUT_BUFFER)
               EIrpSp->Parameters.FileSystemControl.Type3InputBuffer;
        RPSB = (PRETRIEVAL_POINTERS_BUFFER) Ext2GetUserBuffer(Irp);
        if (SVIB == NULL || RPSB == NULL) {
            Status = STATUS_INVALID_USER_BUFFER;
            __leave;
        }

        /* the starting VCN is read once, under the probe's protection */
        __try {
            if (Irp->RequestorMode != KernelMode) {
                ProbeForRead (SVIB, InputSize,  sizeof(UCHAR));
                ProbeForWrite(RPSB, OutputSize, sizeof(UCHAR));
            }
            StartVcn = SVIB->StartingVcn.QuadPart;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            Status = STATUS_INVALID_USER_BUFFER;
        }

        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        /* range check before the shift: a huge VCN would wrap to a small Vbn */
        EndVcn = (Fcb->Header.AllocationSize.QuadPart + BLOCK_SIZE - 1) >> BLOCK_BITS;
        if (StartVcn < 0) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }
        if (StartVcn >= EndVcn) {
            Status = STATUS_END_OF_FILE;
            __leave;
        }
        Vbn = StartVcn << BLOCK_BITS;

        /* mapped runs in file order; the gaps between them are holes
           (sparse ranges), reported as Lcn -1 the way NTFS does */
        while (Vbn < Fcb->Header.AllocationSize.QuadPart) {

            ASSERT(Chain == NULL);
            Length = 0x80000000; /* the map is built in 2 GB chunks */
            if (Fcb->Header.AllocationSize.QuadPart < Vbn + Length) {
                Length = (ULONG)(Fcb->Header.AllocationSize.QuadPart - Vbn);
            }

            Status = Ext2BuildExtents(
                         IrpContext,
                         Vcb,
                         Fcb->Mcb,
                         Vbn,
                         Length,
                         FALSE,
                         &Chain);

            if (!NT_SUCCESS(Status)) {
                __leave;
            }

            for (Extent = Chain; Extent; Extent = Extent->Next) {
                LONGLONG RunVcn = (Vbn + Extent->Offset) >> BLOCK_BITS;
                LONGLONG RunEnd = (Vbn + Extent->Offset + Extent->Length) >> BLOCK_BITS;

                if (RunVcn > StartVcn) {
                    Status = Ext2PutRetrievalRun(RPSB, OutputSize, &i, &LastLcnEnd,
                                                 StartVcn, RunVcn, -1);
                    if (Status != STATUS_SUCCESS)
                        __leave;
                }
                Status = Ext2PutRetrievalRun(RPSB, OutputSize, &i, &LastLcnEnd,
                                             RunVcn, RunEnd, Extent->Lba >> BLOCK_BITS);
                if (Status != STATUS_SUCCESS)
                    __leave;
                StartVcn = RunEnd;
            }

            Ext2DestroyExtentChain(Chain);
            Chain = NULL;

            Vbn = Vbn + Length;
        }

        /* a sparse tail up to the allocation */
        if (StartVcn < EndVcn) {
            Status = Ext2PutRetrievalRun(RPSB, OutputSize, &i, &LastLcnEnd,
                                         StartVcn, EndVcn, -1);
        }

    } __finally {

        if (FcbResourceAcquired) {
            ExReleaseResourceLite(&Fcb->MainResource);
        }

        if (Chain) {
            Ext2DestroyExtentChain(Chain);
        }

        if (i != 0 && (NT_SUCCESS(Status) || Status == STATUS_BUFFER_OVERFLOW)) {
            Irp->IoStatus.Information = FIELD_OFFSET(RETRIEVAL_POINTERS_BUFFER, Extents) +
                                        i * sizeof(RPSB->Extents[0]);
        }

        if (!AbnormalTermination()) {
            if (Status == STATUS_PENDING || Status == STATUS_CANT_WAIT) {
                Status = Ext2QueueRequest(IrpContext);
            } else {
                Ext2CompleteIrpContext(IrpContext, Status);
            }
        }
    }

    return Status;
}

NTSTATUS
Ext2GetRetrievalPointerBase (
    IN PEXT2_IRP_CONTEXT IrpContext
)
{
    PIRP                Irp = NULL;
    PIO_STACK_LOCATION  IrpSp;
    PEXTENDED_IO_STACK_LOCATION EIrpSp;

    PDEVICE_OBJECT      DeviceObject;
    PFILE_OBJECT        FileObject;

    PEXT2_VCB           Vcb = NULL;
    PEXT2_FCB           Fcb = NULL;
    PEXT2_CCB           Ccb = NULL;

    PLARGE_INTEGER      FileAreaOffset;

    ULONG               OutputSize;

    NTSTATUS            Status = STATUS_SUCCESS;

    BOOLEAN FcbResourceAcquired = FALSE;

    __try {

        ASSERT(IrpContext);
        Irp = IrpContext->Irp;
        ASSERT(Irp);

        IrpSp = IoGetCurrentIrpStackLocation(Irp);
        EIrpSp = (PEXTENDED_IO_STACK_LOCATION)IrpSp;
        ASSERT(IrpSp);

        OutputSize = EIrpSp->Parameters.FileSystemControl.OutputBufferLength;

        ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
               (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

        DeviceObject = IrpContext->DeviceObject;

        /* This request is not allowed on the main device object */
        if (IsExt2FsDevice(DeviceObject)) {
            Status = STATUS_INVALID_DEVICE_REQUEST;
            __leave;
        }

        Vcb = (PEXT2_VCB) DeviceObject->DeviceExtension;
        ASSERT(Vcb != NULL);
        ASSERT((Vcb->Identifier.Type == EXT2VCB) &&
               (Vcb->Identifier.Size == sizeof(EXT2_VCB)));
        if (!IsMounted(Vcb) || IsFlagOn(Vcb->Flags, VCB_DISMOUNT_PENDING)) {
            Status = STATUS_VOLUME_DISMOUNTED;
            __leave;
        }

        FileObject = IrpContext->FileObject;
        Fcb = (PEXT2_FCB) FileObject->FsContext;

        /* check Fcb is valid or not */
        if (Fcb == NULL || Fcb->Identifier.Type == EXT2VCB) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        ASSERT((Fcb->Identifier.Type == EXT2FCB) &&
               (Fcb->Identifier.Size == sizeof(EXT2_FCB)));

        if (IsInodeDeleted(Fcb)) {
            Status = STATUS_FILE_DELETED;
            __leave;
        }

        Ccb = (PEXT2_CCB) FileObject->FsContext2;
        if (Ccb == NULL) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        ASSERT((Ccb->Identifier.Type == EXT2CCB) &&
               (Ccb->Identifier.Size == sizeof(EXT2_CCB)));

        if (OutputSize < sizeof(LARGE_INTEGER)) {
            Status = STATUS_BUFFER_TOO_SMALL;
            __leave;
        }

        if (!ExAcquireResourceExclusiveLite (
                    &Fcb->MainResource, Ext2CanIWait())) {
            Status = STATUS_PENDING;
            __leave;
        }
        FcbResourceAcquired = TRUE;

        FileAreaOffset = (PLARGE_INTEGER) Ext2GetUserBuffer(Irp);

        /* probe user buffer */

        /* METHOD_NEITHER: the store stays inside the probe's __try */
        __try {
            if (Irp->RequestorMode != KernelMode) {
                ProbeForWrite(FileAreaOffset, OutputSize, sizeof(UCHAR));
            }
            /* ext4 blocks are counted from sector 0 of the volume */
            FileAreaOffset->QuadPart = 0;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            Status = STATUS_INVALID_USER_BUFFER;
        }

        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        Irp->IoStatus.Information = sizeof(LARGE_INTEGER);

    } __finally {

        if (FcbResourceAcquired) {
            ExReleaseResourceLite(&Fcb->MainResource);
        }

        if (!AbnormalTermination()) {
            if (Status == STATUS_PENDING || Status == STATUS_CANT_WAIT) {
                Status = Ext2QueueRequest(IrpContext);
            } else {
                Ext2CompleteIrpContext(IrpContext, Status);
            }
        }
    }

    return Status;
}
