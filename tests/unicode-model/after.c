/* Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 * The production src/unicode, names prefixed.
 */
#define utf8ncursor             after_utf8ncursor
#define utf8byte                after_utf8byte
#define utf8nvalid              after_utf8nvalid
#define utf8leaf                after_utf8leaf
#define Ext4Utf8Supported       after_Ext4Utf8Supported
#define Ext4Utf8Casefold        after_Ext4Utf8Casefold
#define Ext4Utf8CasefoldCompare after_Ext4Utf8CasefoldCompare
#define Ext4Utf8Valid           after_Ext4Utf8Valid
#include "utf8norm.c"
#include "utf8data.c"
#include "casefold.c"

static unsigned char Jamo[10];

/* what the decoder and the tables give for (s, len): the class and the
   expansion of the character (Hangul as its jamo), or -1 */
int after_lookup(const char *s, size_t len, const char **str)
{
    const unsigned char *leaf;
    unsigned int         cp;

    if (!utf8decode((const unsigned char *)s, len, &cp)) {
        return -1;
    }
    leaf = utf8leaf(cp);
    *str = "";
    if (leaf[0] == UTF8_DECOMPOSE && leaf[1] == UTF8_HANGUL) {
        utf8jamo(cp, Jamo);
        *str = (const char *)Jamo;
    } else if (leaf[0] == UTF8_DECOMPOSE) {
        *str = (const char *)leaf + 1;
    }
    return leaf[0];
}
