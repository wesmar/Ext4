/**
 * VolumeTrim.c - FSCTL_EXT4_TRIM: the volume's free blocks given back to the
 * device below, what fstrim does on Linux.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Group by group, the free runs go down as trims
 * (IOCTL_STORAGE_MANAGE_DATA_SET_ATTRIBUTES): an SSD can erase them, and a
 * file under the volume (volume\FileDisk.c) gets holes there.
 *
 * What is dropped stays dropped, so what counts as free is narrow. Blocks
 * whose release has not committed are not: after a crash their old owner
 * still has them. They are committed first, and any freed since stay out of
 * the search (ext4\FreedBlocks.c). A run is trimmed with its group's BlockLock
 * held: nothing can be given a block of it and write there before the trim
 * reaches the disk. A bitmap that calls metadata free is not believed either.
 *
 * A group that was never used since mkfs (BLOCK_UNINIT) is left out: nothing
 * has been written there.
 */

#include "ext4fs.h"
#include <ntddstor.h>
#include "ext4crypt.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2TrimVolume)
#endif

typedef struct _EXT2_TRIM_REQUEST {
    DEVICE_MANAGE_DATA_SET_ATTRIBUTES   Head;
    DEVICE_DATA_SET_RANGE               Range;
} EXT2_TRIM_REQUEST;

/* a run of blocks from a group, down to the device */
static NTSTATUS
Ext2TrimRun(IN PVOID Context, IN ULONGLONG Block, IN ULONG Count)
{
    PEXT2_VCB           Vcb = ((PEXT4_TRIM_RUN)Context)->Vcb;
    EXT2_TRIM_REQUEST   In;
    NTSTATUS            Status;

    RtlZeroMemory(&In, sizeof(In));
    In.Head.Size = sizeof(In.Head);
    In.Head.Action = DeviceDsmAction_Trim;
    In.Head.DataSetRangesOffset = FIELD_OFFSET(EXT2_TRIM_REQUEST, Range);
    In.Head.DataSetRangesLength = sizeof(In.Range);
    In.Range.StartingOffset = (LONGLONG)(Block << BLOCK_BITS);
    In.Range.LengthInBytes = (ULONGLONG)Count << BLOCK_BITS;

    Status = Ext2DiskIoControl(Vcb->TargetDeviceObject, IOCTL_STORAGE_MANAGE_DATA_SET_ATTRIBUTES,
                               &In, sizeof(In), NULL, NULL);
    if (NT_SUCCESS(Status)) {
        ((PEXT4_TRIM_RUN)Context)->Bytes += In.Range.LengthInBytes;
        ((PEXT4_TRIM_RUN)Context)->Runs++;
    }
    return Status;
}

NTSTATUS
Ext2TrimVolume(IN PEXT2_IRP_CONTEXT IrpContext)
{
    PIRP                        Irp = IrpContext->Irp;
    PEXTENDED_IO_STACK_LOCATION IrpSp = (PEXTENDED_IO_STACK_LOCATION)IoGetCurrentIrpStackLocation(Irp);
    PEXT4_TRIM                  Trim = Irp->AssociatedIrp.SystemBuffer;
    PEXT2_VCB                   Vcb = (PEXT2_VCB)IrpContext->DeviceObject->DeviceExtension;
    EXT4_TRIM_RUN               Run;
    ULONGLONG                   MinBlocks;
    ULONG                       Group;
    NTSTATUS                    Status = STATUS_SUCCESS;

    PAGED_CODE();

    __try {

        if (IsExt2FsDevice(IrpContext->DeviceObject) || IrpContext->FileObject->FsContext != (PVOID)Vcb) {
            Status = STATUS_INVALID_PARAMETER;      /* the volume's own handle */
            __leave;
        }
        if (IrpSp->Parameters.FileSystemControl.InputBufferLength < sizeof(EXT4_TRIM) ||
            IrpSp->Parameters.FileSystemControl.OutputBufferLength < sizeof(EXT4_TRIM)) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }
        if (IsVcbReadOnly(Vcb)) {
            Status = STATUS_MEDIA_WRITE_PROTECTED;
            __leave;
        }
        if (!IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT)) {
            Status = STATUS_PENDING;
            __leave;
        }

        /* whatever has been freed so far can go */
        Status = Ext2JournalCommit(Vcb, TRUE);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        RtlZeroMemory(&Run, sizeof(Run));
        Run.Vcb = Vcb;
        MinBlocks = (Trim->MinLength + BLOCK_SIZE - 1) >> BLOCK_BITS;
        if (MinBlocks == 0) {
            MinBlocks = 1;
        }

        for (Group = 0; Group < Vcb->sbi.s_groups_count && NT_SUCCESS(Status); Group++) {
            /* the volume stays while a group is at work; a dismount waits for
               that much, and the next group sees it */
            ExAcquireResourceSharedLite(&Vcb->MainResource, TRUE);
            __try {
                if (IsFlagOn(Vcb->Flags, VCB_DISMOUNT_PENDING) || !IsMounted(Vcb)) {
                    Status = STATUS_VOLUME_DISMOUNTED;
                } else {
                    Status = Ext2TrimGroup(IrpContext, Vcb, Group, MinBlocks, Ext2TrimRun, &Run);
                }
            } __finally {
                ExReleaseResourceLite(&Vcb->MainResource);
            }
        }

        /* a device that takes no trims */
        if (Status == STATUS_INVALID_DEVICE_REQUEST || Status == STATUS_NOT_IMPLEMENTED) {
            Status = STATUS_NOT_SUPPORTED;
        }
        if (NT_SUCCESS(Status)) {
            Trim->Bytes = Run.Bytes;
            Trim->Runs = Run.Runs;
            Irp->IoStatus.Information = sizeof(EXT4_TRIM);
        }

    } __finally {

        if (!AbnormalTermination()) {
            if (Status == STATUS_PENDING) {
                Status = Ext2QueueRequest(IrpContext);
            } else {
                Ext2CompleteIrpContext(IrpContext, Status);
            }
        }
    }

    return Status;
}
