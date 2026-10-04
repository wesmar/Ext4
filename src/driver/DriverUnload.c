/**
 * DriverUnload.c - bare "sc stop" support: unload probe, drain thread, prepare-to-unload.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"

/*
 * Bare "sc stop" support.
 *
 * The SCM stops a kernel driver with NtUnloadDriver. For a file system
 * driver that call can never run DriverUnload on its own: IoRegisterFileSystem
 * sets DRVO_BASE_FILESYSTEM_DRIVER on the driver object for good, and both
 * IopCheckUnloadDriver (the sc stop path) and IopCompleteUnloadOrDelete (the
 * deferred path) refuse to unload such a driver while it owns any device
 * object - reference counts of zero are not enough. All the kernel does is
 * mark every device object DOE_UNLOAD_PENDING and return; the driver is not
 * told, and from then on the control device cannot even be opened.
 *
 * So the driver notices the mark itself and performs the unload sequence
 * the I/O manager would otherwise never reach:
 *
 *   detect   a coalescing no-wake timer (never wakes an idle CPU) queues a
 *            work item that reads DOE_UNLOAD_PENDING from the control
 *            device's own DEVOBJ_EXTENSION. That word is private, but it
 *            is the one and only signal there is: IopCheckDeviceAndDriver,
 *            which is what makes every later open fail, tests exactly
 *            "ExtensionFlags & 1Fh" at +0x20 - a layout unchanged since
 *            Windows 2000 - and the read is guarded by the public Type
 *            and back-pointer fields. Opening the device ourselves (the
 *            documented symptom, STATUS_NO_SUCH_DEVICE) is kept only as a
 *            fallback: once a filter such as FltMgr sits on the control
 *            device, the open is judged against the top of the stack and
 *            says nothing about us (seen on 10.0.28000 with Defender).
 *   drain    a system thread runs the prepare-to-unload sequence (flush and
 *            dismount idle volumes, unregister both file systems). While a
 *            volume still has open handles it sleeps on an event that
 *            cleanup, close and volume teardown signal, so it retries the
 *            moment the state changes and not a tick later.
 *   remove   both control devices are deleted: only then is the driver's
 *            device list empty, which is what the kernel insists on.
 *   release  the last reference to \ext4 is the file object behind the
 *            handle DriverEntry keeps open. The thread that drops it runs
 *            DriverUnload and unmaps the image before it gets control back,
 *            so it must not be a thread executing driver code:
 *            ObDereferenceObjectDeferDelete hands the drop to a system
 *            worker thread and the drain thread simply exits.
 *
 * Every piece of driver code involved is accounted for before the image
 * goes away: DriverUnload deletes the timer (waiting for the callback),
 * waits for the work item's rundown protection and for the drain thread's
 * thread object. The work item itself holds a reference on the control
 * device, and a device object keeps its driver object - and therefore the
 * image - alive until the I/O manager drops that reference after the
 * routine has returned.
 *
 * The user-mode path (IOCTL_PREPARE_TO_UNLOAD, then sc stop) still works:
 * the IOCTL unregisters, sc stop marks, the machinery here finishes.
 */

/* 50 ms period: optimal engineering balance between instant sc stop
   responsiveness (~25 ms avg) and practically zero CPU overhead (fast check
   at DISPATCH_LEVEL returns in ~5 ns without queuing any work items) */
#define EXT2_UNLOAD_PROBE_PERIOD    ((LONGLONG)50 * 10 * 1000)

/* the private part of DEVOBJ_EXTENSION we need one bit of */
typedef struct _EXT2_DEVOBJ_EXTENSION {
    CSHORT          Type;               /* IO_TYPE_DEVICE_OBJECT_EXTENSION */
    USHORT          Size;               /* 0: IoCreateDevice never sets it */
    PDEVICE_OBJECT  DeviceObject;       /* back-pointer, our second guard */
    ULONG           PowerFlags;
    PVOID           Dope;
    ULONG           ExtensionFlags;     /* DOE_UNLOAD_PENDING and friends */
} EXT2_DEVOBJ_EXTENSION, *PEXT2_DEVOBJ_EXTENSION;

#define EXT2_DOE_UNLOAD_PENDING     0x00000001

static BOOLEAN Ext2IsUnloadPendingByOpen(VOID);

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2PrepareToUnload)
#endif

