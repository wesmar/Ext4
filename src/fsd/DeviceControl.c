/**
 * DeviceControl.c - IRP_MJ_DEVICE_CONTROL: volume pass-through and the control device interface.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"

NTSTATUS
Ext2DeviceControlCompletion (
    IN PDEVICE_OBJECT   DeviceObject,
    IN PIRP             Irp,
    IN PVOID            Context);

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2DeviceControl)
#pragma alloc_text(PAGE, Ext2DeviceControlNormal)
#pragma alloc_text(PAGE, Ext2ProcessVolumeProperty)
#pragma alloc_text(PAGE, Ext2ProcessUserProperty)
#pragma alloc_text(PAGE, Ext2ProcessGlobalProperty)
#pragma alloc_text(PAGE, Ext2ProcessUserPerfStat)
#pragma alloc_text(PAGE, Ext2ProcessMountPoint)
#endif

NTSTATUS
Ext2DeviceControlCompletion (
    IN PDEVICE_OBJECT   DeviceObject,
    IN PIRP             Irp,
    IN PVOID            Context
)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Context);
    if (Irp->PendingReturned) {
        IoMarkIrpPending(Irp);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
Ext2DeviceControlNormal (IN PEXT2_IRP_CONTEXT IrpContext)
{
    PDEVICE_OBJECT  DeviceObject;
    BOOLEAN         CompleteRequest = TRUE;
    NTSTATUS        Status = STATUS_UNSUCCESSFUL;

    PEXT2_VCB       Vcb;

    PIRP            Irp;
    PIO_STACK_LOCATION IrpSp;
    PIO_STACK_LOCATION NextIrpSp;

    PDEVICE_OBJECT  TargetDeviceObject;

    __try {

        ASSERT(IrpContext != NULL);

        ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
               (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

        CompleteRequest = TRUE;

        DeviceObject = IrpContext->DeviceObject;

        if (IsExt2FsDevice(DeviceObject))  {
            Status = STATUS_INVALID_DEVICE_REQUEST;
            __leave;
        }

        Irp = IrpContext->Irp;
        IrpSp = IoGetCurrentIrpStackLocation(Irp);

        Vcb = (PEXT2_VCB) IrpSp->FileObject->FsContext;

        if (!((Vcb) && (Vcb->Identifier.Type == EXT2VCB) &&
                (Vcb->Identifier.Size == sizeof(EXT2_VCB)))) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        TargetDeviceObject = Vcb->TargetDeviceObject;

        /* Pass on the IOCTL to the driver below */

        CompleteRequest = FALSE;

        NextIrpSp = IoGetNextIrpStackLocation( Irp );
        *NextIrpSp = *IrpSp;

        IoSetCompletionRoutine(
            Irp,
            Ext2DeviceControlCompletion,
            NULL,
            FALSE,
            TRUE,
            TRUE );

        Status = IoCallDriver(TargetDeviceObject, Irp);

    } __finally  {

        if (!IrpContext->ExceptionInProgress) {
            if (IrpContext) {
                if (!CompleteRequest) {
                    IrpContext->Irp = NULL;
                }

                Ext2CompleteIrpContext(IrpContext, Status);
            }
        }
    }

    return Status;
}

/*
 * The property commands come in three levels, each structure starting with
 * the one below it (EXT2_VOLUME_PROPERTY3 with 2, 2 with 1): a level-3 set
 * sets levels 3, 2 and 1. Returns the level, 0 for no property command, and
 * whether it sets or queries.
 */
static ULONG
Ext2PropertyLevel(IN ULONG Command, OUT PBOOLEAN Set)
{
    switch (Command) {
    case APP_CMD_SET_PROPERTY:      *Set = TRUE;  return 1;
    case APP_CMD_QUERY_PROPERTY:    *Set = FALSE; return 1;
    case APP_CMD_SET_PROPERTY2:     *Set = TRUE;  return 2;
    case APP_CMD_QUERY_PROPERTY2:   *Set = FALSE; return 2;
    case APP_CMD_SET_PROPERTY3:     *Set = TRUE;  return 3;
    case APP_CMD_QUERY_PROPERTY3:   *Set = FALSE; return 3;
    default:                        *Set = FALSE; return 0;
    }
}

/* the structure a level reads and writes, index = level */
static const ULONG Ext2PropertySize[] = {
    0,
    sizeof(EXT2_VOLUME_PROPERTY),
    sizeof(EXT2_VOLUME_PROPERTY2),
    sizeof(EXT2_VOLUME_PROPERTY3)
};

