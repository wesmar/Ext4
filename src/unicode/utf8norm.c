/**
 * utf8norm.c - UTF-8 normalization (NFD) and case folding over the trie.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Linux fs/unicode/utf8-norm.c (Copyright (c) 2014 SGI).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * A port of the Linux normalizer, with the ordering logic unchanged: a
 * casefolded ext4 directory hashes and compares names in exactly this
 * form, so any deviation would put entries where Linux does not look.
 *
 * Linux finds the leaf of a character by walking a trie over the bits of
 * its UTF-8 encoding. That trie accepts exactly strict UTF-8, so here the
 * sequence is decoded and its code point indexes the tables of utf8data.c.
 * A leaf holds the canonical combining class and, if the character folds
 * or decomposes, its form. Linux also tests each leaf's Unicode version
 * against the one asked for; ext4 asks for 12.1, the newest the tables
 * know, so that test never holds and the tables carry no versions.
 * utf8byte() walks a string and emits the folded form one byte at a time,
 * sorting combining marks by class with repeated scans instead of a
 * buffer. Hangul syllables are decomposed algorithmically (Unicode 3.12).
 */

#include "utf8n.h"

/* bytes in the UTF-8 sequence that starts at s (s is valid UTF-8) */
static __inline int utf8clen(const char *s)
{
    unsigned char c = (unsigned char)*s;

    return 1 + (c >= 0xC0) + (c >= 0xE0) + (c >= 0xF0);
}

/* decode a 3-byte UTF-8 sequence */
static unsigned int utf8decode3(const char *str)
{
    unsigned int uc;

    uc = *str++ & 0x0F;
    uc <<= 6;
    uc |= *str++ & 0x3F;
    uc <<= 6;
    uc |= *str++ & 0x3F;

    return uc;
}

/* encode a 3-byte UTF-8 sequence */
static int utf8encode3(char *str, unsigned int val)
{
    str[2] = (char)((val & 0x3F) | 0x80);
    val >>= 6;
    str[1] = (char)((val & 0x3F) | 0x80);
    val >>= 6;
    str[0] = (char)(val | 0xE0);

    return 3;
}

/*
 * A leaf: [0] the canonical combining class, or DECOMPOSE; then the
 * NUL-terminated decomposition.
 */
typedef const unsigned char utf8leaf_t;

#define LEAF_CCC(LEAF)  ((LEAF)[0])
#define LEAF_STR(LEAF)  ((const char *)((LEAF) + 1))

#define MINCCC          (0)
#define MAXCCC          (254)
#define STOPPER         (0)
#define DECOMPOSE       (255)

/* marker of a Hangul syllable decomposition: the first byte of its leaf string */
#define HANGUL          (0xFF)

/* Hangul (Unicode 3.12): syllable base, leading, vowel and trailing jamo
   bases and counts */
#define SB  (0xAC00)
#define LB  (0x1100)
#define VB  (0x1161)
#define TB  (0x11A7)
#define LC  (19)
#define VC  (21)
#define TC  (28)
#define NC  (VC * TC)
#define SC  (LC * NC)

/* the leaf of a Hangul syllable, built in the caller's buffer */
static utf8leaf_t *utf8hangul(const char *str, unsigned char *hangul)
{
    unsigned int    si, li, vi, ti;
    unsigned char  *h;

    si = utf8decode3(str) - SB;
    li = si / NC;
    vi = (si % NC) / TC;
    ti = si % TC;

    h = hangul;
    LEAF_CCC(h) = DECOMPOSE;
    h += 1;

    h += utf8encode3((char *)h, li + LB);
    h += utf8encode3((char *)h, vi + VB);
    if (ti) {
        h += utf8encode3((char *)h, ti + TB);
    }
    h[0] = '\0';

    return hangul;
}

/*
 * The leaf of the sequence at s, touching at most len bytes; NULL unless
 * it is strict UTF-8 of a scalar value - the shortest form, no surrogate,
 * nothing past U+10FFFF - which is exactly what the Linux trie accepts.
 */
