/**
 * casefold.c - names in casefolded directories, folded as Linux folds them.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Linux fs/unicode/utf8-core.c.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * A directory with EXT4_CASEFOLD_FL (mkfs -O casefold, chattr +F; SteamOS
 * formats its microSD cards so) treats names that differ only in case as
 * one name. Linux hashes such a directory's index with the folded form of
 * each name (NFD, then full case folding, Unicode 12.1) and compares names
 * in that form; this driver must do both identically.
 */

#include "utf8n.h"
#include "ext4fs_unicode.h"

/* UNICODE_AGE(12, 1, 0): the version of EXT4_ENC_UTF8_12_1 */
#define EXT4_UNICODE_VERSION    ((12 << 16) | (1 << 8) | 0)

/* the entry of a table for Version: the newest one not newer */
static const struct utf8data *
Ext4Utf8Table(const struct utf8data *Table, int Entries, unsigned int Version)
{
    int i = Entries - 1;

    while (i > 0 && Version < Table[i].maxage) {
        i--;
    }
    return Version == Table[i].maxage ? &Table[i] : NULL;
}

/* the tables of Unicode 12.1, looked up once; a race only repeats it */
static const struct unicode_map *
Ext4Utf8Map(VOID)
{
    static struct unicode_map   Map;
    static volatile LONG        Ready;

    if (!Ready) {
        Map.version = EXT4_UNICODE_VERSION;
        Map.tables = &utf8_data_table;
        Map.ntab[UTF8_NFDI] = Ext4Utf8Table(utf8_data_table.utf8nfdidata,
                                            utf8_data_table.utf8nfdidata_size,
                                            EXT4_UNICODE_VERSION);
        Map.ntab[UTF8_NFDICF] = Ext4Utf8Table(utf8_data_table.utf8nfdicfdata,
                                              utf8_data_table.utf8nfdicfdata_size,
                                              EXT4_UNICODE_VERSION);
        if (Map.ntab[UTF8_NFDI] == NULL || Map.ntab[UTF8_NFDICF] == NULL) {
            return NULL;
        }
        InterlockedExchange(&Ready, TRUE);
    }
    return &Map;
}

BOOLEAN
Ext4Utf8Supported(IN USHORT Encoding)
{
    return Encoding == EXT4_ENC_UTF8_12_1 && Ext4Utf8Map() != NULL;
}

int
Ext4Utf8Casefold(IN const char *Name, IN int Length, OUT unsigned char *Out, IN int OutLength)
{
    const struct unicode_map   *Map = Ext4Utf8Map();
    struct utf8cursor           Cursor;
    int                         n;

    if (Map == NULL || Length < 0 ||
        utf8ncursor(&Cursor, Map, UTF8_NFDICF, Name, (size_t)Length) < 0) {
        return -1;
    }
    for (n = 0; n < OutLength; n++) {
        int c = utf8byte(&Cursor);

        if (c < 0) {
            return -1;
        }
        Out[n] = (unsigned char)c;
        if (c == 0) {
            return n;
        }
    }
    return EXT4_UTF8_TOO_LONG;
}

int
Ext4Utf8CasefoldCompare(IN const char *A, IN int ALength, IN const char *B, IN int BLength)
{
    const struct unicode_map   *Map = Ext4Utf8Map();
    struct utf8cursor           Ca, Cb;
    int                         a, b;

    if (Map == NULL || ALength < 0 || BLength < 0 ||
        utf8ncursor(&Ca, Map, UTF8_NFDICF, A, (size_t)ALength) < 0 ||
        utf8ncursor(&Cb, Map, UTF8_NFDICF, B, (size_t)BLength) < 0) {
        return -1;
    }
    do {
        a = utf8byte(&Ca);
        b = utf8byte(&Cb);
        if (a < 0 || b < 0) {
            return -1;
        }
        if (a != b) {
            return 1;
        }
    } while (a);

    return 0;
}

BOOLEAN
Ext4Utf8Valid(IN const char *Name, IN int Length)
{
    const struct unicode_map   *Map = Ext4Utf8Map();
    struct utf8cursor           Cursor;
    int                         c;

    if (Map == NULL || Length < 0 ||
        utf8ncursor(&Cursor, Map, UTF8_NFDI, Name, (size_t)Length) < 0) {
        return FALSE;
    }
    do {
        c = utf8byte(&Cursor);
    } while (c > 0);

    return c == 0;
}
