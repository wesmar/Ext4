/**
 * ext4fs_nls.h - code page tables.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _EXT4_EXT4FS_NLS_H_
#define _EXT4_EXT4FS_NLS_H_

int
Ext2LoadAllNls();

VOID
Ext2UnloadAllNls();

#endif /* _EXT4_EXT4FS_NLS_H_ */
