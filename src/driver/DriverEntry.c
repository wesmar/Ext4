/**
 * DriverEntry.c - driver entry and exit: global state, device objects, dispatch table, file system registration.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include <wdmsec.h>

PEXT2_GLOBAL    Ext2Global   = NULL;

NTSTATUS
DriverEntry(
    IN PDRIVER_OBJECT DriverObject,
    IN PUNICODE_STRING RegistryPath   );

DECLARE_INIT(journal_init);
DECLARE_EXIT(journal_exit);

static VOID Ext2DeleteLookasides(VOID);

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

    Ext2DeleteLookasides();

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

/* an ERESOURCE must be 8-byte aligned wherever it is embedded */
C_ASSERT((FIELD_OFFSET(EXT2_GLOBAL, Resource) & 7) == 0);
C_ASSERT((FIELD_OFFSET(EXT2_VCB, MainResource) & 7) == 0);
C_ASSERT((FIELD_OFFSET(EXT2_VCB, PagingIoResource) & 7) == 0);
C_ASSERT((FIELD_OFFSET(EXT2_VCB, SuperLock) & 7) == 0);
C_ASSERT((FIELD_OFFSET(EXT2_VCB, LinkLock) & 7) == 0);
C_ASSERT((FIELD_OFFSET(EXT2_VCB, FcbLock) & 7) == 0);
C_ASSERT((FIELD_OFFSET(EXT2_VCB, bd.bd_bh_lock) & 7) == 0);
C_ASSERT((FIELD_OFFSET(EXT2_VCB, sbi.s_gd_lock) & 7) == 0);
C_ASSERT((FIELD_OFFSET(EXT2_FCBVCB, MainResource) & 7) == 0);
C_ASSERT((FIELD_OFFSET(EXT2_FCBVCB, PagingIoResource) & 7) == 0);
C_ASSERT((FIELD_OFFSET(EXT2_FCB, MainResource) & 7) == 0);
C_ASSERT((FIELD_OFFSET(EXT2_FCB, PagingIoResource) & 7) == 0);
C_ASSERT((FIELD_OFFSET(EXT2_ICB, DirResource) & 7) == 0);
C_ASSERT((FIELD_OFFSET(EXT2_ICB, EntryResource) & 7) == 0);

/* the on-disk superblock is overlaid on this structure: its size and the
   magic's offset are the format's */
C_ASSERT(sizeof(EXT2_SUPER_BLOCK) == SUPER_BLOCK_SIZE);
C_ASSERT(FIELD_OFFSET(EXT2_SUPER_BLOCK, s_magic) == 56);

/* the requests the driver takes, every one through Ext2BuildRequest */
static const UCHAR Ext2MajorFunctions[] = {
    IRP_MJ_CREATE, IRP_MJ_CLOSE, IRP_MJ_READ, IRP_MJ_WRITE,
    IRP_MJ_FLUSH_BUFFERS, IRP_MJ_SHUTDOWN,
    IRP_MJ_QUERY_INFORMATION, IRP_MJ_SET_INFORMATION,
    IRP_MJ_QUERY_VOLUME_INFORMATION, IRP_MJ_SET_VOLUME_INFORMATION,
    IRP_MJ_DIRECTORY_CONTROL, IRP_MJ_FILE_SYSTEM_CONTROL,
    IRP_MJ_DEVICE_CONTROL, IRP_MJ_LOCK_CONTROL,
    IRP_MJ_QUERY_EA, IRP_MJ_SET_EA,
    IRP_MJ_CLEANUP, IRP_MJ_PNP
};

/* The name and file caches are trimmed past 64 entries per unit of depth
   (Ext2McbHighWater, Ext2FcbHighWater): a larger system keeps more. */
#define EXT2_CACHE_DEPTH_SMALL      64
#define EXT2_CACHE_DEPTH_MEDIUM     128
#define EXT2_CACHE_DEPTH_LARGE      256

static USHORT
Ext2CacheDepth(VOID)
{
    switch (MmQuerySystemSize()) {
    case MmSmallSystem:     return EXT2_CACHE_DEPTH_SMALL;
    case MmMediumSystem:    return EXT2_CACHE_DEPTH_MEDIUM;
    default:                return EXT2_CACHE_DEPTH_LARGE;
    }
}

/* the fixed-size objects, each from a lookaside list of its own */
#define EXT2_LOOKASIDES     6

typedef struct _EXT2_LOOKASIDE {
    PNPAGED_LOOKASIDE_LIST  List;
    SIZE_T                  Size;
    ULONG                   Tag;
} EXT2_LOOKASIDE;

