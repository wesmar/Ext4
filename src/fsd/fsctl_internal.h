/**
 * fsctl_internal.h - definitions shared by the files split from FileSystemControl.c.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _EXT4_FSD_FSCTL_INTERNAL_H_
#define _EXT4_FSD_FSCTL_INTERNAL_H_

/*
 * The request came on a handle to the volume itself (FsContext is the Vcb),
 * not on a file of it. A lock, an unlock, a dismount or extended DASD I/O
 * is taken from such a handle only, as NTFS takes it: a volume opened for
 * its data passed the volume device's ACL, any file handle did not - through
 * one, any user could lock or dismount the volume.
 */
static __inline BOOLEAN
Ext2IsVolumeHandle(IN PEXT2_IRP_CONTEXT IrpContext)
{
    PEXT2_FCBVCB Xcb = (PEXT2_FCBVCB)IrpContext->Fcb;

    return Xcb != NULL && Xcb->Identifier.Type == EXT2VCB && IrpContext->Ccb != NULL;
}

NTSTATUS
Ext2GetReparsePoint (IN PEXT2_IRP_CONTEXT IrpContext);

NTSTATUS
Ext2SetReparsePoint (IN PEXT2_IRP_CONTEXT IrpContext);

NTSTATUS
Ext2DeleteReparsePoint (IN PEXT2_IRP_CONTEXT IrpContext);

#endif /* _EXT4_FSD_FSCTL_INTERNAL_H_ */
