/**
 * registry.c - driver-wide settings read from the service registry key.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Values under Services\ext4\Parameters (every one optional):
 *
 *   WritingSupport    REG_DWORD  0: mount every volume read-only
 *   Ext3ForceWriting  REG_DWORD  0: mount journalled volumes read-only
 *   CheckingBitmap    REG_DWORD  1: verify the block bitmaps at mount
 *   AutoMount         REG_DWORD  0: do not mount volumes automatically
 *   CodePage          REG_SZ     code page of file names on disk (utf8)
 *   HidingPrefix      REG_SZ     names starting with it are reported hidden
 *   HidingSuffix      REG_SZ     names ending with it are reported hidden
 *
 * Per-volume settings live under Services\ext4\Volumes (vcb_params.c).
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"

/* a REG_DWORD switch: flags set when it is non-zero, cleared when zero */
typedef struct _EXT4_REG_SWITCH {
    PCWSTR  Name;
    ULONG   SetFlags;
    ULONG   ClearFlags;
} EXT4_REG_SWITCH;

static const EXT4_REG_SWITCH Ext4RegSwitches[] = {
    { WRITING_SUPPORT,   EXT2_SUPPORT_WRITING,                      EXT2_SUPPORT_WRITING | EXT3_FORCE_WRITING },
    { EXT3_FORCEWRITING, EXT3_FORCE_WRITING | EXT2_SUPPORT_WRITING, EXT3_FORCE_WRITING },
    { CHECKING_BITMAP,   EXT2_CHECKING_BITMAP,                      EXT2_CHECKING_BITMAP },
    { AUTO_MOUNT,        EXT2_AUTO_MOUNT,                           EXT2_AUTO_MOUNT },
};

/* a REG_SZ setting copied into a fixed WCHAR buffer of EXT2_GLOBAL */
typedef struct _EXT4_REG_STRING {
    PCWSTR  Name;
    ULONG   Offset;         /* of the buffer in EXT2_GLOBAL */
    ULONG   Chars;          /* buffer size, terminator included */
} EXT4_REG_STRING;

static const EXT4_REG_STRING Ext4RegStrings[] = {
    { CODEPAGE_NAME, FIELD_OFFSET(EXT2_GLOBAL, Codepage.PageName), CODEPAGE_MAXLEN },
    { HIDING_PREFIX, FIELD_OFFSET(EXT2_GLOBAL, wHidingPrefix),     HIDINGPAT_LEN },
    { HIDING_SUFFIX, FIELD_OFFSET(EXT2_GLOBAL, wHidingSuffix),     HIDINGPAT_LEN },
};

static RTL_QUERY_REGISTRY_ROUTINE Ext4RegistryValue;

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, Ext4RegistryValue)
#pragma alloc_text(INIT, Ext2QueryGlobalParameters)
#pragma alloc_text(INIT, Ext2QueryRegistrySettings)
#endif

/* called by RtlQueryRegistryValues for every value of the Parameters key */
static NTSTATUS
Ext4RegistryValue(
    IN PWSTR ValueName,
    IN ULONG ValueType,
    IN PVOID ValueData,
    IN ULONG ValueLength,
    IN PVOID Context,
    IN PVOID EntryContext
    )
{
    ULONG i;

    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(EntryContext);

    if (ValueName == NULL || ValueData == NULL) {
        return STATUS_SUCCESS;
    }

    if (ValueType == REG_DWORD && ValueLength == sizeof(ULONG)) {
        for (i = 0; i < ARRAYSIZE(Ext4RegSwitches); i++) {
            if (_wcsicmp(ValueName, Ext4RegSwitches[i].Name) == 0) {
                if (*(PULONG)ValueData != 0) {
                    SetLongFlag(Ext2Global->Flags, Ext4RegSwitches[i].SetFlags);
                } else {
                    ClearLongFlag(Ext2Global->Flags, Ext4RegSwitches[i].ClearFlags);
                }
                break;
            }
        }
    } else if (ValueType == REG_SZ) {
        for (i = 0; i < ARRAYSIZE(Ext4RegStrings); i++) {
            if (_wcsicmp(ValueName, Ext4RegStrings[i].Name) == 0) {
                PWCHAR  Target = (PWCHAR)((PUCHAR)Ext2Global + Ext4RegStrings[i].Offset);
                PCWCH   Source = ValueData;
                ULONG   Chars = ValueLength / sizeof(WCHAR);

                /* the registry does not promise a terminator; the buffer
                   always gets one, so a value that fills it is refused */
                while (Chars > 0 && Source[Chars - 1] == UNICODE_NULL) {
                    Chars--;
                }
                if (Chars < Ext4RegStrings[i].Chars) {
                    RtlCopyMemory(Target, Source, Chars * sizeof(WCHAR));
                    Target[Chars] = UNICODE_NULL;
                } else {
                    DEBUG(DL_ERR, ("Ext4RegistryValue: %ws is too long, ignored\n", ValueName));
                }
                break;
            }
        }
    }

    return STATUS_SUCCESS;
}