static VOID
Ext2LookasideTable(OUT EXT2_LOOKASIDE Lists[EXT2_LOOKASIDES])
{
    const EXT2_LOOKASIDE Table[EXT2_LOOKASIDES] = {
        { &Ext2Global->Ext2IrpContextLookasideList, sizeof(EXT2_IRP_CONTEXT),   'PRIE' },
        { &Ext2Global->Ext2FcbLookasideList,        sizeof(EXT2_FCB),           'BCFE' },
        { &Ext2Global->Ext2CcbLookasideList,        sizeof(EXT2_CCB),           'BCCE' },
        { &Ext2Global->Ext2McbLookasideList,        sizeof(EXT2_MCB),           'BCME' },
        { &Ext2Global->Ext2ExtLookasideList,        sizeof(EXT2_EXTENT),        'STXE' },
        { &Ext2Global->Ext2DentryLookasideList,     sizeof(struct dentry),      'TNED' },
    };

    RtlCopyMemory(Lists, Table, sizeof(Table));
}

static VOID
Ext2InitializeLookasides(VOID)
{
    EXT2_LOOKASIDE  Lists[EXT2_LOOKASIDES];
    ULONG           i;

    Ext2LookasideTable(Lists);
    for (i = 0; i < EXT2_LOOKASIDES; i++) {
        ExInitializeNPagedLookasideList(Lists[i].List, NULL, NULL, 0, Lists[i].Size, Lists[i].Tag, 0);
    }
}

static VOID
Ext2DeleteLookasides(VOID)
{
    EXT2_LOOKASIDE  Lists[EXT2_LOOKASIDES];
    ULONG           i;

    Ext2LookasideTable(Lists);
    for (i = EXT2_LOOKASIDES; i-- > 0; ) {
        ExDeleteNPagedLookasideList(Lists[i].List);
    }
}

/* Ext2Global: the lists, locks and events the rest takes for granted, and
   the registry settings */