static utf8leaf_t *utf8nlookup(unsigned char *hangul, const char *s, size_t len)
{
    const unsigned char *u = (const unsigned char *)s;
    utf8leaf_t          *leaf;
    unsigned int         cp;
    size_t               n;
    size_t               i;

    if (len == 0) {
        return NULL;
    }
    if (u[0] < 0x80) {
        n = 1;
        cp = u[0];
    } else if (u[0] < 0xC2) {
        return NULL;                    /* a continuation, or an overlong lead */
    } else if (u[0] < 0xE0) {
        n = 2;
        cp = u[0] & 0x1F;
    } else if (u[0] < 0xF0) {
        n = 3;
        cp = u[0] & 0x0F;
    } else if (u[0] < 0xF5) {
        n = 4;
        cp = u[0] & 0x07;
    } else {
        return NULL;
    }
    if (len < n) {
        return NULL;
    }
    for (i = 1; i < n; i++) {
        if ((u[i] & 0xC0) != 0x80) {
            return NULL;
        }
        cp = (cp << 6) | (u[i] & 0x3F);
    }
    if ((n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
        return NULL;
    }

    leaf = utf8leaf(cp);
    if (LEAF_CCC(leaf) == DECOMPOSE && (unsigned char)LEAF_STR(leaf)[0] == HANGUL) {
        leaf = utf8hangul(s, hangul);
    }
    return leaf;
}

/* the same, for a NUL-terminated decomposition */
static utf8leaf_t *utf8lookup(unsigned char *hangul, const char *s)
{
    return utf8nlookup(hangul, s, (size_t)-1);
}

int utf8nvalid(const char *s, size_t len)
{
    unsigned char hangul[UTF8HANGULLEAF];

    while (len && *s) {
        if (!utf8nlookup(hangul, s, len)) {
            return 0;
        }
        len -= (size_t)utf8clen(s);
        s += utf8clen(s);
    }
    return 1;
}

int utf8ncursor(struct utf8cursor *u8c, const char *s, size_t len)
{
    if (!s) {
        return -1;
    }
    u8c->s = s;
    u8c->p = NULL;
    u8c->ss = NULL;
    u8c->sp = NULL;
    u8c->len = (unsigned int)len;
    u8c->slen = 0;
    u8c->ccc = STOPPER;
    u8c->nccc = STOPPER;
    /* the length must fit */
    if (u8c->len != len) {
        return -1;
    }
    /* a string may not start with a continuation byte */
    if (len > 0 && (*s & 0xC0) == 0x80) {
        return -1;
    }
    return 0;
}

/*
 * The next byte of the normalized form. u8c->s is the position in the
 * string, or in a decomposition while u8c->p holds the position after the
 * decomposed character (decomposition bytes do not count against len).
 * Characters of the class u8c->ccc are emitted; the first scan past a
 * stopper finds the lowest class, each further scan emits one class and
 * finds the next (u8c->ss / u8c->sp: where the scans restart).
 */
int utf8byte(struct utf8cursor *u8c)
{
    utf8leaf_t *leaf;
    int         ccc;

    for (;;) {
        /* the end of a decomposition */
        if (u8c->p && *u8c->s == '\0') {
            u8c->s = u8c->p;
            u8c->p = NULL;
        }

        /* the end of the string */
        if (!u8c->p && (u8c->len == 0 || *u8c->s == '\0')) {
            if (u8c->ccc == STOPPER) {
                return 0;
            }
            /* during a scan it counts as a stopper */
            ccc = STOPPER;
            goto ccc_mismatch;
        } else if ((*u8c->s & 0xC0) == 0x80) {
            /* a continuation of the current character */
            if (!u8c->p) {
                u8c->len--;
            }
            return (unsigned char)*u8c->s++;
        }

        if (u8c->p) {
            leaf = utf8lookup(u8c->hangul, u8c->s);
        } else {
            leaf = utf8nlookup(u8c->hangul, u8c->s, u8c->len);
        }

        /* not UTF-8: a binary name */
        if (!leaf) {
            return -1;
        }

        ccc = LEAF_CCC(leaf);
        if (ccc == DECOMPOSE) {
            u8c->len -= (unsigned int)utf8clen(u8c->s);
            u8c->p = u8c->s + utf8clen(u8c->s);
            u8c->s = LEAF_STR(leaf);
            /* an empty decomposition (an ignorable) has class 0 */
            if (*u8c->s == '\0') {
                if (u8c->ccc == STOPPER) {
                    continue;
                }
                ccc = STOPPER;
                goto ccc_mismatch;
            }

            leaf = utf8lookup(u8c->hangul, u8c->s);
            if (!leaf) {
                return -1;
            }
            ccc = LEAF_CCC(leaf);
        }

        /* a lower class than the next one found so far */
        if (ccc != STOPPER && u8c->ccc < ccc && ccc < u8c->nccc) {
            u8c->nccc = (short)ccc;
        }

        /* the class being emitted */
        if (ccc == u8c->ccc) {
            if (!u8c->p) {
                u8c->len--;
            }
            return (unsigned char)*u8c->s++;
        }

ccc_mismatch:
        if (u8c->nccc == STOPPER) {
            /* start a scan for the first class to emit */
            u8c->ccc = MINCCC - 1;
            u8c->nccc = (short)ccc;
            u8c->sp = u8c->p;
            u8c->ss = u8c->s;
            u8c->slen = u8c->len;
            if (!u8c->p) {
                u8c->len -= (unsigned int)utf8clen(u8c->s);
            }
            u8c->s += utf8clen(u8c->s);
        } else if (ccc != STOPPER) {
            /* not the class being emitted: skip it */
            if (!u8c->p) {
                u8c->len -= (unsigned int)utf8clen(u8c->s);
            }
            u8c->s += utf8clen(u8c->s);
        } else if (u8c->nccc != MAXCCC + 1) {
            /* at a stopper: rescan for the next class */
            u8c->ccc = u8c->nccc;
            u8c->nccc = MAXCCC + 1;
            u8c->s = u8c->ss;
            u8c->p = u8c->sp;
            u8c->len = u8c->slen;
        } else {
            /* every class emitted: go on from here */
            u8c->ccc = STOPPER;
            u8c->nccc = STOPPER;
            u8c->sp = NULL;
            u8c->ss = NULL;
            u8c->slen = 0;
        }
    }
}
