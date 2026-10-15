/**
 * utf8n.h - UTF-8 normalization and case folding, internal to src/unicode.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Linux fs/unicode/utf8n.h (Copyright (c) 2014 SGI).
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _EXT4_UTF8N_H_
#define _EXT4_UTF8N_H_

#include "ext4fs.h"

/* the leaf of a scalar value (utf8data.c): its canonical combining class,
   or DECOMPOSE followed by the NUL-terminated form it folds to */
const unsigned char *utf8leaf(unsigned int cp);

/* size of the synthesized leaf of a Hangul syllable: DECOMPOSE, three
   jamo of three bytes, NUL */
#define UTF8HANGULLEAF  (11)

/* the normalizer's position in a string */
struct utf8cursor {
    const char                 *s;
    const char                 *p;
    const char                 *ss;
    const char                 *sp;
    unsigned int                len;
    unsigned int                slen;
    short int                   ccc;
    short int                   nccc;
    unsigned char               hangul[UTF8HANGULLEAF];
};

int utf8ncursor(struct utf8cursor *u8c, const char *s, size_t len);

/* the next byte of the folded string: 1..255, 0 at the end, -1 for input
   that is not valid UTF-8 */
int utf8byte(struct utf8cursor *u8c);

/* every character before a NUL or the end of len bytes is strict UTF-8 of
   a scalar value: what Linux's utf8nlen() accepts */
int utf8nvalid(const char *s, size_t len);

#endif /* _EXT4_UTF8N_H_ */
