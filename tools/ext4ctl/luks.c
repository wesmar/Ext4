/**
 * luks.c - LUKS volumes for the commands: which version, what it holds, its volume key.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * As cryptsetup does it, and as the on-disk format documents describe it.
 * The version comes from the first header (luks1.c, luks2.c); the keyslot
 * machinery both share is in luks_keyslot.c, the device in luks_device.c.
 */

#include "luks_internal.h"

int
LuksProbe(LUKS_DEVICE *Dev, LUKS_VOLUME *V)
{
    LUKS1_PHDR H;

    memset(V, 0, sizeof(*V));
    if (!LuksReadAt(Dev, 0, &H, sizeof(H))) {
        return -1;
    }
    if (memcmp(H.Magic, LuksMagic, LUKS_MAGIC_BYTES) != 0) {
        return 0;
    }
    V->Version = Be16(H.Version);
    if (V->Version == 1) {
        return Luks1Probe(&H, V);
    }
    if (V->Version == 2) {
        return Luks2Probe(Dev, V);
    }
    return -1;
}

ULONG
LuksCipher(const LUKS_VOLUME *V)
{
    return V->Problem[0] == 0 ? LuksParseCipher(V->Cipher) : 0;
}

BOOL
LuksUnlockKey(LUKS_DEVICE *Dev, const LUKS_VOLUME *V, const void *Pass, ULONG PassLength,
              UINT8 Key[LUKS_MAX_KEY], int *Keyslot)
{
    if (V->Version == 1) {
        LUKS1_PHDR H;
        return LuksReadAt(Dev, 0, &H, sizeof(H)) && Luks1Unlock(Dev, &H, Pass, PassLength, Key, Keyslot);
    }
    if (V->Version == 2) {
        ULONG KeyBytes = 0;
        return Luks2Unlock(Dev, Pass, PassLength, Key, &KeyBytes, Keyslot) && KeyBytes == V->KeyBytes;
    }
    return FALSE;
}
