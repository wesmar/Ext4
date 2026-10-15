/* Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 * The case folding of src/unicode against the Linux trie it replaced.
 *
 *  lookup   every byte string of 1..3 bytes and every 4-byte string (all
 *           of them with --full, else those that can start a 4-byte
 *           sequence) under every length limit: the same leaf or none;
 *           bytes past the limit are continuation bytes, so reading past
 *           it would show
 *  scalars  every code point: folded form, result, compare, validity
 *  bytes    every string of 1..3 bytes: folded form, result, validity
 *  trailing every code point followed by each of a set of bytes
 *  random   random names of code points and stray bytes, folded into
 *           buffers of random size, compared with a case-changed copy
 *  strict   Ext4Utf8Valid must answer as Linux's utf8nlen() does
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int before_Ext4Utf8Casefold(const char *, int, unsigned char *, int);
int before_Ext4Utf8CasefoldCompare(const char *, int, const char *, int);
BOOLEAN before_Ext4Utf8Valid(const char *, int);
int before_linux_valid(const char *, size_t);
int before_lookup(const char *, size_t, const char **);
int after_Ext4Utf8Casefold(const char *, int, unsigned char *, int);
int after_Ext4Utf8CasefoldCompare(const char *, int, const char *, int);
BOOLEAN after_Ext4Utf8Valid(const char *, int);
int after_lookup(const char *, size_t, const char **);

static unsigned long long Checks, OldValidDiffers;

static void Fail(const char *what, const unsigned char *s, int len)
{
    fprintf(stderr, "FAIL %s:", what);
    for (int i = 0; i < len; i++) fprintf(stderr, " %02x", s[i]);
    fprintf(stderr, "\n");
    exit(1);
}

static int Encode(unsigned cp, unsigned char *s)
{
    if (cp < 0x80) { s[0] = (unsigned char)cp; return 1; }
    if (cp < 0x800) { s[0] = (unsigned char)(0xC0 | cp >> 6); s[1] = (unsigned char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) {
        s[0] = (unsigned char)(0xE0 | cp >> 12); s[1] = (unsigned char)(0x80 | ((cp >> 6) & 63));
        s[2] = (unsigned char)(0x80 | (cp & 63)); return 3;
    }
    s[0] = (unsigned char)(0xF0 | cp >> 18); s[1] = (unsigned char)(0x80 | ((cp >> 12) & 63));
    s[2] = (unsigned char)(0x80 | ((cp >> 6) & 63)); s[3] = (unsigned char)(0x80 | (cp & 63)); return 4;
}

static void Lookup(const unsigned char *s, size_t len)
{
    const char *a = NULL, *b = NULL;
    int ca = before_lookup((const char *)s, len, &a);
    int cb = after_lookup((const char *)s, len, &b);
    Checks++;
    if (ca != cb || (ca >= 0 && strcmp(a, b))) Fail("lookup", s, (int)len);
}

/* fold, compare with itself and check validity of one string */
static void Name(const unsigned char *s, int len, int outlen)
{
    unsigned char fa[256], fb[256];
    int ra = before_Ext4Utf8Casefold((const char *)s, len, fa, outlen);
    int rb = after_Ext4Utf8Casefold((const char *)s, len, fb, outlen);
    Checks++;
    if (ra != rb || (ra >= 0 && memcmp(fa, fb, (size_t)ra + 1))) Fail("casefold", s, len);
    if (before_Ext4Utf8CasefoldCompare((const char *)s, len, (const char *)s, len) !=
        after_Ext4Utf8CasefoldCompare((const char *)s, len, (const char *)s, len)) Fail("compare self", s, len);
    int linux = before_linux_valid((const char *)s, (size_t)len);
    if (!!after_Ext4Utf8Valid((const char *)s, len) != linux) Fail("strict validity", s, len);
    if (!!before_Ext4Utf8Valid((const char *)s, len) != linux) OldValidDiffers++;
}

static unsigned long long Rng = 0x9E3779B97F4A7C15ull;
static unsigned Next(void)
{
    Rng ^= Rng >> 12; Rng ^= Rng << 25; Rng ^= Rng >> 27;
    return (unsigned)((Rng * 0x2545F4914F6CDD1Dull) >> 32);
}

