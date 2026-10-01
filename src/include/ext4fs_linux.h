/**
 * ext4fs_linux.h - Linux compatibility layer: buffer heads, memory caches, wait queues.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _EXT4_EXT4FS_LINUX_H_
#define _EXT4_EXT4FS_LINUX_H_

int
ext2_init_linux();

void
ext2_destroy_linux();

#endif /* _EXT4_EXT4FS_LINUX_H_ */
