/**
 * utf8n.h - the NFDICF form of UTF-8 names, internal to src/unicode.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 */

#ifndef _EXT4_UTF8N_H_
#define _EXT4_UTF8N_H_

#include "ext4fs.h"

/* a leaf whose class byte is DECOMPOSE is followed by the NUL-terminated
   form; a Hangul syllable's form is the single byte UTF8_HANGUL */
#define UTF8_DECOMPOSE  255
#define UTF8_HANGUL     0xFF

/* the leaf of a scalar value (utf8data.c): its canonical combining class,
   or UTF8_DECOMPOSE followed by the form it folds to */
const unsigned char *utf8leaf(unsigned int cp);

/* where the folding reads: a source byte, or a byte of an expansion */
struct utf8pos {
    const unsigned char    *src;            /* the next source byte */
    size_t                  left;           /* source bytes from src on */
    const unsigned char    *exp;            /* the next byte of an expansion, or NULL */
};

/* the folding of one string, one byte at a time */
struct utf8cursor {
    struct utf8pos          pos;
    struct utf8pos          restart;        /* the first mark of the run being sorted */
    const unsigned char    *out;            /* bytes of the character being emitted */
    size_t                  pending;
    BOOLEAN                 sorting;        /* inside a run of combining marks */
    int                     emitting;       /* the class this pass emits; -1 in the first */
    int                     next;           /* the lowest class above it seen in this pass */
    unsigned char           jamo[10];       /* a Hangul syllable as three jamo, NUL-terminated */
};

/* 0, or -1 when s cannot be folded at all */
int utf8ncursor(struct utf8cursor *u8c, const char *s, size_t len);

/* the next byte of the folded string: 1..255, 0 at the end, -1 for input
   that is not UTF-8 */
int utf8byte(struct utf8cursor *u8c);

/* every character before a NUL or the end of len bytes is strict UTF-8 of
   a scalar value: what Linux's utf8nlen() accepts */
BOOLEAN utf8nvalid(const char *s, size_t len);

#endif /* _EXT4_UTF8N_H_ */
