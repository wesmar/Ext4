/**
 * utf8norm.c - the NFDICF form of UTF-8 names, byte for byte as Linux makes it.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * A casefolded ext4 directory hashes and compares names in the form the
 * Linux kernel emits for them (fs/unicode, Unicode 12.1): every character
 * replaced by its case-folded canonical decomposition, combining marks
 * sorted by class. The emitted bytes, not only the normal form, have to
 * match, because Linux hashes them whatever they are. This is an
 * implementation of that behaviour, written from its definition:
 *
 * The string is read as tokens. A token is a character of class k (a
 * character the tables do not expand, or one character of an expansion),
 * an ignorable (a character that expands to nothing), a stray byte (a
 * continuation byte where a character should start), invalid input, or
 * the end (len bytes read, or a NUL).
 *
 *   outside a run       a class-0 character is emitted, an ignorable is
 *                       skipped, a stray byte is emitted, the end ends the
 *                       string; a character of class k > 0 starts a run
 *   inside a run        passes go over the run: the first pass emits no
 *                       character, each later pass the characters of one
 *                       class, lowest first, in the order they stand;
 *                       every pass emits each stray byte it crosses
 *   end of a pass       at a class-0 character, an ignorable or the end;
 *                       after the last class the stopper is read again
 *                       outside the run
 *   invalid input       ends the folding with an error when it is read
 *
 * Hangul syllables expand to their jamo by the arithmetic of the Unicode
 * standard (3.12); the jamo are class 0.
 */

#include "utf8n.h"

#define NOCLASS         256             /* above every canonical combining class */

/* Hangul (Unicode 3.12): first syllable, syllable count, jamo bases, counts */
#define SBASE   0xAC00u
#define SCOUNT  11172u
#define LBASE   0x1100u
#define VBASE   0x1161u
#define TBASE   0x11A7u
#define VCOUNT  21u
#define TCOUNT  28u

enum utf8token {
    TOKEN_CHAR,
    TOKEN_IGNORABLE,
    TOKEN_STRAY,
    TOKEN_INVALID,
    TOKEN_END,
};

/* the scalar value of the strict UTF-8 sequence at u, reading at most left
   bytes: its length, or 0 when it is not one (overlong, surrogate, past
   U+10FFFF, cut short, or not a lead byte) */
static size_t utf8decode(const unsigned char *u, size_t left, unsigned int *cp)
{
    size_t          n;
    size_t          i;
    unsigned int    v;

    if (left == 0) {
        return 0;
    }
    if (u[0] < 0x80) {
        *cp = u[0];
        return 1;
    }
    if (u[0] < 0xC2 || u[0] > 0xF4) {
        return 0;
    }
    n = u[0] < 0xE0 ? 2 : u[0] < 0xF0 ? 3 : 4;
    if (left < n) {
        return 0;
    }
    v = u[0] & (0x7Fu >> n);
    for (i = 1; i < n; i++) {
        if ((u[i] & 0xC0) != 0x80) {
            return 0;
        }
        v = (v << 6) | (u[i] & 0x3Fu);
    }
    if ((n == 3 && v < 0x800) || (n == 4 && (v < 0x10000 || v > 0x10FFFF)) ||
        (v >= 0xD800 && v <= 0xDFFF)) {
        return 0;
    }
    *cp = v;
    return n;
}

