/**
 * luks_internal.h - LUKS on disk, and what the LUKS files share among themselves.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The headers as the format documents lay them out (LUKS1 On-Disk Format
 * Specification 1.2.3; LUKS2 On-Disk Format Specification 1.1.x). Every
 * integer is big-endian, so every field is bytes, read through Be16/32/64;
 * the offsets are checked against the documents at compile time.
 */

#ifndef _LUKS_INTERNAL_H_
#define _LUKS_INTERNAL_H_

#include "ext4ctl.h"

#define LUKS_SECTOR             512
#define LUKS_MAX_STRIPES        4000
#define LUKS_MAX_SALT           64
#define LUKS_MIN_DIGEST         20      /* SHA-1's, the shortest hash LUKS uses */
#define LUKS_MAX_DIGEST         64      /* SHA-512's, the longest */
#define LUKS_MAGIC_BYTES        6

extern const UINT8 LuksMagic[LUKS_MAGIC_BYTES];         /* "LUKS\xBA\xBE": LUKS1, LUKS2 primary */
extern const UINT8 LuksMagic2[LUKS_MAGIC_BYTES];        /* "SKUL\xBA\xBE": LUKS2 secondary */

/* ---------------------------------------------------------------- LUKS1 */

#define LUKS1_KEYSLOTS          8
#define LUKS1_KEY_ENABLED       0x00AC71F3
#define LUKS1_DIGEST            20

typedef struct _LUKS1_KEYSLOT {
    UINT8   Active[4];                  /* LUKS1_KEY_ENABLED */
    UINT8   Iterations[4];
    UINT8   Salt[32];
    UINT8   KeyMaterialOffset[4];       /* in 512-byte sectors */
    UINT8   Stripes[4];
} LUKS1_KEYSLOT;

typedef struct _LUKS1_PHDR {
    UINT8           Magic[LUKS_MAGIC_BYTES];
    UINT8           Version[2];
    char            CipherName[32];
    char            CipherMode[32];
    char            HashSpec[32];
    UINT8           PayloadOffset[4];   /* in 512-byte sectors */
    UINT8           KeyBytes[4];
    UINT8           MkDigest[LUKS1_DIGEST];
    UINT8           MkDigestSalt[32];
    UINT8           MkDigestIterations[4];
    char            Uuid[40];
    LUKS1_KEYSLOT   Keyslot[LUKS1_KEYSLOTS];
} LUKS1_PHDR;

C_ASSERT(sizeof(LUKS1_KEYSLOT) == 48);
C_ASSERT(FIELD_OFFSET(LUKS1_PHDR, CipherName) == 8);
C_ASSERT(FIELD_OFFSET(LUKS1_PHDR, HashSpec) == 72);
C_ASSERT(FIELD_OFFSET(LUKS1_PHDR, PayloadOffset) == 104);
C_ASSERT(FIELD_OFFSET(LUKS1_PHDR, MkDigest) == 112);
C_ASSERT(FIELD_OFFSET(LUKS1_PHDR, MkDigestIterations) == 164);
C_ASSERT(FIELD_OFFSET(LUKS1_PHDR, Uuid) == 168);
C_ASSERT(FIELD_OFFSET(LUKS1_PHDR, Keyslot) == 208);
C_ASSERT(sizeof(LUKS1_PHDR) == 592);

/* ---------------------------------------------------------------- LUKS2 */

#define LUKS2_BINARY            4096
#define LUKS2_MAX_HEADER        (4 * 1024 * 1024)

typedef struct _LUKS2_BINARY_HDR {
    UINT8   Magic[LUKS_MAGIC_BYTES];
    UINT8   Version[2];
    UINT8   HeaderSize[8];              /* binary + JSON area */
    UINT8   SeqId[8];
    char    Label[48];
    char    ChecksumAlg[32];
    UINT8   Salt[64];
    char    Uuid[40];
    char    Subsystem[48];
    UINT8   HeaderOffset[8];            /* of this copy */
    UINT8   Padding[184];
    UINT8   Checksum[64];
    UINT8   Padding4096[7 * 512];
} LUKS2_BINARY_HDR;

C_ASSERT(FIELD_OFFSET(LUKS2_BINARY_HDR, HeaderSize) == 8);
C_ASSERT(FIELD_OFFSET(LUKS2_BINARY_HDR, SeqId) == 16);
C_ASSERT(FIELD_OFFSET(LUKS2_BINARY_HDR, Label) == 24);
C_ASSERT(FIELD_OFFSET(LUKS2_BINARY_HDR, ChecksumAlg) == 72);
C_ASSERT(FIELD_OFFSET(LUKS2_BINARY_HDR, Uuid) == 168);
C_ASSERT(FIELD_OFFSET(LUKS2_BINARY_HDR, HeaderOffset) == 256);
C_ASSERT(FIELD_OFFSET(LUKS2_BINARY_HDR, Checksum) == 448);
C_ASSERT(sizeof(LUKS2_BINARY_HDR) == LUKS2_BINARY);

/* ---------------------------------------------------------------- shared */

static __inline UINT16 Be16(const UINT8 *p) { return (UINT16)((p[0] << 8) | p[1]); }
static __inline UINT32 Be32(const UINT8 *p) { return ((UINT32)p[0] << 24) | ((UINT32)p[1] << 16) | ((UINT32)p[2] << 8) | p[3]; }
static __inline UINT64 Be64(const UINT8 *p) { return ((UINT64)Be32(p) << 32) | Be32(p + 4); }

/* a NUL-padded text field of the header, cut to the room there is */
void LuksCopyField(char *Out, size_t OutSize, const char *Field, size_t FieldSize);

/* a text from the header, whole or not at all */
BOOL LuksCopyText(char *Out, size_t OutSize, const char *In);

/* "aes-xts-plain64" (LUKS2) or "aes" + "-" + "xts-plain64" (LUKS1): the
   EXT4_CIPHER_* of it, 0 if not one the driver has */
ULONG LuksParseCipher(const char *Spec);

/* the candidate volume key of one keyslot, from its derived keyslot key:
   the area read, decrypted (Cipher), merged by the anti-forensic splitter */
BOOL LuksOpenKeyslot(LUKS_DEVICE *Dev, ULONG Cipher, const UINT8 *SlotKey, ULONG SlotKeyBytes,
                     ULONGLONG AreaOffset, LPCWSTR AfHash, ULONG KeyBytes, ULONG Stripes,
                     UINT8 *Key);

int  Luks1Probe(const LUKS1_PHDR *H, LUKS_VOLUME *V);
BOOL Luks1Unlock(LUKS_DEVICE *Dev, const LUKS1_PHDR *H, const void *Pass, ULONG PassLength,
                 UINT8 *Key, int *Keyslot);

int  Luks2Probe(LUKS_DEVICE *Dev, LUKS_VOLUME *V);
BOOL Luks2Unlock(LUKS_DEVICE *Dev, const void *Pass, ULONG PassLength, UINT8 *Key,
                 ULONG *KeyBytes, int *Keyslot);

#endif /* _LUKS_INTERNAL_H_ */
