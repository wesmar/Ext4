/**
 * version.h - driver identity and version, shared by the C sources and ext4.rc.
 *
 * Copyright (c) 2026 Marek Wesołowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _EXT4_VERSION_H_
#define _EXT4_VERSION_H_

#define EXT4_VERSION_MAJOR      1
#define EXT4_VERSION_MINOR      0
#define EXT4_VERSION_PATCH      0
#define EXT4_VERSION_BUILD      0

#define EXT4_STR2(x)            #x
#define EXT4_STR(x)             EXT4_STR2(x)

/* "1.0.0" for messages, "1.0.0.0" for the version resource */
#define EXT4_VERSION_STRING     EXT4_STR(EXT4_VERSION_MAJOR) "." EXT4_STR(EXT4_VERSION_MINOR) "." \
                                EXT4_STR(EXT4_VERSION_PATCH)
#define EXT4_VERSION_STRING4    EXT4_VERSION_STRING "." EXT4_STR(EXT4_VERSION_BUILD)

#define EXT4_DRIVER_FILE        "ext4.sys"
#define EXT4_PRODUCT_NAME       "ext4 - ext2/ext3/ext4 file system driver for Windows"
#define EXT4_COMPANY            "WESMAR - Marek Wesolowski"
#define EXT4_COPYRIGHT          "© Marek Wesołowski (WESMAR) 2026 "
#define EXT4_CONTACT            "marek@wesolowski.eu.org - https://kvc.pl"

#endif /* _EXT4_VERSION_H_ */
