/**
 * ext4fs_driver.h - driver entry, registry settings, unload, dispatch and exception handling.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _EXT4_EXT4FS_DRIVER_H_
#define _EXT4_EXT4FS_DRIVER_H_

NTSTATUS
Ext2PrepareToUnload (IN PEXT2_IRP_CONTEXT IrpContext);

VOID
Ext2StartUnloadWatch (VOID);

VOID
Ext2StopUnloadWatch (VOID);

VOID
Ext2DeleteControlDevices (VOID);

VOID
Ext2UnloadKick (VOID);

VOID
Ext2OplockComplete (
    IN PVOID Context,
    IN PIRP Irp
);

VOID
Ext2LockIrp (
    IN PVOID Context,
    IN PIRP Irp
);

NTSTATUS
Ext2QueueRequest (IN PEXT2_IRP_CONTEXT IrpContext);

IO_WORKITEM_ROUTINE Ext2DeQueueRequest;

NTSTATUS
Ext2DispatchRequest (IN PEXT2_IRP_CONTEXT IrpContext);

NTSTATUS
Ext2BuildRequest (
    IN PDEVICE_OBJECT   DeviceObject,
    IN PIRP             Irp
);

NTSTATUS
Ext2ExceptionFilter (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXCEPTION_POINTERS ExceptionPointer
);

NTSTATUS
Ext2ExceptionHandler (IN PEXT2_IRP_CONTEXT IrpContext);

NTSTATUS
Ext2QueryGlobalParameters(IN PUNICODE_STRING RegistryPath);

BOOLEAN
Ext2QueryRegistrySettings(IN PUNICODE_STRING  RegistryPath);

VOID
DriverUnload (IN PDRIVER_OBJECT DriverObject);

#endif /* _EXT4_EXT4FS_DRIVER_H_ */
