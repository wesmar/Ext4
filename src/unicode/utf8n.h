/**
 * utf8n.h - the UTF-8 normalization trie, internal to src/unicode.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Linux fs/unicode/utf8n.h (Copyright (c) 2014 SGI).
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _EXT4_UTF8N_H_
#define _EXT4_UTF8N_H_

#include "ext4fs.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a)   (sizeof(a) / sizeof((a)[0]))
#endif

/* the two normalizations the tables hold */
enum utf8_normalization {
    UTF8_NFDI = 0,          /* canonical decomposition, ignorables dropped */
    UTF8_NFDICF,            /* the same, case folded: what ext4 hashes */
    UTF8_NMAX,
};

/* one Unicode version of a normalization: where its trie starts */
struct utf8data {
    unsigned int maxage;
    unsigned int offset;
};

struct utf8data_table {
    const unsigned int     *utf8agetab;
    int                     utf8agetab_size;

    const struct utf8data  *utf8nfdicfdata;
    int                     utf8nfdicfdata_size;

    const struct utf8data  *utf8nfdidata;
    int                     utf8nfdidata_size;

    const unsigned char    *utf8data;
};

extern const struct utf8data_table utf8_data_table;

/* the tables of one Unicode version */
struct unicode_map {
    unsigned int                    version;
    const struct utf8data          *ntab[UTF8_NMAX];
    const struct utf8data_table    *tables;
};

/* size of the synthesized leaf of a Hangul syllable decomposition */
#define UTF8HANGULLEAF  (12)

/* the normalizer's position in a string */
struct utf8cursor {
    const struct unicode_map   *um;
    enum utf8_normalization     n;
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

int utf8ncursor(struct utf8cursor *u8c, const struct unicode_map *um,
                enum utf8_normalization n, const char *s, size_t len);

/* the next byte of the normalized string: 1..255, 0 at the end, -1 for
   input that is not valid UTF-8 */
int utf8byte(struct utf8cursor *u8c);

#endif /* _EXT4_UTF8N_H_ */
