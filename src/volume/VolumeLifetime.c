/**
 * VolumeLifetime.c - the life of a mounted volume: the volume list, its metadata stream, its end.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * A volume is built from its superblock in VolumeInitialize.c (Ext2InitializeVcb).
 */

#include "ext4fs.h"
#include "../core/core_internal.h"
#include "vcb_internal.h"

/* the longest the teardown waits for the metadata stream's last close (a
   cache manager work item drops it), in 100 ns units: 5 s */
#define EXT2_STREAM_CLOSE_WAIT  (5LL * 1000 * 1000 * 10)

/*
 * The last reference of ours to the metadata stream (its cache map gone,
 * its section closed). It is not always the last one: the cache manager
 * takes one of its own when the first cache map of a device is set up
 * (CcGetDeviceGuidAsync, a worker item) and drops it when that item has
 * run - after a short mount (a volume refused right away), only after the
 * Vcb was gone. The stream's close then came to a freed Vcb: its
 * resources, and the per-stream contexts Filter Manager looks up through
 * FsContext. So the stream no longer points at the Vcb (FsContext2 only
 * marks it as ours for Ext2Close), and, when its close is to come to this
 * volume, the Vcb stays until it has.
 */
VOID
Ext2ReleaseStream(IN PEXT2_VCB Vcb, IN PFILE_OBJECT Stream)
{
    BOOLEAN       Ours = Stream->Vpb != NULL && Stream->Vpb->DeviceObject == Vcb->DeviceObject;
    LARGE_INTEGER Wait;

    Stream->FsContext = NULL;
    Stream->FsContext2 = Vcb;
    ObDereferenceObject(Stream);
    if (Ours) {
        Wait.QuadPart = -(LONGLONG)EXT2_STREAM_CLOSE_WAIT;
        if (KeWaitForSingleObject(&Vcb->StreamClosed, Executive, KernelMode,
                                  FALSE, &Wait) == STATUS_TIMEOUT) {
            DbgPrint("ext4: the metadata stream of %p is not closed yet\n", Vcb);
        }
    }
}

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2TearDownStream)
#pragma alloc_text(PAGE, Ext2DestroyVcb)
#pragma alloc_text(PAGE, Ext2SyncUninitializeCacheMap)
#endif

/* Ext2Global->Resource should be already acquired */
VOID
Ext2InsertVcb(PEXT2_VCB Vcb)
{
    InsertTailList(&(Ext2Global->VcbList), &Vcb->Next);
}

/* Ext2Global->Resource should be already acquired. A dismounted volume
   moves to the dismounting list until Ext2DestroyVcb: its cached Fcbs
   still reference it and the Fcb reaper has to see it to drain them. */
VOID
Ext2RemoveVcb(PEXT2_VCB Vcb)
{
    RemoveEntryList(&Vcb->Next);
    InsertTailList(&(Ext2Global->DismountingVcbList), &Vcb->Next);
}

/* the counterpart, from Ext2DestroyVcb: off whichever list it is on */
VOID
Ext2UnlinkVcb(PEXT2_VCB Vcb)
{
    ExAcquireResourceExclusiveLite(&Ext2Global->Resource, TRUE);
    RemoveEntryList(&Vcb->Next);
    InitializeListHead(&Vcb->Next);
    ExReleaseResourceLite(&Ext2Global->Resource);
}

NTSTATUS
Ext2InitializeLabel(
    IN PEXT2_VCB            Vcb,
    IN PEXT2_SUPER_BLOCK    Sb
)
{
    NTSTATUS            status;

    USHORT              Length;
    UNICODE_STRING      Label;
    OEM_STRING          OemName;

    Label.MaximumLength = 16 * sizeof(WCHAR);
    Label.Length    = 0;
    Label.Buffer    = Vcb->Vpb->VolumeLabel;
    Vcb->Vpb->VolumeLabelLength = 0;
    RtlZeroMemory(Label.Buffer, Label.MaximumLength);

    Length = 16;
    while (  (Length > 0) &&
             ((Sb->s_volume_name[Length -1]  == 0x00) ||
              (Sb->s_volume_name[Length - 1] == 0x20)  )
          ) {
        Length--;
    }

    if (Length == 0) {
        return STATUS_SUCCESS;
    }

    OemName.Buffer =  Sb->s_volume_name;
    OemName.MaximumLength = 16;
    OemName.Length = Length;

    status = Ext2OEMToUnicode(Vcb, &Label, &OemName);
    if (NT_SUCCESS(status)) {
        Vcb->Vpb->VolumeLabelLength = Label.Length;
    }

    return status;
}

