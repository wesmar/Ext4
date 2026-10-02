/**
 * ext4ctl.h - ext4ctl: LUKS unlock and volume control for ext4.sys.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _EXT4CTL_H_
#define _EXT4CTL_H_

#define WIN32_LEAN_AND_MEAN
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <winioctl.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

#include "ext4crypt.h"
#include "ext4xts.h"

/* ---------------------------------------------------------------- BLAKE2b */

#define BLAKE2B_BLOCK   128
#define BLAKE2B_OUT     64

typedef struct _BLAKE2B {
    UINT8   b[BLAKE2B_BLOCK];
    UINT64  h[8];
    UINT64  t[2];
    size_t  c;
    size_t  OutLen;
} BLAKE2B;

void Blake2bInit(BLAKE2B *S, size_t OutLen);
void Blake2bUpdate(BLAKE2B *S, const void *In, size_t Length);
void Blake2bFinal(BLAKE2B *S, void *Out);
void Blake2b(void *Out, size_t OutLen, const void *In, size_t InLen);

/* ---------------------------------------------------------------- Argon2 */

#define ARGON2_I        1
#define ARGON2_ID       2
#define ARGON2_MAX_LANES 64     /* the most a header may ask for */

typedef struct _ARGON2_INPUT {
    const void *Password;   size_t PasswordLength;
    const void *Salt;       size_t SaltLength;
    const void *Secret;     size_t SecretLength;    /* RFC 9106 test vectors only */
    const void *Data;       size_t DataLength;
    UINT32      Type;       /* ARGON2_I, ARGON2_ID */
    UINT32      Passes;     /* t */
    UINT32      MemoryKiB;  /* m */
    UINT32      Lanes;      /* p */
} ARGON2_INPUT;

BOOL Argon2(const ARGON2_INPUT *In, void *Out, UINT32 OutLength);

/* ---------------------------------------------------------------- JSON */

typedef enum _JSON_TYPE {
    JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT
} JSON_TYPE;

typedef struct _JSON {
    JSON_TYPE       Type;
    const char     *Key;        /* member name inside an object */
    const char     *Text;       /* string value, number or literal as written */
    struct _JSON   *Child;
    struct _JSON   *Next;
} JSON;

typedef struct _JSON_DOC {
    JSON   *Root;
    char   *Arena;
    size_t  Used, Size;
} JSON_DOC;

BOOL        JsonParse(JSON_DOC *Doc, const char *Text, size_t Length);
void        JsonFree(JSON_DOC *Doc);
const JSON *JsonGet(const JSON *Object, const char *Key);
const char *JsonText(const JSON *Node);
BOOL        JsonU64(const JSON *Node, UINT64 *Value);

/* ---------------------------------------------------------------- crypto helpers */

/* "sha1", "sha256", "sha512" ... to a CNG algorithm id; NULL if unknown */
LPCWSTR     CryptoHashId(const char *Name);
BOOL        CryptoHash(LPCWSTR HashId, const void *In1, ULONG Len1, const void *In2, ULONG Len2,
                       UINT8 *Out, ULONG OutLength);
ULONG       CryptoHashLength(LPCWSTR HashId);
BOOL        CryptoPbkdf2(LPCWSTR HashId, const void *Password, ULONG PasswordLength,
                         const void *Salt, ULONG SaltLength, ULONGLONG Iterations,
                         UINT8 *Out, ULONG OutLength);
BCRYPT_ALG_HANDLE CryptoAesEcb(void);
BOOL        Base64Decode(const char *Text, UINT8 *Out, ULONG *OutLength);

/* ---------------------------------------------------------------- LUKS */

#define LUKS_MAX_KEY    64

