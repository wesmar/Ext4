/* Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 * The reference: src/unicode with the Linux trie (taken from git by
 * unicode-model.ps1), names prefixed, plus what Linux checks in strict mode.
 */
#define utf8ncursor             before_utf8ncursor
#define utf8byte                before_utf8byte
#define utf8_data_table         before_utf8_data_table
#define Ext4Utf8Supported       before_Ext4Utf8Supported
#define Ext4Utf8Casefold        before_Ext4Utf8Casefold
#define Ext4Utf8CasefoldCompare before_Ext4Utf8CasefoldCompare
#define Ext4Utf8Valid           before_Ext4Utf8Valid
#include "utf8norm.c"
#include "utf8data.c"
#include "casefold.c"

static unsigned char Hangul[UTF8HANGULLEAF];

/* Linux utf8_validate(): utf8nlen(um, UTF8_NFDI, s, len) >= 0 */
int before_linux_valid(const char *s, size_t len)
{
    const struct unicode_map *um = Ext4Utf8Map();

    while (len && *s) {
        if (!utf8nlookup(um, UTF8_NFDI, Hangul, s, len)) {
            return 0;
        }
        len -= (size_t)utf8clen(s);
        s += utf8clen(s);
    }
    return 1;
}

/* the NFDICF leaf the trie finds for (s, len): its class and string, or -1 */
int before_lookup(const char *s, size_t len, const char **str)
{
    utf8leaf_t *leaf = utf8nlookup(Ext4Utf8Map(), UTF8_NFDICF, Hangul, s, len);

    if (!leaf) {
        return -1;
    }
    *str = LEAF_CCC(leaf) == DECOMPOSE ? LEAF_STR(leaf) : "";
    return LEAF_CCC(leaf);
}