NTSTATUS
Ext2PrepareToUnload (IN PEXT2_IRP_CONTEXT IrpContext)
{
    PDEVICE_OBJECT  DeviceObject;
    NTSTATUS        Status = STATUS_UNSUCCESSFUL;
    BOOLEAN         GlobalDataResourceAcquired = FALSE;

    __try {

        ASSERT(IrpContext != NULL);

        ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
               (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

        DeviceObject = IrpContext->DeviceObject;

        /* only meaningful on the control device (\\.\ext4), never on a
           mounted volume */
        if (!IsExt2FsDevice(DeviceObject)) {
            Status = STATUS_INVALID_DEVICE_REQUEST;
            __leave;
        }

        /* no new mounts or drive letters from now on; waits for the
           workers in flight, so it must run before we take the global
           resource (a probe may be mounting under it right now) */
        Ext2StopLetterService();

        ExAcquireResourceExclusiveLite(
            &Ext2Global->Resource,
            TRUE );

        GlobalDataResourceAcquired = TRUE;

        if (FlagOn(Ext2Global->Flags, EXT2_UNLOAD_PENDING)) {
            DEBUG(DL_ERR, ( "Ext2PrepareUnload:  Already ready to unload.\n"));

            Status = STATUS_ACCESS_DENIED;

            __leave;
        }

        /*
         * Phase 0: take back the drive letters we created and tell the
         * shell, before anything else. Explorer, Total Commander, the
         * search indexer keep handles on every drive they can see and let
         * go only when the drive disappears - so with the letter still
         * there an idle volume that is merely displayed would keep us
         * loaded for ever. Done with the global resource dropped: the
         * notification is synchronous with user mode.
         */
        for (;;) {

            PEXT2_VCB               Vcb = NULL;
            PDEVICE_OBJECT          Device = NULL;
            PLIST_ENTRY             ListEntry;

            for (ListEntry  = Ext2Global->VcbList.Flink;
                 ListEntry != &(Ext2Global->VcbList);
                 ListEntry  = ListEntry->Flink) {

                PEXT2_VCB v = CONTAINING_RECORD(ListEntry, EXT2_VCB, Next);

                if (v && IsMounted(v) && IsFlagOn(v->Flags, VCB_LETTER_ASSIGNED)) {
                    Vcb = v;
                    Device = v->DeviceObject;
                    ObReferenceObject(Device);
                    break;
                }
            }

            if (Vcb == NULL) {
                break;
            }

            ExReleaseResourceLite(&Ext2Global->Resource);
            GlobalDataResourceAcquired = FALSE;

            /* clears VCB_LETTER_ASSIGNED, so the loop makes progress */
            Ext2ReleaseLetter(Vcb, TRUE);
            ObDereferenceObject(Device);

            ExAcquireResourceExclusiveLite(&Ext2Global->Resource, TRUE);
            GlobalDataResourceAcquired = TRUE;
        }

        /*
         * Phase 1 & 2: flush and force-dismount every mounted volume.
         * Existing handles will receive STATUS_VOLUME_DISMOUNTED, and
         * the VCB will safely sit on DismountingVcbList until closed.
         */
        for (;;) {

            PEXT2_VCB               Vcb = NULL;
            PLIST_ENTRY             ListEntry;

            if (!GlobalDataResourceAcquired) {
                ExAcquireResourceExclusiveLite(&Ext2Global->Resource, TRUE);
                GlobalDataResourceAcquired = TRUE;
            }

            for (ListEntry  = Ext2Global->VcbList.Flink;
                 ListEntry != &(Ext2Global->VcbList);
                 ListEntry  = ListEntry->Flink) {

                PEXT2_VCB v = CONTAINING_RECORD(ListEntry, EXT2_VCB, Next);

                if (v && IsMounted(v) &&
                        !IsFlagOn(v->Flags, VCB_DISMOUNT_PENDING)) {
                    Vcb = v;
                    break;
                }
            }

            ExReleaseResourceLite(&Ext2Global->Resource);
            GlobalDataResourceAcquired = FALSE;

            if (Vcb == NULL) {
                break;
            }

            ExAcquireResourceExclusiveLite(&Vcb->MainResource, TRUE);

            if (Vcb->NotifySync != NULL) {
                FsRtlNotifyCleanup(Vcb->NotifySync, &Vcb->NotifyList, NULL);
            }

            (void)Ext2FlushFiles(IrpContext, Vcb, FALSE);
            (void)Ext2FlushVolume(IrpContext, Vcb, FALSE);
            ExReleaseResourceLite(&Vcb->MainResource);

            Ext2PurgeVolume(Vcb, TRUE);
            Ext2CheckDismount(IrpContext, Vcb, TRUE);

            /* Allocation failure can leave the same volume mounted.
               Do not spin on it or dereference a possibly destroyed VCB. */
            ExAcquireResourceExclusiveLite(&Ext2Global->Resource, TRUE);
            GlobalDataResourceAcquired = TRUE;
            for (ListEntry = Ext2Global->VcbList.Flink;
                 ListEntry != &Ext2Global->VcbList; ListEntry = ListEntry->Flink) {
                PEXT2_VCB Current = CONTAINING_RECORD(ListEntry, EXT2_VCB, Next);
                if (Current == Vcb && IsMounted(Current) &&
                    !IsFlagOn(Current->Flags, VCB_DISMOUNT_PENDING)) {
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    __leave;
                }
            }
        }

        /* Wait for any asynchronous lazy writer activity to settle before checking VCBs */
        CcWaitForCurrentLazyWriterActivity();

        /*
         * Phase 3: reap any dismount-pending VCBs left behind, then confirm
         * nothing is still mounted before we unregister.
         */
        {
            if (!GlobalDataResourceAcquired) {
                ExAcquireResourceExclusiveLite(&Ext2Global->Resource, TRUE);
                GlobalDataResourceAcquired = TRUE;
            }

            while (!IsListEmpty(&Ext2Global->DismountingVcbList)) {

                PEXT2_VCB v = CONTAINING_RECORD(Ext2Global->DismountingVcbList.Flink, EXT2_VCB, Next);

                if (IsFlagOn(v->Flags, VCB_BEING_DROPPED)) {
                    break;
                }

                ExReleaseResourceLite(&Ext2Global->Resource);
                GlobalDataResourceAcquired = FALSE;

                Ext2DropIdleFcbs(v);

                if (v->ReferenceCount == 0) {
                    Ext2CheckDismount(IrpContext, v, FALSE);
                }

                ExAcquireResourceExclusiveLite(&Ext2Global->Resource, TRUE);
                GlobalDataResourceAcquired = TRUE;

                if (!IsListEmpty(&Ext2Global->DismountingVcbList) &&
                    CONTAINING_RECORD(Ext2Global->DismountingVcbList.Flink, EXT2_VCB, Next) == v) {
                    break;
                }
            }
        }

        if (!IsListEmpty(&(Ext2Global->VcbList)) ||
            !IsListEmpty(&(Ext2Global->DismountingVcbList))) {

            DEBUG(DL_ERR, ( "Ext2PrepareUnload: volumes still exist.\n"));

            Status = STATUS_ACCESS_DENIED;

            __leave;
        }

        /* a mount on its way has a device of its own (VolumeMount.c) */
        if (Ext2Global->MountsInFlight != 0 ||
            Ext2Global->VcbTeardownsInFlight != 0) {

            DEBUG(DL_ERR, ( "Ext2PrepareUnload: mount or teardown is in progress.\n"));

            Status = STATUS_ACCESS_DENIED;

            __leave;
        }

        /* Reclaim our published VPBs before surrendering unload control.
           An I/O-manager reference can outlive IRP_MJ_CLOSE; retaining
           ownership and retrying is safer than leaking Swap or freeing Old. */
        if (!Ext2ReclaimVpbs()) {
            Status = STATUS_DEVICE_BUSY;
            __leave;
        }

        /* DriverUnload is already wired up in DriverEntry when EXT2_UNLOAD is on;
           from here on every mount refuses (VolumeMount.c checks the flag under
           the global resource), the file systems are unregistered below */
        SetLongFlag(Ext2Global->Flags ,EXT2_UNLOAD_PENDING);
        Status = STATUS_SUCCESS;

        DEBUG(DL_INF, ( "Ext2PrepareToUnload: Driver is ready to unload.\n"));

    } __finally {

        if (GlobalDataResourceAcquired) {
            ExReleaseResourceLite(&Ext2Global->Resource);
        }

        /* Not under the global resource: IoUnregisterFileSystem takes the
           I/O manager's file system database exclusively, and a mount in
           flight holds it shared while it waits for the global resource
           (VolumeMount.c rechecks the unload state under it) - a deadlock that
           a slow mount, an MMP wait above all, makes easy to hit. */
        if (NT_SUCCESS(Status)) {
            IoUnregisterFileSystem(Ext2Global->DiskdevObject);
            IoUnregisterFileSystem(Ext2Global->CdromdevObject);
        }

        if (!IrpContext->ExceptionInProgress) {
            Ext2CompleteIrpContext(IrpContext, Status);
        }
    }

    return Status;
}

/* NULL unless the extension looks the way the kernel lays it out */
static PEXT2_DEVOBJ_EXTENSION
Ext2DevObjExtension(IN PDEVICE_OBJECT DeviceObject)
{
    PEXT2_DEVOBJ_EXTENSION Ext = (PEXT2_DEVOBJ_EXTENSION)DeviceObject->DeviceObjectExtension;

    if (Ext == NULL ||
        Ext->Type != IO_TYPE_DEVICE_OBJECT_EXTENSION ||
        Ext->DeviceObject != DeviceObject) {
        return NULL;
    }
    return Ext;
}

/* the primary signal: the mark the kernel itself tests on every open */
static BOOLEAN
Ext2IsUnloadPending(VOID)
{
    PEXT2_DEVOBJ_EXTENSION Ext;

    if (Ext2Global->UnloadFlagUsable &&
        (Ext = Ext2DevObjExtension(Ext2Global->DiskdevObject)) != NULL) {
        return FlagOn(Ext->ExtensionFlags, EXT2_DOE_UNLOAD_PENDING) ? TRUE : FALSE;
    }
    return Ext2IsUnloadPendingByOpen();
}

/* the fallback: the I/O manager refuses to open a device marked for
   unload - unless a filter above us answers first */
static BOOLEAN
Ext2IsUnloadPendingByOpen(VOID)
{
    OBJECT_ATTRIBUTES   oa;
    UNICODE_STRING      Name;
    IO_STATUS_BLOCK     Iosb;
    HANDLE              Handle = NULL;
    NTSTATUS            Status;

    RtlInitUnicodeString(&Name, DEVICE_NAME);
    InitializeObjectAttributes(&oa, &Name,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL, NULL);
    Status = ZwOpenFile(&Handle, FILE_READ_ATTRIBUTES, &oa, &Iosb,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, 0);
    if (NT_SUCCESS(Status)) {
        ZwClose(Handle);
        return FALSE;
    }

    return (Status == STATUS_NO_SUCH_DEVICE ||
            Status == STATUS_DELETE_PENDING) ? TRUE : FALSE;
}

/*
 * Run IOCTL_PREPARE_TO_UNLOAD against ourselves, on the calling thread.
 *
 * IoBuildDeviceIoControlRequest leaves the stack location without a file
 * object, and IoIsOperationSynchronous (used by Ext2AllocateIrpContext)
 * dereferences it unconditionally. The file object behind SelfHandle is
 * filled in, so the IRP looks exactly like a DeviceIoControl on \\.\ext4
 * from user mode. Tail.Overlay.OriginalFileObject stays NULL on purpose:
 * the I/O manager would dereference it on completion, and the reference
 * taken here is released here.
 */
static NTSTATUS
Ext2SendPrepareToUnload(VOID)
{
    KEVENT              Event;
    IO_STATUS_BLOCK     Iosb;
    PIRP                Irp;
    PFILE_OBJECT        FileObject = NULL;
    NTSTATUS            Status;

    Status = ObReferenceObjectByHandle(Ext2Global->SelfHandle,
                                       FILE_READ_ATTRIBUTES, *IoFileObjectType,
                                       KernelMode, (PVOID *)&FileObject, NULL);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoBuildDeviceIoControlRequest(IOCTL_PREPARE_TO_UNLOAD,
                                        Ext2Global->DiskdevObject,
                                        NULL, 0, NULL, 0, FALSE,
                                        &Event, &Iosb);
    if (Irp == NULL) {
        ObDereferenceObject(FileObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    IoGetNextIrpStackLocation(Irp)->FileObject = FileObject;

    Status = IoCallDriver(Ext2Global->DiskdevObject, Irp);
    if (Status == STATUS_PENDING) {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Iosb.Status;
    }

    ObDereferenceObject(FileObject);

    return Status;
}

/*
 * Delete both control devices (and the DOS name that points at one of
 * them). Runs once, from the drain thread or from DriverUnload, whichever
 * comes first. The pointer references DriverEntry took keep the device
 * objects readable until DriverUnload drops them; a device with open file
 * objects stays on the driver object, marked DOE_DELETE_PENDING, until the
 * last one is closed.
 */
VOID
Ext2DeleteControlDevices(VOID)
{
    UNICODE_STRING DosDeviceName;

    if (IsFlagOn(Ext2Global->Flags, EXT2_DEVICES_DELETED)) {
        return;
    }
    SetLongFlag(Ext2Global->Flags, EXT2_DEVICES_DELETED);

    /* the disk devices of unlocked LUKS volumes: their file systems are
       gone by now, the keys go with the devices */
    Ext4CryptTeardownAll();

    RtlInitUnicodeString(&DosDeviceName, DOS_DEVICE_NAME);
    IoDeleteSymbolicLink(&DosDeviceName);

    IoDeleteDevice(Ext2Global->CdromdevObject);
    IoDeleteDevice(Ext2Global->DiskdevObject);
}

/*
 * Called wherever a volume may just have become idle (last handle gone,
 * last reference gone, volume torn down). Wakes the drain thread if - and
 * only if - it is waiting for exactly that.
 */
VOID
Ext2UnloadKick(VOID)
{
    if (Ext2Global->UnloadState == EXT2_UNLOAD_DRAINING) {
        KeSetEvent(&Ext2Global->VolumeReleased, IO_NO_INCREMENT, FALSE);
    }
}

/* how soon to look again while the I/O manager finishes a close (see below) */
#define EXT2_UNLOAD_CLOSE_RECHECK   ((LONGLONG)10 * 10 * 1000)     /* 10 ms */

/*
 * TRUE while a dismounting volume waits for nothing but the I/O manager.
 * IRP_MJ_CLOSE sets VolumeReleased before it returns, and the I/O manager
 * drops the file object's Vpb reference only after that, in IopDeleteFile,
 * without calling the driver again. A retry woken by the close can still
 * see that reference; this recognises the window: the driver itself holds
 * nothing (no FCB, CCB or open file object: Vcb->ReferenceCount is 0) and
 * the Vpb counts more than the metadata stream accounts for.
 */
static BOOLEAN
Ext2IoManagerHoldsVolume(VOID)
{
    PLIST_ENTRY Link;
    BOOLEAN     Pending = FALSE;
    KIRQL       Irql;

    ExAcquireResourceSharedLite(&Ext2Global->Resource, TRUE);
    for (Link  = Ext2Global->DismountingVcbList.Flink;
         Link != &Ext2Global->DismountingVcbList && !Pending;
         Link  = Link->Flink) {

        PEXT2_VCB Vcb = CONTAINING_RECORD(Link, EXT2_VCB, Next);

        IoAcquireVpbSpinLock(&Irql);
        Pending = Vcb->ReferenceCount == 0 &&
                  Vcb->Vpb->ReferenceCount > (ULONG)(Vcb->Volume != NULL ? 1 : 0);
        IoReleaseVpbSpinLock(Irql);
    }
    ExReleaseResourceLite(&Ext2Global->Resource);

    /* A raw-volume reference can outlive the VCB entirely. The I/O
       manager releases it without a callback, just like a final close. */
    return Pending || Ext2VpbsPending();
}

/*
 * Who keeps the driver loaded? Printed by the drain thread while a volume
 * is still in use: per volume the counts that gate its teardown, and the
 * files holding it (handles or references). Only when the picture changed
 * since the last print, so a long wait costs one line per change. The
 * number of files listed is bounded; the counts always cover all of them.
 */
#define EXT2_UNLOAD_REPORT_FILES    8

static VOID
Ext2ReportVolumeHolders(IN OUT PLONGLONG LastPicture)
{
    PLIST_ENTRY VcbLink;
    LONGLONG    Picture = 0;
    ULONG       Pass;

    ExAcquireResourceSharedLite(&Ext2Global->Resource, TRUE);

    /* pass 0 computes a fingerprint of all counts, pass 1 prints */
    for (Pass = 0; Pass < 2; Pass++) {

        PLIST_ENTRY Lists[2] = { &Ext2Global->VcbList, &Ext2Global->DismountingVcbList };
        ULONG       l;

        if (Pass == 1 && Picture == *LastPicture) {
            break;
        }

        for (l = 0; l < 2; l++) {
            for (VcbLink = Lists[l]->Flink; VcbLink != Lists[l]; VcbLink = VcbLink->Flink) {

                PEXT2_VCB   Vcb = CONTAINING_RECORD(VcbLink, EXT2_VCB, Next);
                PLIST_ENTRY FcbLink;
                ULONG       Listed = 0, Holders = 0;

                if (Pass == 0) {
                    Picture = Picture * 31 + Vcb->ReferenceCount;
                    Picture = Picture * 31 + Vcb->OpenHandleCount;
                    Picture = Picture * 31 + (LONG)Vcb->Vpb->ReferenceCount;
                    continue;
                }

                DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                           "ext4: %s volume %p: references %d, handles %d, "
                           "Vpb references %u, names cached %d\n",
                           l ? "dismounting" : "mounted", Vcb, Vcb->ReferenceCount,
                           Vcb->OpenHandleCount, Vcb->Vpb->ReferenceCount, Vcb->NumOfMcb);

                ExAcquireResourceSharedLite(&Vcb->FcbLock, TRUE);
                for (FcbLink = Vcb->FcbList.Flink; FcbLink != &Vcb->FcbList;
                     FcbLink = FcbLink->Flink) {

                    PEXT2_FCB Fcb = CONTAINING_RECORD(FcbLink, EXT2_FCB, Next);

                    if (Fcb->OpenHandleCount == 0 && Fcb->ReferenceCount == 0) {
                        continue;
                    }
                    Holders++;
                    if (Listed < EXT2_UNLOAD_REPORT_FILES) {
                        Listed++;
                        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                                   "ext4:   %wZ handles %d references %d file objects %d flags %x data %p cache %p image %p\n",
                                   Fcb->Mcb ? &Fcb->Mcb->FullName : NULL,
                                   Fcb->OpenHandleCount, Fcb->ReferenceCount, Fcb->CcbCount, Fcb->Flags,
                                   Fcb->SectionObject.DataSectionObject,
                                   Fcb->SectionObject.SharedCacheMap,
                                   Fcb->SectionObject.ImageSectionObject);

                        /* the file objects still without their close; a
                           busy Fcb is skipped rather than waited for */
                        if (ExAcquireResourceSharedLite(&Fcb->MainResource, FALSE)) {
                            PLIST_ENTRY CcbLink;
                            for (CcbLink = Fcb->CcbList.Flink; CcbLink != &Fcb->CcbList;
                                 CcbLink = CcbLink->Flink) {
                                PEXT2_CCB Ccb = CONTAINING_RECORD(CcbLink, EXT2_CCB, FcbLink);
                                DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                                           "ext4:     file object %p flags %x ccb flags %x\n",
                                           Ccb->FileObject, Ccb->FileObject->Flags, Ccb->Flags);
                            }
                            ExReleaseResourceLite(&Fcb->MainResource);
                        }
                    }
                }
                ExReleaseResourceLite(&Vcb->FcbLock);

                if (Holders > Listed) {
                    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                               "ext4:   ... and %u more\n", Holders - Listed);
                }
            }
        }
    }

    *LastPicture = Picture;
    ExReleaseResourceLite(&Ext2Global->Resource);
}

