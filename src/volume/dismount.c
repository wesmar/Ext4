/**
 * dismount.c - dismount, purge and deferred VCB teardown.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "../fsd/fsctl_internal.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2PurgeVolume)
#pragma alloc_text(PAGE, Ext2PurgeFile)
#pragma alloc_text(PAGE, Ext2DismountVolume)
#endif

NTSTATUS
Ext2DismountVolume (IN PEXT2_IRP_CONTEXT IrpContext)
{
    PDEVICE_OBJECT  DeviceObject;
    NTSTATUS        Status = STATUS_UNSUCCESSFUL;
    PEXT2_VCB       Vcb = NULL;
    BOOLEAN         VcbResourceAcquired = FALSE;

    __try {

        ASSERT(IrpContext != NULL);

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

        ExAcquireResourceExclusiveLite(
            &Vcb->MainResource,
            TRUE );

        VcbResourceAcquired = TRUE;

        if ( IsFlagOn(Vcb->Flags, VCB_DISMOUNT_PENDING)) {
            Status = STATUS_VOLUME_DISMOUNTED;
            __leave;
        }

        Ext2FlushFiles(IrpContext, Vcb, FALSE);
        Ext2FlushVolume(IrpContext, Vcb, FALSE);

        ExReleaseResourceLite(&Vcb->MainResource);
        VcbResourceAcquired = FALSE;

        Ext2PurgeVolume(Vcb, TRUE);
        Ext2CheckDismount(IrpContext, Vcb, TRUE);

        DEBUG(DL_INF, ( "Ext2Dismount: Volume dismount pending.\n"));
        Status = STATUS_SUCCESS;

    } __finally {

        if (VcbResourceAcquired) {
            ExReleaseResourceLite(&Vcb->MainResource);
        }

        if (!IrpContext->ExceptionInProgress) {
            Ext2CompleteIrpContext(IrpContext,  Status);
        }
    }

    return Status;
}

BOOLEAN
Ext2CheckDismount (
    IN PEXT2_IRP_CONTEXT IrpContext,
    IN PEXT2_VCB         Vcb,
    IN BOOLEAN           bForce   )
{
    KIRQL   Irql;
    PVPB    Vpb = Vcb->Vpb, NewVpb = NULL;
    BOOLEAN bDeleted = FALSE, bTearDown = FALSE;
    ULONG   UnCleanCount = 0;

    NewVpb = Ext2AllocatePool(NonPagedPool, VPB_SIZE, TAG_VPB);
    if (NewVpb == NULL) {
        DEBUG(DL_ERR, ( "Ext2CheckDismount: failed to allocate NewVpb.\n"));
        return FALSE;
    }
    DEBUG(DL_DBG, ("Ext2CheckDismount: NewVpb allocated: %p\n", NewVpb));
    INC_MEM_COUNT(PS_VPB, NewVpb, sizeof(VPB));
    memset(NewVpb, '_', VPB_SIZE);
    RtlZeroMemory(NewVpb, sizeof(VPB));

    ExAcquireResourceExclusiveLite(
        &Ext2Global->Resource, TRUE );

    ExAcquireResourceExclusiveLite(
        &Vcb->MainResource, TRUE );

    if (IsFlagOn(Vcb->Flags, VCB_BEING_DROPPED)) {
        ExReleaseResourceLite(&Vcb->MainResource);
        ExReleaseResourceLite(&Ext2Global->Resource);
        if (NewVpb != NULL) {
            Ext2FreePool(NewVpb, TAG_VPB);
            DEC_MEM_COUNT(PS_VPB, NewVpb, sizeof(VPB));
        }
        return FALSE;
    }

    if (IrpContext && 
        IrpContext->MajorFunction == IRP_MJ_CREATE &&
        IrpContext->RealDevice == Vcb->RealDevice) {
        UnCleanCount = 2;
    } else {
        UnCleanCount = 1;
    }

    IoAcquireVpbSpinLock (&Irql);

    DEBUG(DL_DBG, ("Ext2CheckDismount: Vpb %p ioctl=%d Device %p\n",
                   Vpb, Vpb->ReferenceCount, Vpb->RealDevice));

    if (Vpb->ReferenceCount <= UnCleanCount) {

        if (!IsFlagOn(Vcb->Flags, VCB_DISMOUNT_PENDING)) {

            ClearFlag(Vpb->Flags, VPB_MOUNTED);
            ClearFlag(Vpb->Flags, VPB_LOCKED);

            if ((Vcb->RealDevice != Vpb->RealDevice) &&
                    (Vcb->RealDevice->Vpb == Vpb)) {
                SetFlag(Vcb->RealDevice->Flags, DO_DEVICE_INITIALIZING);
                SetFlag(Vpb->Flags, VPB_PERSISTENT );
            }

            Ext2RemoveVcb(Vcb);
            SetLongFlag(Vcb->Flags, VCB_DISMOUNT_PENDING);

            /* the Fcb reaper drains the dismounted volume's cache; the
               last Fcb it frees destroys the Vcb */
            Ext2ReaperKick(&Ext2Global->FcbReaper, TRUE);
        }

        if (Vpb->ReferenceCount) {
            bTearDown = TRUE;
        } else {
            bDeleted = TRUE;
            Vpb->DeviceObject = NULL;
        }

        DEBUG(DL_DBG, ("Ext2CheckDismount: Vpb: %p bDeleted=%d bTearDown=%d\n",
                        Vpb, bDeleted, bTearDown));

    } else if (bForce) {

        DEBUG(DL_DBG, ( "Ext2CheckDismount: New/Old Vpb %p/%p Realdevice = %p\n",
                        NewVpb, Vcb->Vpb, Vpb->RealDevice));

        if (!IsFlagOn(Vcb->Flags, VCB_NEW_VPB)) {
            NewVpb->Type = IO_TYPE_VPB;
            NewVpb->Size = sizeof(VPB);
            NewVpb->Flags = Vpb->Flags & VPB_REMOVE_PENDING;
            NewVpb->RealDevice = Vpb->RealDevice;
            if (!Ext2TrackVpbSwap(Vpb, NewVpb)) {
                /* No published allocation may escape ownership tracking. */
                goto ReleaseVpb;
            }
            SetFlag(Vpb->Flags, VPB_PERSISTENT);
            NewVpb->RealDevice->Vpb = NewVpb;
            NewVpb = NULL;
            SetLongFlag(Vcb->Flags, VCB_NEW_VPB);
        }
        ClearFlag(Vpb->Flags, VPB_MOUNTED);
        ClearLongFlag(Vcb->Flags, VCB_MOUNTED);

        if (!IsFlagOn(Vcb->Flags, VCB_DISMOUNT_PENDING)) {
            Ext2RemoveVcb(Vcb);
            SetLongFlag(Vcb->Flags, VCB_DISMOUNT_PENDING);
        }

        /* User file objects are still open on the volume. The metadata
           stream (Vcb->Volume) stays with them: their cleanup and close
           still save inodes and free blocks through it, and a stream torn
           down here leaves those requests with Vcb->Volume == NULL - the
           cleanup dies in the exception handler before it can uninitialize
           the cache map, the file object never closes and the volume never
           goes. The Fcb reaper frees the last Fcb once the last file object
           has closed and tears the stream down then (Ext2FreeFcb ->
           Ext2CheckDismount, the branch above: the stream is the only
           reference left). Wake it up for the cached files whose handles
           are already closed. */
        Ext2ReaperKick(&Ext2Global->FcbReaper, TRUE);
    }

