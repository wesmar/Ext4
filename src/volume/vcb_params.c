/**
 * vcb_params.c - per-volume settings from the registry (codepage, read-only, mount point).
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "../core/core_internal.h"

NTSTATUS
Ext2QueryVolumeParams(IN PEXT2_VCB Vcb, IN PUNICODE_STRING Params)
{
    NTSTATUS                    Status;
    RTL_QUERY_REGISTRY_TABLE    QueryTable[2];

    UNICODE_STRING UniName = { 0 };
    PUSHORT                     UniBuffer = NULL;

    USHORT                      UUID[50];

    int                         i;
    int                         len = 0;

    /* zero params */
    RtlZeroMemory(Params, sizeof(UNICODE_STRING));

    /* constructing volume UUID name */
    memset(UUID, 0, sizeof(USHORT) * 50);
    for (i=0; i < 16; i++) {
        if (i == 0) {
            swprintf((wchar_t *)&UUID[len], L"{%2.2X",Vcb->SuperBlock->s_uuid[i]);
            len += 3;
        } else if (i == 15) {
            swprintf((wchar_t *)&UUID[len], L"-%2.2X}", Vcb->SuperBlock->s_uuid[i]);
            len +=4;
        } else {
            swprintf((wchar_t *)&UUID[len], L"-%2.2X", Vcb->SuperBlock->s_uuid[i]);
            len += 3;
        }
    }

    /* allocating memory for UniBuffer */
    UniBuffer = Ext2AllocatePool(PagedPool, 1024, EXT2_PARAM_MAGIC);
    if (NULL == UniBuffer) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto errorout;
    }
    RtlZeroMemory(UniBuffer, 1024);

    /* querying volume parameter string */
    RtlZeroMemory(&QueryTable[0], sizeof(RTL_QUERY_REGISTRY_TABLE) * 2);
    QueryTable[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_REQUIRED;
    QueryTable[0].Name = UUID;
    QueryTable[0].EntryContext = &(UniName);
    UniName.MaximumLength = 1024;
    UniName.Length = 0;
    UniName.Buffer = UniBuffer;

    Status = RtlQueryRegistryValues(
                 RTL_REGISTRY_ABSOLUTE,
                 Ext2Global->RegistryPath.Buffer,
                 &QueryTable[0],
                 NULL,
                 NULL
             );

    if (!NT_SUCCESS(Status)) {
        goto errorout;
    }

errorout:

    if (NT_SUCCESS(Status)) {
        *Params = UniName;
    } else {
        if (UniBuffer) {
            Ext2FreePool(UniBuffer, EXT2_PARAM_MAGIC);
        }
    }

    return Status;
}

