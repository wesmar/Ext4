/**
 * luks_keyslot.c - one keyslot opened: area cipher, anti-forensic merge, header text fields.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The same in both versions of the format:
 *
 *   keyslot area --AES-XTS(keyslot key, 512-byte sectors)--> AF material
 *   AF material --AF-merge(stripes, hash)--> candidate volume key
 */

#include "luks_internal.h"

const UINT8 LuksMagic[LUKS_MAGIC_BYTES]  = { 'L', 'U', 'K', 'S', 0xBA, 0xBE };
const UINT8 LuksMagic2[LUKS_MAGIC_BYTES] = { 'S', 'K', 'U', 'L', 0xBA, 0xBE };

/* ---------------------------------------------------------------- header text */

/* a header with a longer text than the field holds is not a header this tool
   reads (strcpy_s would end the process, and every "list" probes every volume) */
BOOL
LuksCopyText(char *Out, size_t OutSize, const char *In)
{
    size_t n = strlen(In);

    if (n >= OutSize) {
        return FALSE;
    }
    memcpy(Out, In, n + 1);
    return TRUE;
}

void
LuksCopyField(char *Out, size_t OutSize, const char *Field, size_t FieldSize)
{
    size_t n = strnlen(Field, FieldSize);

    if (n >= OutSize) {
        n = OutSize - 1;
    }
    memcpy(Out, Field, n);
    Out[n] = 0;
}

ULONG
LuksParseCipher(const char *Spec)
{
    if (_stricmp(Spec, "aes-xts-plain64") == 0) {
        return EXT4_CIPHER_AES_XTS_PLAIN64;
    }
    if (_stricmp(Spec, "aes-xts-plain") == 0) {
        return EXT4_CIPHER_AES_XTS_PLAIN;
    }
    return 0;
}

/* ---------------------------------------------------------------- area */

/* decrypt Length bytes of a keyslot area in place, sectors numbered from 0 */
static BOOL
DecryptArea(ULONG Cipher, const UINT8 *Key, ULONG KeyBytes, UINT8 *Area, ULONG Length)
{
    EXT4_XTS    Xts;
    UINT8       Scratch[LUKS_SECTOR];
    ULONG       Sector;
    BOOL        Ok = TRUE;

    if (CryptoAesEcb() == NULL || !NT_SUCCESS(Ext4XtsInit(&Xts, CryptoAesEcb(), Key, KeyBytes))) {
        return FALSE;
    }
    for (Sector = 0; Sector * LUKS_SECTOR < Length && Ok; Sector++) {
        ULONGLONG Iv = Sector;
        if (Cipher == EXT4_CIPHER_AES_XTS_PLAIN) {
            Iv &= MAXUINT32;              /* "plain": 32-bit sector numbers */
        }
        Ok = NT_SUCCESS(Ext4XtsUnit(&Xts, FALSE, Iv, Area + (SIZE_T)Sector * LUKS_SECTOR,
                                    LUKS_SECTOR, Scratch));
    }
    Ext4XtsFree(&Xts);
    SecureZeroMemory(Scratch, sizeof(Scratch));
    return Ok;
}

/* ---------------------------------------------------------------- AF */

/* each digest-sized block replaced by Hash(BE32(index) || block) */
static BOOL
Diffuse(LPCWSTR HashId, UINT8 *Buffer, ULONG Size)
{
    ULONG   Digest = CryptoHashLength(HashId);
    ULONG   Blocks = Size / Digest, Rest = Size % Digest, i;
    UINT8   Index[4];

    for (i = 0; i <= Blocks; i++) {
        ULONG Length = i < Blocks ? Digest : Rest;
        if (Length == 0) {
            break;
        }
        Index[0] = (UINT8)(i >> 24);
        Index[1] = (UINT8)(i >> 16);
        Index[2] = (UINT8)(i >> 8);
        Index[3] = (UINT8)i;
        if (!CryptoHash(HashId, Index, sizeof(Index), Buffer + (SIZE_T)i * Digest, Length,
                        Buffer + (SIZE_T)i * Digest, Length)) {
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL
AfMerge(LPCWSTR HashId, const UINT8 *Material, ULONG KeyBytes, ULONG Stripes, UINT8 *Key)
{
    UINT8   Block[LUKS_MAX_KEY];
    ULONG   i, j;

    memset(Block, 0, sizeof(Block));
    for (i = 0; i + 1 < Stripes; i++) {
        for (j = 0; j < KeyBytes; j++) {
            Block[j] ^= Material[(SIZE_T)i * KeyBytes + j];
        }
        if (!Diffuse(HashId, Block, KeyBytes)) {
            SecureZeroMemory(Block, sizeof(Block));
            return FALSE;
        }
    }
    for (j = 0; j < KeyBytes; j++) {
        Key[j] = Block[j] ^ Material[(SIZE_T)(Stripes - 1) * KeyBytes + j];
    }
    SecureZeroMemory(Block, sizeof(Block));
    return TRUE;
}

/* ---------------------------------------------------------------- keyslot */

BOOL
LuksOpenKeyslot(LUKS_DEVICE *Dev, ULONG Cipher, const UINT8 *SlotKey, ULONG SlotKeyBytes,
                ULONGLONG AreaOffset, LPCWSTR AfHash, ULONG KeyBytes, ULONG Stripes,
                UINT8 *Key)
{
    ULONG   AfBytes = KeyBytes * Stripes;
    ULONG   Span = (AfBytes + LUKS_SECTOR - 1) & ~(ULONG)(LUKS_SECTOR - 1);
    UINT8  *Area;
    BOOL    Ok;

    if (Stripes == 0 || Stripes > LUKS_MAX_STRIPES || KeyBytes == 0 || KeyBytes > LUKS_MAX_KEY) {
        return FALSE;
    }
    Area = (UINT8 *)VirtualAlloc(NULL, Span, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (Area == NULL) {
        return FALSE;
    }
    Ok = LuksReadAt(Dev, AreaOffset, Area, Span) &&
         DecryptArea(Cipher, SlotKey, SlotKeyBytes, Area, Span) &&
         AfMerge(AfHash, Area, KeyBytes, Stripes, Key);
    SecureZeroMemory(Area, Span);
    VirtualFree(Area, 0, MEM_RELEASE);
    return Ok;
}
