/**
 * ext4fs_volume.h - mount, dismount, VCB life cycle, volume locking and drive letters.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _EXT4_EXT4FS_VOLUME_H_
#define _EXT4_EXT4FS_VOLUME_H_

NTSTATUS
Ext2StartLetterService (IN PDRIVER_OBJECT DriverObject);

VOID
Ext2StopLetterService (VOID);

VOID
Ext2QueueLetterAssign (IN PEXT2_VCB Vcb);

VOID
Ext2ReleaseLetter (IN PEXT2_VCB Vcb, IN BOOLEAN Announce);

VOID
Ext2QueueVolumeProbe (IN PCUNICODE_STRING Name);

NTSTATUS
Ext2MountMgrIoctl (IN ULONG Code, IN PVOID Input, IN ULONG InputLength,
                   OUT PVOID Output, IN ULONG OutputLength, OUT PULONG Returned);

BOOLEAN
Ext2VolumeHasLetter (IN PUNICODE_STRING DeviceName, OUT PWCHAR Letter);

BOOLEAN
Ext2LetterInUse (IN WCHAR Letter);

NTSTATUS
Ext2CreateLetter (IN WCHAR Letter, IN PUNICODE_STRING DeviceName);

//
// LUKS volumes (crypt.c)
//

VOID
Ext4CryptInitialize (VOID);

BOOLEAN
Ext4IsCryptDevice (IN PDEVICE_OBJECT DeviceObject);

NTSTATUS
Ext4CryptDispatch (IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp);

BOOLEAN
Ext4IsCryptControl (IN ULONG IoControlCode);

NTSTATUS
Ext4CryptControl (IN PIRP Irp);

VOID
Ext4CryptTeardownAll (VOID);

PDEVICE_OBJECT
Ext4CryptReference (IN ULONG Number, OUT PULONGLONG Size);

/* shared by the LUKS and the LVM disk devices */
NTSTATUS
Ext4DiskComplete (IN PIRP Irp, IN NTSTATUS Status, IN ULONG_PTR Information);

NTSTATUS
Ext4DiskIoctl (IN PIRP Irp, IN PCUNICODE_STRING Name, IN PCSTR UniquePrefix, IN PCSTR Uuid,
               IN ULONGLONG Size, IN BOOLEAN ReadOnly);

NTSTATUS
Ext4DismountDevice (IN PDEVICE_OBJECT Device, IN PCUNICODE_STRING Name, IN BOOLEAN Force);

NTSTATUS
Ext4AnnounceDevice (IN PCUNICODE_STRING Name);

NTSTATUS
Ext4DeleteDevicePoints (IN PCUNICODE_STRING Link);

VOID
Ext4ForgetDevice (IN PCUNICODE_STRING DeviceName);

VOID
Ext4SetDeviceLetter (IN PCUNICODE_STRING Name, IN WCHAR Wanted);

//
// LVM volumes inside LUKS (lvm.c); called under the LUKS control lock
//

struct _EXT4_LV_OPEN;
struct _EXT4_LV_CLOSE;
struct _EXT4_LV_QUERY;

BOOLEAN
Ext4IsLvDevice (IN PDEVICE_OBJECT DeviceObject);

NTSTATUS
Ext4LvDispatch (IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp);

NTSTATUS
Ext4LvOpen (IN OUT struct _EXT4_LV_OPEN *In, IN ULONG InLength);

NTSTATUS
Ext4LvClose (IN struct _EXT4_LV_CLOSE *In);

NTSTATUS
Ext4LvQuery (OUT struct _EXT4_LV_QUERY *Out);

BOOLEAN
Ext4LvUsing (IN ULONG Crypt);

VOID
Ext4LvCloseOn (IN ULONG Crypt);

VOID
Ext4LvTeardownAll (VOID);

/* multi-mount protection (mmp.c) */
NTSTATUS
Ext4MmpStart (IN PEXT2_VCB Vcb);