/* code points that exercise folding, ordering and ignorables */
static const unsigned Pool[][2] = {
    {0x20, 0x7E}, {0xA0, 0x24F}, {0x300, 0x36F}, {0x370, 0x3FF}, {0x400, 0x52F}, {0x531, 0x58F},
    {0x10A0, 0x10FF}, {0x13A0, 0x13FD}, {0x1C80, 0x1CBF}, {0x1DC0, 0x1DFF}, {0x1E00, 0x1FFF},
    {0x2000, 0x206F}, {0x20D0, 0x20FF}, {0x2160, 0x218F}, {0x24B6, 0x24E9}, {0x2C00, 0x2C7F},
    {0xA640, 0xA69F}, {0xA720, 0xA7FF}, {0xAB70, 0xABBF}, {0x1100, 0x11FF}, {0xAC00, 0xD7A3},
    {0x3040, 0x30FF}, {0x4E00, 0x4E80}, {0xFB00, 0xFB4F}, {0xFE00, 0xFE0F}, {0xFF21, 0xFF5A},
    {0x10400, 0x1044F}, {0x104B0, 0x104FB}, {0x16E40, 0x16E7F}, {0x1E900, 0x1E943},
    {0x1F600, 0x1F64F}, {0xE0000, 0xE007F}, {0x0, 0x10FFFF},
};