/* the header every command has, Command included */
#define EXT2_PROPERTY_HEADER    RTL_SIZEOF_THROUGH_FIELD(EXT2_VOLUME_PROPERTY, Command)

/* a hiding pattern from user mode: off, or the text ended inside the field */
static VOID
Ext2TakePattern(OUT PBOOLEAN On, OUT PCHAR Pattern, IN BOOLEAN GivenOn, IN const CHAR *Given)
{
    *On = GivenOn;
    RtlZeroMemory(Pattern, HIDINGPAT_LEN);
    if (GivenOn) {
        RtlCopyMemory(Pattern, Given, HIDINGPAT_LEN - 1);
    }
}

/* a code page by name, NULL for one the driver has no table for - which
   changes nothing (it used to take a volume off UTF-8). The name comes
   from user mode: it is ended inside its field first. */
static struct nls_table *
Ext2FindCodepage(IN OUT PCHAR Name)
{
    Name[CODEPAGE_MAXLEN - 1] = 0;
    return load_nls(Name);
}

/* level 3 of the driver: automount */
static VOID
Ext2SetGlobalProperty3(IN PEXT2_VOLUME_PROPERTY3 Property3)
{
    if (Property3->Flags2 & EXT2_VPROP3_AUTOMOUNT) {
        if (Property3->AutoMount) {
            SetLongFlag(Ext2Global->Flags, EXT2_AUTO_MOUNT);
        } else {
            ClearLongFlag(Ext2Global->Flags, EXT2_AUTO_MOUNT);
        }
    }
}

/* level 2 of the driver: the hiding patterns new volumes start with */
static VOID
Ext2SetGlobalProperty2(IN PEXT2_VOLUME_PROPERTY2 Property2)
{
    Ext2TakePattern(&Ext2Global->bHidingPrefix, Ext2Global->sHidingPrefix,
                    Property2->bHidingPrefix, Property2->sHidingPrefix);
    Ext2TakePattern(&Ext2Global->bHidingSuffix, Ext2Global->sHidingSuffix,
                    Property2->bHidingSuffix, Property2->sHidingSuffix);
}

/* level 1 of the driver: writing, and the code page new volumes start with */
static VOID
Ext2SetGlobalProperty1(IN PEXT2_VOLUME_PROPERTY Property)
{
    struct nls_table *PageTable;

    if (Property->bReadonly) {
        ClearLongFlag(Ext2Global->Flags, EXT2_SUPPORT_WRITING);
        ClearLongFlag(Ext2Global->Flags, EXT3_FORCE_WRITING);
    } else {
        SetLongFlag(Ext2Global->Flags, EXT2_SUPPORT_WRITING);
        if (Property->bExt3Writable) {
            SetLongFlag(Ext2Global->Flags, EXT3_FORCE_WRITING);
        } else {
            ClearLongFlag(Ext2Global->Flags, EXT3_FORCE_WRITING);
        }
    }
    PageTable = Ext2FindCodepage(Property->Codepage);
    if (PageTable != NULL) {
        RtlCopyMemory(Ext2Global->Codepage.AnsiName, Property->Codepage, CODEPAGE_MAXLEN);
        Ext2Global->Codepage.PageTable = PageTable;
    }
}

