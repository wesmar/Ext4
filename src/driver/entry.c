/**
 * entry.c - driver entry and exit: global state, device objects, dispatch table, file system registration.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include <wdmsec.h>

PEXT2_GLOBAL    Ext2Global   = NULL;

/*
 *   Ext2Fsd version, building date/time
 */

CHAR            gVersion[]   = EXT4_VERSION_STRING;
CHAR            gTime[] = __TIME__;
CHAR            gDate[] = __DATE__;

NTSTATUS
DriverEntry(
    IN PDRIVER_OBJECT DriverObject,
    IN PUNICODE_STRING RegistryPath   );

DECLARE_INIT(journal_init);
DECLARE_EXIT(journal_exit);

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, DriverUnload)
#endif

/*
 * FUNCTION: Called by the system to unload the driver
 * ARGUMENTS:
 *           DriverObject = object describing this driver
 * RETURNS:  None
 */

VOID
DriverUnload (IN PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);


    DEBUG(DL_FUN, ( "ext4: Unloading routine.\n"));
    DbgPrint("ext4: DriverUnload\n");
    Ext2UnloadStep(L"UnloadStep", EXT2_STEP_DRIVER_UNLOAD);

    /*
     *  Stop the volume watch, the unload watch and the reaper threads
     *  first: they walk global lists and touch Ext2Global, so they must be
     *  gone before we tear any of it down.
     */
    Ext2StopLetterService();
    Ext2UnloadStep(L"UnloadStep", EXT2_STEP_LETTERS_STOPPED);
    Ext2StopUnloadWatch();
    Ext2UnloadStep(L"UnloadStep", EXT2_STEP_WATCH_STOPPED);
    Ext2StopReaper(&Ext2Global->FcbReaper);
    Ext2StopReaper(&Ext2Global->McbReaper);
    Ext2StopReaper(&Ext2Global->bhReaper);
    Ext2UnloadStep(L"UnloadStep", EXT2_STEP_REAPERS_STOPPED);
    if (Ext2Global->LowMemoryHandle) {
        ZwClose(Ext2Global->LowMemoryHandle);
        Ext2Global->LowMemoryHandle = NULL;
        Ext2Global->LowMemory = NULL;
    }

    /*
     *  Unregister the file systems unless a prepare-to-unload IOCTL has
     *  already done so (that path sets EXT2_UNLOAD_PENDING before it runs).
     */
    if (!IsFlagOn(Ext2Global->Flags, EXT2_UNLOAD_PENDING)) {
        IoUnregisterFileSystem(Ext2Global->DiskdevObject);
        IoUnregisterFileSystem(Ext2Global->CdromdevObject);
    }

    /*
     *  Remove the DOS name and both control devices, unless the unload
     *  watch has already done it (a file system driver is only unloaded
     *  once its device list is empty, so that is what triggers this call).
     */
    Ext2DeleteControlDevices();

    /* Prepare-to-unload returned every published VPB before handing off. */
    ASSERT(!Ext2VpbsPending());

    Ext2UnloadAllNls();
    Ext2UnloadStep(L"UnloadStep", EXT2_STEP_UNLOADED);

    ExDeleteResourceLite(&Ext2Global->Resource);

    ExDeleteNPagedLookasideList(&(Ext2Global->Ext2DentryLookasideList));
    ExDeleteNPagedLookasideList(&(Ext2Global->Ext2ExtLookasideList));
    ExDeleteNPagedLookasideList(&(Ext2Global->Ext2McbLookasideList));
    ExDeleteNPagedLookasideList(&(Ext2Global->Ext2CcbLookasideList));
    ExDeleteNPagedLookasideList(&(Ext2Global->Ext2FcbLookasideList));
    ExDeleteNPagedLookasideList(&(Ext2Global->Ext2IrpContextLookasideList));

    /*
     *  Drop the extra reference DriverEntry took on each control device;
     *  the device objects are freed once that last reference goes away.
     */
    ObDereferenceObject(Ext2Global->DiskdevObject);
    ObDereferenceObject(Ext2Global->CdromdevObject);

    /* cleanup journal engine and journal related caches */
    Ext2JournalGlobalExit();
    UNLOAD_MODULE(journal_exit);

    /* cleanup linux lib */
    ext2_destroy_linux();

    if (Ext2Global->RegistryPath.Buffer) {
        Ext2FreePool(Ext2Global->RegistryPath.Buffer, EXT4_TAG_GLOBAL);
    }
    Ext2FreePool(Ext2Global, EXT4_TAG_GLOBAL);
    Ext2Global = NULL;
}

