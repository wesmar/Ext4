/*
 * names.c - the names tests/htree-vm.ps1 puts into one indexed directory:
 * random Windows-valid UTF-8 names, and pairs that share their major hash
 * on the volume they are made for (the index must keep such a pair in one
 * leaf, or mark the leaf after a split as continuing the hash).
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * usage: names <out> <count> <rng> <version> <unsigned> <seed-uuid> <min> <max> <pairs>
 *
 *   version    0 legacy, 1 half_md4, 2 tea (the volume's default hash)
 *   unsigned   1 when the volume hashes names as unsigned bytes
 *   seed-uuid  the volume's Directory Hash Seed (dumpe2fs)
 *   min, max   the length of a name in bytes, of UTF-8
 *   pairs      how many colliding pairs (or triples) to put in, at most
 *
 * The names are distinct after case folding of every letter they use (so
 * they stay distinct in a casefolded directory too), begin with neither a
 * blank nor a letter a reserved device name begins with, and end with
 * neither a dot nor a blank. One name per line.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/ext4/NameHash.c"

static unsigned long long Rng;

static unsigned int Next(void)
{
    Rng ^= Rng >> 12;
    Rng ^= Rng << 25;
    Rng ^= Rng >> 27;
    return (unsigned int)((Rng * 2685821657736338717ull) >> 32);
}

/* ---- characters ---- */

static const char Punct[] = " -_.,;=+!@#$%&'()[]{}~^";
static const unsigned int PolishLower[] = { 0x105, 0x107, 0x119, 0x142, 0x144, 0xF3, 0x15B, 0x17A, 0x17C };
static const unsigned int PolishUpper[] = { 0x104, 0x106, 0x118, 0x141, 0x143, 0xD3, 0x15A, 0x179, 0x17B };

/* a code point of the alphabet */
static unsigned int Char(int first)
{
    unsigned int pick = Next() % 100, k;

    if (first) {
        static const char safe[] = "0123456789bdefghijkmoqrstuvwxyzBDEFGHIJKMOQRSTUVWXYZ";
        unsigned int c;

        if (pick < 80)
            return (unsigned char)safe[Next() % (sizeof(safe) - 1)];
        do c = Char(0);
        while (c < 0x80);
        return c;
    }
    if (pick < 45) {
        unsigned int c = 'a' + Next() % 26;
        return Next() % 2 ? c - 32 : c;
    }
    if (pick < 55)
        return '0' + Next() % 10;
    if (pick < 63)
        return (unsigned char)Punct[Next() % (sizeof(Punct) - 1)];
    if (pick < 75) {
        k = Next() % 9;
        return Next() % 2 ? PolishUpper[k] : PolishLower[k];
    }
    if (pick < 84)
        return 0x410 + Next() % 64;                     /* Cyrillic А-я */
    if (pick < 90) {
        unsigned int c;
        do c = 0x391 + Next() % 25;                     /* Greek Α-Ω */
        while (c == 0x3A2);                             /* unassigned */
        return Next() % 2 ? c + 0x20 : c;               /* or α-ω (no final sigma) */
    }
    if (pick < 97)
        return 0x4E00 + Next() % 0x200;                 /* CJK */
    return 0x1F600 + Next() % 0x50;                     /* emoji */
}

/* the folded form, for the distinctness of names */
static unsigned int Fold(unsigned int c)
{
    unsigned int k;

    if (c >= 'A' && c <= 'Z')
        return c + 32;
    for (k = 0; k < 9; k++)
        if (c == PolishUpper[k])
            return PolishLower[k];
    if (c >= 0x410 && c <= 0x42F)
        return c + 0x20;
    if (c >= 0x391 && c <= 0x3A9)
        return c + 0x20;
    return c;
}

static unsigned int Utf8(unsigned int c, unsigned char *out)
{
    if (c < 0x80) {
        out[0] = (unsigned char)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (unsigned char)(0xC0 | c >> 6);
        out[1] = (unsigned char)(0x80 | (c & 63));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = (unsigned char)(0xE0 | c >> 12);
        out[1] = (unsigned char)(0x80 | (c >> 6 & 63));
        out[2] = (unsigned char)(0x80 | (c & 63));
        return 3;
    }
    out[0] = (unsigned char)(0xF0 | c >> 18);
    out[1] = (unsigned char)(0x80 | (c >> 12 & 63));
    out[2] = (unsigned char)(0x80 | (c >> 6 & 63));
    out[3] = (unsigned char)(0x80 | (c & 63));
    return 4;
}

/* ---- the set of folded names ---- */

typedef struct {
    unsigned char   Name[256];
    unsigned char   Key[256];
    unsigned int    Length, KeyLength;
} NAME;

static unsigned long long *Slots;
static unsigned int SlotMask;

static unsigned long long KeyHash(const NAME *n)
{
    unsigned long long h = 1469598103934665603ull;
    unsigned int i;

    for (i = 0; i < n->KeyLength; i++)
        h = (h ^ n->Key[i]) * 1099511628211ull;
    return h | 1;
}

/* 1: the name was not there and is now */
static int Remember(const NAME *n)
{
    unsigned long long h = KeyHash(n);
    unsigned int at = (unsigned int)h & SlotMask;

    while (Slots[at]) {
        if (Slots[at] == h)
            return 0;   /* the same (or a 64-bit collision: left out as well) */
        at = (at + 1) & SlotMask;
    }
    Slots[at] = h;
    return 1;
}

