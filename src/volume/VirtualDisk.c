/**
 * VirtualDisk.c - the disk side of the driver's own devices (LUKS, LVM).
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * A device of ours that a file system mounts on has to answer as a disk:
 * geometry, length, partition information, its name and unique id for the
 * mount manager (Ext4DiskIoctl). Having no PnP node, it is announced to
 * the mount manager by name, given a drive letter, and its records are
 * deleted again when it goes; a file system on it is dismounted the way
 * mountvol /P does.
 */

#include "ext4fs.h"
#include <mountdev.h>
#include <mountmgr.h>
#include <ntddvol.h>
#include <ntstrsafe.h>
#include "ext4crypt.h"

#define VDISK_TAG               'DV4E'
#define EXT4_PARTITION_LINUX    0x83                /* what the device reports as its type */
#define EXT4_PREFIX_CHARS       32                  /* room for any unique id prefix */
#define EXT4_UNIQUE_ID_CHARS    (EXT4_PREFIX_CHARS + EXT4_CRYPT_UUID_CHARS)
#define EXT4_LINK_CHARS         128                 /* \DosDevices\X:, \??\Volume{GUID} */
#define EXT4_POINTS_REPLY       1024                /* DELETE_POINTS echoes what it deleted */
#define EXT4_POINTS_FIRST       (16 * 1024)         /* QUERY_POINTS, first guess of its size */
#define EXT4_POINTS_TRIES       4                   /* the list may grow between two asks */

/* ---------------------------------------------------------------- disk */

NTSTATUS
Ext4DiskComplete(IN PIRP Irp, IN NTSTATUS Status, IN ULONG_PTR Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_DISK_INCREMENT);
    return Status;
}

static NTSTATUS
Ext4DiskCopyOut(IN PIRP Irp, IN const VOID *Data, IN ULONG Length)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);

    if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength < Length) {
        return Ext4DiskComplete(Irp, STATUS_BUFFER_TOO_SMALL, 0);
    }
    RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, Data, Length);
    return Ext4DiskComplete(Irp, STATUS_SUCCESS, Length);
}

/*
 * MOUNTDEV_NAME and MOUNTDEV_UNIQUE_ID share their layout (a USHORT length
 * and the bytes): a buffer too short for the bytes gets the length and
 * STATUS_BUFFER_OVERFLOW, so that the mount manager can ask again.
 */
static NTSTATUS
Ext4DiskVariable(IN PIRP Irp, IN const VOID *Data, IN USHORT Length)
{
    PIO_STACK_LOCATION  IrpSp = IoGetCurrentIrpStackLocation(Irp);
    ULONG               Out = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
    PMOUNTDEV_NAME      Name = (PMOUNTDEV_NAME)Irp->AssociatedIrp.SystemBuffer;
    ULONG               Needed = FIELD_OFFSET(MOUNTDEV_NAME, Name) + Length;

    if (Out < sizeof(MOUNTDEV_NAME)) {
        return Ext4DiskComplete(Irp, STATUS_INVALID_PARAMETER, 0);
    }
    Name->NameLength = Length;
    if (Out < Needed) {
        return Ext4DiskComplete(Irp, STATUS_BUFFER_OVERFLOW, sizeof(MOUNTDEV_NAME));
    }
    RtlCopyMemory(Name->Name, Data, Length);
    return Ext4DiskComplete(Irp, STATUS_SUCCESS, Needed);
}

/*
 * The device controls of the driver's own disk devices (LUKS and LVM):
 * what a file system and the mount manager ask a disk. Name: the NT device
 * name; the unique id, UniquePrefix and Uuid, stays the same for the same
 * volume at every unlock, so the mount manager remembers its letter.
 */