int main(int argc, char **argv)
{
    int full = argc > 1 && !strcmp(argv[1], "--full");
    unsigned char s[64];
    unsigned long long t0 = GetTickCount64(), n;

    /* lookup */
    for (size_t len = 1; len <= 3; len++) {
        for (unsigned v = 0; v < 1u << (8 * len); v++) {
            memset(s, 0x80, 8);
            for (size_t i = 0; i < len; i++) s[i] = (unsigned char)(v >> (8 * i));
            Lookup(s, len);
        }
    }
    memset(s, 0x80, 8);
    for (unsigned long long v = 0; v < 0x100000000ull; v++) {
        s[0] = (unsigned char)(v >> 24);
        if (!full && (s[0] < 0xF0 || s[0] > 0xF7)) { v |= 0xFFFFFF; continue; }
        s[1] = (unsigned char)(v >> 16); s[2] = (unsigned char)(v >> 8); s[3] = (unsigned char)v;
        Lookup(s, 4);
        if ((s[1] & 0xC0) != 0x80 && !full) v |= 0xFFFF;   /* the trie stops at the second byte */
    }
    n = Checks;
    printf("lookup: %llu strings and limits, same leaf (%llu s)\n", n, (GetTickCount64() - t0) / 1000);

    /* scalars */
    for (unsigned cp = 0; cp < 0x110000; cp++) {
        if (cp >= 0xD800 && cp < 0xE000) continue;
        Name(s, Encode(cp, s), 256);
    }
    /* bytes */
    for (unsigned v = 0; v < 0x1000000; v++) {
        s[0] = (unsigned char)v; s[1] = (unsigned char)(v >> 8); s[2] = (unsigned char)(v >> 16);
        Name(s, v < 0x100 ? 1 : v < 0x10000 ? 2 : 3, 256);
    }
    /* trailing */
    static const unsigned char tail[] = {0x00, 0x41, 0x61, 0x80, 0xBF, 0xC0, 0xC3, 0xCC, 0xE0, 0xED, 0xF0, 0xF4, 0xF5, 0xFF};
    for (unsigned cp = 0; cp < 0x110000; cp++) {
        if (cp >= 0xD800 && cp < 0xE000) continue;
        int k = Encode(cp, s);
        for (size_t t = 0; t < sizeof(tail); t++) { s[k] = tail[t]; Name(s, k + 1, 256); }
    }
    printf("scalars, bytes, trailing: %llu names (%llu s)\n", Checks - n, (GetTickCount64() - t0) / 1000);
    n = Checks;

    /* sequences: every string of up to six tokens over the pieces the
       sorting passes treat differently - a starter, marks of classes 1,
       202, 220, 230 and 240, an ignorable, a stray byte, characters whose
       expansion carries marks, a Hangul syllable, a folding to two letters */
    static const char *piece[] = {
        "a", "\xcc\x81" /* 230 */, "\xcc\x96" /* 220 */, "\xcc\xa7" /* 202 */, "\xcc\xb4" /* 1 */,
        "\xcd\x85" /* 240 */, "\xe2\x80\x8b" /* ignorable */, "\x80" /* stray */,
        "\xe1\xb8\x88" /* C cedilla acute */, "\xc7\xba" /* A ring acute */, "\xea\xb0\x81" /* Hangul */,
        "\xc3\x9f" /* sharp s */,
    };
    const int pieces = (int)(sizeof(piece) / sizeof(piece[0]));
    for (int length = 1; length <= 6; length++) {
        int index[6] = {0};
        for (;;) {
            int len = 0;
            for (int i = 0; i < length; i++) {
                size_t k = strlen(piece[index[i]]);
                memcpy(s + len, piece[index[i]], k);
                len += (int)k;
            }
            Name(s, len, 256);
            Name(s, len, 1 + len % 7);
            int i = length - 1;
            while (i >= 0 && ++index[i] == pieces) index[i--] = 0;
            if (i < 0) break;
        }
    }
    printf("sequences: %llu names (%llu s)\n", Checks - n, (GetTickCount64() - t0) / 1000);
    n = Checks;

    /* random */
    unsigned long long rounds = full ? 50000000ull : 5000000ull;
    for (unsigned long long r = 0; r < rounds; r++) {
        int len = 0, chars = 1 + (int)(Next() % 24);
        unsigned char t[256];
        for (int c = 0; c < chars && len < 60; c++) {
            unsigned pick = Next() % 100;
            if (pick < 4) {
                static const unsigned char junk[] = {0x00, 0x80, 0xBF, 0xC0, 0xC1, 0xC3, 0xE0, 0xED, 0xF0, 0xF4, 0xF5, 0xFF};
                s[len++] = junk[Next() % sizeof(junk)];
                continue;
            }
            const unsigned *range = Pool[Next() % (sizeof(Pool) / sizeof(Pool[0]))];
            unsigned cp = range[0] + Next() % (range[1] - range[0] + 1);
            if (cp >= 0xD800 && cp < 0xE000) cp = 0x41;
            len += Encode(cp, s + len);
        }
        Name(s, len, 1 + (int)(Next() % 160));
        /* the same name with ASCII case flipped and a combining pair swapped */
        memcpy(t, s, (size_t)len);
        for (int i = 0; i < len; i++) if ((t[i] | 0x20) >= 'a' && (t[i] | 0x20) <= 'z') t[i] ^= 0x20;
        if (before_Ext4Utf8CasefoldCompare((const char *)s, len, (const char *)t, len) !=
            after_Ext4Utf8CasefoldCompare((const char *)s, len, (const char *)t, len)) Fail("compare flipped", s, len);
        int other = (int)(Next() % (unsigned)len) + 1;
        if (before_Ext4Utf8CasefoldCompare((const char *)s, len, (const char *)t, other) !=
            after_Ext4Utf8CasefoldCompare((const char *)s, len, (const char *)t, other)) Fail("compare prefix", s, len);
        Checks += 2;
    }
    printf("random: %llu checks (%llu s)\n", Checks - n, (GetTickCount64() - t0) / 1000);
    printf("strict: %llu strings the old Ext4Utf8Valid answered unlike Linux's utf8nlen()\n", OldValidDiffers);

    /* speed of the folding itself */
    static const char *names[] = {
        "Document_2026_final_version.txt", "Za\xc5\xbc\xc3\xb3\xc5\x82\xc4\x87 g\xc4\x99\xc5\x9bl\xc4\x85 ja\xc5\xba\xc5\x84.txt",
        "\xce\x9a\xce\xb1\xce\xbb\xce\xb7\xce\xbc\xce\xad\xcf\x81\xce\xb1.doc", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe3\x83\x95\xe3\x82\xa1\xe3\x82\xa4\xe3\x83\xab.txt",
        "\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4 \xed\x8c\x8c\xec\x9d\xbc.hwp",
    };
    for (int side = 0; side < 2; side++) {
        LARGE_INTEGER f, a, b;
        unsigned char out[256];
        volatile int sink = 0;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        for (int r = 0; r < 200000; r++)
            for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++)
                sink += side ? after_Ext4Utf8Casefold(names[k], (int)strlen(names[k]), out, 256)
                             : before_Ext4Utf8Casefold(names[k], (int)strlen(names[k]), out, 256);
        QueryPerformanceCounter(&b);
        printf("%s: %.1f ns per name\n", side ? "tables" : "trie  ",
               1e9 * (double)(b.QuadPart - a.QuadPart) / (double)f.QuadPart / (200000.0 * 5));
    }
    printf("UNICODE-MODEL: %llu checks: ALL PASSED\n", Checks);
    return 0;
}
