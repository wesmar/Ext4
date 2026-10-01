/**
 * letter.c - drive letters for hidden Linux partitions through the mount manager.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The volume manager creates a volume device for every partition, but the
 * mount manager hands out drive letters only to partition types it knows.
 * An MBR type 0x83 or a GPT "Linux filesystem" partition is treated as
 * hidden: volmgr registers only the hidden-volume interface for it, so the
 * mount manager never hears of the device - no letter, no \\?\Volume{...}
 * link - and nobody opens it, so no file system is ever asked to mount it.
 * This module does both jobs:
 *
 *  - it listens for volume arrivals (the hidden-volume and the mounted-
 *    device interfaces volmgr registers, existing volumes included) and
 *    opens each volume once, which makes the I/O manager offer it to the
 *    file systems: ours mounts an ext volume, anything else is refused as
 *    before. USB sticks, external disks, MBR and GPT all arrive this way;
 *  - after a mount it registers the mounted-device interface on the
 *    volume's PDO itself. From then on the mount manager treats the volume
 *    like any other: it creates the \\?\Volume{...} link, applies the
 *    drive letter its database remembers for the partition, and removes
 *    the links again when the device goes. Only a volume the database has
 *    never seen gets a letter from here (the first free one from D:, via
 *    IOCTL_MOUNTMGR_CREATE_POINT, which is honoured for hidden volumes
 *    while IOCTL_MOUNTMGR_NEXT_DRIVE_LETTER is not); the mount manager
 *    records it and hands it out itself next time. The mount manager
 *    database is the only place a letter lives, so mountvol and Disk
 *    Management work on these volumes as on NTFS ones;
 *  - when the driver stops it takes the interface down again and deletes
 *    the letter links at once, so nothing shows the volume as RAW.
 *
 * Everything runs on system worker threads, never inside the mount
 * itself: the mount manager and the file systems call each other.
 */

#include "ext4fs.h"
#include <initguid.h>
#include <mountmgr.h>
#include <wdmguid.h>
#include <ioevent.h>
#include <ntstrsafe.h>

#define LETTER_TAG          'TL2E'
#define FIRST_LETTER        L'D'

/* GUID_DEVINTERFACE_HIDDEN_VOLUME: volmgr registers it, instead of the
   volume interface, for partition types the mount manager ignores. Spelled
   out because ntddstor.h is already in before initguid.h can define it. */
static const GUID Ext2HiddenVolumeGuid =
    { 0x7f108a28L, 0x9833, 0x4b3b, { 0xb7, 0x80, 0x2c, 0x6b, 0x5f, 0xa5, 0xc0, 0x62 } };

typedef struct _EXT2_LETTER_WORK {
    PIO_WORKITEM        Item;
    UNICODE_STRING      Link;       /* volume interface link (probe) */
} EXT2_LETTER_WORK, *PEXT2_LETTER_WORK;

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2StartLetterService)
#pragma alloc_text(PAGE, Ext2StopLetterService)
#pragma alloc_text(PAGE, Ext2ReleaseLetter)
#endif

/*
 * Send an IOCTL to the mount manager. Buffered, synchronous.
 */
NTSTATUS
Ext2MountMgrIoctl(
    IN ULONG        Code,
    IN PVOID        Input,
    IN ULONG        InputLength,
    OUT PVOID       Output,
    IN ULONG        OutputLength,
    OUT PULONG      Returned
)
{
    UNICODE_STRING  Name;
    PFILE_OBJECT    FileObject = NULL;
    PDEVICE_OBJECT  DeviceObject = NULL;
    KEVENT          Event;
    IO_STATUS_BLOCK IoStatus;
    PIRP            Irp;
    NTSTATUS        Status;

    RtlInitUnicodeString(&Name, MOUNTMGR_DEVICE_NAME);
    Status = IoGetDeviceObjectPointer(&Name, FILE_READ_ATTRIBUTES,
                                      &FileObject, &DeviceObject);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoBuildDeviceIoControlRequest(Code, DeviceObject, Input, InputLength,
                                        Output, OutputLength, FALSE,
                                        &Event, &IoStatus);
    if (Irp == NULL) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
    } else {
        Status = IoCallDriver(DeviceObject, Irp);
        if (Status == STATUS_PENDING) {
            KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
            Status = IoStatus.Status;
        }
        if (Returned) {
            *Returned = (ULONG)IoStatus.Information;
        }
    }

    ObDereferenceObject(FileObject);
    return Status;
}

/*
 * The name the mount manager knows the volume by (\Device\HarddiskVolumeN),
 * asked from the top of the volume's own stack. The buffer is pool.
 */
