/**
 * utf8norm.c - UTF-8 normalization (NFD) and case folding over the trie.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Linux fs/unicode/utf8-norm.c (Copyright (c) 2014 SGI).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * A port of the Linux normalizer, with the logic unchanged: a casefolded
 * ext4 directory hashes and compares names in exactly this form, so any
 * deviation would put entries where Linux does not look for them.
 *
 * The trie (utf8data.c) decodes one UTF-8 sequence to a leaf: the Unicode
 * version that defined the code point, its canonical combining class and,
 * if it decomposes, the decomposition. utf8byte() walks a string and emits
 * the normalized form one byte at a time, sorting combining marks by class
 * with repeated scans instead of a buffer. Hangul syllables are decomposed
 * algorithmically (Unicode 3.12).
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
 * The trie: internal nodes are one byte plus up to three bytes of offset.
 *  NEXTBYTE  - advance to the next input byte
 *  BITNUM    - the bit of the byte tested
 *  OFFLEN    - bytes of offset (0: a non-branching node)
 * non-branching: RIGHTPATH - the node follows for a set bit,
 *                TRIENODE  - what follows is a node, not a leaf
 * branching:     LEFTNODE / RIGHTNODE - that side is a node, not a leaf
 */
typedef const unsigned char utf8trie_t;
#define BITNUM          0x07
#define NEXTBYTE        0x08
#define OFFLEN          0x30
#define OFFLEN_SHIFT    4
#define RIGHTPATH       0x40
#define TRIENODE        0x80
#define RIGHTNODE       0x40
#define LEFTNODE        0x80

/*
 * A leaf: [0] the generation (index into utf8agetab), [1] the canonical
 * combining class, or DECOMPOSE; then the NUL-terminated decomposition.
 */
typedef const unsigned char utf8leaf_t;

#define LEAF_GEN(LEAF)  ((LEAF)[0])
#define LEAF_CCC(LEAF)  ((LEAF)[1])
#define LEAF_STR(LEAF)  ((const char *)((LEAF) + 2))

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
    LEAF_GEN(h) = 2;
    LEAF_CCC(h) = DECOMPOSE;
    h += 2;

    h += utf8encode3((char *)h, li + LB);
    h += utf8encode3((char *)h, vi + VB);
    if (ti) {
        h += utf8encode3((char *)h, ti + TB);
    }
    h[0] = '\0';

    return hangul;
}

/*
 * The leaf of the sequence at s, touching at most len bytes; NULL when it
 * is not valid UTF-8 of a known code point.
 */
static utf8leaf_t *utf8nlookup(const struct unicode_map *um, enum utf8_normalization n,
                               unsigned char *hangul, const char *s, size_t len)
{
    utf8trie_t *trie = um->tables->utf8data + um->ntab[n]->offset;
    int         offlen;
    int         offset;
    int         mask;
    int         node;

    if (len == 0) {
        return NULL;
    }

    node = 1;
    while (node) {
        offlen = (*trie & OFFLEN) >> OFFLEN_SHIFT;
        if (*trie & NEXTBYTE) {
            if (--len == 0) {
                return NULL;
            }
            s++;
        }
        mask = 1 << (*trie & BITNUM);
        if (*s & mask) {
            /* right leg */
            if (offlen) {
                node = (*trie & RIGHTNODE);
                offset = trie[offlen];
                while (--offlen) {
                    offset <<= 8;
                    offset |= trie[offlen];
                }
                trie += offset;
            } else if (*trie & RIGHTPATH) {
                node = (*trie & TRIENODE);
                trie++;
            } else {
                return NULL;
            }
        } else {
            /* left leg */
            if (offlen) {
                node = (*trie & LEFTNODE);
                trie += offlen + 1;
            } else if (*trie & RIGHTPATH) {
                return NULL;
            } else {
                node = (*trie & TRIENODE);
                trie++;
            }
        }
    }

    /* Hangul syllables (U+AC00..U+D7A3) are 3 bytes; s is at the last */
    if (LEAF_CCC(trie) == DECOMPOSE && (unsigned char)LEAF_STR(trie)[0] == HANGUL) {
        trie = utf8hangul(s - 2, hangul);
    }
    return trie;
}

/* the same, for a NUL-terminated decomposition */
static utf8leaf_t *utf8lookup(const struct unicode_map *um, enum utf8_normalization n,
                              unsigned char *hangul, const char *s)
{
    return utf8nlookup(um, n, hangul, s, (size_t)-1);
}

int utf8ncursor(struct utf8cursor *u8c, const struct unicode_map *um,
                enum utf8_normalization n, const char *s, size_t len)
{
    if (!s) {
        return -1;
    }
    u8c->um = um;
    u8c->n = n;
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
            leaf = utf8lookup(u8c->um, u8c->n, u8c->hangul, u8c->s);
        } else {
            leaf = utf8nlookup(u8c->um, u8c->n, u8c->hangul, u8c->s, u8c->len);
        }

        /* not UTF-8: a binary name */
        if (!leaf) {
            return -1;
        }

        ccc = LEAF_CCC(leaf);
        /* code points newer than the version asked for have class 0 */
        if (u8c->um->tables->utf8agetab[LEAF_GEN(leaf)] > u8c->um->ntab[u8c->n]->maxage) {
            ccc = STOPPER;
        } else if (ccc == DECOMPOSE) {
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

            leaf = utf8lookup(u8c->um, u8c->n, u8c->hangul, u8c->s);
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