NTSTATUS
Ext2ProcessGlobalProperty(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PEXT2_VOLUME_PROPERTY3 Property3,
    IN  ULONG Length
)
{
    PEXT2_VOLUME_PROPERTY   Property = (PVOID)Property3;
    ULONG                   Level;
    BOOLEAN                 Set;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (Length < EXT2_PROPERTY_HEADER || !IsFlagOn(Property->Flags, EXT2_FLAG_VP_SET_GLOBAL)) {
        return STATUS_INVALID_PARAMETER;
    }

    /* the version, terminated; no build date or time - the same source
       builds to the same bytes */
    if (Property->Command == APP_CMD_QUERY_VERSION) {
        PEXT2_VOLUME_PROPERTY_VERSION PVPV = (PEXT2_VOLUME_PROPERTY_VERSION)Property;

        if (Length < sizeof(EXT2_VOLUME_PROPERTY_VERSION)) {
            return STATUS_INVALID_PARAMETER;
        }
        RtlZeroMemory(PVPV->Version, sizeof(PVPV->Version));
        RtlZeroMemory(PVPV->Time, sizeof(PVPV->Time));
        RtlZeroMemory(PVPV->Date, sizeof(PVPV->Date));
        RtlCopyMemory(PVPV->Version, EXT4_VERSION_STRING4,
                      min(sizeof(EXT4_VERSION_STRING4), sizeof(PVPV->Version)) - 1);
        return STATUS_SUCCESS;
    }

    /* the driver's settings are only set here; a query goes to a volume */
    Level = Ext2PropertyLevel(Property->Command, &Set);
    if (Level == 0 || !Set || Length < Ext2PropertySize[Level]) {
        return STATUS_INVALID_PARAMETER;
    }

    ExAcquireResourceExclusiveLite(&Ext2Global->Resource, TRUE);
    if (Level >= 3) {
        Ext2SetGlobalProperty3(Property3);
    }
    if (Level >= 2) {
        Ext2SetGlobalProperty2((PEXT2_VOLUME_PROPERTY2)Property3);
    }
    Ext2SetGlobalProperty1(Property);
    ExReleaseResourceLite(&Ext2Global->Resource);

    return STATUS_SUCCESS;
}

/* level 3 of a volume: automount, and the ids files are made with */
static VOID
Ext2SetVolumeProperty3(IN PEXT2_VCB Vcb, IN PEXT2_VOLUME_PROPERTY3 Property3)
{
    Ext2SetGlobalProperty3(Property3);

    if (Property3->Flags2 & EXT2_VPROP3_USERIDS) {
        SetLongFlag(Vcb->Flags, VCB_USER_IDS);
        Vcb->uid = Property3->uid;
        Vcb->gid = Property3->gid;
        if (Property3->EIDS) {
            Vcb->euid = Property3->euid;
            Vcb->egid = Property3->egid;
            SetLongFlag(Vcb->Flags, VCB_USER_EIDS);
        } else {
            Vcb->euid = Vcb->egid = 0;
            ClearLongFlag(Vcb->Flags, VCB_USER_EIDS);
        }
    } else {
        ClearLongFlag(Vcb->Flags, VCB_USER_IDS);
        ClearLongFlag(Vcb->Flags, VCB_USER_EIDS);
        Vcb->uid = Vcb->gid = 0;
        Vcb->euid = Vcb->egid = 0;
    }
}

/* level 2 of a volume: the names its listings leave out, its letter */
static VOID
Ext2SetVolumeProperty2(IN PEXT2_VCB Vcb, IN PEXT2_VOLUME_PROPERTY2 Property2)
{
    /* the flags were compared with the volume's, never taken: hiding was
       off on every volume whatever the settings said */
    Ext2TakePattern(&Vcb->bHidingPrefix, Vcb->sHidingPrefix,
                    Property2->bHidingPrefix, Property2->sHidingPrefix);
    Ext2TakePattern(&Vcb->bHidingSuffix, Vcb->sHidingSuffix,
                    Property2->bHidingSuffix, Property2->sHidingSuffix);
    Vcb->DrvLetter = Property2->DrvLetter;
}

/*
 * Level 1 of a volume: read-only or writable, and the code page. A change
 * happens only when the state does - asked to be what it is, the volume is
 * left alone (writable again it used to go through the transition once
 * more). A volume that stopped writing on an error stays read-only until
 * it is dismounted, as Linux refuses to remount an aborted file system
 * read-write.
 */
