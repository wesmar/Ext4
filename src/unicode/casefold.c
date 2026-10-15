/**
 * casefold.c - names in casefolded directories, folded as Linux folds them.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * A directory with EXT4_CASEFOLD_FL (mkfs -O casefold, chattr +F; SteamOS
 * formats its microSD cards so) treats names that differ only in case as
 * one name. Linux hashes such a directory's index with the folded form of
 * each name (utf8norm.c) and compares names in that form; this driver must
 * do both identically.
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
    int                 Written;

    if (Length < 0 || utf8ncursor(&Cursor, Name, (size_t)Length) < 0) {
        return -1;
    }
    for (Written = 0; Written < OutLength; Written++) {
        int Byte = utf8byte(&Cursor);

        if (Byte < 0) {
            return -1;
        }
        Out[Written] = (unsigned char)Byte;
        if (Byte == 0) {
            return Written;
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
    for (;;) {
        a = utf8byte(&Ca);
        b = utf8byte(&Cb);
        if (a < 0 || b < 0) {
            return -1;
        }
        if (a != b) {
            return 1;
        }
        if (a == 0) {
            return 0;
        }
    }
}

/* Linux's utf8_validate(): every character position, stepping by the
   length its first byte announces, starts a scalar value */
BOOLEAN
Ext4Utf8Valid(IN const char *Name, IN int Length)
{
    return Length >= 0 && utf8nvalid(Name, (size_t)Length);
}