static unsigned char *utf8put3(unsigned char *d, unsigned int cp)
{
    d[0] = (unsigned char)(0xE0 | (cp >> 12));
    d[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
    d[2] = (unsigned char)(0x80 | (cp & 0x3F));
    return d + 3;
}

/* the jamo of Hangul syllable cp, NUL-terminated */
static void utf8jamo(unsigned int cp, unsigned char *jamo)
{
    unsigned int    s = cp - SBASE;
    unsigned char  *d = jamo;

    d = utf8put3(d, LBASE + s / (VCOUNT * TCOUNT));
    d = utf8put3(d, VBASE + (s % (VCOUNT * TCOUNT)) / TCOUNT);
    if (s % TCOUNT) {
        d = utf8put3(d, TBASE + s % TCOUNT);
    }
    *d = 0;
}

/*
 * The token at pos, which moves past it. A character's class and bytes
 * go to *klass, *bytes and *size; a stray byte's to *bytes. An expansion
 * is entered here and left as soon as its last character is read, so a
 * saved position never points at the end of one.
 */
static enum utf8token utf8token(struct utf8pos *pos, unsigned char *jamo,
                                int *klass, const unsigned char **bytes, size_t *size)
{
    const unsigned char *leaf;
    unsigned int         cp;
    size_t               n;

    if (pos->exp) {
        /* an expansion holds strict UTF-8 of characters that expand no further */
        n = utf8decode(pos->exp, 4, &cp);
        *klass = utf8leaf(cp)[0];
        *bytes = pos->exp;
        *size = n;
        pos->exp += n;
        if (*pos->exp == 0) {
            pos->exp = NULL;
        }
        return TOKEN_CHAR;
    }

    if (pos->left == 0 || *pos->src == 0) {
        return TOKEN_END;
    }
    if ((*pos->src & 0xC0) == 0x80) {
        *bytes = pos->src;
        *size = 1;
        pos->src++;
        pos->left--;
        return TOKEN_STRAY;
    }
    n = utf8decode(pos->src, pos->left, &cp);
    if (n == 0) {
        return TOKEN_INVALID;
    }

    leaf = utf8leaf(cp);
    if (leaf[0] != UTF8_DECOMPOSE) {
        *klass = leaf[0];
        *bytes = pos->src;
        *size = n;
        pos->src += n;
        pos->left -= n;
        return TOKEN_CHAR;
    }
    pos->src += n;
    pos->left -= n;
    if (leaf[1] == 0) {
        return TOKEN_IGNORABLE;
    }
    if (leaf[1] == UTF8_HANGUL) {
        utf8jamo(cp, jamo);
        pos->exp = jamo;
    } else {
        pos->exp = leaf + 1;
    }
    return utf8token(pos, jamo, klass, bytes, size);
}

int utf8ncursor(struct utf8cursor *u8c, const char *s, size_t len)
{
    /* Linux keeps the length in 32 bits and refuses a string that starts
       inside a character */
    if (s == NULL || len > 0xFFFFFFFFu || (len > 0 && (*s & 0xC0) == 0x80)) {
        return -1;
    }
    RtlZeroMemory(u8c, sizeof(*u8c));
    u8c->pos.src = (const unsigned char *)s;
    u8c->pos.left = len;
    return 0;
}

int utf8byte(struct utf8cursor *u8c)
{
    for (;;) {
        struct utf8pos          here;
        const unsigned char    *bytes = NULL;
        size_t                  size = 0;
        int                     klass = 0;
        enum utf8token          token;

        if (u8c->pending) {
            u8c->pending--;
            return *u8c->out++;
        }

        here = u8c->pos;
        token = utf8token(&u8c->pos, u8c->jamo, &klass, &bytes, &size);

        switch (token) {
        case TOKEN_INVALID:
            return -1;

        case TOKEN_STRAY:
            return *bytes;

        case TOKEN_CHAR:
            if (!u8c->sorting && klass == 0) {
                u8c->out = bytes;
                u8c->pending = size;
                continue;
            }
            if (!u8c->sorting) {
                /* a run of marks begins: the first pass finds its lowest class */
                u8c->sorting = TRUE;
                u8c->emitting = -1;
                u8c->next = klass;
                u8c->restart = here;
                continue;
            }
            if (klass > 0) {
                if (u8c->emitting < klass && klass < u8c->next) {
                    u8c->next = klass;
                }
                if (klass == u8c->emitting) {
                    u8c->out = bytes;
                    u8c->pending = size;
                }
                continue;
            }
            break;                      /* a starter ends the pass */

        case TOKEN_IGNORABLE:
            if (!u8c->sorting) {
                continue;
            }
            break;                      /* inside a run it ends the pass */

        case TOKEN_END:
            if (!u8c->sorting) {
                return 0;
            }
            break;
        }

        /* the end of a pass */
        if (u8c->next != NOCLASS) {
            u8c->emitting = u8c->next;
            u8c->next = NOCLASS;
            u8c->pos = u8c->restart;
        } else {
            u8c->sorting = FALSE;
            u8c->pos = here;
        }
    }
}

BOOLEAN utf8nvalid(const char *s, size_t len)
{
    const unsigned char *u = (const unsigned char *)s;
    unsigned int         cp;

    while (len && *u) {
        size_t n = utf8decode(u, len, &cp);

        if (n == 0) {
            return FALSE;
        }
        u += n;
        len -= n;
    }
    return TRUE;
}