/* a random name of Min..Max bytes */
static void MakeName(NAME *n, unsigned int Min, unsigned int Max)
{
    unsigned int target = Min + Next() % (Max - Min + 1);

    for (;;) {
        n->Length = n->KeyLength = 0;
        while (n->Length < target) {
            unsigned int c = Char(n->Length == 0), size;
            unsigned char bytes[4];

            size = Utf8(c, bytes);
            if (n->Length + size > target)
                break;
            memcpy(n->Name + n->Length, bytes, size);
            n->Length += size;
            n->KeyLength += Utf8(Fold(c), n->Key + n->KeyLength);
        }
        if (n->Length && n->Name[0] != ' ' && n->Name[n->Length - 1] != '.' && n->Name[n->Length - 1] != ' ')
            return;
    }
}

/* ---- the colliding names ---- */

typedef struct {
    unsigned int    Major;
    unsigned int    Index;
} HASHED;

static int ByMajor(const void *a, const void *b)
{
    const HASHED *x = a, *y = b;
    if (x->Major != y->Major)
        return x->Major < y->Major ? -1 : 1;
    return x->Index < y->Index ? -1 : x->Index > y->Index;
}

/* a candidate name: lower-case ASCII and digits, 6 to 12 of them, from Index */
static void Candidate(NAME *n, unsigned int Index, unsigned long long Salt)
{
    static const char safe[] = "0123456789bdefghijkmoqrstuvwxyz";
    unsigned long long x = (Index + Salt) * 0x9E3779B97F4A7C15ull;
    unsigned int i;

    n->Length = 6 + (unsigned int)(x >> 60) % 7;
    for (i = 0; i < n->Length; i++) {
        x ^= x >> 29;
        x *= 0xBF58476D1CE4E5B9ull;
        n->Name[i] = (unsigned char)safe[(x >> 33) % (sizeof(safe) - 1)];
    }
    memcpy(n->Key, n->Name, n->Length);
    n->KeyLength = n->Length;
}

int main(int argc, char **argv)
{
    unsigned int count, version, min, max, pairs, seed[4], i, k, made = 0, collided = 0, groups = 0;
    unsigned char uuid[16];
    unsigned long long salt;
    const char *s;
    FILE *out;
    NAME n;

    if (argc < 10) {
        fprintf(stderr, "usage: names <out> <count> <rng> <version> <unsigned> <seed-uuid> <min> <max> <pairs>\n");
        return 2;
    }
    count = (unsigned int)strtoul(argv[2], NULL, 10);
    Rng = strtoull(argv[3], NULL, 10) * 0x9E3779B97F4A7C15ull + 1;
    salt = Rng;
    version = (unsigned int)strtoul(argv[4], NULL, 10) + (strtoul(argv[5], NULL, 10) ? NH_UNSIGNED_DELTA : 0);
    for (s = argv[6], k = 0; *s && k < 32; s++) {
        unsigned int d;
        if (*s == '-')
            continue;
        d = *s <= '9' ? (unsigned int)(*s - '0') : (unsigned int)((*s | 32) - 'a' + 10);
        if (k % 2 == 0)
            uuid[k / 2] = (unsigned char)(d << 4);
        else
            uuid[k / 2] |= (unsigned char)d;
        k++;
    }
    if (k != 32) {
        fprintf(stderr, "names: a seed of 32 hex digits expected\n");
        return 2;
    }
    for (k = 0; k < 4; k++)
        seed[k] = uuid[4 * k] | uuid[4 * k + 1] << 8 | uuid[4 * k + 2] << 16 | (unsigned int)uuid[4 * k + 3] << 24;
    min = (unsigned int)strtoul(argv[7], NULL, 10);
    max = (unsigned int)strtoul(argv[8], NULL, 10);
    pairs = (unsigned int)strtoul(argv[9], NULL, 10);
    if (!count || !min || min > max || max > 240)
        return 2;

    for (SlotMask = 1; SlotMask < count * 4 + 64; SlotMask <<= 1)
        ;
    Slots = calloc(SlotMask, sizeof(*Slots));
    SlotMask--;
    out = fopen(argv[1], "wb");
    if (!Slots || !out)
        return 2;

    /* the colliding ones first: among 2^21 candidates some 1000 pairs share
       a major hash; each group goes in whole */
    if (pairs) {
        unsigned int total = 1u << 21;
        HASHED *h = malloc(total * sizeof(*h));

        if (!h)
            return 2;
        for (i = 0; i < total; i++) {
            unsigned int minor;
            Candidate(&n, i, salt);
            if (NhHash(n.Name, n.Length, version, seed, &h[i].Major, &minor))
                return 2;
            h[i].Index = i;
        }
        qsort(h, total, sizeof(*h), ByMajor);
        for (i = 0; i < total && groups < pairs; ) {
            unsigned int j = i + 1;
            while (j < total && h[j].Major == h[i].Major)
                j++;
            if (j - i > 1) {
                for (k = i; k < j && made < count; k++) {
                    Candidate(&n, h[k].Index, salt);
                    if (Remember(&n)) {
                        fwrite(n.Name, 1, n.Length, out);
                        fputc('\n', out);
                        made++;
                        collided++;
                    }
                }
                groups++;
            }
            i = j;
        }
        free(h);
    }

    while (made < count) {
        MakeName(&n, min, max);
        if (!Remember(&n))
            continue;
        fwrite(n.Name, 1, n.Length, out);
        fputc('\n', out);
        made++;
    }
    fclose(out);
    printf("%u names, %u of them in %u groups sharing a major hash\n", made, collided, groups);
    return 0;
}