NTSTATUS
Ext2QueryGlobalParameters(IN PUNICODE_STRING RegistryPath)
{
    /* one entry without a name: the routine sees every value of the key */
    RTL_QUERY_REGISTRY_TABLE QueryTable[2];

    RtlZeroMemory(QueryTable, sizeof(QueryTable));
    QueryTable[0].QueryRoutine = Ext4RegistryValue;

    return RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, RegistryPath->Buffer,
                                  QueryTable, NULL, NULL);
}

/* ANSI copy of a UNICODE setting; FALSE when it is empty or not convertible */
static BOOLEAN
Ext4AnsiSetting(IN PCWSTR Wide, OUT PCHAR Ansi, IN USHORT AnsiChars)
{
    UNICODE_STRING  Uni;
    ANSI_STRING     Str;

    Ansi[0] = 0;
    RtlInitUnicodeString(&Uni, Wide);
    if (Uni.Length == 0) {
        return FALSE;
    }

    Str.Buffer = Ansi;
    Str.Length = 0;
    Str.MaximumLength = AnsiChars - 1;      /* room for the terminator */
    if (!NT_SUCCESS(RtlUnicodeStringToAnsiString(&Str, &Uni, FALSE))) {
        Ansi[0] = 0;
        return FALSE;
    }
    Ansi[Str.Length] = 0;
    return TRUE;
}

BOOLEAN
Ext2QueryRegistrySettings(IN PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING  Path;
    NTSTATUS        Status;

    /* one buffer serves both paths: <service>\Parameters now and
       <service>\Volumes for the per-volume settings afterwards; it becomes
       Ext2Global->RegistryPath and DriverUnload frees it */
    Path.Length = 0;
    Path.MaximumLength = RegistryPath->Length +
                         (USHORT)max(sizeof(PARAMETERS_KEY), sizeof(VOLUMES_KEY));
    Path.Buffer = Ext2AllocatePool(PagedPool, Path.MaximumLength, EXT4_TAG_GLOBAL);
    if (Path.Buffer == NULL) {
        DEBUG(DL_ERR, ("Ext2QueryRegistrySettings: no memory for the registry path\n"));
        return FALSE;
    }

    /* defaults: mount automatically and read-write, journalled volumes
       included; the Parameters key can turn each of them off */
    SetLongFlag(Ext2Global->Flags, EXT2_AUTO_MOUNT | EXT2_SUPPORT_WRITING | EXT3_FORCE_WRITING);

    RtlCopyUnicodeString(&Path, RegistryPath);
    RtlAppendUnicodeToString(&Path, PARAMETERS_KEY);
    Status = Ext2QueryGlobalParameters(&Path);
    if (!NT_SUCCESS(Status) && Status != STATUS_OBJECT_NAME_NOT_FOUND) {
        DEBUG(DL_ERR, ("Ext2QueryRegistrySettings: %wZ: %xh, defaults used\n", &Path, Status));
    }

    /* no "default" table exists (it would fall back to the system OEM code
       page and mangle anything non-ASCII); every Linux file system made
       this century carries UTF-8 names */
    if (!Ext4AnsiSetting(Ext2Global->Codepage.PageName, Ext2Global->Codepage.AnsiName,
                         CODEPAGE_MAXLEN)) {
        RtlCopyMemory(Ext2Global->Codepage.AnsiName, "utf8", sizeof("utf8"));
    }
    Ext2Global->bHidingPrefix = Ext4AnsiSetting(Ext2Global->wHidingPrefix,
                                                Ext2Global->sHidingPrefix, HIDINGPAT_LEN);
    Ext2Global->bHidingSuffix = Ext4AnsiSetting(Ext2Global->wHidingSuffix,
                                                Ext2Global->sHidingSuffix, HIDINGPAT_LEN);

    Path.Length = 0;
    RtlCopyUnicodeString(&Path, RegistryPath);
    RtlAppendUnicodeToString(&Path, VOLUMES_KEY);
    Ext2Global->RegistryPath = Path;

    return TRUE;
}
