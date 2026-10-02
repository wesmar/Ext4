/**
 * luks1.c - LUKS1: the phdr, its eight keyslots, the volume key digest.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 *
 *   passphrase --PBKDF2(slot salt, slot iterations)--> keyslot key
 *   keyslot key --> candidate volume key (luks_keyslot.c)
 *   candidate --PBKDF2(digest salt, digest iterations)--> must equal MkDigest
 */

#include "luks_internal.h"

/* "aes" + "-" + "xts-plain64", as the two fields of the header give it */
static void
Luks1CipherSpec(const LUKS1_PHDR *H, char *Out, size_t OutSize)
{
    char Name[sizeof(H->CipherName) + 1], Mode[sizeof(H->CipherMode) + 1];

    LuksCopyField(Name, sizeof(Name), H->CipherName, sizeof(H->CipherName));
    LuksCopyField(Mode, sizeof(Mode), H->CipherMode, sizeof(H->CipherMode));
    snprintf(Out, OutSize, "%s-%s", Name, Mode);
}

int
Luks1Probe(const LUKS1_PHDR *H, LUKS_VOLUME *V)
{
    int i;

    Luks1CipherSpec(H, V->Cipher, sizeof(V->Cipher));
    V->KeyBytes = Be32(H->KeyBytes);
    V->SectorSize = LUKS_SECTOR;
    V->PayloadOffset = (ULONGLONG)Be32(H->PayloadOffset) * LUKS_SECTOR;
    LuksCopyField(V->Uuid, sizeof(V->Uuid), H->Uuid, sizeof(H->Uuid));
    strcpy_s(V->Kdf, sizeof(V->Kdf), "pbkdf2");
    for (i = 0; i < LUKS1_KEYSLOTS; i++) {
        if (Be32(H->Keyslot[i].Active) == LUKS1_KEY_ENABLED) {
            V->Keyslots++;
        }
    }
    if (LuksParseCipher(V->Cipher) == 0) {
        /* a LUKS volume all the same: listed, not opened */
        snprintf(V->Problem, sizeof(V->Problem), "cipher %s is not supported (aes-xts-plain64 is)", V->Cipher);
    }
    return 1;
}

BOOL
Luks1Unlock(LUKS_DEVICE *Dev, const LUKS1_PHDR *H, const void *Pass, ULONG PassLength,
            UINT8 *Key, int *Keyslot)
{
    char        Spec[sizeof(H->CipherName) + sizeof(H->CipherMode) + 2];
    char        HashName[sizeof(H->HashSpec) + 1];
    ULONG       Cipher;
    LPCWSTR     HashId;
    ULONG       KeyBytes = Be32(H->KeyBytes);
    UINT8       SlotKey[LUKS_MAX_KEY], Digest[LUKS1_DIGEST];
    int         i;

    Luks1CipherSpec(H, Spec, sizeof(Spec));
    LuksCopyField(HashName, sizeof(HashName), H->HashSpec, sizeof(H->HashSpec));
    Cipher = LuksParseCipher(Spec);
    if (Cipher == 0) {
        Fail("cipher %s is not supported (aes-xts-plain64 is)", Spec);
        return FALSE;
    }
    HashId = CryptoHashId(HashName);
    if (HashId == NULL || KeyBytes == 0 || KeyBytes > LUKS_MAX_KEY) {
        Fail("hash %s / key size %lu is not supported", HashName, KeyBytes);
        return FALSE;
    }

    for (i = 0; i < LUKS1_KEYSLOTS; i++) {
        const LUKS1_KEYSLOT *Slot = &H->Keyslot[i];
        BOOL                 Ok;

        if (Be32(Slot->Active) != LUKS1_KEY_ENABLED) {
            continue;
        }
        Ok = CryptoPbkdf2(HashId, Pass, PassLength, Slot->Salt, sizeof(Slot->Salt),
                          Be32(Slot->Iterations), SlotKey, KeyBytes) &&
             LuksOpenKeyslot(Dev, Cipher, SlotKey, KeyBytes,
                             (ULONGLONG)Be32(Slot->KeyMaterialOffset) * LUKS_SECTOR,
                             HashId, KeyBytes, Be32(Slot->Stripes), Key) &&
             CryptoPbkdf2(HashId, Key, KeyBytes, H->MkDigestSalt, sizeof(H->MkDigestSalt),
                          Be32(H->MkDigestIterations), Digest, LUKS1_DIGEST) &&
             memcmp(Digest, H->MkDigest, LUKS1_DIGEST) == 0;
        SecureZeroMemory(SlotKey, sizeof(SlotKey));
        if (Ok) {
            *Keyslot = i;
            return TRUE;
        }
    }
    SecureZeroMemory(Key, LUKS_MAX_KEY);
    return FALSE;
}
