/**
 * access.c - POSIX permission checks against the caller.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"

int Ext2CheckInodeAccess(PEXT2_VCB Vcb, struct inode *in, int attempt)
{
    int granted = 0;

    uid_t uid = Vcb->uid;
    gid_t gid = Vcb->gid;

    if (IsFlagOn(Vcb->Flags, VCB_USER_EIDS)) {
        uid = Vcb->euid;
        gid = Vcb->egid;
    }

    if (!uid || uid == in->i_uid) {
        /* grant all access for inode owner or root */
        granted = Ext2FileCanRead | Ext2FileCanWrite | Ext2FileCanExecute;
    } else if (gid == in->i_gid) {
        if (Ext2IsGroupReadOnly(in->i_mode))
            granted = Ext2FileCanRead | Ext2FileCanExecute;
        else if (Ext2IsGroupWritable(in->i_mode))
            granted = Ext2FileCanRead | Ext2FileCanWrite | Ext2FileCanExecute;
    } else {
        if (Ext2IsOtherReadOnly(in->i_mode))
            granted = Ext2FileCanRead | Ext2FileCanExecute;
        else if (Ext2IsOtherWritable(in->i_mode))
            granted = Ext2FileCanRead | Ext2FileCanWrite | Ext2FileCanExecute;

    }

    return IsFlagOn(granted, attempt);
}

int Ext2CheckFileAccess(PEXT2_VCB Vcb, PEXT2_MCB Mcb, int attempt)
{
    return Ext2CheckInodeAccess(Vcb, Mcb->Inode, attempt);
}
