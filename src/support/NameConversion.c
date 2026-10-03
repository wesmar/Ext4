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
    ULONG Length = 0;
    int i, mbc = 0;
    WCHAR  uc;

    /* Count the length of the resulting Unicode. */
    for (i = 0; i < Mbs->Length; i += mbc) {

        mbc = PageTable->char2uni(
                  (PUCHAR)&(Mbs->Buffer[i]),
                  Mbs->Length - i,
                  &uc
              );

        if (mbc <= 0) {

            /* invalid character. */
            if (mbc == 0 && Length > 0) {
                break;
            }
            return 0;
        }

        Length += 2;
    }

    if (Unicode) {
        if (Unicode->MaximumLength < Length) {

            return 0;
        }

        Unicode->Length = 0;
        mbc = 0;

        for (i = 0; i < Mbs->Length; i += mbc) {

            mbc = PageTable->char2uni(
                      (PUCHAR)&(Mbs->Buffer[i]),
                      Mbs->Length - i,
                      &uc
                  );
            Unicode->Buffer[Unicode->Length/2] = uc;
            Unicode->Length += 2;
        }
    }

    return Length;
}

ULONG
Ext2UnicodeToMbs (
    struct nls_table *  PageTable,
    IN OUT PANSI_STRING Mbs,
    IN PUNICODE_STRING  Unicode)
{
    ULONG Length = 0;
    UCHAR mbs[0x10];
    int i, mbc;

    /* Count the length of the resulting mbc-8. */
    for (i = 0; i < (Unicode->Length / 2); i++) {

        RtlZeroMemory(mbs, 0x10);
        mbc = PageTable->uni2char(
                  Unicode->Buffer[i],
                  mbs,
                  0x10
              );

        if (mbc <= 0) {

            /* Invalid character. */
            return 0;
        }

        Length += mbc;
    }

    if (Mbs) {

        if (Mbs->MaximumLength < Length) {

            return 0;
        }

        Mbs->Length = 0;

        for (i = 0; i < (Unicode->Length / 2); i++) {

            mbc = PageTable->uni2char(
                      Unicode->Buffer[i],
                      mbs,
                      0x10
                  );

            RtlCopyMemory(
                (PUCHAR)&(Mbs->Buffer[Mbs->Length]),
                &mbs[0],
                mbc
            );

            Mbs->Length += (USHORT)mbc;
        }
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