static NTSTATUS
Ext2SetVolumeProperty1(IN PEXT2_VCB Vcb, IN PEXT2_VOLUME_PROPERTY Property)
{
    struct nls_table *PageTable;
    BOOLEAN     ReadOnly = IsFlagOn(Vcb->Flags, VCB_READ_ONLY);
    BOOLEAN     TransitionOwned = FALSE;
    NTSTATUS    Status = STATUS_SUCCESS;

    __try {

        if (Property->bReadonly) {

            if (!ReadOnly && IsFlagOn(Vcb->Flags, VCB_INITIALIZED)) {
                (void)Ext2FlushFiles(NULL, Vcb, FALSE);
                (void)Ext2FlushVolume(NULL, Vcb, FALSE);
                /* going read-only: leave the fs clean, like unmount */
                if (Vcb->Journal) {
                    Ext2JournalStop(Vcb, TRUE);
                }
                Ext4MmpStop(Vcb, !IsFlagOn(Vcb->Flags, VCB_DEVICE_REMOVED));
            }
            SetLongFlag(Vcb->Flags, VCB_READ_ONLY);

        } else if (IsFlagOn(Vcb->Flags, VCB_WRITE_STOPPED)) {

            Status = STATUS_MEDIA_WRITE_PROTECTED;

        } else {

            if (Property->bExt3Writable) {
                SetLongFlag(Vcb->Flags, VCB_FORCE_WRITING);
            }

            if (!Vcb->IsExt3fs) {
                if (Ext4MmpHold(Vcb)) {
                    ClearLongFlag(Vcb->Flags, VCB_READ_ONLY);
                }
            } else if (!Property->bExt3Writable) {
                SetLongFlag(Vcb->Flags, VCB_READ_ONLY);
            } else if (ReadOnly && IsFlagOn(Vcb->Flags, VCB_INITIALIZED)) {
                /* going writable: replay if needed and start logging;
                   without a working journal the volume stays read-only */
                SetLongFlag(Vcb->Flags, VCB_WRITE_TRANSITION);
                TransitionOwned = TRUE;
                ClearLongFlag(Vcb->Flags, VCB_READ_ONLY);
                if (!Ext4MmpHold(Vcb) ||
                    Ext2RecoverJournal(NULL, Vcb) != 0 ||
                    IsFlagOn(Vcb->Flags, VCB_JOURNAL_RECOVER)) {
                    SetLongFlag(Vcb->Flags, VCB_READ_ONLY);
                }
                ClearLongFlag(Vcb->Flags, VCB_WRITE_TRANSITION);
                TransitionOwned = FALSE;
            } else {
                ClearLongFlag(Vcb->Flags, VCB_READ_ONLY);
            }
        }

        PageTable = Ext2FindCodepage(Property->Codepage);
        if (PageTable != NULL) {
            RtlCopyMemory(Vcb->Codepage.AnsiName, Property->Codepage, CODEPAGE_MAXLEN);
            Vcb->Codepage.PageTable = PageTable;
            Ext2InitializeLabel(Vcb, Vcb->SuperBlock);
        }

    } __finally {

        /* a transition cut short leaves the volume read-only */
        if (TransitionOwned) {
            SetLongFlag(Vcb->Flags, VCB_READ_ONLY);
            ClearLongFlag(Vcb->Flags, VCB_WRITE_TRANSITION);
        }
    }

    return Status;
}

/* level 3 of a volume, read back */
static VOID
Ext2QueryVolumeProperty3(IN PEXT2_VCB Vcb, OUT PEXT2_VOLUME_PROPERTY3 Property3)
{
    if (IsFlagOn(Ext2Global->Flags, EXT2_AUTO_MOUNT)) {
        SetFlag(Property3->Flags2, EXT2_VPROP3_AUTOMOUNT);
        Property3->AutoMount = TRUE;
    } else {
        ClearFlag(Property3->Flags2, EXT2_VPROP3_AUTOMOUNT);
        Property3->AutoMount = FALSE;
    }

    if (IsFlagOn(Vcb->Flags, VCB_USER_IDS)) {
        SetFlag(Property3->Flags2, EXT2_VPROP3_USERIDS);
        Property3->uid = Vcb->uid;
        Property3->gid = Vcb->gid;
        if (IsFlagOn(Vcb->Flags, VCB_USER_EIDS)) {
            Property3->EIDS = TRUE;
            Property3->euid = Vcb->euid;
            Property3->egid = Vcb->egid;
        } else {
            Property3->EIDS = FALSE;
        }
    } else {
        ClearFlag(Property3->Flags2, EXT2_VPROP3_USERIDS);
    }
}

/* level 2 of a volume, read back */
static VOID
Ext2QueryVolumeProperty2(IN PEXT2_VCB Vcb, OUT PEXT2_VOLUME_PROPERTY2 Property2)
{
    RtlCopyMemory(Property2->UUID, Vcb->SuperBlock->s_uuid, sizeof(Property2->UUID));
    Property2->DrvLetter = Vcb->DrvLetter;
    Property2->bHidingPrefix = Vcb->bHidingPrefix;
    RtlCopyMemory(Property2->sHidingPrefix, Vcb->sHidingPrefix, HIDINGPAT_LEN);
    Property2->bHidingSuffix = Vcb->bHidingSuffix;
    RtlCopyMemory(Property2->sHidingSuffix, Vcb->sHidingSuffix, HIDINGPAT_LEN);
}

