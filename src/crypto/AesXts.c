/**
 * AesXts.c - AES-XTS (IEEE 1619) over CNG's AES-ECB, for ext4.sys and ext4ctl.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * dm-crypt's aes-xts-plain64: every sector is one data unit whose tweak is
 * its number, 64 bits little-endian, zero-extended to a block and encrypted
 * with the second half of the key. Block j of the unit is then
 *
 *     C_j = E_K1(P_j xor T_j) xor T_j,    T_0 = E_K2(n),  T_j+1 = T_j * x
 *
 * in GF(2^128) with the polynomial x^128 + x^7 + x^2 + x + 1, the byte
 * string taken as a little-endian number. The tweaks of a unit are laid out
 * first, so the AES work is a single ECB call per unit instead of one per
 * 16 bytes; CNG uses AES-NI underneath. Units are whole blocks (512 to
 * 4096 bytes), so ciphertext stealing never arises.
 */

#ifdef _KERNEL_MODE
#include <ntifs.h>
#include <bcrypt.h>
#else
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <bcrypt.h>
#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif
#endif
#include "ext4xts.h"

#define EXT4_XTS_REDUCE     0x87            /* x^7 + x^2 + x + 1 */

NTSTATUS
Ext4XtsInit(PEXT4_XTS Xts, BCRYPT_ALG_HANDLE AesEcb, const UCHAR *Key, ULONG KeyBytes)
{
    NTSTATUS Status;
    ULONG    Half = KeyBytes / 2;

    Xts->Data = NULL;
    Xts->Tweak = NULL;
    if (KeyBytes != 32 && KeyBytes != 64) {
        return STATUS_INVALID_PARAMETER;
    }

    Status = BCryptGenerateSymmetricKey(AesEcb, &Xts->Data, NULL, 0,
                                        (PUCHAR)Key, Half, 0);
    if (NT_SUCCESS(Status)) {
        Status = BCryptGenerateSymmetricKey(AesEcb, &Xts->Tweak, NULL, 0,
                                            (PUCHAR)Key + Half, Half, 0);
    }
    if (!NT_SUCCESS(Status)) {
        Ext4XtsFree(Xts);
    }
    return Status;
}

VOID
Ext4XtsFree(PEXT4_XTS Xts)
{
    if (Xts->Data) {
        BCryptDestroyKey(Xts->Data);
        Xts->Data = NULL;
    }
    if (Xts->Tweak) {
        BCryptDestroyKey(Xts->Tweak);
        Xts->Tweak = NULL;
    }
}

NTSTATUS
Ext4XtsUnit(PEXT4_XTS Xts, BOOLEAN Encrypt, ULONGLONG Unit,
            PUCHAR Buffer, ULONG Length, PUCHAR Scratch)
{
    return Ext4XtsUnitCopy(Xts, Encrypt, Unit, Buffer, Buffer, Length, Scratch);
}

/* the same from In to Out: the first whitening pass is the copy */
NTSTATUS
Ext4XtsUnitCopy(PEXT4_XTS Xts, BOOLEAN Encrypt, ULONGLONG Unit,
                PUCHAR Buffer, const UCHAR *In, ULONG Length, PUCHAR Scratch)
{
    ULONGLONG   T[2];                       /* the tweak as a 128-bit number, low half first */
    ULONGLONG  *Tw = (ULONGLONG *)Scratch;
    ULONGLONG  *Bw = (ULONGLONG *)Buffer;
    const ULONGLONG *Iw = (const ULONGLONG *)In;
    ULONG       Words = Length / sizeof(ULONGLONG);
    ULONG       Done, i;
    NTSTATUS    Status;

    if (Length == 0 || Length > EXT4_XTS_MAX_UNIT || (Length % EXT4_XTS_BLOCK) != 0) {
        return STATUS_INVALID_PARAMETER;
    }

    /* T_0 = E_K2(n) */
    T[0] = Unit;
    T[1] = 0;
    Status = BCryptEncrypt(Xts->Tweak, (PUCHAR)T, EXT4_XTS_BLOCK, NULL, NULL, 0,
                           (PUCHAR)T, EXT4_XTS_BLOCK, &Done, 0);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    /* every block's tweak, and the whitening on the way in */
    for (i = 0; i < Words; i += 2) {
        ULONGLONG Carry;

        Tw[i] = T[0];
        Tw[i + 1] = T[1];
        Bw[i] = Iw[i] ^ T[0];
        Bw[i + 1] = Iw[i + 1] ^ T[1];

        Carry = T[1] >> 63;                 /* multiply by x */
        T[1] = (T[1] << 1) | (T[0] >> 63);
        T[0] = (T[0] << 1) ^ (Carry * EXT4_XTS_REDUCE);
    }

    Status = Encrypt ?
        BCryptEncrypt(Xts->Data, Buffer, Length, NULL, NULL, 0, Buffer, Length, &Done, 0) :
        BCryptDecrypt(Xts->Data, Buffer, Length, NULL, NULL, 0, Buffer, Length, &Done, 0);
    if (!NT_SUCCESS(Status)) {
        /* nothing half-done leaves: neither the whitened input nor a
           partial result */
        RtlSecureZeroMemory(Buffer, Length);
        return Status;
    }

    /* and on the way out */
    for (i = 0; i < Words; i++) {
        Bw[i] ^= Tw[i];
    }

    return Status;
}