ReleaseVpb:
    IoReleaseVpbSpinLock(Irql);

    /* The last file object closing brings several threads here at once:
       the Fcb reaper (Ext2FreeFcb), the unload thread (Ext2PrepareToUnload
       phase 3) and a PnP removal. The stream comes down once, by the first
       of them; the others leave it alone - a second Ext2TearDownStream on
       the same stream runs into the cache map the first one is deleting
       (bugcheck 0x34 in CcDeleteBcbs). Claimed under Vcb->MainResource. */
    if (bTearDown) {
        if (IsFlagOn(Vcb->Flags, VCB_STREAM_TEARDOWN)) {
            bTearDown = FALSE;
        } else {
            SetLongFlag(Vcb->Flags, VCB_STREAM_TEARDOWN);
        }
    }

    ExReleaseResourceLite(&Vcb->MainResource);
    ExReleaseResourceLite(&Ext2Global->Resource);

    if (bTearDown) {
        DEBUG(DL_DBG, ( "Ext2CheckDismount: Tearing vcb %p ...\n", Vcb));
        Ext2TearDownStream(Vcb);

        /* the stream was the last file object on the volume; if no Fcb
           references the Vcb either, nobody will ever come back to finish
           it - so finish it here */
        IoAcquireVpbSpinLock(&Irql);
        if (Vpb->ReferenceCount == 0 && Vcb->ReferenceCount == 0) {
            Vpb->DeviceObject = NULL;
            bDeleted = TRUE;
        }
        IoReleaseVpbSpinLock(Irql);
    }

    if (bDeleted) {
        ExAcquireResourceExclusiveLite(&Ext2Global->Resource, TRUE);
        if (!IsFlagOn(Vcb->Flags, VCB_BEING_DROPPED)) {
            SetLongFlag(Vcb->Flags, VCB_BEING_DROPPED);
            ExReleaseResourceLite(&Ext2Global->Resource);
            DEBUG(DL_DBG, ( "Ext2CheckDismount: Deleting vcb %p ...\n", Vcb));
            Ext2DestroyVcb(Vcb);
        } else {
            ExReleaseResourceLite(&Ext2Global->Resource);
            bDeleted = FALSE;
        }
    }

    if (NewVpb != NULL) {
        DEBUG(DL_DBG, ( "Ext2CheckDismount: freeing new Vpb %p\n", NewVpb));
        Ext2FreePool(NewVpb, TAG_VPB);
        DEC_MEM_COUNT(PS_VPB, NewVpb, sizeof(VPB));
    }

    return bDeleted;
}