VOID
Ext2EresourceAlignmentChecking()
{
    /* Verify ERESOURCE alignment in structures */
    CL_ASSERT((FIELD_OFFSET(EXT2_GLOBAL, Resource) & 7) == 0);
    CL_ASSERT((FIELD_OFFSET(EXT2_VCB, MainResource) & 7) == 0);
    CL_ASSERT((FIELD_OFFSET(EXT2_VCB, PagingIoResource) & 7) == 0);
    CL_ASSERT((FIELD_OFFSET(EXT2_VCB, SuperLock) & 7) == 0);
    CL_ASSERT((FIELD_OFFSET(EXT2_VCB, LinkLock) & 7) == 0);
    CL_ASSERT((FIELD_OFFSET(EXT2_VCB, FcbLock) & 7) == 0);
    CL_ASSERT((FIELD_OFFSET(EXT2_VCB, bd.bd_bh_lock) & 7) == 0);
    CL_ASSERT((FIELD_OFFSET(EXT2_VCB, sbi.s_gd_lock) & 7) == 0);
    CL_ASSERT((FIELD_OFFSET(EXT2_FCBVCB, MainResource) & 7) == 0);
    CL_ASSERT((FIELD_OFFSET(EXT2_FCBVCB, PagingIoResource) & 7) == 0);
    CL_ASSERT((FIELD_OFFSET(EXT2_FCB, MainResource) & 7) == 0);
    CL_ASSERT((FIELD_OFFSET(EXT2_FCB, PagingIoResource) & 7) == 0);
    CL_ASSERT((FIELD_OFFSET(EXT2_ICB, DirResource) & 7) == 0);
    CL_ASSERT((FIELD_OFFSET(EXT2_ICB, EntryResource) & 7) == 0);
}

/*
 * NAME: DriverEntry
 * FUNCTION: Called by the system to initialize the driver
 *
 * ARGUMENTS:
 *           DriverObject = object describing this driver
 *           RegistryPath = path to our configuration entries
 * RETURNS: Success or failure
 */
