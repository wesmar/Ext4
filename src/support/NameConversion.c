/**
 * NameConversion.c - conversion between file names on disk and UNICODE_STRING.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"

ULONG
Ext2MbsToUnicode(
    struct nls_table *     PageTable,
    IN OUT PUNICODE_STRING Unicode,
    IN     PANSI_STRING    Mbs   )
{
    ULONG Length = 0, Room = Unicode ? Unicode->MaximumLength : 0;
    int i, mbc;
    WCHAR  uc;

    /* One pass: every character is converted once, stored while the buffer
       has room for it and counted always. An invalid character fails the
       whole name and leaves Unicode->Length alone; a converter's 0 ends the
       name after its first character. */
    for (i = 0; i < Mbs->Length; i += mbc) {

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
    UCHAR mbs[0x10];
    int i, mbc;

    /* One pass, as Ext2MbsToUnicode: converted once, stored while there is
       room, counted always; an invalid character fails the whole name. */
    for (i = 0; i < (Unicode->Length / 2); i++) {

        mbc = PageTable->uni2char(
                  Unicode->Buffer[i],
                  mbs,
                  0x10
              );

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
