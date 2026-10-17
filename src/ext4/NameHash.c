/**
 * NameHash.c - the hashes an indexed (htree) directory orders its names by.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * The hashes are part of the ext4 disk format - every implementation must
 * put a name where every other one looks for it - so they are fixed to the
 * bit (the format's documentation, "Hash Tree Directories"):
 *
 *  legacy    two words run through the name; each byte is mixed in by
 *            multiplying it by 7152373, xoring the first word and adding
 *            the second, folded below 2^31; the major hash is the first word
 *            doubled, the minor hash 0
 *  half MD4  the name in 32-byte pieces, each read into eight words and mixed
 *            into a four-word state by MD4's three rounds, eight steps each;
 *            major: state word 1, minor: state word 2
 *  TEA       the name in 16-byte pieces, four words each, mixed into the
 *            first two state words by 16 rounds of TEA; major: word 0,
 *            minor: word 1
 *
 * A piece reads its bytes into words from the low end of the name, each new
 * byte shifted in at the bottom; the words it does not fill hold the length
 * left of the name, repeated in every byte. The state starts as MD4's
 * constants, or as the volume's seed when the seed is not zero. A name is
 * read as signed or as unsigned bytes, by the version. The major hash's low
 * bit is cleared, and NH_END, which the index reserves, becomes the even
 * value below it.
 *
 * Tested by tests/name-hash-model.ps1 against e2fsprogs (debugfs dx_hash).
 */

#include "NameHash.h"

static unsigned int Rol(unsigned int x, unsigned int s)
{
    return (x << s) | (x >> (32 - s));
}

static unsigned int ByteAt(const unsigned char *Name, unsigned int i, int Unsigned)
{
    return Unsigned ? (unsigned int)Name[i] : (unsigned int)(int)(signed char)Name[i];
}

/* Count words from a piece; Left is the length of the name from it on */
static void Words(const unsigned char *Piece, unsigned int Left, int Unsigned,
                  unsigned int *Out, unsigned int Count)
{
    unsigned int pad = Left | (Left << 8), take, i, w = 0, word;

    pad |= pad << 16;
    take = Left < Count * 4 ? Left : Count * 4;
    word = pad;
    for (i = 0; i < take; i++) {
        if ((i & 3) == 0)
            word = pad;
        word = (word << 8) + ByteAt(Piece, i, Unsigned);
        if ((i & 3) == 3)
            Out[w++] = word;
    }
    if (take & 3)
        Out[w++] = word;        /* a word begun: its top bytes are pad */
    while (w < Count)
        Out[w++] = pad;
}

/* MD4's rounds, cut to eight steps: which word each step takes, its shift */
static const unsigned char Md4Word[3][8] = {
    { 0, 1, 2, 3, 4, 5, 6, 7 }, { 1, 3, 5, 7, 0, 2, 4, 6 }, { 3, 7, 2, 6, 1, 5, 0, 4 },
};
static const unsigned char Md4Shift[3][4] = { { 3, 7, 11, 19 }, { 3, 5, 9, 13 }, { 3, 9, 11, 15 } };
static const unsigned int Md4Add[3] = { 0, 0x5A827999u, 0x6ED9EBA1u };

static void HalfMd4(unsigned int State[4], const unsigned int In[8])
{
    unsigned int s[4], r, j;

    for (j = 0; j < 4; j++)
        s[j] = State[j];
    for (r = 0; r < 3; r++) {
        for (j = 0; j < 8; j++) {
            /* the word changed goes a, d, c, b, ...; the others follow it */
            unsigned int t = (4 - j) & 3;
            unsigned int x = s[(t + 1) & 3], y = s[(t + 2) & 3], z = s[(t + 3) & 3], f;

            if (r == 0)
                f = z ^ (x & (y ^ z));                  /* x selects y or z */
            else if (r == 1)
                f = (x & y) | (x & z) | (y & z);        /* majority */
            else
                f = x ^ y ^ z;                          /* parity */
            s[t] = Rol(s[t] + f + In[Md4Word[r][j]] + Md4Add[r], Md4Shift[r][j & 3]);
        }
    }
    for (j = 0; j < 4; j++)
        State[j] += s[j];
}

static void Tea(unsigned int State[4], const unsigned int In[4])
{
    unsigned int sum = 0, x = State[0], y = State[1], n;

    for (n = 0; n < 16; n++) {
        sum += 0x9E3779B9u;
        x += ((y << 4) + In[0]) ^ (y + sum) ^ ((y >> 5) + In[1]);
        y += ((x << 4) + In[2]) ^ (x + sum) ^ ((x >> 5) + In[3]);
    }
    State[0] += x;
    State[1] += y;
}

static unsigned int Legacy(const unsigned char *Name, unsigned int Length, int Unsigned)
{
    unsigned int w0 = 0x12A3FE2Du, w1 = 0x37ABE8F9u, i;

    for (i = 0; i < Length; i++) {
        unsigned int next = w1 + (w0 ^ (ByteAt(Name, i, Unsigned) * 7152373u));
        if (next & 0x80000000u)
            next -= 0x7FFFFFFFu;
        w1 = w0;
        w0 = next;
    }
    return w0 << 1;
}

int NhHash(const unsigned char *Name, unsigned int Length, unsigned int Version,
           const unsigned int *Seed, unsigned int *Major, unsigned int *Minor)
{
    unsigned int state[4] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u };
    unsigned int in[8], left, major, minor = 0, i;
    int          isUnsigned;

    if (Version > NH_TEA_UNSIGNED)
        return -1;
    isUnsigned = Version >= NH_UNSIGNED_DELTA;
    if (Seed && (Seed[0] | Seed[1] | Seed[2] | Seed[3]))
        for (i = 0; i < 4; i++)
            state[i] = Seed[i];

    switch (Version % NH_UNSIGNED_DELTA) {
    case NH_LEGACY:
        major = Legacy(Name, Length, isUnsigned);
        break;
    case NH_HALF_MD4:
        for (left = Length; left > 0; left = left > 32 ? left - 32 : 0, Name += 32) {
            Words(Name, left, isUnsigned, in, 8);
            HalfMd4(state, in);
        }
        major = state[1];
        minor = state[2];
        break;
    default:
        for (left = Length; left > 0; left = left > 16 ? left - 16 : 0, Name += 16) {
            Words(Name, left, isUnsigned, in, 4);
            Tea(state, in);
        }
        major = state[0];
        minor = state[1];
        break;
    }
    major &= ~1u;
    if (major == NH_END)
        major = NH_END - 2;
    *Major = major;
    *Minor = minor;
    return 0;
}
