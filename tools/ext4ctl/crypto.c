/**
 * crypto.c - hashes, PBKDF2, AES and Base64 for ext4ctl, on CNG.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4ctl.h"

typedef struct _HASH_NAME {
    const char *Name;           /* as LUKS writes it */
    LPCWSTR     Id;
} HASH_NAME;

static const HASH_NAME HashNames[] = {
    { "sha1",   BCRYPT_SHA1_ALGORITHM },
    { "sha256", BCRYPT_SHA256_ALGORITHM },
    { "sha384", BCRYPT_SHA384_ALGORITHM },
    { "sha512", BCRYPT_SHA512_ALGORITHM },
};

LPCWSTR
CryptoHashId(const char *Name)
{
    size_t i;

    for (i = 0; i < ARRAYSIZE(HashNames); i++) {
        if (_stricmp(Name, HashNames[i].Name) == 0) {
            return HashNames[i].Id;
        }
    }
    return NULL;
}

ULONG
CryptoHashLength(LPCWSTR HashId)
{
    if (wcscmp(HashId, BCRYPT_SHA1_ALGORITHM) == 0)   return 20;
    if (wcscmp(HashId, BCRYPT_SHA256_ALGORITHM) == 0) return 32;
    if (wcscmp(HashId, BCRYPT_SHA384_ALGORITHM) == 0) return 48;
    if (wcscmp(HashId, BCRYPT_SHA512_ALGORITHM) == 0) return 64;
    return 0;
}

/* Out = Hash(In1 || In2), OutLength <= the digest length */
BOOL
CryptoHash(LPCWSTR HashId, const void *In1, ULONG Len1, const void *In2, ULONG Len2,
           UINT8 *Out, ULONG OutLength)
{
    BCRYPT_ALG_HANDLE   Alg = NULL;
    BCRYPT_HASH_HANDLE  Hash = NULL;
    UINT8               Digest[64];
    ULONG               Length = CryptoHashLength(HashId);
    BOOL                Ok = FALSE;

    if (Length == 0 || OutLength > Length) {
        return FALSE;
    }
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&Alg, HashId, NULL, 0)) &&
        BCRYPT_SUCCESS(BCryptCreateHash(Alg, &Hash, NULL, 0, NULL, 0, 0)) &&
        BCRYPT_SUCCESS(BCryptHashData(Hash, (PUCHAR)In1, Len1, 0)) &&
        (Len2 == 0 || BCRYPT_SUCCESS(BCryptHashData(Hash, (PUCHAR)In2, Len2, 0))) &&
        BCRYPT_SUCCESS(BCryptFinishHash(Hash, Digest, Length, 0))) {
        memcpy(Out, Digest, OutLength);
        Ok = TRUE;
    }
    if (Hash) BCryptDestroyHash(Hash);
    if (Alg) BCryptCloseAlgorithmProvider(Alg, 0);
    SecureZeroMemory(Digest, sizeof(Digest));
    return Ok;
}

BOOL
CryptoPbkdf2(LPCWSTR HashId, const void *Password, ULONG PasswordLength,
             const void *Salt, ULONG SaltLength, ULONGLONG Iterations,
             UINT8 *Out, ULONG OutLength)
{
    BCRYPT_ALG_HANDLE   Alg = NULL;
    NTSTATUS            Status;

    Status = BCryptOpenAlgorithmProvider(&Alg, HashId, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!BCRYPT_SUCCESS(Status)) {
        return FALSE;
    }
    Status = BCryptDeriveKeyPBKDF2(Alg, (PUCHAR)Password, PasswordLength,
                                   (PUCHAR)Salt, SaltLength, Iterations, Out, OutLength, 0);
    BCryptCloseAlgorithmProvider(Alg, 0);
    return BCRYPT_SUCCESS(Status);
}

BCRYPT_ALG_HANDLE
CryptoAesEcb(void)
{
    static BCRYPT_ALG_HANDLE Alg;

    if (Alg == NULL) {
        BCRYPT_ALG_HANDLE New = NULL;
        if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&New, BCRYPT_AES_ALGORITHM, NULL, 0)) &&
            BCRYPT_SUCCESS(BCryptSetProperty(New, BCRYPT_CHAINING_MODE,
                                             (PUCHAR)BCRYPT_CHAIN_MODE_ECB,
                                             sizeof(BCRYPT_CHAIN_MODE_ECB), 0))) {
            Alg = New;
        } else if (New) {
            BCryptCloseAlgorithmProvider(New, 0);
        }
    }
    return Alg;
}

/* standard alphabet, padding optional; *OutLength in: room, out: bytes */
BOOL
Base64Decode(const char *Text, UINT8 *Out, ULONG *OutLength)
{
    ULONG   Room = *OutLength, n = 0, Bits = 0;
    UINT32  Acc = 0;

    for (; *Text && *Text != '='; Text++) {
        int v;
        char c = *Text;
        if (c >= 'A' && c <= 'Z')      v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+')             v = 62;
        else if (c == '/')             v = 63;
        else if (c == '\n' || c == '\r' || c == ' ') continue;
        else return FALSE;

        Acc = (Acc << 6) | (UINT32)v;
        Bits += 6;
        if (Bits >= 8) {
            Bits -= 8;
            if (n >= Room) {
                return FALSE;
            }
            Out[n++] = (UINT8)(Acc >> Bits);
        }
    }
    *OutLength = n;
    return TRUE;
}