NTSTATUS
DriverEntry (
    IN PDRIVER_OBJECT   DriverObject,
    IN PUNICODE_STRING  RegistryPath
)
{
    PDEVICE_OBJECT              DiskdevObject = NULL;
    PDEVICE_OBJECT              CdromdevObject = NULL;
    UNICODE_STRING              DeviceName;
    UNICODE_STRING              DosDeviceName;

    PFAST_IO_DISPATCH           FastIoDispatch;
    PCACHE_MANAGER_CALLBACKS    CacheManagerCallbacks;

    NTSTATUS                    Status;

    int                         rc = 0;
    BOOLEAN                     linux_lib_inited = FALSE;
    BOOLEAN                     journal_module_inited = FALSE;

    /* the on-disk superblock is overlaid on this structure: its size and the magic offset are fixed by the format */
    ASSERT(sizeof(EXT2_SUPER_BLOCK) == 1024);
    ASSERT(FIELD_OFFSET(EXT2_SUPER_BLOCK, s_magic) == 56);

    DbgPrint(
        "ext4 -- version "
        EXT4_VERSION_STRING
#if EXT2_DEBUG
        " Debug"
#else
        " Release"
#endif
        " -- "
        __DATE__ " "
        __TIME__ ".\n");

    DEBUG(DL_FUN, ( "Ext2 DriverEntry ...\n"));

    /* initialize winlib structures */
    if (ext2_init_linux()) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto errorout;
    }
    linux_lib_inited = TRUE;

    /* initialize journal module structures */
    LOAD_MODULE(journal_init);
    if (rc != 0) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto errorout;
    }
    journal_module_inited = TRUE;

    /* per-thread handle registry of the write-ahead journal engine */
    Ext2JournalGlobalInit();

    /* allocate memory for Ext2Global */
    Ext2Global = Ext2AllocatePool(NonPagedPool, sizeof(EXT2_GLOBAL), EXT4_TAG_GLOBAL);
    if (!Ext2Global) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto errorout;
    }

    /* initialize Ext2Global */
    RtlZeroMemory(Ext2Global, sizeof(EXT2_GLOBAL));
    Ext2Global->Identifier.Type = EXT2FGD;
    Ext2Global->Identifier.Size = sizeof(EXT2_GLOBAL);

    InitializeListHead(&(Ext2Global->VcbList));
    InitializeListHead(&(Ext2Global->DismountingVcbList));
    InitializeListHead(&(Ext2Global->SwapVpbList));
    KeInitializeSpinLock(&Ext2Global->SwapVpbLock);
    Ext4CryptInitialize();
    ExInitializeResourceLite(&(Ext2Global->Resource));
    /* before the file systems are registered: a mount may wait on it */
    KeInitializeEvent(&Ext2Global->UnloadStarted, NotificationEvent, FALSE);

    /* memory pressure makes the reapers give back everything idle; without
       the event they still trim by deadlines */
    {
        UNICODE_STRING LowMemoryName;

        RtlInitUnicodeString(&LowMemoryName, L"\\KernelObjects\\LowMemoryCondition");
        Ext2Global->LowMemory = IoCreateNotificationEvent(&LowMemoryName,
                                                          &Ext2Global->LowMemoryHandle);
    }

    /* query registry settings */
    Ext2QueryRegistrySettings(RegistryPath);
    Ext2UnloadStep(L"UnloadStep", EXT2_STEP_LOADED);

    /* The control devices take the driver's own IOCTLs (volume properties,
       mount points, statistics): SYSTEM and administrators only, enforced
       on every open below them too (FILE_DEVICE_SECURE_OPEN). Created with
       the default security, any user could open \\.\ext4 and send them. */

    /* control device for CD-ROM media: the I/O manager offers optical volumes only to CD file systems */
    RtlInitUnicodeString(&DeviceName, CDROM_NAME);
    Status = IoCreateDeviceSecure(
                 DriverObject,
                 0,
                 &DeviceName,
                 FILE_DEVICE_CD_ROM_FILE_SYSTEM,
                 FILE_DEVICE_SECURE_OPEN,
                 FALSE,
                 &SDDL_DEVOBJ_SYS_ALL_ADM_ALL,
                 NULL,
                 &CdromdevObject );

    if (!NT_SUCCESS(Status)) {
        DEBUG(DL_ERR, ( "IoCreateDevice cdrom device object error.\n"));
        goto errorout;
    }

    /* control device for disk media, \ext4: the one mounts are requested through */
    RtlInitUnicodeString(&DeviceName, DEVICE_NAME);
    Status = IoCreateDeviceSecure(
                 DriverObject,
                 0,
                 &DeviceName,
                 FILE_DEVICE_DISK_FILE_SYSTEM,
                 FILE_DEVICE_SECURE_OPEN,
                 FALSE,
                 &SDDL_DEVOBJ_SYS_ALL_ADM_ALL,
                 NULL,
                 &DiskdevObject );

    if (!NT_SUCCESS(Status)) {
        DEBUG(DL_ERR, ( "IoCreateDevice disk device object error.\n"));
        goto errorout;
    }

    Status= Ext2StartReaper(
                &Ext2Global->FcbReaper,
                Ext2FcbReaperThread);
    if (!NT_SUCCESS(Status)) {
        goto errorout;
    }

    /* start resource reaper thread */
    Status= Ext2StartReaper(
                &Ext2Global->McbReaper,
                Ext2McbReaperThread);
    if (!NT_SUCCESS(Status)) {
        Ext2StopReaper(&Ext2Global->FcbReaper);
        goto errorout;
    }

    Status= Ext2StartReaper(
                &Ext2Global->bhReaper,
                Ext2bhReaperThread);
    if (!NT_SUCCESS(Status)) {
        Ext2StopReaper(&Ext2Global->FcbReaper);
        Ext2StopReaper(&Ext2Global->McbReaper);
        goto errorout;
    }


    /* initializing */
    Ext2Global->DiskdevObject  = DiskdevObject;
    Ext2Global->CdromdevObject = CdromdevObject;

    DriverObject->MajorFunction[IRP_MJ_CREATE]              = Ext2BuildRequest;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]               = Ext2BuildRequest;
    DriverObject->MajorFunction[IRP_MJ_READ]                = Ext2BuildRequest;
    DriverObject->MajorFunction[IRP_MJ_WRITE]               = Ext2BuildRequest;

    DriverObject->MajorFunction[IRP_MJ_FLUSH_BUFFERS]       = Ext2BuildRequest;
    DriverObject->MajorFunction[IRP_MJ_SHUTDOWN]            = Ext2BuildRequest;

    DriverObject->MajorFunction[IRP_MJ_QUERY_INFORMATION]   = Ext2BuildRequest;
    DriverObject->MajorFunction[IRP_MJ_SET_INFORMATION]     = Ext2BuildRequest;

    DriverObject->MajorFunction[IRP_MJ_QUERY_VOLUME_INFORMATION]    = Ext2BuildRequest;
    DriverObject->MajorFunction[IRP_MJ_SET_VOLUME_INFORMATION]      = Ext2BuildRequest;

    DriverObject->MajorFunction[IRP_MJ_DIRECTORY_CONTROL]   = Ext2BuildRequest;
    DriverObject->MajorFunction[IRP_MJ_FILE_SYSTEM_CONTROL] = Ext2BuildRequest;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL]      = Ext2BuildRequest;
    DriverObject->MajorFunction[IRP_MJ_LOCK_CONTROL]        = Ext2BuildRequest;

    DriverObject->MajorFunction[IRP_MJ_QUERY_EA] = Ext2BuildRequest;
    DriverObject->MajorFunction[IRP_MJ_SET_EA] = Ext2BuildRequest;

    DriverObject->MajorFunction[IRP_MJ_CLEANUP]             = Ext2BuildRequest;

    DriverObject->MajorFunction[IRP_MJ_PNP]                 = Ext2BuildRequest;

    DriverObject->DriverUnload                              = DriverUnload;

    /* Initialize the fast I/O entry points */

    FastIoDispatch = &(Ext2Global->FastIoDispatch);

    FastIoDispatch->SizeOfFastIoDispatch          = sizeof(FAST_IO_DISPATCH);
    FastIoDispatch->FastIoCheckIfPossible         = Ext2FastIoCheckIfPossible;
    FastIoDispatch->FastIoRead                    = Ext2FastIoRead;
    FastIoDispatch->FastIoWrite                   = Ext2FastIoWrite;
    FastIoDispatch->FastIoQueryBasicInfo          = Ext2FastIoQueryBasicInfo;
    FastIoDispatch->FastIoQueryStandardInfo       = Ext2FastIoQueryStandardInfo;
    FastIoDispatch->FastIoLock                    = Ext2FastIoLock;
    FastIoDispatch->FastIoUnlockSingle            = Ext2FastIoUnlockSingle;
    FastIoDispatch->FastIoUnlockAll               = Ext2FastIoUnlockAll;
    FastIoDispatch->FastIoUnlockAllByKey          = Ext2FastIoUnlockAllByKey;
    FastIoDispatch->FastIoQueryNetworkOpenInfo    = Ext2FastIoQueryNetworkOpenInfo;

    FastIoDispatch->AcquireForModWrite            = Ext2AcquireFileForModWrite;
    FastIoDispatch->ReleaseForModWrite            = Ext2ReleaseFileForModWrite;
    FastIoDispatch->AcquireForCcFlush             = Ext2AcquireFileForCcFlush;
    FastIoDispatch->ReleaseForCcFlush             = Ext2ReleaseFileForCcFlush;
    FastIoDispatch->AcquireFileForNtCreateSection = Ext2AcquireForCreateSection;
    FastIoDispatch->ReleaseFileForNtCreateSection = Ext2ReleaseForCreateSection;

    DriverObject->FastIoDispatch = FastIoDispatch;

    /* initializing structure sizes for statistics
       1 means flexible/not fixed for all allocations (for different volumes). */
    Ext2Global->PerfStat.Magic   = EXT2_PERF_STAT_MAGIC;
    Ext2Global->PerfStat.Version = EXT2_PERF_STAT_VER2;
    Ext2Global->PerfStat.Length  = sizeof(EXT2_PERF_STATISTICS_V2);

    Ext2Global->PerfStat.Unit.Slot[PS_IRP_CONTEXT] = sizeof(EXT2_IRP_CONTEXT);  /* 0 */
    Ext2Global->PerfStat.Unit.Slot[PS_VCB] = sizeof(EXT2_VCB);                  /* 1 */
    Ext2Global->PerfStat.Unit.Slot[PS_FCB] = sizeof(EXT2_FCB);                  /* 2 */
    Ext2Global->PerfStat.Unit.Slot[PS_CCB] = sizeof(EXT2_CCB);                  /* 3 */
    Ext2Global->PerfStat.Unit.Slot[PS_MCB] = sizeof(EXT2_MCB);                  /* 4 */
    Ext2Global->PerfStat.Unit.Slot[PS_EXTENT] = sizeof(EXT2_EXTENT);            /* 5 */
    Ext2Global->PerfStat.Unit.Slot[PS_RW_CONTEXT] = sizeof(EXT2_RW_CONTEXT);    /* 6 */
    Ext2Global->PerfStat.Unit.Slot[PS_VPB] = sizeof(VPB);                       /* 7 */
    Ext2Global->PerfStat.Unit.Slot[PS_FILE_NAME] = 1;                           /* 8 */
    Ext2Global->PerfStat.Unit.Slot[PS_MCB_NAME] = 1;                            /* 9 */
    Ext2Global->PerfStat.Unit.Slot[PS_INODE_NAME] = 1;                          /* a */
    Ext2Global->PerfStat.Unit.Slot[PS_DIR_ENTRY] = sizeof(EXT2_DIR_ENTRY2);     /* b */
    Ext2Global->PerfStat.Unit.Slot[PS_DIR_PATTERN] = 1;                         /* c */
    Ext2Global->PerfStat.Unit.Slot[PS_DISK_EVENT] = sizeof(KEVENT);             /* d */
    Ext2Global->PerfStat.Unit.Slot[PS_DISK_BUFFER] = 1;                         /* e */
    Ext2Global->PerfStat.Unit.Slot[PS_BLOCK_DATA] = 1;                          /* f */
    Ext2Global->PerfStat.Unit.Slot[PS_EXT2_INODE] = 1;                          /* 10 */
    Ext2Global->PerfStat.Unit.Slot[PS_DENTRY] = sizeof(struct dentry);          /* 11 */
    Ext2Global->PerfStat.Unit.Slot[PS_BUFF_HEAD] = sizeof(struct buffer_head);  /* 12 */
    Ext2Global->PerfStat.Unit.Slot[PS_ICB] = sizeof(EXT2_ICB);                  /* 13 */

    switch ( MmQuerySystemSize() ) {

    case MmSmallSystem:

        Ext2Global->MaxDepth = 64;
        break;

    case MmMediumSystem:

        Ext2Global->MaxDepth = 128;
        break;

    case MmLargeSystem:
    default:

        Ext2Global->MaxDepth = 256;
        break;
    }

    /* Initialize the Cache Manager callbacks */

    CacheManagerCallbacks = &(Ext2Global->CacheManagerCallbacks);
    CacheManagerCallbacks->AcquireForLazyWrite  = Ext2AcquireForLazyWrite;
    CacheManagerCallbacks->ReleaseFromLazyWrite = Ext2ReleaseFromLazyWrite;
    CacheManagerCallbacks->AcquireForReadAhead  = Ext2AcquireForReadAhead;
    CacheManagerCallbacks->ReleaseFromReadAhead = Ext2ReleaseFromReadAhead;

    Ext2Global->CacheManagerNoOpCallbacks.AcquireForLazyWrite  = Ext2NoOpAcquire;
    Ext2Global->CacheManagerNoOpCallbacks.ReleaseFromLazyWrite = Ext2NoOpRelease;
    Ext2Global->CacheManagerNoOpCallbacks.AcquireForReadAhead  = Ext2NoOpAcquire;
    Ext2Global->CacheManagerNoOpCallbacks.ReleaseFromReadAhead = Ext2NoOpRelease;

    /* Initialize FS Filter callbacks */

    RtlZeroMemory(&Ext2Global->FilterCallbacks,  sizeof(FS_FILTER_CALLBACKS));
    Ext2Global->FilterCallbacks.SizeOfFsFilterCallbacks = sizeof(FS_FILTER_CALLBACKS);
    Ext2Global->FilterCallbacks.PreAcquireForSectionSynchronization = Ext2PreAcquireForCreateSection;
    FsRtlRegisterFileSystemFilterCallbacks(DriverObject,  &Ext2Global->FilterCallbacks );


    /* Initialize the global data */

    ExInitializeNPagedLookasideList( &(Ext2Global->Ext2IrpContextLookasideList),
                                     NULL,
                                     NULL,
                                     0,
                                     sizeof(EXT2_IRP_CONTEXT),
                                     'PRIE',
                                     0 );

    ExInitializeNPagedLookasideList( &(Ext2Global->Ext2FcbLookasideList),
                                     NULL,
                                     NULL,
                                     0,
                                     sizeof(EXT2_FCB),
                                     'BCFE',
                                     0 );

    ExInitializeNPagedLookasideList( &(Ext2Global->Ext2CcbLookasideList),
                                    NULL,
                                    NULL,
                                    0,
                                    sizeof(EXT2_CCB),
                                    'BCCE',
                                    0 );

    ExInitializeNPagedLookasideList( &(Ext2Global->Ext2McbLookasideList),
                                    NULL,
                                    NULL,
                                    0,
                                    sizeof(EXT2_MCB),
                                    'BCME',
                                    0 );

    ExInitializeNPagedLookasideList( &(Ext2Global->Ext2ExtLookasideList),
                                    NULL,
                                    NULL,
                                    0,
                                    sizeof(EXT2_EXTENT),
                                    'STXE',
                                    0 );

    ExInitializeNPagedLookasideList( &(Ext2Global->Ext2DentryLookasideList),
                                    NULL,
                                    NULL,
                                    0,
                                    sizeof(struct dentry),
                                    'TNED',
                                    0 );

    RtlInitUnicodeString(&DosDeviceName, DOS_DEVICE_NAME);
    IoCreateSymbolicLink(&DosDeviceName, &DeviceName);

