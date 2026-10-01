/**
 * linux_internal.h - definitions shared by the files split from linux.c.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _EXT4_LINUX_LINUX_INTERNAL_H_
#define _EXT4_LINUX_LINUX_INTERNAL_H_

int
ext2_init_bh();

void
ext2_destroy_bh();

#endif /* _EXT4_LINUX_LINUX_INTERNAL_H_ */
