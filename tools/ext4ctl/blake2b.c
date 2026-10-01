/**
 * blake2b.c - BLAKE2b (RFC 7693), unkeyed, for Argon2.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4ctl.h"

static const UINT64 Blake2bIv[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL
};

static const UINT8 Blake2bSigma[12][16] = {
    {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
    { 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 },
    { 11,  8, 12,  0,  5,  2, 15, 13, 10, 14,  3,  6,  7,  1,  9,  4 },
    {  7,  9,  3,  1, 13, 12, 11, 14,  2,  6,  5, 10,  4,  0, 15,  8 },
    {  9,  0,  5,  7,  2,  4, 10, 15, 14,  1, 11, 12,  6,  8,  3, 13 },
    {  2, 12,  6, 10,  0, 11,  8,  3,  4, 13,  7,  5, 15, 14,  1,  9 },
    { 12,  5,  1, 15, 14, 13,  4, 10,  0,  7,  6,  3,  9,  2,  8, 11 },
    { 13, 11,  7, 14, 12,  1,  3,  9,  5,  0, 15,  4,  8,  6,  2, 10 },
    {  6, 15, 14,  9, 11,  3,  0,  8, 12,  2, 13,  7,  1,  4, 10,  5 },
    { 10,  2,  8,  4,  7,  6,  1,  5, 15, 11,  9, 14,  3, 12, 13,  0 },
    {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
    { 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 }
};

static UINT64 Rotr64(UINT64 x, int n) { return (x >> n) | (x << (64 - n)); }

#define B2B_G(a, b, c, d, x, y)                         \
    do {                                                \
        v[a] = v[a] + v[b] + (x); v[d] = Rotr64(v[d] ^ v[a], 32); \
        v[c] = v[c] + v[d];       v[b] = Rotr64(v[b] ^ v[c], 24); \
        v[a] = v[a] + v[b] + (y); v[d] = Rotr64(v[d] ^ v[a], 16); \
        v[c] = v[c] + v[d];       v[b] = Rotr64(v[b] ^ v[c], 63); \
    } while (0)

static void
Blake2bCompress(BLAKE2B *S, BOOL Last)
{
    UINT64 v[16], m[16];
    int i;

    for (i = 0; i < 8; i++) {
        v[i] = S->h[i];
        v[i + 8] = Blake2bIv[i];
    }
    v[12] ^= S->t[0];
    v[13] ^= S->t[1];
    if (Last) {
        v[14] = ~v[14];
    }
    memcpy(m, S->b, sizeof(m));             /* little-endian host */

    for (i = 0; i < 12; i++) {
        const UINT8 *s = Blake2bSigma[i];
        B2B_G(0, 4,  8, 12, m[s[ 0]], m[s[ 1]]);
        B2B_G(1, 5,  9, 13, m[s[ 2]], m[s[ 3]]);
        B2B_G(2, 6, 10, 14, m[s[ 4]], m[s[ 5]]);
        B2B_G(3, 7, 11, 15, m[s[ 6]], m[s[ 7]]);
        B2B_G(0, 5, 10, 15, m[s[ 8]], m[s[ 9]]);
        B2B_G(1, 6, 11, 12, m[s[10]], m[s[11]]);
        B2B_G(2, 7,  8, 13, m[s[12]], m[s[13]]);
        B2B_G(3, 4,  9, 14, m[s[14]], m[s[15]]);
    }
    for (i = 0; i < 8; i++) {
        S->h[i] ^= v[i] ^ v[i + 8];
    }
}

void
Blake2bInit(BLAKE2B *S, size_t OutLen)
{
    int i;

    memset(S, 0, sizeof(*S));
    for (i = 0; i < 8; i++) {
        S->h[i] = Blake2bIv[i];
    }
    S->h[0] ^= 0x01010000ULL ^ (UINT64)OutLen;     /* fanout 1, depth 1, no key */
    S->OutLen = OutLen;
}

void
Blake2bUpdate(BLAKE2B *S, const void *In, size_t Length)
{
    const UINT8 *p = (const UINT8 *)In;

    while (Length > 0) {
        size_t n;
        if (S->c == BLAKE2B_BLOCK) {        /* full block and more to come */
            S->t[0] += BLAKE2B_BLOCK;
            if (S->t[0] < BLAKE2B_BLOCK) {
                S->t[1]++;
            }
            Blake2bCompress(S, FALSE);
            S->c = 0;
        }
        n = min(Length, BLAKE2B_BLOCK - S->c);
        memcpy(S->b + S->c, p, n);
        S->c += n;
        p += n;
        Length -= n;
    }
}

void
Blake2bFinal(BLAKE2B *S, void *Out)
{
    S->t[0] += S->c;
    if (S->t[0] < S->c) {
        S->t[1]++;
    }
    memset(S->b + S->c, 0, BLAKE2B_BLOCK - S->c);
    Blake2bCompress(S, TRUE);
    memcpy(Out, S->h, S->OutLen);
    SecureZeroMemory(S, sizeof(*S));
}

void
Blake2b(void *Out, size_t OutLen, const void *In, size_t InLen)
{
    BLAKE2B S;
    Blake2bInit(&S, OutLen);
    Blake2bUpdate(&S, In, InLen);
    Blake2bFinal(&S, Out);
}
