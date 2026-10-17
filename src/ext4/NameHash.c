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
 * Tested by tests/name-hash-wsl.sh against e2fsprogs (debugfs dx_hash).
 *
 * Every name of a directory listing is hashed once, so the hashes are
 * written for speed as well: the words are built a whole word at a time and
 * MD4's steps are written out, as RFC 1320 gives them, which keeps the state
 * in registers.
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

/*
 * One word of four bytes, each shifted in at the bottom. A signed byte below
 * zero borrows from the bytes before it; what it borrows from the start
 * value is shifted out by the end, so a whole word needs none.
 */
static unsigned int Word(const unsigned char *Bytes, unsigned int Count, unsigned int Start, int Unsigned)
{
    unsigned int word = Start, i;

    for (i = 0; i < Count; i++)
        word = (word << 8) + ByteAt(Bytes, i, Unsigned);
    return word;
}

/* Count words from a piece; Left is the length of the name from it on */
static void Words(const unsigned char *Piece, unsigned int Left, int Unsigned,
                  unsigned int *Out, unsigned int Count)
{
    unsigned int pad = Left | (Left << 8), take, w;

    pad |= pad << 16;
    take = Left < Count * 4 ? Left : Count * 4;
    for (w = 0; w < take / 4; w++)
        Out[w] = Word(Piece + w * 4, 4, 0, Unsigned);
    if (take & 3) {
        Out[w] = Word(Piece + w * 4, take & 3, pad, Unsigned);     /* its top bytes are pad */
        w++;
    }
    while (w < Count)
        Out[w++] = pad;
}

/* MD4's three rounds, eight steps each: x selects y or z, majority, parity */
#define MD4_F(x, y, z)  ((z) ^ ((x) & ((y) ^ (z))))
#define MD4_G(x, y, z)  (((x) & (y)) | ((x) & (z)) | ((y) & (z)))
#define MD4_H(x, y, z)  ((x) ^ (y) ^ (z))
#define MD4_STEP(f, a, b, c, d, word, shift) ((a) = Rol((a) + f((b), (c), (d)) + (word), (shift)))
#define MD4_K2          0x5A827999u
#define MD4_K3          0x6ED9EBA1u

static void HalfMd4(unsigned int State[4], const unsigned int In[8])
{
    unsigned int a = State[0], b = State[1], c = State[2], d = State[3];

    MD4_STEP(MD4_F, a, b, c, d, In[0], 3);
    MD4_STEP(MD4_F, d, a, b, c, In[1], 7);
    MD4_STEP(MD4_F, c, d, a, b, In[2], 11);
    MD4_STEP(MD4_F, b, c, d, a, In[3], 19);
    MD4_STEP(MD4_F, a, b, c, d, In[4], 3);
    MD4_STEP(MD4_F, d, a, b, c, In[5], 7);
    MD4_STEP(MD4_F, c, d, a, b, In[6], 11);
    MD4_STEP(MD4_F, b, c, d, a, In[7], 19);

    MD4_STEP(MD4_G, a, b, c, d, In[1] + MD4_K2, 3);
    MD4_STEP(MD4_G, d, a, b, c, In[3] + MD4_K2, 5);
    MD4_STEP(MD4_G, c, d, a, b, In[5] + MD4_K2, 9);
    MD4_STEP(MD4_G, b, c, d, a, In[7] + MD4_K2, 13);
    MD4_STEP(MD4_G, a, b, c, d, In[0] + MD4_K2, 3);
    MD4_STEP(MD4_G, d, a, b, c, In[2] + MD4_K2, 5);
    MD4_STEP(MD4_G, c, d, a, b, In[4] + MD4_K2, 9);
    MD4_STEP(MD4_G, b, c, d, a, In[6] + MD4_K2, 13);

    MD4_STEP(MD4_H, a, b, c, d, In[3] + MD4_K3, 3);
    MD4_STEP(MD4_H, d, a, b, c, In[7] + MD4_K3, 9);
    MD4_STEP(MD4_H, c, d, a, b, In[2] + MD4_K3, 11);
    MD4_STEP(MD4_H, b, c, d, a, In[6] + MD4_K3, 15);
    MD4_STEP(MD4_H, a, b, c, d, In[1] + MD4_K3, 3);
    MD4_STEP(MD4_H, d, a, b, c, In[5] + MD4_K3, 9);
    MD4_STEP(MD4_H, c, d, a, b, In[0] + MD4_K3, 11);
    MD4_STEP(MD4_H, b, c, d, a, In[4] + MD4_K3, 15);

    State[0] += a;
    State[1] += b;
    State[2] += c;
    State[3] += d;
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
