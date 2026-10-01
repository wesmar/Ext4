/**
 * core_internal.h - definitions shared by the files split from memory.c.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _EXT4_CORE_CORE_INTERNAL_H_
#define _EXT4_CORE_CORE_INTERNAL_H_

PEXT2_ICB
Ext2FindIcbLocked(IN PEXT2_VCB Vcb, IN ULONG Ino);

NTSTATUS
Ext2PerformRegistryVolumeParams(IN PEXT2_VCB Vcb);

#endif /* _EXT4_CORE_CORE_INTERNAL_H_ */