typedef struct _LUKS_VOLUME {
    int         Version;                        /* 1 or 2 */
    char        Uuid[EXT4_CRYPT_UUID_CHARS];
    char        Label[49];
    char        Cipher[64];                     /* "aes-xts-plain64" */
    ULONG       KeyBytes;
    ULONG       SectorSize;
    ULONGLONG   PayloadOffset;
    ULONGLONG   PayloadSize;                    /* 0: to the end */
    ULONGLONG   IvOffset;
    ULONG       Keyslots;                       /* active ones */
    char        Kdf[16];                        /* of the first active keyslot */
    char        Problem[128];                   /* set: a LUKS volume this tool must not open */
} LUKS_VOLUME;

typedef struct _LUKS_DEVICE {
    HANDLE      Handle;
    WCHAR       NtName[EXT4_CRYPT_DEVICE_CHARS];  /* \Device\HarddiskVolumeN */
    ULONGLONG   Size;
} LUKS_DEVICE;

BOOL LuksOpenDevice(LUKS_DEVICE *Dev, const WCHAR *Name);
void LuksCloseDevice(LUKS_DEVICE *Dev);
BOOL LuksReadAt(LUKS_DEVICE *Dev, ULONGLONG Offset, void *Buffer, ULONG Length);

/* 1: a LUKS header was read into Volume, 0: no LUKS here, -1: error */
int  LuksProbe(LUKS_DEVICE *Dev, LUKS_VOLUME *Volume);

/* the EXT4_CIPHER_* of the data the volume holds; 0: not one the driver has */
ULONG LuksCipher(const LUKS_VOLUME *Volume);

/* the volume key for Passphrase, verified against the header's digest */
BOOL LuksUnlockKey(LUKS_DEVICE *Dev, const LUKS_VOLUME *Volume,
                   const void *Passphrase, ULONG PassphraseLength,
                   UINT8 Key[LUKS_MAX_KEY], int *Keyslot);

/* ---------------------------------------------------------------- common */

#define EXT4CTL_LETTER_WAIT_MS  15000               /* for the mount and the letter */
#define EXT4CTL_LETTER_POLL_MS  100
#define EXT4CTL_GIB             1073741824.0        /* sizes are shown in GiB */
#define EXT4CTL_NO_DRIVER       ((DWORD)-1)         /* DriverControl: not even opened */
#define EXT2_SUPER_MAGIC_OFFSET 1080                /* s_magic of the superblock at 1024 */
#define EXT2_SUPER_MAGIC        0xEF53

/* ---------------------------------------------------------------- commands */

void Usage(void);                                               /* main.c */
int  CmdList(void);                                             /* cmd_crypt.c */
int  CmdUnlock(int argc, WCHAR **argv);
int  CmdLock(int argc, WCHAR **argv);
int  CmdStatus(void);
int  CmdLvList(ULONG Crypt);                                    /* lvm_cmd.c */
int  CmdLvOpen(ULONG Crypt, const WCHAR *Volume, WCHAR Letter);
int  CmdLvClose(const WCHAR *Which, BOOL Force);
void LvStatus(void);
int  CmdSelfTest(void);                                         /* selftest.c */

/* the passphrase into Out (Room bytes): from KeyFile, a line of stdin, or
   typed at the console (stdin when there is none); passphrase.c */
BOOL ReadPassphrase(const WCHAR *KeyFile, BOOL Stdin, char *Out, ULONG Room, ULONG *Length);

/* ---------------------------------------------------------------- driver.c */

void   Fail(const char *Format, ...);
void   Say(const char *Format, ...);

/* \\.\ext4, or NULL after a message */
HANDLE OpenDriver(void);
DWORD  DriverControl(DWORD Code, void *In, DWORD InBytes, void *Out, DWORD OutBytes);
BOOL   CryptQuery(EXT4_CRYPT_QUERY *Q);
DWORD  LvQuery(EXT4_LV_QUERY *Q);

WCHAR  DeviceLetter(const WCHAR *Device);
BOOL   LetterNames(const WCHAR *Arg, const WCHAR *Device);
WCHAR  WaitForMount(const WCHAR *Device, WCHAR *Fs, DWORD FsChars);

#endif /* _EXT4CTL_H_ */
