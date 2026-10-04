/**
 * FileSystemShutdown.c - IRP_MJ_SHUTDOWN.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2ShutDown)
#endif

NTSTATUS
Ext2ShutDown (IN PEXT2_IRP_CONTEXT IrpContext)
{
    NTSTATUS Status = STATUS_UNSUCCESSFUL;

    PIRP                    Irp;

    PEXT2_VCB               Vcb;
    PLIST_ENTRY             ListEntry;

    BOOLEAN                 GlobalResourceAcquired = FALSE;

    LARGE_INTEGER           SysTime;

    __try {

        Status = STATUS_SUCCESS;

        ASSERT(IrpContext);
        ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
               (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

        Irp = IrpContext->Irp;

        if (!ExAcquireResourceExclusiveLite(
                    &Ext2Global->Resource,
                    IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT) )) {
            Status = STATUS_PENDING;
            __leave;
        }

        GlobalResourceAcquired = TRUE;

        for (ListEntry = Ext2Global->VcbList.Flink;
                ListEntry != &(Ext2Global->VcbList);
                ListEntry = ListEntry->Flink ) {

            Vcb = CONTAINING_RECORD(ListEntry, EXT2_VCB, Next);

            if (ExAcquireResourceExclusiveLite(
                        &Vcb->MainResource,
                        TRUE )) {

                if (IsMounted(Vcb)) {

                    /* update fs write time */
                    KeQuerySystemTime(&SysTime);
                    Ext2SetSuperTime(Vcb->SuperBlock, s_wtime, Ext2UnixTime(&SysTime));

                    if (!Ext2JournalIsActive(Vcb)) {
                        /* journaled volumes count mounts when the journal
                           starts, unjournaled ones here as before */
                        Vcb->SuperBlock->s_mnt_count++;
                    }

                    /* A shutdown has no one to report to: what fails to
                       reach the disk here is replayed from the journal at
                       the next mount, or found by e2fsck without one. */
                    (void)Ext2SaveSuper(IrpContext, Vcb);

                    /* flush dirty cache for all files */
                    (void)Ext2FlushFiles(IrpContext, Vcb, TRUE);

                    /* flush volume stream's cache to disk */
                    (void)Ext2FlushVolume(IrpContext, Vcb, TRUE);

                    /* commit, checkpoint, empty the journal and mark the
                       fs cleanly unmounted; the engine stays up and would
                       re-mark the fs if anything else got written */
                    if (Vcb->Journal) {
                        (void)Ext2JournalMarkClean(Vcb);
                    }
                    /* the volume is free for other nodes again */
                    Ext4MmpStop(Vcb, !IsFlagOn(Vcb->Flags, VCB_DEVICE_REMOVED));

                    /* send shutdown request to underlying disk */
                    Ext2DiskShutDown(Vcb);
                }

                ExReleaseResourceLite(&Vcb->MainResource);
            }
        }

        IoUnregisterFileSystem(Ext2Global->DiskdevObject);
        IoUnregisterFileSystem(Ext2Global->CdromdevObject);

    } __finally {

        if (GlobalResourceAcquired) {
            ExReleaseResourceLite(&Ext2Global->Resource);
        }

        if (!IrpContext->ExceptionInProgress) {
            if (Status == STATUS_PENDING) {
                Ext2QueueRequest(IrpContext);
            } else {
                Ext2CompleteIrpContext(IrpContext, Status);
            }
        }
    }

    return Status;
}
