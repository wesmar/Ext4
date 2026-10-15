/* SPDX-License-Identifier: GPL-2.0-only
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

static unsigned char Hangul[UTF8HANGULLEAF];

/* the leaf the tables give for (s, len): its class and string, or -1 */
int after_lookup(const char *s, size_t len, const char **str)
{
    utf8leaf_t *leaf = utf8nlookup(Hangul, s, len);

    if (!leaf) {
        return -1;
    }
    *str = LEAF_CCC(leaf) == DECOMPOSE ? LEAF_STR(leaf) : "";
    return LEAF_CCC(leaf);
}