VOID
Ext2ParseRegistryVolumeParams(
    IN  PUNICODE_STRING         Params,
    OUT PEXT2_VOLUME_PROPERTY3  Property
)
{
    WCHAR       Codepage[CODEPAGE_MAXLEN];
    WCHAR       Prefix[HIDINGPAT_LEN];
    WCHAR       Suffix[HIDINGPAT_LEN];
    USHORT      MountPoint[4];
    CHAR        DrvLetter[4];
    WCHAR       wUID[8], wGID[8], wEUID[8], wEGID[8];
    CHAR        sUID[8], sGID[8], sEUID[8], sEGID[8];

    BOOLEAN     bWriteSupport = FALSE,
                bCheckBitmap = FALSE,
                bCodeName = FALSE,
                bMountPoint = FALSE;
    BOOLEAN     bUID = 0, bGID = 0, bEUID = 0, bEGID = 0;

    struct {
        PWCHAR   Name;      /* parameters name */
        PBOOLEAN bExist;    /* is it contained in params */
        USHORT   Length;    /* parameter value length */
        PWCHAR   uValue;    /* value buffer in unicode */
        PCHAR    aValue;    /* value buffer in ansi */
    } ParamPattern[] = {
        /* writing support */
        {READING_ONLY, &Property->bReadonly, 0, NULL, NULL},
        {WRITING_SUPPORT, &bWriteSupport, 0, NULL, NULL},
        {EXT3_FORCEWRITING, &Property->bExt3Writable, 0, NULL, NULL},

        /* need check bitmap */
        {CHECKING_BITMAP, &bCheckBitmap, 0, NULL, NULL},
        /* codepage */
        {CODEPAGE_NAME, &bCodeName, CODEPAGE_MAXLEN,
         &Codepage[0], Property->Codepage},
        /* filter prefix and suffix */
        {HIDING_PREFIX, &Property->bHidingPrefix, HIDINGPAT_LEN,
         &Prefix[0], Property->sHidingPrefix},
        {HIDING_SUFFIX, &Property->bHidingSuffix, HIDINGPAT_LEN,
         &Suffix[0], Property->sHidingSuffix},
        {MOUNT_POINT, &bMountPoint, 4,
         &MountPoint[0], &DrvLetter[0]},

        {UID,  &bUID,  8, &wUID[0],  &sUID[0],},
        {GID,  &bGID,  8, &wGID[0],  &sGID[0]},
        {EUID, &bEUID, 8, &wEUID[0], &sEUID[0]},
        {EGID, &bEGID, 8, &wEGID[0], &sEGID[0]},

        /* end */
        {NULL, NULL, 0, NULL}
    };

    USHORT i, j, k;

    RtlZeroMemory(Codepage, sizeof(WCHAR) * CODEPAGE_MAXLEN);
    RtlZeroMemory(Prefix, sizeof(WCHAR) * HIDINGPAT_LEN);
    RtlZeroMemory(Suffix, sizeof(WCHAR) * HIDINGPAT_LEN);
    RtlZeroMemory(MountPoint, sizeof(USHORT) * 4);
    RtlZeroMemory(DrvLetter, sizeof(CHAR) * 4);

    RtlZeroMemory(Property, sizeof(EXT2_VOLUME_PROPERTY3));
    Property->Magic = EXT2_VOLUME_PROPERTY_MAGIC;
    Property->Command = APP_CMD_SET_PROPERTY3;

    for (i=0; ParamPattern[i].Name != NULL; i++) {

        UNICODE_STRING  Name1=*Params, Name2;
        RtlInitUnicodeString(&Name2, ParamPattern[i].Name);
        *ParamPattern[i].bExist = FALSE;

        for (j=0; j * sizeof(WCHAR) + Name2.Length <= Params->Length ; j++) {

            Name1.MaximumLength = Params->Length - j * sizeof(WCHAR);
            Name1.Length = Name2.Length;
            Name1.Buffer = &Params->Buffer[j];

            if (!RtlCompareUnicodeString(&Name1, &Name2, TRUE)) {
                if (j * sizeof(WCHAR) + Name2.Length == Params->Length ||
                        Name1.Buffer[Name2.Length/sizeof(WCHAR)] == L';' ||
                        Name1.Buffer[Name2.Length/sizeof(WCHAR)] == L','  ) {
                    *(ParamPattern[i].bExist) = TRUE;
                } else if ((j * 2 + Name2.Length < Params->Length + 2) ||
                           (Name1.Buffer[Name2.Length/sizeof(WCHAR)] == L'=' )) {
                    j += Name2.Length/sizeof(WCHAR) + 1;
                    k = 0;
                    while ( j + k < Params->Length/2 &&
                            k < ParamPattern[i].Length &&
                            Params->Buffer[j+k] != L';' &&
                            Params->Buffer[j+k] != L',' ) {
                        ParamPattern[i].uValue[k] = Params->Buffer[j + k++];
                    }
                    if (k) {
                        NTSTATUS status;
                        ANSI_STRING AnsiName;
                        AnsiName.Length = 0;
                        AnsiName.MaximumLength =ParamPattern[i].Length;
                        AnsiName.Buffer = ParamPattern[i].aValue;

                        Name2.Buffer = ParamPattern[i].uValue;
                        Name2.MaximumLength = Name2.Length = k * sizeof(WCHAR);
                        status = RtlUnicodeStringToAnsiString(
                                     &AnsiName, &Name2, FALSE);
                        if (NT_SUCCESS(status)) {
                            *(ParamPattern[i].bExist) = TRUE;
                        } else {
                            *ParamPattern[i].bExist = FALSE;
                        }
                    }
                }
                break;
            }
        }
    }

    if (bMountPoint) {
        Property->DrvLetter = DrvLetter[0];
        Property->DrvLetter |= 0x80;
    }

    if (bUID && bGID) {
        SetFlag(Property->Flags2, EXT2_VPROP3_USERIDS);
        sUID[7] = sGID[7] = sEUID[7] = sEGID[7] = 0;
        Property->uid = (USHORT)atoi(sUID);
        Property->gid = (USHORT)atoi(sGID);
        if (bEUID) {
            Property->euid = (USHORT)atoi(sEUID);
            Property->egid = (USHORT)atoi(sEGID);
            Property->EIDS = TRUE;
        } else {
            Property->EIDS = FALSE;
        }
    } else {
        ClearFlag(Property->Flags2, EXT2_VPROP3_USERIDS);
    }
}

