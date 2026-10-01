/**
 * ext4xts.h - AES-XTS (IEEE 1619) over CNG, shared by ext4.sys and ext4ctl.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The includer provides the Windows and <bcrypt.h> declarations: ntifs.h in
 * the driver, windows.h in ext4ctl.
 */

#ifndef _EXT4_EXT4XTS_H_
#define _EXT4_EXT4XTS_H_

#define EXT4_XTS_BLOCK          16          /* AES block */
#define EXT4_XTS_MAX_UNIT       4096        /* largest data unit (sector) */

typedef struct _EXT4_XTS {
    BCRYPT_KEY_HANDLE   Data;               /* K1: encrypts the data */
    BCRYPT_KEY_HANDLE   Tweak;              /* K2: encrypts the sector number */
} EXT4_XTS, *PEXT4_XTS;

/* AesEcb: an AES provider set to BCRYPT_CHAIN_MODE_ECB. Key: K1 || K2,
   32 bytes (AES-128-XTS) or 64 (AES-256-XTS). */
NTSTATUS
Ext4XtsInit(PEXT4_XTS Xts, BCRYPT_ALG_HANDLE AesEcb, const UCHAR *Key, ULONG KeyBytes);

VOID
Ext4XtsFree(PEXT4_XTS Xts);

/* One data unit in place: Length a multiple of 16, at most EXT4_XTS_MAX_UNIT;
   Scratch holds Length bytes (the tweak of every block). */
NTSTATUS
Ext4XtsUnit(PEXT4_XTS Xts, BOOLEAN Encrypt, ULONGLONG Unit,
            PUCHAR Buffer, ULONG Length, PUCHAR Scratch);

/* the same, reading the input from In (may equal Buffer) */
NTSTATUS
Ext4XtsUnitCopy(PEXT4_XTS Xts, BOOLEAN Encrypt, ULONGLONG Unit,
                PUCHAR Buffer, const UCHAR *In, ULONG Length, PUCHAR Scratch);

#endif /* _EXT4_EXT4XTS_H_ */