static NTSTATUS
Ext2InitializeGlobal(IN PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING LowMemoryName;

    Ext2Global = Ext2AllocatePool(NonPagedPool, sizeof(EXT2_GLOBAL), EXT4_TAG_GLOBAL);
    if (Ext2Global == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
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
    RtlInitUnicodeString(&LowMemoryName, L"\\KernelObjects\\LowMemoryCondition");
    Ext2Global->LowMemory = IoCreateNotificationEvent(&LowMemoryName, &Ext2Global->LowMemoryHandle);

    Ext2Global->MaxDepth = Ext2CacheDepth();

    Ext2QueryRegistrySettings(RegistryPath);
    Ext2UnloadStep(L"UnloadStep", EXT2_STEP_LOADED);
    return STATUS_SUCCESS;
}

/* what Ext2InitializeGlobal set up, after a failed start */
static VOID
Ext2FreeGlobal(VOID)
{
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

/* The control devices take the driver's own IOCTLs (volume properties,
   mount points, statistics): SYSTEM and administrators only, enforced on
   every open below them too (FILE_DEVICE_SECURE_OPEN). Created with the
   default security, any user could open \\.\ext4 and send them. */
static NTSTATUS
Ext2CreateControlDevice(IN PDRIVER_OBJECT DriverObject, IN PCWSTR Name, IN DEVICE_TYPE Type,
                        OUT PDEVICE_OBJECT *Device)
{
    UNICODE_STRING DeviceName;

    RtlInitUnicodeString(&DeviceName, Name);
    return IoCreateDeviceSecure(DriverObject, 0, &DeviceName, Type, FILE_DEVICE_SECURE_OPEN,
                                FALSE, &SDDL_DEVOBJ_SYS_ALL_ADM_ALL, NULL, Device);
}

/* the reapers of the FCB, MCB and buffer-head caches: all or none */
static NTSTATUS
Ext2StartReapers(VOID)
{
    NTSTATUS Status;

    Status = Ext2StartReaper(&Ext2Global->FcbReaper, Ext2FcbReaperThread);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = Ext2StartReaper(&Ext2Global->McbReaper, Ext2McbReaperThread);
    if (!NT_SUCCESS(Status)) {
        Ext2StopReaper(&Ext2Global->FcbReaper);
        return Status;
    }
    Status = Ext2StartReaper(&Ext2Global->bhReaper, Ext2bhReaperThread);
    if (!NT_SUCCESS(Status)) {
        Ext2StopReaper(&Ext2Global->McbReaper);
        Ext2StopReaper(&Ext2Global->FcbReaper);
    }
    return Status;
}

/* the entry points the I/O, cache and memory managers call besides the IRPs */
static VOID
Ext2InitializeCallbacks(IN PDRIVER_OBJECT DriverObject)
{
    PFAST_IO_DISPATCH           FastIo = &Ext2Global->FastIoDispatch;
    PCACHE_MANAGER_CALLBACKS    Cache = &Ext2Global->CacheManagerCallbacks;
    PCACHE_MANAGER_CALLBACKS    NoOp = &Ext2Global->CacheManagerNoOpCallbacks;

    FastIo->SizeOfFastIoDispatch          = sizeof(FAST_IO_DISPATCH);
    FastIo->FastIoCheckIfPossible         = Ext2FastIoCheckIfPossible;
    FastIo->FastIoRead                    = Ext2FastIoRead;
    FastIo->FastIoWrite                   = Ext2FastIoWrite;
    FastIo->FastIoQueryBasicInfo          = Ext2FastIoQueryBasicInfo;
    FastIo->FastIoQueryStandardInfo       = Ext2FastIoQueryStandardInfo;
    FastIo->FastIoLock                    = Ext2FastIoLock;
    FastIo->FastIoUnlockSingle            = Ext2FastIoUnlockSingle;
    FastIo->FastIoUnlockAll               = Ext2FastIoUnlockAll;
    FastIo->FastIoUnlockAllByKey          = Ext2FastIoUnlockAllByKey;
    FastIo->FastIoQueryNetworkOpenInfo    = Ext2FastIoQueryNetworkOpenInfo;
    FastIo->AcquireForModWrite            = Ext2AcquireFileForModWrite;
    FastIo->ReleaseForModWrite            = Ext2ReleaseFileForModWrite;
    FastIo->AcquireForCcFlush             = Ext2AcquireFileForCcFlush;
    FastIo->ReleaseForCcFlush             = Ext2ReleaseFileForCcFlush;
    FastIo->AcquireFileForNtCreateSection = Ext2AcquireForCreateSection;
    FastIo->ReleaseFileForNtCreateSection = Ext2ReleaseForCreateSection;
    DriverObject->FastIoDispatch = FastIo;

    Cache->AcquireForLazyWrite  = Ext2AcquireForLazyWrite;
    Cache->ReleaseFromLazyWrite = Ext2ReleaseFromLazyWrite;
    Cache->AcquireForReadAhead  = Ext2AcquireForReadAhead;
    Cache->ReleaseFromReadAhead = Ext2ReleaseFromReadAhead;

    NoOp->AcquireForLazyWrite  = Ext2NoOpAcquire;
    NoOp->ReleaseFromLazyWrite = Ext2NoOpRelease;
    NoOp->AcquireForReadAhead  = Ext2NoOpAcquire;
    NoOp->ReleaseFromReadAhead = Ext2NoOpRelease;

    RtlZeroMemory(&Ext2Global->FilterCallbacks, sizeof(FS_FILTER_CALLBACKS));
    Ext2Global->FilterCallbacks.SizeOfFsFilterCallbacks = sizeof(FS_FILTER_CALLBACKS);
    Ext2Global->FilterCallbacks.PreAcquireForSectionSynchronization = Ext2PreAcquireForCreateSection;
    FsRtlRegisterFileSystemFilterCallbacks(DriverObject, &Ext2Global->FilterCallbacks);
}

/* the unit sizes of the statistics (IOCTL_APP_QUERY_PERFSTAT); 1: the
   size varies from one allocation to the next */
static VOID
Ext2InitializePerfStat(VOID)
{
    PULONG Slot = Ext2Global->PerfStat.Unit.Slot;

    Ext2Global->PerfStat.Magic   = EXT2_PERF_STAT_MAGIC;
    Ext2Global->PerfStat.Version = EXT2_PERF_STAT_VER2;
    Ext2Global->PerfStat.Length  = sizeof(EXT2_PERF_STATISTICS_V2);

    Slot[PS_IRP_CONTEXT] = sizeof(EXT2_IRP_CONTEXT);
    Slot[PS_VCB] = sizeof(EXT2_VCB);
    Slot[PS_FCB] = sizeof(EXT2_FCB);
    Slot[PS_CCB] = sizeof(EXT2_CCB);
    Slot[PS_MCB] = sizeof(EXT2_MCB);
    Slot[PS_EXTENT] = sizeof(EXT2_EXTENT);
    Slot[PS_RW_CONTEXT] = sizeof(EXT2_RW_CONTEXT);
    Slot[PS_VPB] = sizeof(VPB);
    Slot[PS_FILE_NAME] = 1;
    Slot[PS_MCB_NAME] = 1;
    Slot[PS_INODE_NAME] = 1;
    Slot[PS_DIR_ENTRY] = sizeof(EXT2_DIR_ENTRY2);
    Slot[PS_DIR_PATTERN] = 1;
    Slot[PS_DISK_EVENT] = sizeof(KEVENT);
    Slot[PS_DISK_BUFFER] = 1;
    Slot[PS_BLOCK_DATA] = 1;
    Slot[PS_EXT2_INODE] = 1;
    Slot[PS_DENTRY] = sizeof(struct dentry);
    Slot[PS_BUFF_HEAD] = sizeof(struct buffer_head);
    Slot[PS_ICB] = sizeof(EXT2_ICB);
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
    PDEVICE_OBJECT  DiskdevObject = NULL;
    PDEVICE_OBJECT  CdromdevObject = NULL;
    UNICODE_STRING  DeviceName;
    UNICODE_STRING  DosDeviceName;
    NTSTATUS        Status;
    ULONG           i;
    int             rc = 0;
    BOOLEAN         LinuxLib = FALSE;
    BOOLEAN         JournalModule = FALSE;

    /* the version names the build; no build time is recorded, so that the
       same source builds to the same bytes */
    DbgPrint("ext4 -- version " EXT4_VERSION_STRING4
#if EXT2_DEBUG
             " Debug"
#else
             " Release"
#endif
             "\n");

    /* initialize winlib structures */
    if (ext2_init_linux()) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto errorout;
    }
    LinuxLib = TRUE;

    /* initialize journal module structures */
    LOAD_MODULE(journal_init);
    if (rc != 0) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto errorout;
    }
    JournalModule = TRUE;

    /* per-thread handle registry of the write-ahead journal engine */
    Ext2JournalGlobalInit();

    Status = Ext2InitializeGlobal(RegistryPath);
    if (!NT_SUCCESS(Status)) {
        goto errorout;
    }

    /* for CD-ROM media - the I/O manager offers optical volumes only to CD
       file systems - and for disks, \ext4, the one mounts come through */
    Status = Ext2CreateControlDevice(DriverObject, CDROM_NAME, FILE_DEVICE_CD_ROM_FILE_SYSTEM, &CdromdevObject);
    if (!NT_SUCCESS(Status)) {
        goto errorout;
    }
    Status = Ext2CreateControlDevice(DriverObject, DEVICE_NAME, FILE_DEVICE_DISK_FILE_SYSTEM, &DiskdevObject);
    if (!NT_SUCCESS(Status)) {
        goto errorout;
    }

    Status = Ext2StartReapers();
    if (!NT_SUCCESS(Status)) {
        goto errorout;
    }

    /* nothing below fails */
    Ext2Global->DiskdevObject  = DiskdevObject;
    Ext2Global->CdromdevObject = CdromdevObject;

    for (i = 0; i < ARRAYSIZE(Ext2MajorFunctions); i++) {
        DriverObject->MajorFunction[Ext2MajorFunctions[i]] = Ext2BuildRequest;
    }
    DriverObject->DriverUnload = DriverUnload;
    Ext2InitializeCallbacks(DriverObject);
    Ext2InitializePerfStat();
    Ext2InitializeLookasides();

    /* \\.\ext4 for ext4ctl; without it the file system works, the control
       tool does not reach it */
    RtlInitUnicodeString(&DeviceName, DEVICE_NAME);
    RtlInitUnicodeString(&DosDeviceName, DOS_DEVICE_NAME);
    (void)IoCreateSymbolicLink(&DosDeviceName, &DeviceName);

#if EXT2_DEBUG
    ProcessNameOffset = Ext2GetProcessNameOffset();
#endif

    Ext2LoadAllNls();
    Ext2Global->Codepage.PageTable = load_nls(Ext2Global->Codepage.AnsiName);

    /* register file system devices for disk and cdrom */
    IoRegisterFileSystem(DiskdevObject);
    ObReferenceObject(DiskdevObject);
    IoRegisterFileSystem(CdromdevObject);
    ObReferenceObject(CdromdevObject);

    /* let a bare "sc stop" unload us (see DeviceControl.c) */
    Ext2StartUnloadWatch();

    /* last: existing volumes are reported (and probed) right away, so
       everything above must be in place (see DriveLetters.c) */
    Ext2StartLetterService(DriverObject);
    return STATUS_SUCCESS;

errorout:

    if (Ext2Global) {
        Ext2FreeGlobal();
    }
    if (CdromdevObject) {
        IoDeleteDevice(CdromdevObject);
    }
    if (DiskdevObject) {
        IoDeleteDevice(DiskdevObject);
    }
    if (JournalModule) {
        Ext2JournalGlobalExit();
        /* cleanup journal related caches */
        UNLOAD_MODULE(journal_exit);
    }
    if (LinuxLib) {
        /* cleanup linux lib */
        ext2_destroy_linux();
    }
    return Status;
}
