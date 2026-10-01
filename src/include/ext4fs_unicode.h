/**
 * ext4fs_unicode.h - casefolded directories: Unicode 12.1 folding of names.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _EXT4_EXT4FS_UNICODE_H_
#define _EXT4_EXT4FS_UNICODE_H_

/* s_encoding: the one encoding ext4 defines, UTF-8 with Unicode 12.1 */
#define EXT4_ENC_UTF8_12_1          1

/* s_encoding_flags: names that are not valid UTF-8 are refused */
#define EXT4_ENC_STRICT_MODE_FL     0x0001

/* the most a folded name may take, as Linux sizes its buffer (PATH_MAX);
   a longer fold hashes the name as it is, as in Linux */
#define EXT4_UTF8_FOLDED_MAX        4096

/* the encoding of a file system can be folded by this driver */
BOOLEAN
Ext4Utf8Supported (IN USHORT Encoding);

/* Ext4Utf8Casefold: the folded form does not fit into Out */
#define EXT4_UTF8_TOO_LONG          (-2)

/* the case-folded NFD form of Name into Out; its length, -1 when Name is
   not valid UTF-8, EXT4_UTF8_TOO_LONG when the form does not fit */
int
Ext4Utf8Casefold (IN const char *Name, IN int Length, OUT unsigned char *Out, IN int OutLength);

/* 0: the names fold to the same string, 1: they do not, -1: one of them
   is not valid UTF-8 */
int
Ext4Utf8CasefoldCompare (IN const char *A, IN int ALength, IN const char *B, IN int BLength);

/* Name is valid UTF-8 of known code points */
BOOLEAN
Ext4Utf8Valid (IN const char *Name, IN int Length);

#endif /* _EXT4_EXT4FS_UNICODE_H_ */