VOID
Ext4MmpStop (IN PEXT2_VCB Vcb, IN BOOLEAN Release);

BOOLEAN
Ext4MmpHold (IN PEXT2_VCB Vcb);

/* the gate of the driver's own IOCTLs: kernel or administrator, a header */
NTSTATUS
Ext2CheckAppIoctl (IN PIRP Irp, IN ULONG Code);

//
// MountPoint process workitem
//

VOID
Ext2SetVpbFlag (IN PVPB     Vpb,
                IN USHORT   Flag );

VOID
Ext2ClearVpbFlag (IN PVPB     Vpb,
                  IN USHORT   Flag );

BOOLEAN
Ext2GiveVpbBack(IN PVPB Old, IN PVPB Swap);
BOOLEAN
Ext2DeferVpb(IN PVPB Old, IN PVPB Swap);
VOID
Ext2ReclaimVpbs(VOID);

BOOLEAN
Ext2CheckDismount (
    IN PEXT2_IRP_CONTEXT IrpContext,
    IN PEXT2_VCB         Vcb,
    IN BOOLEAN           bForce   );

NTSTATUS
Ext2PurgeVolume (IN PEXT2_VCB Vcb,
                 IN BOOLEAN  FlushBeforePurge);

NTSTATUS
Ext2PurgeFile (IN PEXT2_FCB Fcb,
               IN BOOLEAN  FlushBeforePurge);

BOOLEAN
Ext2IsHandleCountZero(IN PEXT2_VCB Vcb);

NTSTATUS
Ext2LockVcb (IN PEXT2_VCB    Vcb,
             IN PFILE_OBJECT FileObject);

NTSTATUS
Ext2LockVolume (IN PEXT2_IRP_CONTEXT IrpContext);

NTSTATUS
Ext2UnlockVcb (IN PEXT2_VCB    Vcb,
               IN PFILE_OBJECT FileObject);

NTSTATUS
Ext2UnlockVolume (IN PEXT2_IRP_CONTEXT IrpContext);

BOOLEAN
Ext2IsMediaWriteProtected (
    IN PEXT2_IRP_CONTEXT   IrpContext,
    IN PDEVICE_OBJECT TargetDevice
);

NTSTATUS
Ext2MountVolume (IN PEXT2_IRP_CONTEXT IrpContext);

VOID
Ext2VerifyVcb (IN PEXT2_IRP_CONTEXT IrpContext,
               IN PEXT2_VCB         Vcb );

NTSTATUS
Ext2VerifyVolume (IN PEXT2_IRP_CONTEXT IrpContext);

NTSTATUS
Ext2IsVolumeMounted (IN PEXT2_IRP_CONTEXT IrpContext);

NTSTATUS
Ext2DismountVolume (IN PEXT2_IRP_CONTEXT IrpContext);

BOOLEAN
Ext2CheckSetBlock(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB Vcb, LONGLONG Block
);

BOOLEAN
Ext2CheckBitmapConsistency(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB Vcb
);

VOID
Ext2InsertVcb(PEXT2_VCB Vcb);

VOID
Ext2RemoveVcb(PEXT2_VCB Vcb);

VOID
Ext2UnlinkVcb(PEXT2_VCB Vcb);

NTSTATUS
Ext2InitializeLabel(
    IN PEXT2_VCB            Vcb,
    IN PEXT2_SUPER_BLOCK    Sb
);

NTSTATUS
Ext2InitializeVcb(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB Vcb,
    PEXT2_SUPER_BLOCK Ext2Sb,
    PDEVICE_OBJECT TargetDevice,
    PDEVICE_OBJECT VolumeDevice,
    PVPB Vpb                   );

VOID
Ext2TearDownStream (IN PEXT2_VCB Vcb);

VOID
Ext2DestroyVcb (IN PEXT2_VCB Vcb);

VOID
Ext2SyncUninitializeCacheMap (
    IN PFILE_OBJECT FileObject    );

#endif /* _EXT4_EXT4FS_VOLUME_H_ */