NTSTATUS
Ext2PerformRegistryVolumeParams(IN PEXT2_VCB Vcb)
{
    NTSTATUS        Status;
    UNICODE_STRING  VolumeParams;

    Status = Ext2QueryVolumeParams(Vcb, &VolumeParams);
    if (NT_SUCCESS(Status)) {

        /* set Vcb settings from registery */
        EXT2_VOLUME_PROPERTY3  Property;
        Ext2ParseRegistryVolumeParams(&VolumeParams, &Property);
        Ext2ProcessVolumeProperty(Vcb, &Property, sizeof(Property));

    } else {

        /* don't support auto mount */
        if (IsFlagOn(Ext2Global->Flags, EXT2_AUTO_MOUNT)) {
            Status = STATUS_SUCCESS;
        } else {
            Status = STATUS_UNSUCCESSFUL;
            goto errorout;
        }

        /* set Vcb settings from Ext2Global */
        if (IsFlagOn(Ext2Global->Flags, EXT2_SUPPORT_WRITING)) {
            if (Vcb->IsExt3fs) {
                if (IsFlagOn(Ext2Global->Flags, EXT3_FORCE_WRITING)) {
                    ClearLongFlag(Vcb->Flags, VCB_READ_ONLY);
                } else {
                    SetLongFlag(Vcb->Flags, VCB_READ_ONLY);
                }
            } else {
                ClearLongFlag(Vcb->Flags, VCB_READ_ONLY);
            }
        } else {
            SetLongFlag(Vcb->Flags, VCB_READ_ONLY);
        }

        /* set the default codepage */
        Vcb->Codepage.PageTable = Ext2Global->Codepage.PageTable;
        memcpy(Vcb->Codepage.AnsiName, Ext2Global->Codepage.AnsiName, CODEPAGE_MAXLEN);
        Vcb->Codepage.PageTable = Ext2Global->Codepage.PageTable;

        if (Vcb->bHidingPrefix == Ext2Global->bHidingPrefix) {
            RtlCopyMemory( Vcb->sHidingPrefix,
                           Ext2Global->sHidingPrefix,
                           HIDINGPAT_LEN);
        } else {
            RtlZeroMemory( Vcb->sHidingPrefix,
                           HIDINGPAT_LEN);
        }

        if (Vcb->bHidingSuffix == Ext2Global->bHidingSuffix) {
            RtlCopyMemory( Vcb->sHidingSuffix,
                           Ext2Global->sHidingSuffix,
                           HIDINGPAT_LEN);
        } else {
            RtlZeroMemory( Vcb->sHidingSuffix,
                           HIDINGPAT_LEN);
        }
    }

errorout:

    if (VolumeParams.Buffer) {
        Ext2FreePool(VolumeParams.Buffer, EXT2_PARAM_MAGIC);
    }

    return Status;
}