/* level 1 of a volume, read back */
static VOID
Ext2QueryVolumeProperty1(IN PEXT2_VCB Vcb, OUT PEXT2_VOLUME_PROPERTY Property)
{
    Property->bExt2 = TRUE;
    Property->bExt3 = Vcb->IsExt3fs;
    Property->bReadonly = IsFlagOn(Vcb->Flags, VCB_READ_ONLY);
    Property->bExt3Writable = !Property->bReadonly && Vcb->IsExt3fs;

    RtlZeroMemory(Property->Codepage, CODEPAGE_MAXLEN);
    strncpy(Property->Codepage,
            Vcb->Codepage.PageTable ? Vcb->Codepage.PageTable->charset : "default",
            CODEPAGE_MAXLEN - 1);
}

NTSTATUS
Ext2ProcessVolumeProperty(
    IN  PEXT2_VCB              Vcb,
    IN  PEXT2_VOLUME_PROPERTY3 Property3,
    IN  ULONG Length
)
{
    PEXT2_VOLUME_PROPERTY2  Property2 = (PVOID)Property3;
    PEXT2_VOLUME_PROPERTY   Property = (PVOID)Property3;
    NTSTATUS                Status = STATUS_SUCCESS;
    ULONG                   Level;
    BOOLEAN                 Set;

    if (Length < EXT2_PROPERTY_HEADER) {
        return STATUS_INVALID_PARAMETER;
    }
    Level = Ext2PropertyLevel(Property->Command, &Set);
    if (Level == 0 || Length < Ext2PropertySize[Level]) {
        return STATUS_INVALID_PARAMETER;
    }

    ExAcquireResourceExclusiveLite(&Vcb->MainResource, TRUE);
    __try {
        if (Set) {
            if (Level >= 3) {
                Ext2SetVolumeProperty3(Vcb, Property3);
            }
            if (Level >= 2) {
                Ext2SetVolumeProperty2(Vcb, Property2);
            }
            Status = Ext2SetVolumeProperty1(Vcb, Property);
        } else {
            if (Level >= 3) {
                Ext2QueryVolumeProperty3(Vcb, Property3);
            }
            if (Level >= 2) {
                Ext2QueryVolumeProperty2(Vcb, Property2);
            }
            Ext2QueryVolumeProperty1(Vcb, Property);
        }
    } __finally {
        ExReleaseResourceLite(&Vcb->MainResource);
    }

    return Status;
}

