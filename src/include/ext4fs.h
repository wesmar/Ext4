/**
 * ext4fs.h - the one header every driver source includes.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _EXT4_EXT4FS_H_
#define _EXT4_EXT4FS_H_

#include <linux/module.h>
#include <ntdddisk.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <linux/ext4.h>

#include "ext4fs_config.h"
#include "ext4fs_global.h"
#include "ext4fs_objects.h"
#include "ext4fs_common.h"

#include "ext4fs_driver.h"
#include "ext4fs_support.h"
#include "ext4fs_core.h"
#include "ext4fs_volume.h"
#include "ext4fs_fsd.h"
#include "ext4fs_ext4.h"
#include "ext4fs_unicode.h"
#include "ext4fs_journal.h"
#include "ext4fs_linux.h"
#include "ext4fs_nls.h"

#endif /* _EXT4_EXT4FS_H_ */