NTSTATUS
Ext4DiskIoctl(IN PIRP Irp, IN PCUNICODE_STRING Name, IN PCSTR UniquePrefix, IN PCSTR Uuid,
              IN ULONGLONG Size, IN BOOLEAN ReadOnly)
{
    PIO_STACK_LOCATION  IrpSp = IoGetCurrentIrpStackLocation(Irp);
    ULONG               Code = IrpSp->Parameters.DeviceIoControl.IoControlCode;

    switch (Code) {

    case IOCTL_DISK_GET_DRIVE_GEOMETRY:
    case IOCTL_DISK_GET_DRIVE_GEOMETRY_EX: {
        DISK_GEOMETRY_EX Geometry;

        /* 512-byte logical sectors whatever the encryption sector (512e):
           the device takes any 512-aligned request (a part of a larger
           encryption sector is read, merged and rewritten), and file
           systems written for 512-byte sectors - ext4.sys among them -
           round paging I/O up to the sector size they are given, past
           the end of the caller's buffer at 4096. One sector per track:
           every sector is addressable, whatever the size. */
        RtlZeroMemory(&Geometry, sizeof(Geometry));
        Geometry.Geometry.Cylinders.QuadPart = (LONGLONG)(Size >> EXT4_SECTOR_SHIFT);
        Geometry.Geometry.MediaType = FixedMedia;
        Geometry.Geometry.TracksPerCylinder = 1;
        Geometry.Geometry.SectorsPerTrack = 1;
        Geometry.Geometry.BytesPerSector = EXT4_SECTOR;
        Geometry.DiskSize.QuadPart = (LONGLONG)Size;
        return Ext4DiskCopyOut(Irp, &Geometry, Code == IOCTL_DISK_GET_DRIVE_GEOMETRY ?
                               sizeof(DISK_GEOMETRY) : FIELD_OFFSET(DISK_GEOMETRY_EX, Data));
    }

    case IOCTL_DISK_GET_LENGTH_INFO: {
        GET_LENGTH_INFORMATION Info;
        Info.Length.QuadPart = (LONGLONG)Size;
        return Ext4DiskCopyOut(Irp, &Info, sizeof(Info));
    }

    case IOCTL_DISK_GET_PARTITION_INFO: {
        PARTITION_INFORMATION Info;
        RtlZeroMemory(&Info, sizeof(Info));
        Info.PartitionLength.QuadPart = (LONGLONG)Size;
        Info.PartitionNumber = 1;
        Info.PartitionType = EXT4_PARTITION_LINUX;
        Info.RecognizedPartition = TRUE;
        return Ext4DiskCopyOut(Irp, &Info, sizeof(Info));
    }

    case IOCTL_DISK_GET_PARTITION_INFO_EX: {
        PARTITION_INFORMATION_EX Info;
        RtlZeroMemory(&Info, sizeof(Info));
        Info.PartitionStyle = PARTITION_STYLE_MBR;
        Info.PartitionLength.QuadPart = (LONGLONG)Size;
        Info.PartitionNumber = 1;
        Info.Mbr.PartitionType = EXT4_PARTITION_LINUX;
        Info.Mbr.RecognizedPartition = TRUE;
        return Ext4DiskCopyOut(Irp, &Info, sizeof(Info));
    }

    case IOCTL_DISK_IS_WRITABLE:
        return Ext4DiskComplete(Irp, ReadOnly ? STATUS_MEDIA_WRITE_PROTECTED : STATUS_SUCCESS, 0);

    case IOCTL_DISK_CHECK_VERIFY:
    case IOCTL_STORAGE_CHECK_VERIFY:
    case IOCTL_STORAGE_CHECK_VERIFY2:
        if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(ULONG)) {
            *(PULONG)Irp->AssociatedIrp.SystemBuffer = 0;       /* media change count */
            return Ext4DiskComplete(Irp, STATUS_SUCCESS, sizeof(ULONG));
        }
        return Ext4DiskComplete(Irp, STATUS_SUCCESS, 0);

    case IOCTL_DISK_MEDIA_REMOVAL:
    case IOCTL_STORAGE_MEDIA_REMOVAL:
    case IOCTL_VOLUME_ONLINE:
    case IOCTL_MOUNTDEV_LINK_CREATED:
    case IOCTL_MOUNTDEV_LINK_DELETED:
        return Ext4DiskComplete(Irp, STATUS_SUCCESS, 0);

    case IOCTL_STORAGE_GET_HOTPLUG_INFO: {
        STORAGE_HOTPLUG_INFO Info;
        RtlZeroMemory(&Info, sizeof(Info));
        Info.Size = sizeof(Info);
        return Ext4DiskCopyOut(Irp, &Info, sizeof(Info));
    }

    case IOCTL_MOUNTDEV_QUERY_DEVICE_NAME:
        return Ext4DiskVariable(Irp, Name->Buffer, Name->Length);

    case IOCTL_MOUNTDEV_QUERY_UNIQUE_ID: {
        CHAR    Id[EXT4_UNIQUE_ID_CHARS];
        size_t  Length = 0;
        RtlStringCbPrintfA(Id, sizeof(Id), "%s%s", UniquePrefix, Uuid);
        RtlStringCbLengthA(Id, sizeof(Id), &Length);
        return Ext4DiskVariable(Irp, Id, (USHORT)Length);
    }

    default:
        /* SUGGESTED_LINK_NAME, STABLE_GUID, UNIQUE_ID_CHANGE_NOTIFY, disk
           extents, storage properties: nothing to say about them */
        return Ext4DiskComplete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

/* ---------------------------------------------------------------- mount manager */

/* Is a file system mounted on the device (ours, RAW, any other)? */
static BOOLEAN
Ext4DeviceMounted(IN PDEVICE_OBJECT Device)
{
    KIRQL   Irql;
    BOOLEAN Mounted;

    IoAcquireVpbSpinLock(&Irql);
    Mounted = Device->Vpb != NULL && IsFlagOn(Device->Vpb->Flags, VPB_MOUNTED);
    IoReleaseVpbSpinLock(Irql);
    return Mounted;
}

/*
 * Take the file system off a device the way mountvol /P does: open the
 * volume, lock it (every other handle must be gone, unless Force), dismount.
 */
NTSTATUS
Ext4DismountDevice(IN PDEVICE_OBJECT Device, IN PCUNICODE_STRING Name, IN BOOLEAN Force)
{
    OBJECT_ATTRIBUTES   Attributes;
    IO_STATUS_BLOCK     IoStatus;
    HANDLE              Handle;
    NTSTATUS            Status;

    if (!Ext4DeviceMounted(Device)) {
        return STATUS_SUCCESS;
    }

    InitializeObjectAttributes(&Attributes, (PUNICODE_STRING)Name,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwCreateFile(&Handle, FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE,
                          &Attributes, &IoStatus, NULL, 0,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    if (Status == STATUS_MEDIA_WRITE_PROTECTED || Status == STATUS_ACCESS_DENIED) {
        /* a read-only volume: reading is enough to lock and dismount it */
        Status = ZwCreateFile(&Handle, FILE_READ_DATA | SYNCHRONIZE,
                              &Attributes, &IoStatus, NULL, 0,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    }
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Status = ZwFsControlFile(Handle, NULL, NULL, NULL, &IoStatus,
                             FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0);
    if (!NT_SUCCESS(Status) && !Force) {
        ZwClose(Handle);
        return STATUS_DEVICE_BUSY;
    }

    Status = ZwFsControlFile(Handle, NULL, NULL, NULL, &IoStatus,
                             FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0);
    ZwClose(Handle);
    return Status;
}

/* Tell the mount manager a device came: it gives it its links. */
NTSTATUS
Ext4AnnounceDevice(IN PCUNICODE_STRING Name)
{
    UCHAR                   Buffer[sizeof(MOUNTMGR_TARGET_NAME) + 64 * sizeof(WCHAR)];
    PMOUNTMGR_TARGET_NAME   Target = (PMOUNTMGR_TARGET_NAME)Buffer;

    if (Name->Length > sizeof(Buffer) - sizeof(MOUNTMGR_TARGET_NAME)) {
        return STATUS_NAME_TOO_LONG;
    }
    Target->DeviceNameLength = Name->Length;
    RtlCopyMemory(Target->DeviceName, Name->Buffer, Name->Length);
    return Ext2MountMgrIoctl(IOCTL_MOUNTMGR_VOLUME_ARRIVAL_NOTIFICATION, Target,
                             FIELD_OFFSET(MOUNTMGR_TARGET_NAME, DeviceName) + Name->Length,
                             NULL, 0, NULL);
}

/*
 * Remove one link the mount manager made (a drive letter, a volume name)
 * and its record of it. The mount manager drops the record even when the
 * link object itself is gone already and it answers STATUS_OBJECT_NAME_NOT_FOUND.
 */
NTSTATUS
Ext4DeleteDevicePoints(IN PCUNICODE_STRING Link)
{
    UCHAR                   In[sizeof(MOUNTMGR_MOUNT_POINT) + EXT4_LINK_CHARS * sizeof(WCHAR)];
    UCHAR                   Out[sizeof(MOUNTMGR_MOUNT_POINTS) + EXT4_POINTS_REPLY];
    PMOUNTMGR_MOUNT_POINT   Point = (PMOUNTMGR_MOUNT_POINT)In;

    if (Link->Length > EXT4_LINK_CHARS * sizeof(WCHAR)) {
        return STATUS_NAME_TOO_LONG;
    }
    RtlZeroMemory(In, sizeof(In));
    Point->SymbolicLinkNameOffset = sizeof(MOUNTMGR_MOUNT_POINT);
    Point->SymbolicLinkNameLength = Link->Length;
    RtlCopyMemory(In + sizeof(MOUNTMGR_MOUNT_POINT), Link->Buffer, Link->Length);
    return Ext2MountMgrIoctl(IOCTL_MOUNTMGR_DELETE_POINTS, In,
                             sizeof(MOUNTMGR_MOUNT_POINT) + Link->Length,
                             Out, sizeof(Out), NULL);
}

/*
 * Make the mount manager forget a device name: every link it holds for it
 * goes, one by one. Found in the list of all its points, never by asking
 * for the device name - for that the mount manager opens the device, and
 * while the driver unloads the I/O manager fails every open of its devices.
 * Named devices have no PnP removal the mount manager would hear of, so a
 * record it keeps outlives the device; the next device of the same name
 * (numbers start at 0 again after a restart of the driver) then counts as
 * known and gets no drive letter. Called when a device goes, and before a
 * new one is announced, for the records an earlier instance left behind.
 */
VOID
Ext4ForgetDevice(IN PCUNICODE_STRING DeviceName)
{
    MOUNTMGR_MOUNT_POINT    All;
    PMOUNTMGR_MOUNT_POINTS  Points = NULL;
    ULONG                   Size = EXT4_POINTS_FIRST, Try, i;
    NTSTATUS                Status = STATUS_BUFFER_OVERFLOW;

    PAGED_CODE();

    RtlZeroMemory(&All, sizeof(All));
    for (Try = 0; Try < EXT4_POINTS_TRIES && Status == STATUS_BUFFER_OVERFLOW; Try++) {
        if (Points) {
            Size = max(Points->Size, Size * 2);
            Ext2FreePool(Points, VDISK_TAG);
        }
        Points = Ext2AllocatePool(PagedPool, Size, VDISK_TAG);
        if (Points == NULL) {
            return;
        }
        Status = Ext2MountMgrIoctl(IOCTL_MOUNTMGR_QUERY_POINTS, &All, sizeof(All),
                                   Points, Size, NULL);
    }
    if (!NT_SUCCESS(Status)) {
        Ext2FreePool(Points, VDISK_TAG);
        return;
    }

    for (i = 0; i < Points->NumberOfMountPoints; i++) {
        PMOUNTMGR_MOUNT_POINT   P = &Points->MountPoints[i];
        UNICODE_STRING          Device, Link;

        if (P->DeviceNameLength == 0 || P->SymbolicLinkNameLength == 0 ||
            (ULONGLONG)P->DeviceNameOffset + P->DeviceNameLength > Size ||
            (ULONGLONG)P->SymbolicLinkNameOffset + P->SymbolicLinkNameLength > Size) {
            continue;
        }
        Device.Buffer = (PWCH)((PUCHAR)Points + P->DeviceNameOffset);
        Device.Length = Device.MaximumLength = P->DeviceNameLength;
        if (!RtlEqualUnicodeString(&Device, DeviceName, TRUE)) {
            continue;
        }
        Link.Buffer = (PWCH)((PUCHAR)Points + P->SymbolicLinkNameOffset);
        Link.Length = Link.MaximumLength = P->SymbolicLinkNameLength;
        Ext4DeleteDevicePoints(&Link);
    }
    Ext2FreePool(Points, VDISK_TAG);
}

/*
 * Give a device the letter asked for, if it is free. The mount manager may
 * have assigned one itself on arrival; that one is replaced.
 */
VOID
Ext4SetDeviceLetter(IN PCUNICODE_STRING Name, IN WCHAR Wanted)
{
    WCHAR   Current = 0;

    if (Wanted >= L'a' && Wanted <= L'z') {
        Wanted = (WCHAR)(Wanted - L'a' + L'A');
    }
    if (Wanted < L'A' || Wanted > L'Z') {
        return;
    }
    if (Ext2VolumeHasLetter((PUNICODE_STRING)Name, &Current) && Current == Wanted) {
        return;
    }
    if (Ext2LetterInUse(Wanted)) {
        DbgPrint("ext4: %c: is taken, %wZ keeps the letter it has\n", Wanted, Name);
        return;
    }
    if (Current) {
        WCHAR           LinkBuffer[16];
        UNICODE_STRING  Link;
        RtlStringCbPrintfW(LinkBuffer, sizeof(LinkBuffer), L"\\DosDevices\\%c:", Current);
        RtlInitUnicodeString(&Link, LinkBuffer);
        Ext4DeleteDevicePoints(&Link);
    }
    Ext2CreateLetter(Wanted, (PUNICODE_STRING)Name);
}
