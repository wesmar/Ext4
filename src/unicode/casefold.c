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

BOOLEAN
Ext4Utf8Supported(IN USHORT Encoding)
{
    return Encoding == EXT4_ENC_UTF8_12_1;
}

int
Ext4Utf8Casefold(IN const char *Name, IN int Length, OUT unsigned char *Out, IN int OutLength)
{
    struct utf8cursor   Cursor;
    int                 n;

    if (Length < 0 || utf8ncursor(&Cursor, Name, (size_t)Length) < 0) {
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
    struct utf8cursor   Ca, Cb;
    int                 a, b;

    if (ALength < 0 || BLength < 0 ||
        utf8ncursor(&Ca, A, (size_t)ALength) < 0 ||
        utf8ncursor(&Cb, B, (size_t)BLength) < 0) {
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

/* utf8_validate() of Linux: every character position, stepping by the
   length its first byte announces, starts a known code point */
BOOLEAN
Ext4Utf8Valid(IN const char *Name, IN int Length)
{
    return Length >= 0 && utf8nvalid(Name, (size_t)Length);
}