/*
 * Where an unload has got to, as a DWORD in the service key (UnloadStep,
 * and UnloadWaitStatus while volumes keep it waiting). A stop that hangs is
 * located with reg query from outside: no debugger, no dump - a stuck
 * kernel cannot be asked otherwise without risk to the machine reading it.
 * PASSIVE_LEVEL; a failure to write is of no consequence.
 */
VOID
Ext2UnloadStep(IN PCWSTR Name, IN ULONG Value)
{
    WCHAR           Key[EXT2_UNLOAD_KEY_CHARS];
    UNICODE_STRING  Path;
    USHORT          Suffix = (USHORT)(sizeof(VOLUMES_KEY) - sizeof(WCHAR));

    if (Ext2Global == NULL || Ext2Global->RegistryPath.Buffer == NULL ||
        Ext2Global->RegistryPath.Length <= Suffix) {
        return;
    }
    /* <service>\Volumes without the \Volumes: the service key */
    RtlInitEmptyUnicodeString(&Path, Key, sizeof(Key) - sizeof(WCHAR));
    Path.Length = min((USHORT)(Ext2Global->RegistryPath.Length - Suffix), Path.MaximumLength);
    RtlCopyMemory(Key, Ext2Global->RegistryPath.Buffer, Path.Length);
    Key[Path.Length / sizeof(WCHAR)] = UNICODE_NULL;

    RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, Key, Name, REG_DWORD, &Value, sizeof(Value));
}