#if EXT2_DEBUG
    ProcessNameOffset = Ext2GetProcessNameOffset();
#endif

    Ext2LoadAllNls();

    Ext2Global->Codepage.PageTable =
        load_nls(Ext2Global->Codepage.AnsiName);

    /* register file system devices for disk and cdrom */
    IoRegisterFileSystem(DiskdevObject);
    ObReferenceObject(DiskdevObject);

    IoRegisterFileSystem(CdromdevObject);
    ObReferenceObject(CdromdevObject);

    /* let a bare "sc stop" unload us (see devctl.c) */
    Ext2StartUnloadWatch();

    /* last: existing volumes are reported (and probed) right away, so
       everything above must be in place (see letter.c) */
    Ext2StartLetterService(DriverObject);

errorout:

    if (!NT_SUCCESS(Status)) {

        /*
         *  stop reaper thread ...
         */


        /*
         *  cleanup resources ...
         */

        if (Ext2Global) {
            ExDeleteResourceLite(&Ext2Global->Resource);
            if (Ext2Global->LowMemoryHandle) {
                ZwClose(Ext2Global->LowMemoryHandle);
            }
            if (Ext2Global->RegistryPath.Buffer) {
                Ext2FreePool(Ext2Global->RegistryPath.Buffer, EXT4_TAG_GLOBAL);
            }
            Ext2FreePool(Ext2Global, EXT4_TAG_GLOBAL);
            Ext2Global = NULL;
        }

        if (CdromdevObject) {
            IoDeleteDevice(CdromdevObject);
        }

        if (DiskdevObject) {
            IoDeleteDevice(DiskdevObject);
        }

        if (journal_module_inited) {
            Ext2JournalGlobalExit();
            /* cleanup journal related caches */
            UNLOAD_MODULE(journal_exit);
        }

        if (linux_lib_inited) {
            /* cleanup linux lib */
            ext2_destroy_linux();
        }
    }

    return Status;
}
