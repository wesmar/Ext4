/**
 * write_internal.h - definitions shared by the files split from FileWrite.c.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _EXT4_FSD_WRITE_INTERNAL_H_
#define _EXT4_FSD_WRITE_INTERNAL_H_

NTSTATUS
Ext2WriteVolume (IN PEXT2_IRP_CONTEXT IrpContext);

VOID
Ext2DeferWrite(IN PEXT2_IRP_CONTEXT, PIRP Irp);

#endif /* _EXT4_FSD_WRITE_INTERNAL_H_ */
