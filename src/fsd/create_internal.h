/**
 * create_internal.h - definitions shared by the files split from create.c.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _EXT4_FSD_CREATE_INTERNAL_H_
#define _EXT4_FSD_CREATE_INTERNAL_H_

NTSTATUS Ext2AddDotEntries(struct ext2_icb *icb, struct inode *dir,
                           struct inode *inode);

NTSTATUS
Ext2OverwriteEa(
	PEXT2_IRP_CONTEXT    IrpContext,
	PEXT2_VCB Vcb,
	PEXT2_FCB Fcb,
	PIO_STATUS_BLOCK Iosb
);

/* a new inode takes the SELinux label of its directory, as on Linux */
VOID
Ext4InheritSecurityLabel(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_MCB            Parent,
    IN PEXT2_MCB            Mcb
);

#endif /* _EXT4_FSD_CREATE_INTERNAL_H_ */