/* the drain thread: from "marked" to "last reference handed over" */
static VOID
Ext2UnloadDrainThread(IN PVOID Context)
{
    PFILE_OBJECT    FileObject = NULL;
    NTSTATUS        Status;
    LONGLONG        Reported = 0;

    UNREFERENCED_PARAMETER(Context);

    Ext2UnloadStep(L"UnloadStep", EXT2_STEP_DRAIN);

    /* step 1: flush, dismount idle volumes, unregister the file systems.
       A busy volume keeps the driver loaded until its handles go away;
       Ext2UnloadKick wakes us the moment that happens. */
    while (!IsFlagOn(Ext2Global->Flags, EXT2_UNLOAD_PENDING)) {

        Status = Ext2SendPrepareToUnload();
        if (NT_SUCCESS(Status)) {
            break;
        }

        DbgPrint("ext4: unload requested, a volume is still in use "
                 "(%xh), waiting for it to be released\n", Status);
        Ext2UnloadStep(L"UnloadStep", EXT2_STEP_VOLUMES_BUSY);
        Ext2UnloadStep(L"UnloadWaitStatus", (ULONG)Status);
        Ext2ReportVolumeHolders(&Reported);

        /* The reaper frees the FCBs nobody references; the last one of a
           dismounted volume destroys it. Cleanup, close and Ext2DestroyVcb
           set VolumeReleased on every transition the driver takes part
           in, and a synchronization event keeps a signal that arrives
           before the wait. The one transition it does not see is the I/O
           manager dropping a Vpb reference after a close has returned: only
           in that window is the next look scheduled, 10 ms later. */
        Ext2ReaperKick(&Ext2Global->FcbReaper, TRUE);
        {
            LARGE_INTEGER Recheck;

            Recheck.QuadPart = -EXT2_UNLOAD_CLOSE_RECHECK;
            KeWaitForSingleObject(&Ext2Global->VolumeReleased, Executive, KernelMode, FALSE,
                                  (Status == STATUS_INSUFFICIENT_RESOURCES ||
                                   Ext2IoManagerHoldsVolume()) ? &Recheck : NULL);
        }
    }

    DbgPrint("ext4: unload requested, file systems unregistered, "
             "removing the control devices\n");

    /* step 2: no device object may be left on the driver object */
    Ext2UnloadStep(L"UnloadStep", EXT2_STEP_CONTROL_DEVICES);
    Ext2DeleteControlDevices();

    /* step 3: closing the handle only drops the handle count, the extra
       reference keeps the file object alive; the deferred dereference lets
       a system worker thread delete it, remove the last device object and
       run DriverUnload - on a stack with no driver code on it */
    Status = ObReferenceObjectByHandle(Ext2Global->SelfHandle, 0,
                                       *IoFileObjectType, KernelMode,
                                       (PVOID *)&FileObject, NULL);
    ZwClose(Ext2Global->SelfHandle);
    Ext2Global->SelfHandle = NULL;

    if (NT_SUCCESS(Status)) {
        DbgPrint("ext4: unload requested, handing the last reference "
                 "to a system worker thread\n");
        Ext2UnloadStep(L"UnloadStep", EXT2_STEP_HANDED_OVER);
        ObDereferenceObjectDeferDelete(FileObject);
    } else {
        DbgPrint("ext4: unload requested, cannot reference own file "
                 "object (%xh)\n", Status);
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

/*
 * The probe: PASSIVE_LEVEL, on a system worker thread. The rundown
 * protection was acquired by the timer callback before the item was
 * queued, so "queued but not yet running" is covered as well; it is
 * released here as the very last thing.
 */
static VOID
Ext2UnloadProbe(IN PDEVICE_OBJECT DeviceObject, IN PVOID Context)
{
    OBJECT_ATTRIBUTES   oa;
    HANDLE              Handle;
    NTSTATUS            Status;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Context);

    if (!Ext2IsUnloadPending()) {
        InterlockedExchange(&Ext2Global->UnloadState, EXT2_UNLOAD_IDLE);
        ExReleaseRundownProtection(&Ext2Global->UnloadRundown);
        return;
    }

    /* seen: no more probes, hand over to the drain thread */
    InterlockedExchange(&Ext2Global->UnloadState, EXT2_UNLOAD_DRAINING);
    KeSetEvent(&Ext2Global->UnloadStarted, IO_NO_INCREMENT, FALSE);
    ExCancelTimer(Ext2Global->UnloadTimer, NULL);

    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = PsCreateSystemThread(&Handle, THREAD_ALL_ACCESS, &oa, NULL, NULL,
                                  Ext2UnloadDrainThread, NULL);
    if (NT_SUCCESS(Status)) {
        ObReferenceObjectByHandle(Handle, THREAD_ALL_ACCESS, *PsThreadType,
                                  KernelMode, (PVOID *)&Ext2Global->UnloadThread,
                                  NULL);
        ZwClose(Handle);
    } else {
        DbgPrint("ext4: unload requested, cannot start the drain thread "
                 "(%xh)\n", Status);
    }

    ExReleaseRundownProtection(&Ext2Global->UnloadRundown);
}

/* the timer: DISPATCH_LEVEL, checks flag directly if usable, queues probe on unload */
static VOID
Ext2UnloadTimerCallback(IN PEX_TIMER Timer, IN PVOID Context)
{
    UNREFERENCED_PARAMETER(Timer);
    UNREFERENCED_PARAMETER(Context);

    /*
     * Fast path at DISPATCH_LEVEL: when kernel extension flags are usable,
     * read DOE_UNLOAD_PENDING directly from non-paged memory.
     * If unload is NOT pending, return immediately without queuing any work
     * items or waking worker threads (practically zero CPU overhead).
     */
    if (Ext2Global->UnloadFlagUsable) {
        PEXT2_DEVOBJ_EXTENSION Ext = Ext2DevObjExtension(Ext2Global->DiskdevObject);
        if (Ext == NULL || !FlagOn(Ext->ExtensionFlags, EXT2_DOE_UNLOAD_PENDING)) {
            return;
        }
    }

    if (InterlockedCompareExchange(&Ext2Global->UnloadState,
                                   EXT2_UNLOAD_PROBING,
                                   EXT2_UNLOAD_IDLE) != EXT2_UNLOAD_IDLE) {
        return;                     /* a probe is already in flight */
    }

    if (!ExAcquireRundownProtection(&Ext2Global->UnloadRundown)) {
        return;                     /* DriverUnload is already running */
    }

    IoQueueWorkItem(Ext2Global->UnloadWorkItem, Ext2UnloadProbe,
                    CriticalWorkQueue, NULL);
}

VOID
Ext2StartUnloadWatch(VOID)
{
    OBJECT_ATTRIBUTES   oa;
    UNICODE_STRING      Name;
    IO_STATUS_BLOCK     Iosb;
    HANDLE              Handle = NULL;
    EXT_SET_PARAMETERS  Parameters;
    NTSTATUS            Status;

    Ext2Global->SelfHandle = NULL;
    Ext2Global->UnloadThread = NULL;
    Ext2Global->UnloadState = EXT2_UNLOAD_IDLE;
    KeInitializeEvent(&Ext2Global->VolumeReleased, SynchronizationEvent, FALSE);
    ExInitializeRundownProtection(&Ext2Global->UnloadRundown);

    /* a freshly created device carries none of the pending bits; if the
       extension does not look right, only the open fallback is trusted */
    {
        PEXT2_DEVOBJ_EXTENSION Ext = Ext2DevObjExtension(Ext2Global->DiskdevObject);
        Ext2Global->UnloadFlagUsable = (Ext && !(Ext->ExtensionFlags & 0x1F)) ? TRUE : FALSE;
    }

    /* our own handle on the control device: releasing it later is what
       hands the last reference back to the I/O manager */
    RtlInitUnicodeString(&Name, DEVICE_NAME);
    InitializeObjectAttributes(&oa, &Name,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL, NULL);
    Status = ZwCreateFile(&Handle, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &oa, &Iosb,
                          NULL, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          FILE_OPEN, 0, NULL, 0);
    if (!NT_SUCCESS(Status)) {
        DbgPrint("ext4: cannot open own control device (%xh); "
                 "a bare 'sc stop' will not unload the driver\n", Status);
        return;
    }
    Ext2Global->SelfHandle = Handle;

    Ext2Global->UnloadWorkItem = IoAllocateWorkItem(Ext2Global->DiskdevObject);
    Ext2Global->UnloadTimer = ExAllocateTimer(Ext2UnloadTimerCallback, NULL, 0);
    if (Ext2Global->UnloadWorkItem == NULL || Ext2Global->UnloadTimer == NULL) {
        DbgPrint("ext4: cannot set up the unload watch; "
                 "a bare 'sc stop' will not unload the driver\n");
        Ext2StopUnloadWatch();
        return;
    }

    ExInitializeSetTimerParameters(&Parameters);
    Parameters.NoWakeTolerance = 0;
    ExSetTimer(Ext2Global->UnloadTimer, -EXT2_UNLOAD_PROBE_PERIOD,
               EXT2_UNLOAD_PROBE_PERIOD, &Parameters);

    DbgPrint("ext4: unload watch armed (%s)\n",
             Ext2Global->UnloadFlagUsable ? "device flag, open as fallback"
                                          : "open only");
}

/*
 * Called first thing in DriverUnload: every thread that can execute the
 * code above is waited for here, in the order it can still be running.
 */
VOID
Ext2StopUnloadWatch(VOID)
{
    if (Ext2Global->UnloadTimer) {
        ExDeleteTimer(Ext2Global->UnloadTimer, TRUE, TRUE, NULL);
        Ext2Global->UnloadTimer = NULL;
    }

    ExWaitForRundownProtectionRelease(&Ext2Global->UnloadRundown);

    if (Ext2Global->UnloadWorkItem) {
        IoFreeWorkItem(Ext2Global->UnloadWorkItem);
        Ext2Global->UnloadWorkItem = NULL;
    }

    if (Ext2Global->UnloadThread) {
        KeWaitForSingleObject(Ext2Global->UnloadThread, Executive, KernelMode,
                              FALSE, NULL);
        ObDereferenceObject(Ext2Global->UnloadThread);
        Ext2Global->UnloadThread = NULL;
    }

    /* still open only when the watch never ran: unload came another way */
    if (Ext2Global->SelfHandle) {
        ZwClose(Ext2Global->SelfHandle);
        Ext2Global->SelfHandle = NULL;
    }
}