VOID
Ext2TearDownStream(IN PEXT2_VCB Vcb)
{
    PFILE_OBJECT    Stream = Vcb->Volume;
    IO_STATUS_BLOCK IoStatus;

    ASSERT(Vcb != NULL);
    ASSERT((Vcb->Identifier.Type == EXT2VCB) &&
           (Vcb->Identifier.Size == sizeof(EXT2_VCB)));

    /* complete all pending directory change notifications on this volume so
       monitoring applications (Total Commander, FileSystemWatcher) can close handles */
    if (Vcb->NotifySync != NULL) {
        FsRtlNotifyCleanup(Vcb->NotifySync, &Vcb->NotifyList, NULL);
    }

    if (Stream) {

        /* commit, checkpoint and mark the fs clean while the cache and the
           journal inode are still usable */
        if (Vcb->Journal) {
            Ext2JournalStop(Vcb, !IsVcbReadOnly(Vcb));
        }
        /* the last write is done: release the volume to other nodes */
        Ext4MmpStop(Vcb, !IsFlagOn(Vcb->Flags, VCB_DEVICE_REMOVED));

        /* every buffer head pins its block in this stream; the cache map
           cannot go while one is left (bugcheck 0x34) */
        Ext2DrainBH(Vcb);

        Vcb->Volume = NULL;

        if (IsFlagOn(Stream->Flags, FO_FILE_MODIFIED)) {
            CcFlushCache(&(Vcb->SectionObject), NULL, 0, &IoStatus);
            ClearFlag(Stream->Flags, FO_FILE_MODIFIED);
        }

        if (Stream->PrivateCacheMap) {
            Ext2SyncUninitializeCacheMap(Stream);
        }

        /* the stream's data section references the stream file object, and
           that file object holds the Vpb reference that keeps the volume
           device alive: delete the section with the cache map instead of
           whenever the memory manager trims it */
        MmForceSectionClosed(&Vcb->SectionObject, TRUE);

        Ext2ReleaseStream(Vcb, Stream);
        Stream = NULL;
    }
}

VOID
Ext2DestroyVcb (IN PEXT2_VCB Vcb)
{
    ASSERT(Vcb != NULL);
    ASSERT((Vcb->Identifier.Type == EXT2VCB) &&
           (Vcb->Identifier.Size == sizeof(EXT2_VCB)));

    DEBUG(DL_FUN, ("Ext2DestroyVcb ...\n"));

    /* List removal is not completion: unload must also wait for the
       remaining teardown and device deletion below. */
    InterlockedIncrement(&Ext2Global->VcbTeardownsInFlight);
    /* off the mounted or the dismounting list, whichever it is on */
    Ext2UnlinkVcb(Vcb);

    /* the update thread may not outlive the Vcb it writes for */
    Ext4MmpStop(Vcb, !IsFlagOn(Vcb->Flags, VCB_DEVICE_REMOVED));

    if (Vcb->Volume) {
        Ext2TearDownStream(Vcb);
    }
    ASSERT(NULL == Vcb->Volume);

    FsRtlNotifyUninitializeSync(&Vcb->NotifySync);
    if (Vcb->Extents.Initialized) {
        Ext2ListExtents(&Vcb->Extents);
        Ext4RunMapDestroy(&(Vcb->Extents));
        Vcb->Extents.Initialized = FALSE;
    }

    Ext2CleanupAllMcbs(Vcb);

    Ext2DropBH(Vcb);
    /* the group descriptor table itself (its blocks went with the BHs):
       left behind, every mount lost it */
    Ext2PutGroup(Vcb);

    /* All metadata users and the stream are gone. The inactive engine
       stays valid through late stream CLOSE and buffer-release callbacks. */
    Ext2JournalDestroy(Vcb);

    if (Vcb->bd.bd_bh_cache) {
        kmem_cache_destroy(Vcb->bd.bd_bh_cache);
        Vcb->bd.bd_bh_cache = NULL;
    }
    ExDeleteResourceLite(&Vcb->bd.bd_bh_lock);

    if (Vcb->SuperBlock) {
        Ext2FreePool(Vcb->SuperBlock, EXT2_SB_MAGIC);
        Vcb->SuperBlock = NULL;
    }

    ObDereferenceObject(Vcb->TargetDeviceObject);

    if (Vcb->MountDevLink.Buffer) {
        /* the device is gone or we are: the interface name is all that is
           left to free (its state went with the PDO or with the release) */
        RtlFreeUnicodeString(&Vcb->MountDevLink);
    }

    ExDeleteNPagedLookasideList(&(Vcb->InodeLookasideList));
    Ext2DestroyNameCache(Vcb);
    Ext2DestroyGroupLocks(Vcb);
    ExDeleteResourceLite(&Vcb->FcbLock);
    ExDeleteResourceLite(&Vcb->SuperLock);
    ExDeleteResourceLite(&Vcb->sbi.s_gd_lock);
    ExDeleteResourceLite(&Vcb->PagingIoResource);
    ExDeleteResourceLite(&Vcb->MainResource);

    DEBUG(DL_DBG, ("Ext2DestroyVcb: DevObject=%p Vcb=%p\n", Vcb->DeviceObject, Vcb));
    IoDeleteDevice(Vcb->DeviceObject);
    DEC_MEM_COUNT(PS_VCB, Vcb->DeviceObject, sizeof(EXT2_VCB));

    /* one volume fewer stands between "sc stop" and DriverUnload */
    InterlockedDecrement(&Ext2Global->VcbTeardownsInFlight);
    Ext2UnloadKick();
}

/* uninitialize cache map */

VOID
Ext2SyncUninitializeCacheMap (
    IN PFILE_OBJECT FileObject
)
{
    CACHE_UNINITIALIZE_EVENT UninitializeCompleteEvent;
    NTSTATUS WaitStatus;
    LARGE_INTEGER Ext2LargeZero = {0,0};

    KeInitializeEvent( &UninitializeCompleteEvent.Event,
                       SynchronizationEvent,
                       FALSE);

    CcUninitializeCacheMap( FileObject,
                            &Ext2LargeZero,
                            &UninitializeCompleteEvent );

    WaitStatus = KeWaitForSingleObject( &UninitializeCompleteEvent.Event,
                                        Executive,
                                        KernelMode,
                                        FALSE,
                                        NULL);

    ASSERT (NT_SUCCESS(WaitStatus));
}
