/**
 * dirctl_internal.h - definitions shared by the files split from DirectoryControl.c.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _EXT4_FSD_DIRCTL_INTERNAL_H_
#define _EXT4_FSD_DIRCTL_INTERNAL_H_

int Ext2FillEntry(void *context, const char *name, int namlen,
                  ULONG offset, __u32 ino, unsigned int d_type);

/* the name buffer a query's Ext2FillEntry calls used */
VOID Ext2FreeFillName(IN OUT PEXT2_FILLDIR_CONTEXT fc);

#endif /* _EXT4_FSD_DIRCTL_INTERNAL_H_ */