NTSTATUS
Ext2ProcessUserProperty(
    IN PEXT2_IRP_CONTEXT        IrpContext,
    IN PEXT2_VOLUME_PROPERTY3   Property,
    IN ULONG                    Length
)
{
    NTSTATUS    Status = STATUS_SUCCESS;
    PEXT2_VCB   Vcb = NULL;
    PDEVICE_OBJECT  DeviceObject = NULL;

    __try {

        ASSERT(IrpContext != NULL);
        ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
               (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

        if (Property->Magic != EXT2_VOLUME_PROPERTY_MAGIC) {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        DeviceObject = IrpContext->DeviceObject;
        if (IsExt2FsDevice(DeviceObject)) {
            Status = Ext2ProcessGlobalProperty(DeviceObject, Property, Length);
        } else {
            Vcb = (PEXT2_VCB) DeviceObject->DeviceExtension;
            if (!((Vcb) && (Vcb->Identifier.Type == EXT2VCB) &&
                    (Vcb->Identifier.Size == sizeof(EXT2_VCB)))) {
                Status = STATUS_INVALID_PARAMETER;
                __leave;
            }
            Status = Ext2ProcessVolumeProperty(Vcb, Property, Length);
        }

        if (NT_SUCCESS(Status)) {
            IrpContext->Irp->IoStatus.Information = Length;
        }

    } __finally {

        if (!IrpContext->ExceptionInProgress) {
            Ext2CompleteIrpContext(IrpContext, Status);
        }
    }

    return Status;
}

NTSTATUS
Ext2ProcessUserPerfStat(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_QUERY_PERFSTAT QueryPerf,
    IN ULONG                Length
)
{
    PDEVICE_OBJECT  DeviceObject = NULL;

    BOOLEAN     GlobalDataResourceAcquired = FALSE;
    NTSTATUS    Status = STATUS_SUCCESS;

    __try {

        ASSERT(IrpContext != NULL);
        ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
               (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

        DeviceObject = IrpContext->DeviceObject;
        if (IsExt2FsDevice(DeviceObject)) {

            if (QueryPerf->Magic != EXT2_QUERY_PERFSTAT_MAGIC) {
                Status = STATUS_INVALID_PARAMETER;
                __leave;
            }

            if (QueryPerf->Command != IOCTL_APP_QUERY_PERFSTAT) {
                Status = STATUS_INVALID_PARAMETER;
                __leave;
            }

            if (Length != EXT2_QUERY_PERFSTAT_SZV1 &&
                Length != EXT2_QUERY_PERFSTAT_SZV2) {
                Status = STATUS_INVALID_PARAMETER;
                __leave;
            }

            ExAcquireResourceSharedLite(&Ext2Global->Resource, TRUE);
            GlobalDataResourceAcquired = TRUE;

            if (Length == EXT2_QUERY_PERFSTAT_SZV2) {
                QueryPerf->Flags = EXT2_QUERY_PERFSTAT_VER2;
                QueryPerf->PerfStatV2 = Ext2Global->PerfStat;
            } else {
                memcpy(&QueryPerf->PerfStatV1.Irps[0], &Ext2Global->PerfStat.Irps[0],
                       FIELD_OFFSET(EXT2_PERF_STATISTICS_V1, Unit));
                memcpy(&QueryPerf->PerfStatV1.Unit, &Ext2Global->PerfStat.Unit,
                       sizeof(EXT2_STAT_ARRAY_V1));
                memcpy(&QueryPerf->PerfStatV1.Current, &Ext2Global->PerfStat.Current,
                       sizeof(EXT2_STAT_ARRAY_V1));
                memcpy(&QueryPerf->PerfStatV1.Size, &Ext2Global->PerfStat.Size,
                       sizeof(EXT2_STAT_ARRAY_V1));
                memcpy(&QueryPerf->PerfStatV1.Total, &Ext2Global->PerfStat.Total,
                       sizeof(EXT2_STAT_ARRAY_V1));
            }

        } else {
            Status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        if (NT_SUCCESS(Status)) {
            IrpContext->Irp->IoStatus.Information = Length;
        }

    } __finally {

        if (GlobalDataResourceAcquired) {
            ExReleaseResourceLite(&Ext2Global->Resource);
        }

        if (!IrpContext->ExceptionInProgress) {
            Ext2CompleteIrpContext(IrpContext, Status);
        }
    }

    return Status;
}

NTSTATUS
Ext2ProcessMountPoint(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_MOUNT_POINT    MountPoint,
    IN ULONG                Length
)
{
    UNICODE_STRING  Link;
    UNICODE_STRING  Target;
    WCHAR           Buffer[] = L"\\DosDevices\\Global\\Z:";
    NTSTATUS        status = STATUS_SUCCESS;

    PDEVICE_OBJECT  DeviceObject = NULL;

    __try {

        ASSERT(IrpContext != NULL);
        ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
               (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

        DeviceObject = IrpContext->DeviceObject;
        if (!IsExt2FsDevice(DeviceObject)) {
            status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        if (Length != sizeof(EXT2_MOUNT_POINT) ||
            MountPoint->Magic != EXT2_APP_MOUNTPOINT_MAGIC) {
            status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        /* a drive letter, and a target that ends inside its array:
           RtlInitUnicodeString would otherwise run past the buffer */
        if (MountPoint->Link[0] < L'A' || MountPoint->Link[0] > L'Z' ||
            (MountPoint->Command == APP_CMD_ADD_DOS_SYMLINK &&
             wcsnlen((PCWSTR)MountPoint->Name, RTL_NUMBER_OF(MountPoint->Name)) ==
                 RTL_NUMBER_OF(MountPoint->Name))) {
            status = STATUS_INVALID_PARAMETER;
            __leave;
        }

        RtlInitUnicodeString(&Link, Buffer);
        Buffer[12] = MountPoint->Link[0];

        switch (MountPoint->Command) {

        case APP_CMD_ADD_DOS_SYMLINK:
            RtlInitUnicodeString(&Target, &MountPoint->Name[0]);
            status = IoCreateSymbolicLink(&Link, &Target);
            break;

        case APP_CMD_DEL_DOS_SYMLINK:
            status = IoDeleteSymbolicLink(&Link);
            break;

        default:
            status = STATUS_INVALID_PARAMETER;
        }

    } __finally {

        if (!IrpContext->ExceptionInProgress) {
            Ext2CompleteIrpContext(IrpContext, status);
        }
    }

    return status;
}

/*
 * The driver's own IOCTLs change how volumes are mounted and written
 * (read-only, uid/gid mapping, code page, hiding), create global drive-letter
 * links or dismount everything. They reach this dispatcher not only through
 * the control devices but through any handle on an ext volume - a plain file
 * included - and are FILE_ANY_ACCESS, so the I/O manager checks nothing.
 * One gate for all of them:
 *   - IOCTL_PREPARE_TO_UNLOAD comes from the drain thread only (kernel);
 *     user mode stops the driver with sc stop;
 *   - the rest take a caller from the kernel or an administrator;
 *   - the buffer exists and holds the common header (Magic, Flags,
 *     Command) before any handler reads a byte of it.
 */
#define EXT2_APP_IOCTL_HEADER   (3 * sizeof(ULONG))

NTSTATUS
Ext2CheckAppIoctl(IN PIRP Irp, IN ULONG Code)
{
    PIO_STACK_LOCATION  IrpSp = IoGetCurrentIrpStackLocation(Irp);

    if (Code == IOCTL_PREPARE_TO_UNLOAD) {
        return Irp->RequestorMode == KernelMode ? STATUS_SUCCESS : STATUS_ACCESS_DENIED;
    }

    if (Irp->RequestorMode != KernelMode) {
        SECURITY_SUBJECT_CONTEXT    Subject;
        BOOLEAN                     Admin;

        SeCaptureSubjectContext(&Subject);
        SeLockSubjectContext(&Subject);
        Admin = SeTokenIsAdmin(SeQuerySubjectContextToken(&Subject));
        SeUnlockSubjectContext(&Subject);
        SeReleaseSubjectContext(&Subject);
        if (!Admin) {
            return STATUS_ACCESS_DENIED;
        }
    }

    if (Irp->AssociatedIrp.SystemBuffer == NULL ||
        IrpSp->Parameters.DeviceIoControl.InputBufferLength < EXT2_APP_IOCTL_HEADER ||
        IrpSp->Parameters.DeviceIoControl.OutputBufferLength < EXT2_APP_IOCTL_HEADER) {
        return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
Ext2DeviceControl (IN PEXT2_IRP_CONTEXT IrpContext)
{
    PIRP                Irp;
    PIO_STACK_LOCATION  irpSp;
    ULONG               code;
    ULONG               length;
    NTSTATUS            Status;

    ASSERT(IrpContext);

    ASSERT((IrpContext->Identifier.Type == EXT2ICX) &&
           (IrpContext->Identifier.Size == sizeof(EXT2_IRP_CONTEXT)));

    Irp = IrpContext->Irp;

    irpSp = IoGetCurrentIrpStackLocation(Irp);

    code = irpSp->Parameters.DeviceIoControl.IoControlCode;
    length = irpSp->Parameters.DeviceIoControl.OutputBufferLength;

    if (code == IOCTL_APP_VOLUME_PROPERTY || code == IOCTL_APP_QUERY_PERFSTAT ||
        code == IOCTL_APP_MOUNT_POINT || code == IOCTL_PREPARE_TO_UNLOAD) {
        Status = Ext2CheckAppIoctl(Irp, code);
        if (!NT_SUCCESS(Status)) {
            Ext2CompleteIrpContext(IrpContext, Status);
            return Status;
        }
    }

    switch (code) {

    case IOCTL_APP_VOLUME_PROPERTY:
        Status = Ext2ProcessUserProperty(
                     IrpContext,
                     Irp->AssociatedIrp.SystemBuffer,
                     length
                 );
        break;

    case IOCTL_APP_QUERY_PERFSTAT:
        Status = Ext2ProcessUserPerfStat(
                     IrpContext,
                     Irp->AssociatedIrp.SystemBuffer,
                     length
                 );
        break;

    case IOCTL_APP_MOUNT_POINT:
        Status = Ext2ProcessMountPoint(
                     IrpContext,
                     Irp->AssociatedIrp.SystemBuffer,
                     length
                 );
        break;

    case IOCTL_PREPARE_TO_UNLOAD:
        Status = Ext2PrepareToUnload(IrpContext);
        break;
    default:
        Status = Ext2DeviceControlNormal(IrpContext);
    }

    return Status;
}