NTSTATUS
Ext2PurgeVolume (IN PEXT2_VCB Vcb,
                 IN BOOLEAN  FlushBeforePurge )
{
    PEXT2_FCB       Fcb;
    LIST_ENTRY      List, *Next;

    BOOLEAN         VcbResourceAcquired = FALSE;
    BOOLEAN         FcbResourceAcquired = FALSE;
    BOOLEAN         gdResourceAcquired = FALSE;

    __try {

        ASSERT(Vcb != NULL);
        ASSERT((Vcb->Identifier.Type == EXT2VCB) &&
               (Vcb->Identifier.Size == sizeof(EXT2_VCB)));

        ExAcquireResourceExclusiveLite(&Vcb->MainResource, TRUE);
        VcbResourceAcquired = TRUE;

        if (IsVcbReadOnly(Vcb)) {
            FlushBeforePurge = FALSE;
        }

        /* A running journal session keeps every journaled metadata block
           pinned in the volume stream (running transaction and checkpoint
           list), and CcPurgeCacheSection below waits for pins forever.
           Every caller purges on the way to a dismount, a surprise
           removal or a raw write to the locked volume - all of which end
           the session anyway - so end it here first: commit, checkpoint,
           mark the fs clean, unpin. */
        if (Vcb->Journal) {
            Ext2JournalStop(Vcb, !IsVcbReadOnly(Vcb));
        }
        Ext4MmpStop(Vcb, !IsFlagOn(Vcb->Flags, VCB_DEVICE_REMOVED));

        InitializeListHead(&List);

        ExAcquireResourceExclusiveLite(&Vcb->FcbLock, TRUE);
        FcbResourceAcquired = TRUE;

        while (!IsListEmpty(&Vcb->FcbList)) {

            Next = RemoveHeadList(&Vcb->FcbList);
            Fcb = CONTAINING_RECORD(Next, EXT2_FCB, Next);

            DEBUG(DL_INF, ( "Ext2PurgeVolume: %wZ refercount=%xh\n",
                            &Fcb->Mcb->FullName, Fcb->ReferenceCount));
            InsertTailList(&List, &Fcb->Next);
        }

        while (!IsListEmpty(&List)) {

            Next = RemoveHeadList(&List);
            Fcb = CONTAINING_RECORD(Next, EXT2_FCB, Next);

            if (ExAcquireResourceExclusiveLite(
                        &Fcb->MainResource,
                        TRUE )) {

                Ext2PurgeFile(Fcb, FlushBeforePurge);

                if (Fcb->ReferenceCount <= 1) {
                    Fcb->TsDrop.QuadPart = 0;
                    InsertHeadList(&Vcb->FcbList, &Fcb->Next);
                } else {
                    InsertTailList(&Vcb->FcbList, &Fcb->Next);
                }
                ExReleaseResourceLite(&Fcb->MainResource);
            }
        }

        if (FcbResourceAcquired) {
            ExReleaseResourceLite(&Vcb->FcbLock);
            FcbResourceAcquired = FALSE;
        }

        /* acquire bd lock to avoid bh creation */
        ExAcquireResourceExclusiveLite(&Vcb->sbi.s_gd_lock, TRUE);
        gdResourceAcquired = TRUE;

        /* discard buffer_headers for group_desc */
        Ext2DropBH(Vcb);

        if (FlushBeforePurge) {
            ExAcquireSharedStarveExclusive(&Vcb->PagingIoResource, TRUE);
            ExReleaseResourceLite(&Vcb->PagingIoResource);

            CcFlushCache(&Vcb->SectionObject, NULL, 0, NULL);
        }

        if (Vcb->SectionObject.ImageSectionObject) {
            MmFlushImageSection(&Vcb->SectionObject, MmFlushForWrite);
        }

        if (Vcb->SectionObject.DataSectionObject) {
            CcPurgeCacheSection(&Vcb->SectionObject, NULL, 0, FALSE);
        }

        DEBUG(DL_INF, ( "Ext2PurgeVolume: Volume flushed and purged.\n"));

    } __finally {

        if (gdResourceAcquired) {
            ExReleaseResourceLite(&Vcb->sbi.s_gd_lock);
        }

        if (FcbResourceAcquired) {
            ExReleaseResourceLite(&Vcb->FcbLock);
        }

        if (VcbResourceAcquired) {
            ExReleaseResourceLite(&Vcb->MainResource);
        }
    }

    /* every purge is on the way to a dismount, a removal or a raw write:
       the Fcb cache is worthless now and its references would keep the
       Vcb - and the driver - alive for as long as the reaper takes */
    Ext2DropIdleFcbs(Vcb);

    return STATUS_SUCCESS;
}
NTSTATUS
Ext2PurgeFile ( IN PEXT2_FCB Fcb,
                IN BOOLEAN  FlushBeforePurge )
{
    IO_STATUS_BLOCK    IoStatus;

    ASSERT(Fcb != NULL);

    ASSERT((Fcb->Identifier.Type == EXT2FCB) &&
           (Fcb->Identifier.Size == sizeof(EXT2_FCB)));

    if (!IsVcbReadOnly(Fcb->Vcb) && FlushBeforePurge) {
        DEBUG(DL_INF, ( "Ext2PurgeFile: CcFlushCache on %wZ.\n",
                        &Fcb->Mcb->FullName));
        ExAcquireSharedStarveExclusive(&Fcb->PagingIoResource, TRUE);
        ExReleaseResourceLite(&Fcb->PagingIoResource);
        CcFlushCache(&Fcb->SectionObject, NULL, 0, &IoStatus);
        ClearLongFlag(Fcb->Flags, FCB_FILE_MODIFIED);
    }

    if (Fcb->SectionObject.ImageSectionObject) {
        DEBUG(DL_INF, ( "Ext2PurgeFile: MmFlushImageSection on %wZ.\n",
                        &Fcb->Mcb->FullName));
        MmFlushImageSection(&Fcb->SectionObject, MmFlushForWrite);
    }

    if (Fcb->SectionObject.DataSectionObject) {
        DEBUG(DL_INF, ( "Ext2PurgeFile: CcPurgeCacheSection on %wZ.\n",
                        &Fcb->Mcb->FullName));
        /* UninitializeCacheMaps: the cache manager's shared cache map keeps
           the section's views mapped and holds a file object of its own
           until the lazy writer gets round to tearing it down - for a file
           written and deleted just before, that is what kept a cleaned-up
           file object without its close, an Fcb reference and the volume
           alive. Every purge here is on the way to a dismount, a removal
           or a raw write: the cache maps go now. */
        CcPurgeCacheSection(&Fcb->SectionObject, NULL, 0, TRUE);

        /* The purge empties the section but does not delete it, and the
           data section holds a reference on the file object that created
           it: no IRP_MJ_CLOSE, no Fcb release, no dismount until the memory
           manager happens to trim it (minutes after boot, while the
           prefetcher tracks files). Every purge is on the way to a
           dismount, a removal or a raw write, so the section goes now - or,
           when a view is still mapped, the moment it is unmapped. A close
           triggered here re-acquires only this Fcb's resource on this
           thread, and the Fcb stays allocated: close never frees it. */
        MmForceSectionClosed(&Fcb->SectionObject, TRUE);
    }

    return STATUS_SUCCESS;
}
