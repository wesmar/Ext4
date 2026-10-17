/**
 * NameConversion.c - conversion between file names on disk and UNICODE_STRING.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"

/*
 * UTF-8 as Linux and every other system stores it: a character beyond the
 * BMP - a surrogate pair in UTF-16 - takes four bytes. The code page tables
 * work one UTF-16 unit at a time; through them each half of a pair became
 * three bytes of its own (CESU-8, which no one else reads as the character),
 * and four bytes read back as a cut-down BMP character. Names written that
 * way before still read as the pair they were. A lone surrogate keeps the
 * three bytes: it has no other form.
 */
static BOOLEAN Ext2IsUtf8(struct nls_table *PageTable)
{
    return strcmp(PageTable->charset, "utf8") == 0;
}

/* the character of four UTF-8 bytes at s (n left), or 0 */
static ULONG Ext2Utf8Four(const UCHAR *s, ULONG n)
{
    ULONG c;

    if (n < 4 || (s[0] & 0xF8) != 0xF0 || (s[1] & 0xC0) != 0x80 ||
        (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) {
        return 0;
    }
    c = ((ULONG)(s[0] & 0x07) << 18) | ((ULONG)(s[1] & 0x3F) << 12) |
        ((ULONG)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    return c >= 0x10000 && c <= 0x10FFFF ? c : 0;
}

ULONG
Ext2MbsToUnicode(
    struct nls_table *     PageTable,
    IN OUT PUNICODE_STRING Unicode,
    IN     PANSI_STRING    Mbs   )
{
    ULONG Length = 0, Room = Unicode ? Unicode->MaximumLength : 0;
    BOOLEAN utf8 = Ext2IsUtf8(PageTable);
    int i, mbc;
    WCHAR  uc;

    /* One pass: every character is converted once, stored while the buffer
       has room for it and counted always. An invalid character fails the
       whole name and leaves Unicode->Length alone; a converter's 0 ends the
       name after its first character. */
    for (i = 0; i < Mbs->Length; i += mbc) {

        if (utf8 && ((UCHAR)Mbs->Buffer[i] & 0xF8) == 0xF0) {
            /* beyond the BMP: a surrogate pair */
            ULONG c = Ext2Utf8Four((PUCHAR)&Mbs->Buffer[i], Mbs->Length - i);

            if (c == 0) {
                return 0;
            }
            c -= 0x10000;
            if (Length + 2 * sizeof(WCHAR) <= Room) {
                Unicode->Buffer[Length / sizeof(WCHAR)] = (WCHAR)(0xD800 + (c >> 10));
                Unicode->Buffer[Length / sizeof(WCHAR) + 1] = (WCHAR)(0xDC00 + (c & 0x3FF));
            }
            Length += 2 * sizeof(WCHAR);
            mbc = 4;
            continue;
        }

        mbc = PageTable->char2uni(
                  (PUCHAR)&(Mbs->Buffer[i]),
                  Mbs->Length - i,
                  &uc
              );

        if (mbc <= 0) {
            if (mbc == 0 && Length > 0) {
                break;
            }
            return 0;
        }

        if (Length + sizeof(WCHAR) <= Room) {
            Unicode->Buffer[Length / sizeof(WCHAR)] = uc;
        }
        Length += sizeof(WCHAR);
    }

    if (Unicode) {
        if (Room < Length) {
            return 0;
        }
        Unicode->Length = (USHORT)Length;
    }

    return Length;
}

ULONG
Ext2UnicodeToMbs (
    struct nls_table *  PageTable,
    IN OUT PANSI_STRING Mbs,
    IN PUNICODE_STRING  Unicode)
{
    ULONG Length = 0, Room = Mbs ? Mbs->MaximumLength : 0;
    BOOLEAN utf8 = Ext2IsUtf8(PageTable);
    UCHAR mbs[0x10];
    int i, mbc, n = Unicode->Length / 2;

    /* One pass, as Ext2MbsToUnicode: converted once, stored while there is
       room, counted always; an invalid character fails the whole name. */
    for (i = 0; i < n; i++) {
        WCHAR c = Unicode->Buffer[i];

        if (utf8 && c >= 0xD800 && c <= 0xDBFF && i + 1 < n &&
            Unicode->Buffer[i + 1] >= 0xDC00 && Unicode->Buffer[i + 1] <= 0xDFFF) {
            /* a surrogate pair: one character, four bytes */
            ULONG u = 0x10000 + (((ULONG)c - 0xD800) << 10) + (Unicode->Buffer[i + 1] - 0xDC00);

            mbs[0] = (UCHAR)(0xF0 | (u >> 18));
            mbs[1] = (UCHAR)(0x80 | ((u >> 12) & 0x3F));
            mbs[2] = (UCHAR)(0x80 | ((u >> 6) & 0x3F));
            mbs[3] = (UCHAR)(0x80 | (u & 0x3F));
            mbc = 4;
            i++;
        } else {
            mbc = PageTable->uni2char(c, mbs, 0x10);
        }

        if (mbc <= 0) {
            return 0;
        }

        if (Length + mbc <= Room) {
            RtlCopyMemory((PUCHAR)&Mbs->Buffer[Length], mbs, mbc);
        }
        Length += mbc;
    }

    if (Mbs) {
        if (Room < Length) {
            return 0;
        }
        Mbs->Length = (USHORT)Length;
    }

    return Length;
}

ULONG
Ext2OEMToUnicodeSize(
    IN PEXT2_VCB        Vcb,
    IN PANSI_STRING     Oem
)
{
    ULONG   Length = 0;

    if (Vcb->Codepage.PageTable) {
        Length = Ext2MbsToUnicode(Vcb->Codepage.PageTable, NULL, Oem);
        if (Length > 0) {
            goto errorout;
        }
    }

    Length = RtlOemStringToCountedUnicodeSize(Oem);

errorout:

    return Length;
}

NTSTATUS
Ext2OEMToUnicode(
    IN PEXT2_VCB           Vcb,
    IN OUT PUNICODE_STRING Unicode,
    IN     POEM_STRING     Oem
)
{
    NTSTATUS  Status;

    if (Vcb->Codepage.PageTable) {
        Status = Ext2MbsToUnicode(Vcb->Codepage.PageTable,
                                  Unicode, Oem);

        if (Status >0 && Status == Unicode->Length) {
            Status = STATUS_SUCCESS;
            goto errorout;
        }
    }

    Status = RtlOemStringToUnicodeString(
                 Unicode, Oem, FALSE );

    if (!NT_SUCCESS(Status)) {
        goto errorout;
    }

errorout:

    return Status;
}

ULONG
Ext2UnicodeToOEMSize(
    IN PEXT2_VCB       Vcb,
    IN PUNICODE_STRING Unicode
)
{
    ULONG   Length = 0;

    if (Vcb->Codepage.PageTable) {
        Length = Ext2UnicodeToMbs(Vcb->Codepage.PageTable,
                                  NULL, Unicode);
        if (Length > 0) {
            return Length;
        }

    }

    return RtlxUnicodeStringToOemSize(Unicode);
}

NTSTATUS
Ext2UnicodeToOEM (
    IN PEXT2_VCB        Vcb,
    IN OUT POEM_STRING  Oem,
    IN PUNICODE_STRING  Unicode)
{
    NTSTATUS Status;

    if (Vcb->Codepage.PageTable) {

        Status = Ext2UnicodeToMbs(Vcb->Codepage.PageTable,
                                  Oem, Unicode);
        if (Status > 0 && Status == Oem->Length) {
            Status = STATUS_SUCCESS;
        } else {
            Status = STATUS_UNSUCCESSFUL;
        }

        goto errorout;
    }

    Status = RtlUnicodeStringToOemString(
                 Oem, Unicode, FALSE );

    if (!NT_SUCCESS(Status))
    {
        goto errorout;
    }

errorout:

    return Status;
}

BOOLEAN
Ext2NameFits(
    IN PEXT2_VCB        Vcb,
    IN PUNICODE_STRING  Unicode)
{
    ULONG Length = Unicode->Length ? Ext2UnicodeToOEMSize(Vcb, Unicode) : 0;

    return Length > 0 && Length <= EXT2_NAME_LEN;
}

BOOLEAN
Ext2UnicodeToOldUtf8(
    IN PEXT2_VCB        Vcb,
    IN OUT POEM_STRING  Oem,
    IN PUNICODE_STRING  Unicode)
{
    struct nls_table *PageTable = Vcb->Codepage.PageTable;
    ULONG Length = 0, i, n = Unicode->Length / sizeof(WCHAR);
    BOOLEAN pair = FALSE;
    UCHAR mbs[0x10];
    int mbc;

    if (!PageTable || !Ext2IsUtf8(PageTable)) {
        return FALSE;
    }
    for (i = 0; i + 1 < n; i++) {
        if (Unicode->Buffer[i] >= 0xD800 && Unicode->Buffer[i] <= 0xDBFF &&
            Unicode->Buffer[i + 1] >= 0xDC00 && Unicode->Buffer[i + 1] <= 0xDFFF) {
            pair = TRUE;
        }
    }
    if (!pair) {
        return FALSE;
    }
    for (i = 0; i < n; i++) {
        mbc = PageTable->uni2char(Unicode->Buffer[i], mbs, sizeof(mbs));
        if (mbc <= 0 || Length + mbc > Oem->MaximumLength) {
            return FALSE;
        }
        RtlCopyMemory((PUCHAR)Oem->Buffer + Length, mbs, mbc);
        Length += mbc;
    }
    Oem->Length = (USHORT)Length;
    return TRUE;
}
