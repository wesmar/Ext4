/**
 * selftest.c - ext4ctl selftest: the published test vectors of every primitive LUKS needs here.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4ctl.h"

#define SELFTEST_MAX_OUT    64

static BOOL
Hex(const char *Text, UINT8 *Out, ULONG Length)
{
    ULONG i;

    for (i = 0; i < Length; i++) {
        unsigned v;
        if (sscanf_s(Text + 2 * i, "%2x", &v) != 1) {
            return FALSE;
        }
        Out[i] = (UINT8)v;
    }
    return TRUE;
}

static BOOL
Check(const char *Name, const UINT8 *Got, const char *WantHex, ULONG Length)
{
    UINT8 Want[SELFTEST_MAX_OUT];
    BOOL  Ok = Length <= sizeof(Want) && Hex(WantHex, Want, Length) && memcmp(Got, Want, Length) == 0;

    Say("  %s  %s", Ok ? "ok  " : "FAIL", Name);
    return Ok;
}

/* RFC 9106 5.2 / 5.3: Argon2i and Argon2id, t=3, m=32 KiB, p=4, all inputs set */
static int
TestArgon2(void)
{
    UINT8           Out[SELFTEST_MAX_OUT], Pwd[32], Salt[16], Secret[8], Ad[12];
    ARGON2_INPUT    In;
    int             Failed = 0;

    memset(Pwd, 1, sizeof(Pwd));
    memset(Salt, 2, sizeof(Salt));
    memset(Secret, 3, sizeof(Secret));
    memset(Ad, 4, sizeof(Ad));
    memset(&In, 0, sizeof(In));
    In.Password = Pwd;  In.PasswordLength = sizeof(Pwd);
    In.Salt = Salt;     In.SaltLength = sizeof(Salt);
    In.Secret = Secret; In.SecretLength = sizeof(Secret);
    In.Data = Ad;       In.DataLength = sizeof(Ad);
    In.Passes = 3;
    In.MemoryKiB = 32;
    In.Lanes = 4;

    In.Type = ARGON2_ID;
    Failed += !(Argon2(&In, Out, 32) && Check("Argon2id (RFC 9106 5.3)", Out,
        "0d640df58d78766c08c037a34a8b53c9d01ef0452d75b65eb52520e96b01e659", 32));
    In.Type = ARGON2_I;
    Failed += !(Argon2(&In, Out, 32) && Check("Argon2i  (RFC 9106 5.2)", Out,
        "c814d9d1dc7f37aa13f0d77f2494bda1c8de6b016dd388d29952a4c4672b6ce8", 32));
    return Failed;
}

/* RFC 6070: PBKDF2-HMAC-SHA1 "password"/"salt", 4096 iterations */
static int
TestPbkdf2(void)
{
    UINT8 Out[SELFTEST_MAX_OUT];

    return !(CryptoPbkdf2(BCRYPT_SHA1_ALGORITHM, "password", 8, "salt", 4, 4096, Out, 20) &&
             Check("PBKDF2-SHA1 (RFC 6070)", Out, "4b007901b765489abead49d926f721d065a429c1", 20));
}

/* IEEE 1619-2007 vector 1: XTS-AES-128, keys and data zero, unit 0; and back */
static int
TestXts(void)
{
    UINT8       Key[32], Data[32], Scratch[32];
    EXT4_XTS    Xts;
    int         Failed = 0;

    memset(Key, 0, sizeof(Key));
    memset(Data, 0, sizeof(Data));
    memset(&Xts, 0, sizeof(Xts));           /* freed below even if never set up */
    Failed += !(CryptoAesEcb() && NT_SUCCESS(Ext4XtsInit(&Xts, CryptoAesEcb(), Key, sizeof(Key))) &&
                NT_SUCCESS(Ext4XtsUnit(&Xts, TRUE, 0, Data, sizeof(Data), Scratch)) &&
                Check("XTS-AES-128 (IEEE 1619 #1)", Data,
                      "917cf69ebd68b2ec9b9fe9a3eadda692cd43d2f59598ed858c02c2652fbf922e", sizeof(Data)));
    Ext4XtsFree(&Xts);

    Failed += !(NT_SUCCESS(Ext4XtsInit(&Xts, CryptoAesEcb(), Key, sizeof(Key))) &&
                NT_SUCCESS(Ext4XtsUnit(&Xts, FALSE, 0, Data, sizeof(Data), Scratch)) &&
                Check("XTS-AES-128 decrypt", Data,
                      "0000000000000000000000000000000000000000000000000000000000000000", sizeof(Data)));
    Ext4XtsFree(&Xts);
    return Failed;
}

int
CmdSelfTest(void)
{
    int Failed = 0;

    /* one statement each: the order of the output is the order here */
    Failed += TestArgon2();
    Failed += TestPbkdf2();
    Failed += TestXts();
    Say("selftest: %d failed", Failed);
    return Failed ? 1 : 0;
}