static NTSTATUS
Ext2QueryVolumeDeviceName(
    IN PDEVICE_OBJECT   RealDevice,
    OUT PUNICODE_STRING Name
)
{
    PDEVICE_OBJECT  Top;
    PMOUNTDEV_NAME  MdName;
    ULONG           Length = sizeof(MOUNTDEV_NAME) + 256 * sizeof(WCHAR);
    NTSTATUS        Status;

    MdName = Ext2AllocatePool(NonPagedPool, Length, LETTER_TAG);
    if (MdName == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Top = IoGetAttachedDeviceReference(RealDevice);
    Status = Ext2DiskIoControl(Top, IOCTL_MOUNTDEV_QUERY_DEVICE_NAME,
                               NULL, 0, MdName, &Length);
    ObDereferenceObject(Top);

    if (NT_SUCCESS(Status) && MdName->NameLength > 0 &&
        MdName->NameLength <= 256 * sizeof(WCHAR)) {
        Name->Buffer = Ext2AllocatePool(NonPagedPool, MdName->NameLength, LETTER_TAG);
        if (Name->Buffer) {
            Name->Length = Name->MaximumLength = MdName->NameLength;
            RtlCopyMemory(Name->Buffer, MdName->Name, MdName->NameLength);
        } else {
            Status = STATUS_INSUFFICIENT_RESOURCES;
        }
    } else if (NT_SUCCESS(Status)) {
        Status = STATUS_UNSUCCESSFUL;
    }

    Ext2FreePool(MdName, LETTER_TAG);
    return Status;
}

/*
 * Where does \GLOBAL??\X: point? FALSE when there is no such link.
 * (The mount manager does not list points of devices it does not track,
 * and a hidden volume is exactly that, so the namespace is the truth.)
 */
static BOOLEAN
Ext2QueryLetter(IN WCHAR Letter, OUT PUNICODE_STRING Target)
{
    WCHAR               Buffer[16];
    UNICODE_STRING      Name;
    OBJECT_ATTRIBUTES   Attributes;
    HANDLE              Handle;
    ULONG               Length = 0;
    NTSTATUS            Status;

    RtlStringCbPrintfW(Buffer, sizeof(Buffer), L"\\GLOBAL??\\%c:", Letter);
    RtlInitUnicodeString(&Name, Buffer);
    InitializeObjectAttributes(&Attributes, &Name,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL, NULL);
    Status = ZwOpenSymbolicLinkObject(&Handle, SYMBOLIC_LINK_QUERY, &Attributes);
    if (!NT_SUCCESS(Status)) {
        return FALSE;
    }
    if (Target) {
        Target->Length = 0;
        Status = ZwQuerySymbolicLinkObject(Handle, Target, &Length);
        if (!NT_SUCCESS(Status)) {
            Target->Length = 0;
        }
    }
    ZwClose(Handle);
    return TRUE;
}

/*
 * Is there already a drive letter leading to this device?
 */
BOOLEAN
Ext2VolumeHasLetter(
    IN PUNICODE_STRING  DeviceName,
    OUT PWCHAR          Letter
)
{
    WCHAR           Buffer[128];
    UNICODE_STRING  Target;
    WCHAR           L;

    Target.Buffer = Buffer;
    Target.MaximumLength = sizeof(Buffer);

    for (L = L'A'; L <= L'Z'; L++) {
        if (Ext2QueryLetter(L, &Target) &&
            RtlEqualUnicodeString(&Target, DeviceName, TRUE)) {
            *Letter = L;
            return TRUE;
        }
    }
    return FALSE;
}

/*
 * A letter is taken when \GLOBAL??\X: exists, whoever created it.
 */
BOOLEAN
Ext2LetterInUse(IN WCHAR Letter)
{
    return Ext2QueryLetter(Letter, NULL);
}

/*
 * Create or delete \DosDevices\X: for a device through the mount manager.
 */
NTSTATUS
Ext2CreateLetter(IN WCHAR Letter, IN PUNICODE_STRING DeviceName)
{
    WCHAR                       Link[16];
    USHORT                      LinkLength;
    PMOUNTMGR_CREATE_POINT_INPUT In;
    ULONG                       InLen;
    NTSTATUS                    Status;

    RtlStringCbPrintfW(Link, sizeof(Link), L"\\DosDevices\\%c:", Letter);
    LinkLength = (USHORT)(wcslen(Link) * sizeof(WCHAR));

    InLen = sizeof(MOUNTMGR_CREATE_POINT_INPUT) + LinkLength + DeviceName->Length;
    In = Ext2AllocatePool(NonPagedPool, InLen, LETTER_TAG);
    if (In == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    In->SymbolicLinkNameOffset = sizeof(MOUNTMGR_CREATE_POINT_INPUT);
    In->SymbolicLinkNameLength = LinkLength;
    In->DeviceNameOffset = In->SymbolicLinkNameOffset + LinkLength;
    In->DeviceNameLength = DeviceName->Length;
    RtlCopyMemory((PUCHAR)In + In->SymbolicLinkNameOffset, Link, LinkLength);
    RtlCopyMemory((PUCHAR)In + In->DeviceNameOffset, DeviceName->Buffer,
                  DeviceName->Length);

    Status = Ext2MountMgrIoctl(IOCTL_MOUNTMGR_CREATE_POINT, In, InLen,
                               NULL, 0, NULL);
    Ext2FreePool(In, LETTER_TAG);
    return Status;
}

/*
 * Restore only the live DOS link for an assignment already present in the
 * Mount Manager database. Asking Mount Manager to create the point again can
 * be rejected by its one-letter-per-volume policy.
 */
static NTSTATUS
Ext2RestoreLetterLink(IN WCHAR Letter, IN PUNICODE_STRING DeviceName)
{
    WCHAR           Link[16];
    UNICODE_STRING  LinkName;

    RtlStringCbPrintfW(Link, sizeof(Link), L"\\DosDevices\\%c:", Letter);
    RtlInitUnicodeString(&LinkName, Link);
    return IoCreateSymbolicLink(&LinkName, DeviceName);
}

/*
 * Remove the letter link right away. The mount manager removes it too once
 * it has processed the interface going down, but that is a work item of
 * its own and the letter must be gone before handles are asked to close.
 * Its database entry stays: that is how the volume gets the same letter
 * next time (IOCTL_MOUNTMGR_DELETE_POINTS would forget it, like mountvol /D).
 */
static NTSTATUS
Ext2DeleteLetter(IN WCHAR Letter)
{
    WCHAR               Link[16];
    UNICODE_STRING      LinkName;

    RtlStringCbPrintfW(Link, sizeof(Link), L"\\DosDevices\\%c:", Letter);
    RtlInitUnicodeString(&LinkName, Link);
    return IoDeleteSymbolicLink(&LinkName);
}

/*
 * The PnP calls below take a PDO and nothing else (anything else is bug
 * check 0xCA, "invalid PDO"). A partition's volume has one at the bottom
 * of its stack; the disk device of an unlocked LUKS volume (crypt.c) has
 * no PnP node at all - the mount manager hears of it directly instead.
 */
static PDEVICE_OBJECT
Ext2VolumePdo(IN PDEVICE_OBJECT RealDevice)
{
    PDEVICE_OBJECT Pdo = IoGetDeviceAttachmentBaseRef(RealDevice);

    if (Pdo && (!IsFlagOn(Pdo->Flags, DO_BUS_ENUMERATED_DEVICE) || Ext4IsCryptDevice(Pdo))) {
        ObDereferenceObject(Pdo);
        Pdo = NULL;
    }
    return Pdo;
}

/*
 * Tell user mode (the shell above all) that a drive letter came or went.
 * The mount manager announces the letters it assigns itself with a
 * GUID_IO_VOLUME_NAME_CHANGE on the volume's PDO; for ours we do the same,
 * plus the mount/dismount event the file systems send.
 */
static VOID
Ext2AnnounceLetter(
    IN PFILE_OBJECT     Volume,
    IN PDEVICE_OBJECT   RealDevice,
    IN BOOLEAN          Arrived
)
{
    TARGET_DEVICE_CUSTOM_NOTIFICATION   Notification;
    PDEVICE_OBJECT                      Pdo;
    NTSTATUS                            Status;

    Status = FsRtlNotifyVolumeEvent(Volume, Arrived ? FSRTL_VOLUME_MOUNT :
                                                      FSRTL_VOLUME_DISMOUNT);
    DEBUG(DL_INF, ("Ext2AnnounceLetter: volume event %xh\n", Status));

    Pdo = Ext2VolumePdo(RealDevice);
    if (Pdo) {
        RtlZeroMemory(&Notification, sizeof(Notification));
        Notification.Version = 1;
        Notification.Size = sizeof(Notification);
        Notification.Event = GUID_IO_VOLUME_NAME_CHANGE;
        Notification.FileObject = NULL;
        Notification.NameBufferOffset = -1;
        Status = IoReportTargetDeviceChangeAsynchronous(Pdo, &Notification, NULL, NULL);
        DEBUG(DL_INF, ("Ext2AnnounceLetter: name change %xh\n", Status));
        ObDereferenceObject(Pdo);
    }
}

/*
 * Does the mount manager track this device (i.e. answer a query for its
 * mount points)? It does not for a hidden volume until Ext2AnnounceVolume.
 * A drive letter among the points is returned when there is one.
 */
static BOOLEAN
Ext2MountMgrTracks(IN PUNICODE_STRING DeviceName, OUT PWCHAR Letter)
{
    PMOUNTMGR_MOUNT_POINT   In = NULL;
    PMOUNTMGR_MOUNT_POINTS  Out = NULL;
    ULONG                   InLen, OutLen = 4096, i;
    NTSTATUS                Status;
    BOOLEAN                 Tracked = FALSE;

    *Letter = 0;
    InLen = sizeof(MOUNTMGR_MOUNT_POINT) + DeviceName->Length;
    In = Ext2AllocatePool(NonPagedPool, InLen, LETTER_TAG);
    Out = Ext2AllocatePool(NonPagedPool, OutLen, LETTER_TAG);
    if (!In || !Out) {
        goto out;
    }
    RtlZeroMemory(In, InLen);
    In->DeviceNameOffset = sizeof(MOUNTMGR_MOUNT_POINT);
    In->DeviceNameLength = DeviceName->Length;
    RtlCopyMemory((PUCHAR)In + In->DeviceNameOffset, DeviceName->Buffer, DeviceName->Length);

    Status = Ext2MountMgrIoctl(IOCTL_MOUNTMGR_QUERY_POINTS, In, InLen, Out, OutLen, NULL);
    if (Status == STATUS_BUFFER_OVERFLOW) {
        Tracked = TRUE;                 /* more points than fit: tracked all right */
    } else if (NT_SUCCESS(Status)) {
        Tracked = TRUE;
        for (i = 0; i < Out->NumberOfMountPoints; i++) {
            UNICODE_STRING Link;
            Link.Length = Link.MaximumLength = Out->MountPoints[i].SymbolicLinkNameLength;
            Link.Buffer = (PWCHAR)((PUCHAR)Out + Out->MountPoints[i].SymbolicLinkNameOffset);
            if (MOUNTMGR_IS_DRIVE_LETTER(&Link)) {
                *Letter = Link.Buffer[12];
                break;
            }
        }
    }
out:
    if (In)  Ext2FreePool(In, LETTER_TAG);
    if (Out) Ext2FreePool(Out, LETTER_TAG);
    return Tracked;
}

/*
 * Hand a hidden volume to the mount manager. volmgr registered only the
 * hidden-volume interface for it, so the mount manager never heard of the
 * device. With the mounted-device interface registered and enabled on the
 * very same PDO it treats the volume like any other: \\?\Volume{...} link,
 * mountvol, GetFinalPathNameByHandle, the shell's own notifications, and
 * it removes the links itself when the device goes away. The mount manager
 * processes the arrival on its own thread; wait, bounded, until it answers
 * for the device.
 */
static VOID
Ext2AnnounceVolume(IN PEXT2_VCB Vcb, IN PDEVICE_OBJECT RealDevice,
                   IN PUNICODE_STRING DeviceName, OUT PWCHAR Letter)
{
    PDEVICE_OBJECT  Pdo;
    UNICODE_STRING  Link = { 0, 0, NULL };
    LARGE_INTEGER   Tick;
    ULONG           i;
    NTSTATUS        Status;

    *Letter = 0;
    if (Ext2MountMgrTracks(DeviceName, Letter)) {
        return;                         /* a normal volume, or done before */
    }

    Pdo = Ext2VolumePdo(RealDevice);
    if (Pdo == NULL) {
        return;
    }
    Status = IoRegisterDeviceInterface(Pdo, &MOUNTDEV_MOUNTED_DEVICE_GUID, NULL, &Link);
    ObDereferenceObject(Pdo);
    if (!NT_SUCCESS(Status)) {
        DbgPrint("ext4: cannot register the volume interface for %wZ (%xh)\n",
                 DeviceName, Status);
        return;
    }
    Status = IoSetDeviceInterfaceState(&Link, TRUE);
    if (!NT_SUCCESS(Status) && Status != STATUS_OBJECT_NAME_EXISTS) {
        DbgPrint("ext4: cannot enable the volume interface for %wZ (%xh)\n",
                 DeviceName, Status);
        RtlFreeUnicodeString(&Link);
        return;
    }

    ExAcquireResourceExclusiveLite(&Ext2Global->Resource, TRUE);
    if (Vcb->MountDevLink.Buffer) {
        RtlFreeUnicodeString(&Vcb->MountDevLink);
    }
    Vcb->MountDevLink = Link;
    ExReleaseResourceLite(&Ext2Global->Resource);

    /* the arrival is processed on the mount manager's own thread; it
       creates the database letter in that same pass, so nothing may be
       decided before it answers for the device (up to 3 s, interruptible) */
    Tick.QuadPart = -20 * 10 * 1000;    /* 20 ms */
    for (i = 0; i < 150; i++) {
        if (Ext2Global->UnloadState != EXT2_UNLOAD_IDLE ||
            IsFlagOn(Ext2Global->Flags, EXT2_UNLOAD_PENDING)) {
            return;
        }
        if (Ext2MountMgrTracks(DeviceName, Letter)) {
            return;
        }
        KeDelayExecutionThread(KernelMode, FALSE, &Tick);
    }
    DbgPrint("ext4: the mount manager did not pick up %wZ\n", DeviceName);
}

/*
 * Worker: give a freshly mounted volume its drive letter.
 */
static VOID
Ext2AssignLetterWorker(IN PDEVICE_OBJECT DeviceObject, IN PVOID Context)
{
    PEXT2_VCB       Vcb = (PEXT2_VCB)DeviceObject->DeviceExtension;
    PIO_WORKITEM    Item = (PIO_WORKITEM)Context;
    PDEVICE_OBJECT  RealDevice = NULL;
    PFILE_OBJECT    Volume = NULL;
    UNICODE_STRING  DeviceName = { 0, 0, NULL };
    WCHAR           Letter = 0, Existing = 0, Live = 0, Candidate;
    BOOLEAN         Kept = FALSE;
    NTSTATUS        Status;

    /* the volume may have gone away while we were queued */
    ExAcquireResourceSharedLite(&Ext2Global->Resource, TRUE);
    if (IsMounted(Vcb) && !IsFlagOn(Vcb->Flags, VCB_DISMOUNT_PENDING) &&
        !IsFlagOn(Ext2Global->Flags, EXT2_UNLOAD_PENDING) &&
        Vcb->Vpb && Vcb->Vpb->RealDevice && Vcb->Volume && Vcb->SuperBlock) {
        RealDevice = Vcb->Vpb->RealDevice;
        ObReferenceObject(RealDevice);
        Volume = Vcb->Volume;
        ObReferenceObject(Volume);
    }
    ExReleaseResourceLite(&Ext2Global->Resource);

    if (RealDevice == NULL) {
        goto out;
    }

    Status = Ext2QueryVolumeDeviceName(RealDevice, &DeviceName);
    if (!NT_SUCCESS(Status)) {
        DbgPrint("ext4: cannot name volume %p for a drive letter (%xh)\n",
                 RealDevice, Status);
        goto out;
    }

    /* the mount manager first: its database may hold a letter for it */
    Ext2AnnounceVolume(Vcb, RealDevice, &DeviceName, &Existing);

    if (Ext2VolumeHasLetter(&DeviceName, &Live)) {
        /* A live DOS link already names this volume. */
        Letter = Live;
    } else {
        /* Mount Manager keeps assignments after the DOS link is removed.
           Recreate that link before considering a different free letter. */
        Status = STATUS_OBJECT_NAME_COLLISION;
        if (Existing && !Ext2LetterInUse(Existing)) {
            Status = Ext2RestoreLetterLink(Existing, &DeviceName);
            if (NT_SUCCESS(Status)) {
                Letter = Existing;
            }
        }

        /* Mount Manager may have completed the link concurrently. */
        if (Letter == 0 && Ext2VolumeHasLetter(&DeviceName, &Live)) {
            Letter = Live;
            Status = STATUS_SUCCESS;
        }

        /* A new volume, or an unavailable retained letter: use the first
           free letter. Two partitions may race, so retry on collisions. */
        for (Candidate = FIRST_LETTER; Letter == 0 && Candidate <= L'Z'; Candidate++) {
            if (Ext2LetterInUse(Candidate)) {
                continue;
            }
            Status = Ext2CreateLetter(Candidate, &DeviceName);
            if (NT_SUCCESS(Status)) {
                Letter = Candidate;
                break;
            }
            if (Status != STATUS_OBJECT_NAME_COLLISION &&
                Status != STATUS_OBJECT_NAME_EXISTS) {
                DbgPrint("ext4: mount manager refused %c: for %wZ (%xh)\n",
                         Candidate, &DeviceName, Status);
                break;
            }

            if (Ext2VolumeHasLetter(&DeviceName, &Live)) {
                Letter = Live;
                Status = STATUS_SUCCESS;
                break;
            }
        }
        if (!NT_SUCCESS(Status) || Letter == 0) {
            DbgPrint("ext4: no drive letter for %wZ (%xh)\n", &DeviceName, Status);
            Letter = 0;
            goto out;
        }
    }

    ExAcquireResourceExclusiveLite(&Ext2Global->Resource, TRUE);
    if (IsMounted(Vcb) && !IsFlagOn(Vcb->Flags, VCB_DEVICE_REMOVED)) {
        Vcb->DrvLetter = (UCHAR)Letter;
        SetLongFlag(Vcb->Flags, VCB_LETTER_ASSIGNED);
        Kept = TRUE;
    }
    ExReleaseResourceLite(&Ext2Global->Resource);

    if (!Kept) {
        /* gone while we were at it (unplugged right after arrival) */
        Ext2DeleteLetter(Letter);
        goto out;
    }

    DbgPrint("ext4: %wZ mounted as %c:\n", &DeviceName, Letter);

    /* let the shell and everybody else know there is a new drive */
    Ext2AnnounceLetter(Volume, RealDevice, TRUE);

out:
    if (DeviceName.Buffer) {
        Ext2FreePool(DeviceName.Buffer, LETTER_TAG);
    }
    if (Volume) {
        ObDereferenceObject(Volume);
    }
    if (RealDevice) {
        ObDereferenceObject(RealDevice);
    }
    IoFreeWorkItem(Item);
    ExReleaseRundownProtection(&Ext2Global->LetterRundown);
}

/*
 * Called by Ext2MountVolume once the volume is mounted.
 */
VOID
Ext2QueueLetterAssign(IN PEXT2_VCB Vcb)
{
    PIO_WORKITEM    Item;

    if (!ExAcquireRundownProtection(&Ext2Global->LetterRundown)) {
        return;
    }
    Item = IoAllocateWorkItem(Vcb->DeviceObject);
    if (Item == NULL) {
        ExReleaseRundownProtection(&Ext2Global->LetterRundown);
        return;
    }
    IoQueueWorkItem(Item, Ext2AssignLetterWorker, DelayedWorkQueue, Item);
}

/*
 * Take back the letter we created. Called with no resources held.
 *
 * Announce: tell the shell the drive is gone. Only when the driver stops.
 * From a PnP removal (the device is being unplugged) it must be FALSE: the
 * PnP manager holds the device tree while it delivers the removal, and a
 * target-device-change report from inside that waits for it - the removal
 * never finishes, the volume devices stay behind and the disk cannot even
 * be plugged in again. The removal itself reaches the shell through PnP.
 */
VOID
Ext2ReleaseLetter(IN PEXT2_VCB Vcb, IN BOOLEAN Announce)
{
    UNICODE_STRING  Link;
    UNICODE_STRING  DeviceName = { 0, 0, NULL };
    WCHAR           Letter = 0;
    NTSTATUS        Status;

    /* the interface we put on the volume's PDO goes with us when the
       driver stops - the mount manager then drops the device and every
       link it made for it (our letter included). On a PnP removal the
       PDO takes its interfaces away by itself, and touching the PnP
       state from inside the removal is not allowed anyway. */
    ExAcquireResourceExclusiveLite(&Ext2Global->Resource, TRUE);
    Link = Vcb->MountDevLink;
    RtlInitEmptyUnicodeString(&Vcb->MountDevLink, NULL, 0);
    ExReleaseResourceLite(&Ext2Global->Resource);
    if (Link.Buffer) {
        if (Announce) {
            IoSetDeviceInterfaceState(&Link, FALSE);
        }
        RtlFreeUnicodeString(&Link);
    }

    if (!IsFlagOn(Vcb->Flags, VCB_LETTER_ASSIGNED) || Vcb->DrvLetter == 0) {
        return;
    }

    /* also by hand, so that the letter is gone before handles are asked to
       close - but only while it still leads to this volume: mountvol may
       have moved it to another one since, and that one keeps it. Lives
       with the mount manager doing the same a moment later. */
    if (Vcb->Vpb && Vcb->Vpb->RealDevice &&
        NT_SUCCESS(Ext2QueryVolumeDeviceName(Vcb->Vpb->RealDevice, &DeviceName))) {
        if (Ext2VolumeHasLetter(&DeviceName, &Letter) && Letter == (WCHAR)Vcb->DrvLetter) {
            Status = Ext2DeleteLetter(Letter);
            DbgPrint("ext4: %c: released (%xh)\n", Letter, Status);
        }
        Ext2FreePool(DeviceName.Buffer, LETTER_TAG);
    }

    if (Announce && Vcb->Volume && Vcb->Vpb && Vcb->Vpb->RealDevice) {
        Ext2AnnounceLetter(Vcb->Volume, Vcb->Vpb->RealDevice, FALSE);
    }

    ClearLongFlag(Vcb->Flags, VCB_LETTER_ASSIGNED);
    Vcb->DrvLetter = 0;
}

/*
 * Worker: open a volume once so that the I/O manager asks the file systems
 * to mount it. Ours takes it if it is ext2/3/4 (Ext2MountVolume), and the
 * mount queues the letter assignment above.
 */
static VOID
Ext2ProbeVolumeWorker(IN PDEVICE_OBJECT DeviceObject, IN PVOID Context)
{
    PEXT2_LETTER_WORK   Work = (PEXT2_LETTER_WORK)Context;
    PFILE_OBJECT        FileObject = NULL;
    PDEVICE_OBJECT      VolumeDevice = NULL;
    OBJECT_ATTRIBUTES   Attributes;
    IO_STATUS_BLOCK     IoStatus;
    HANDLE              Handle;
    UNICODE_STRING      DeviceName = { 0, 0, NULL };
    UNICODE_STRING      ProbePath;
    PUNICODE_STRING     ProbeName = &Work->Link;
    WCHAR               ProbeBuffer[20];
    WCHAR               Existing = 0, Live = 0;
    BOOLEAN             LinkCreated = FALSE;
    NTSTATUS            Status;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (Ext2Global->UnloadState == EXT2_UNLOAD_IDLE &&
        !IsFlagOn(Ext2Global->Flags, EXT2_UNLOAD_PENDING)) {

        /* A normal partition can keep its letter in the Mount Manager
           database after our unload-time DOS link removal. Without a live
           link there is no path whose first open asks file systems to mount
           the volume. Restore the recorded link before probing it. */
        Status = IoGetDeviceObjectPointer(&Work->Link, FILE_READ_ATTRIBUTES,
                                          &FileObject, &VolumeDevice);
        if (NT_SUCCESS(Status) &&
            NT_SUCCESS(Ext2QueryVolumeDeviceName(VolumeDevice, &DeviceName)) &&
            Ext2MountMgrTracks(&DeviceName, &Existing) && Existing) {

            if (Ext2VolumeHasLetter(&DeviceName, &Live)) {
                Existing = Live;
            } else if (!Ext2LetterInUse(Existing)) {
                Status = Ext2RestoreLetterLink(Existing, &DeviceName);
                if (NT_SUCCESS(Status)) {
                    LinkCreated = TRUE;
                }
            }

            if (LinkCreated ||
                Ext2VolumeHasLetter(&DeviceName, &Live)) {
                RtlStringCbPrintfW(ProbeBuffer, sizeof(ProbeBuffer),
                                   L"\\GLOBAL??\\%c:\\", Existing);
                RtlInitUnicodeString(&ProbePath, ProbeBuffer);
                ProbeName = &ProbePath;
            }
        }
        if (FileObject) {
            ObDereferenceObject(FileObject);
        }

        InitializeObjectAttributes(&Attributes, ProbeName,
                                   OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                                   NULL, NULL);
        /* FILE_READ_DATA: an attributes-only open would bypass the mount. */
        Status = ZwCreateFile(&Handle, FILE_READ_DATA | SYNCHRONIZE,
                              &Attributes, &IoStatus, NULL,
                              FILE_ATTRIBUTE_NORMAL,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              FILE_OPEN,
                              FILE_SYNCHRONOUS_IO_NONALERT |
                                  (ProbeName == &ProbePath ? FILE_DIRECTORY_FILE :
                                                            FILE_NON_DIRECTORY_FILE),
                              NULL, 0);
        if (NT_SUCCESS(Status)) {
            ZwClose(Handle);
        } else if (LinkCreated) {
            Ext2DeleteLetter(Existing);
        }
        DEBUG(DL_INF, ("Ext2ProbeVolumeWorker: %wZ -> %xh\n", ProbeName, Status));

        if (DeviceName.Buffer) {
            Ext2FreePool(DeviceName.Buffer, LETTER_TAG);
        }
    }

    IoFreeWorkItem(Work->Item);
    Ext2FreePool(Work->Link.Buffer, LETTER_TAG);
    Ext2FreePool(Work, LETTER_TAG);
    ExReleaseRundownProtection(&Ext2Global->LetterRundown);
}

/*
 * Open the volume Name once on a system worker, so that the I/O manager
 * offers it to the file systems. Used for PnP arrivals and for the disk
 * devices of unlocked LUKS volumes (crypt.c).
 */
VOID
Ext2QueueVolumeProbe(IN PCUNICODE_STRING Name)
{
    PEXT2_LETTER_WORK   Work;

    if (Name == NULL || Name->Length == 0) {
        return;
    }

    if (Ext2Global->UnloadState != EXT2_UNLOAD_IDLE ||
        IsFlagOn(Ext2Global->Flags, EXT2_UNLOAD_PENDING) ||
        !ExAcquireRundownProtection(&Ext2Global->LetterRundown)) {
        return;
    }

    Work = Ext2AllocatePool(NonPagedPool, sizeof(EXT2_LETTER_WORK), LETTER_TAG);
    if (Work == NULL) {
        goto fail;
    }
    RtlZeroMemory(Work, sizeof(EXT2_LETTER_WORK));

    Work->Link.Length = Work->Link.MaximumLength = Name->Length;
    Work->Link.Buffer = Ext2AllocatePool(NonPagedPool, Work->Link.Length, LETTER_TAG);
    if (Work->Link.Buffer == NULL) {
        goto fail;
    }
    RtlCopyMemory(Work->Link.Buffer, Name->Buffer, Work->Link.Length);

    Work->Item = IoAllocateWorkItem(Ext2Global->DiskdevObject);
    if (Work->Item == NULL) {
        goto fail;
    }

    IoQueueWorkItem(Work->Item, Ext2ProbeVolumeWorker, DelayedWorkQueue, Work);
    return;

fail:
    if (Work) {
        if (Work->Link.Buffer) {
            Ext2FreePool(Work->Link.Buffer, LETTER_TAG);
        }
        Ext2FreePool(Work, LETTER_TAG);
    }
    ExReleaseRundownProtection(&Ext2Global->LetterRundown);
}

/*
 * PnP: a volume device came (or went). Arrivals are probed on a worker.
 */
static NTSTATUS
Ext2VolumeInterfaceNotify(IN PVOID NotificationStructure, IN PVOID Context)
{
    PDEVICE_INTERFACE_CHANGE_NOTIFICATION Notification =
        (PDEVICE_INTERFACE_CHANGE_NOTIFICATION)NotificationStructure;

    UNREFERENCED_PARAMETER(Context);

    if (IsEqualGUID(&Notification->Event, &GUID_DEVICE_INTERFACE_ARRIVAL)) {
        Ext2QueueVolumeProbe(Notification->SymbolicLinkName);
    }
    return STATUS_SUCCESS;
}

/*
 * DriverEntry: start listening. Existing volumes are reported right away,
 * on this thread, so the file systems must already be registered.
 */
NTSTATUS
Ext2StartLetterService(IN PDRIVER_OBJECT DriverObject)
{
    NTSTATUS    Status;

    ExInitializeRundownProtection(&Ext2Global->LetterRundown);
    Ext2Global->LetterStopping = FALSE;
    KeInitializeEvent(&Ext2Global->LetterStopped, NotificationEvent, FALSE);

    /* Two interface classes: volmgr registers the volume interface for the
       partition types the mount manager handles, and the hidden-volume
       interface for the ones it does not (0x83, the Linux GPT GUID,
       MSR ...). The latter is where every ext volume without a letter
       shows up. */
    Status = IoRegisterPlugPlayNotification(
                 EventCategoryDeviceInterfaceChange,
                 PNPNOTIFY_DEVICE_INTERFACE_INCLUDE_EXISTING_INTERFACES,
                 (PVOID)&Ext2HiddenVolumeGuid,
                 DriverObject,
                 Ext2VolumeInterfaceNotify,
                 NULL,
                 &Ext2Global->PartitionNotifyEntry);
    if (!NT_SUCCESS(Status)) {
        DbgPrint("ext4: cannot watch for hidden volumes (%xh): ext "
                 "volumes get no drive letters\n", Status);
        Ext2Global->PartitionNotifyEntry = NULL;
    }

    Status = IoRegisterPlugPlayNotification(
                 EventCategoryDeviceInterfaceChange,
                 PNPNOTIFY_DEVICE_INTERFACE_INCLUDE_EXISTING_INTERFACES,
                 (PVOID)&MOUNTDEV_MOUNTED_DEVICE_GUID,
                 DriverObject,
                 Ext2VolumeInterfaceNotify,
                 NULL,
                 &Ext2Global->VolumeNotifyEntry);
    if (!NT_SUCCESS(Status)) {
        DbgPrint("ext4: cannot watch for volumes (%xh)\n", Status);
        Ext2Global->VolumeNotifyEntry = NULL;
    }

    return Status;
}

/*
 * Stop listening and wait for the workers. Safe to call more than once.
 */
VOID
Ext2StopLetterService(VOID)
{
    if (Ext2Global->PartitionNotifyEntry) {
        IoUnregisterPlugPlayNotificationEx(Ext2Global->PartitionNotifyEntry);
        Ext2Global->PartitionNotifyEntry = NULL;
    }
    if (Ext2Global->VolumeNotifyEntry) {
        IoUnregisterPlugPlayNotificationEx(Ext2Global->VolumeNotifyEntry);
        Ext2Global->VolumeNotifyEntry = NULL;
    }
    /* Once only: a rundown that had to wait for a holder is left pointing
       at the waiter's stack block, and a second wait on it never returns.
       Prepare-to-unload stops the service and DriverUnload does it again;
       whoever comes later waits for the first one to finish. */
    if (InterlockedCompareExchange(&Ext2Global->LetterStopping, TRUE, FALSE) == FALSE) {
        ExWaitForRundownProtectionRelease(&Ext2Global->LetterRundown);
        KeSetEvent(&Ext2Global->LetterStopped, IO_NO_INCREMENT, FALSE);
    } else {
        KeWaitForSingleObject(&Ext2Global->LetterStopped, Executive, KernelMode,
                              FALSE, NULL);
    }
}
