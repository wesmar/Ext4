/**
 * cmd_crypt.c - the LUKS commands: list, unlock, lock, status.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4ctl.h"

#define EXT4CTL_MAX_PASSPHRASE  (8 * 1024 * 1024)   /* cryptsetup's keyfile limit */
#define EXT4CTL_MAX_VOLUMES     256                 /* HarddiskVolume1 .. N scanned by list */
#define EXT4CTL_WS_MIN_EXTRA    (4 * 1024 * 1024)   /* working set above the passphrase buffer */
#define EXT4CTL_WS_MAX_EXTRA    (16 * 1024 * 1024)

static void
VolumeDevice(ULONG n, WCHAR *Out, size_t OutChars)
{
    swprintf_s(Out, OutChars, L"\\Device\\HarddiskVolume%lu", n);
}

static void
CryptDevice(ULONG Index, WCHAR *Out, size_t OutChars)
{
    swprintf_s(Out, OutChars, EXT4_CRYPT_DEVICE_PREFIX L"%lu", Index);
}

/* ---------------------------------------------------------------- volumes */

/* the volume whose LUKS header has this UUID (volume numbers change when
   disks come and go; the UUID does not - crypttab uses it for that reason) */
static BOOL
VolumeByUuid(const WCHAR *Uuid, WCHAR *Out, size_t OutChars)
{
    char    Want[EXT4_CRYPT_UUID_CHARS];
    ULONG   n;

    if (WideCharToMultiByte(CP_ACP, 0, Uuid, -1, Want, sizeof(Want), NULL, NULL) <= 0) {
        return FALSE;
    }
    for (n = 1; n <= EXT4CTL_MAX_VOLUMES; n++) {
        WCHAR       Name[64];
        LUKS_DEVICE Dev;
        LUKS_VOLUME V;
        BOOL        Match;

        VolumeDevice(n, Name, ARRAYSIZE(Name));
        if (!LuksOpenDevice(&Dev, Name)) {
            continue;
        }
        Match = LuksProbe(&Dev, &V) == 1 && _stricmp(V.Uuid, Want) == 0;
        LuksCloseDevice(&Dev);
        if (Match) {
            return wcscpy_s(Out, OutChars, Name) == 0;
        }
    }
    Fail("no LUKS volume with UUID %ls", Uuid);
    return FALSE;
}

/* "5", "HarddiskVolume5", "\Device\...", "UUID=..." to an NT device name */
static BOOL
VolumeName(const WCHAR *Arg, WCHAR *Out, size_t OutChars)
{
    static const WCHAR UuidPrefix[] = L"UUID=", DevicePrefix[] = L"\\Device\\";
    static const WCHAR Win32Prefix[] = L"\\\\.\\", Win32Prefix2[] = L"\\\\?\\";
    const WCHAR *p;

    if (_wcsnicmp(Arg, UuidPrefix, ARRAYSIZE(UuidPrefix) - 1) == 0) {
        return VolumeByUuid(Arg + ARRAYSIZE(UuidPrefix) - 1, Out, OutChars);
    }
    for (p = Arg; *p >= L'0' && *p <= L'9'; p++) {
    }
    if (*Arg && *p == 0) {
        return swprintf_s(Out, OutChars, L"\\Device\\HarddiskVolume%ls", Arg) > 0;
    }
    if (_wcsnicmp(Arg, DevicePrefix, ARRAYSIZE(DevicePrefix) - 1) == 0) {
        return wcscpy_s(Out, OutChars, Arg) == 0;
    }
    if (_wcsnicmp(Arg, Win32Prefix, ARRAYSIZE(Win32Prefix) - 1) == 0 ||
        _wcsnicmp(Arg, Win32Prefix2, ARRAYSIZE(Win32Prefix2) - 1) == 0) {
        Arg += ARRAYSIZE(Win32Prefix) - 1;
    }
    return swprintf_s(Out, OutChars, L"\\Device\\%ls", Arg) > 0;
}

int
CmdList(void)
{
    ULONG   n, Found = 0;

    Say("%-28s %10s  %s", "volume", "size", "contents");
    for (n = 1; n <= EXT4CTL_MAX_VOLUMES; n++) {
        WCHAR       Name[64];
        LUKS_DEVICE Dev;
        LUKS_VOLUME V;
        UINT8       Super[2];

        VolumeDevice(n, Name, ARRAYSIZE(Name));
        if (!LuksOpenDevice(&Dev, Name)) {
            continue;
        }
        Found++;
        if (LuksProbe(&Dev, &V) == 1) {
            printf("%-28ls %8.1f G  LUKS%d %s, %lu-byte key, %lu-byte sectors, kdf %s, %lu keyslot(s), UUID %s%s%s\n",
                   Name, Dev.Size / EXT4CTL_GIB, V.Version, V.Cipher, V.KeyBytes, V.SectorSize,
                   V.Kdf, V.Keyslots, V.Uuid, V.Label[0] ? ", label " : "", V.Label);
            if (V.Problem[0]) {
                printf("%-28s cannot be opened here: %s\n", "", V.Problem);
            }
        } else if (LuksReadAt(&Dev, EXT2_SUPER_MAGIC_OFFSET, Super, sizeof(Super)) &&
                   (Super[0] | (Super[1] << 8)) == EXT2_SUPER_MAGIC) {
            printf("%-28ls %8.1f G  ext2/3/4\n", Name, Dev.Size / EXT4CTL_GIB);
        }
        LuksCloseDevice(&Dev);
    }
    if (Found == 0) {
        Fail("no volume could be opened (run elevated)");
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- unlock */

typedef struct _UNLOCK_ARGS {
    WCHAR           Name[EXT4_CRYPT_DEVICE_CHARS];
    const WCHAR    *KeyFile;
    BOOL            Stdin;
    BOOL            ReadOnly;
    WCHAR           Letter;
} UNLOCK_ARGS;

/* 0, or the exit code after the usage or a message */
static int
ParseUnlockArgs(int argc, WCHAR **argv, UNLOCK_ARGS *A)
{
    int i;

    memset(A, 0, sizeof(*A));
    if (argc < 1 || !VolumeName(argv[0], A->Name, ARRAYSIZE(A->Name))) {
        Usage();
        return 2;
    }
    for (i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--letter") == 0 && i + 1 < argc) {
            A->Letter = towupper(argv[++i][0]);
        } else if (_wcsicmp(argv[i], L"--read-only") == 0) {
            A->ReadOnly = TRUE;
        } else if (_wcsicmp(argv[i], L"--key-file") == 0 && i + 1 < argc) {
            A->KeyFile = argv[++i];
        } else if (_wcsicmp(argv[i], L"--passphrase-stdin") == 0) {
            A->Stdin = TRUE;
        } else {
            Usage();
            return 2;
        }
    }
    if (A->Letter && (A->Letter < L'A' || A->Letter > L'Z')) {
        Fail("--letter wants A..Z");
        return 2;
    }
    return 0;
}

/* the header of a volume this tool can open; FALSE after a message */
static BOOL
ProbeForUnlock(LUKS_DEVICE *Dev, const WCHAR *Name, LUKS_VOLUME *V)
{
    if (!LuksOpenDevice(Dev, Name)) {
        Fail("cannot open %ls (error %lu)", Name, GetLastError());
        return FALSE;
    }
    if (LuksProbe(Dev, V) != 1) {
        Fail("%ls holds no readable LUKS header", Name);
        LuksCloseDevice(Dev);
        return FALSE;
    }
    if (LuksCipher(V) == 0) {
        Fail("%ls cannot be opened here: %s", Name, V->Problem[0] ? V->Problem : "unsupported cipher");
        LuksCloseDevice(Dev);
        return FALSE;
    }
    return TRUE;
}

/* the volume key from the passphrase; the device is closed either way */
static BOOL
VolumeKey(LUKS_DEVICE *Dev, const LUKS_VOLUME *V, const UNLOCK_ARGS *A, UINT8 Key[LUKS_MAX_KEY])
{
    char       *Pass;
    ULONG       PassLength = 0;
    int         Keyslot = -1;
    ULONGLONG   Start;
    BOOL        Ok;

    Pass = (char *)VirtualAlloc(NULL, EXT4CTL_MAX_PASSPHRASE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (Pass == NULL) {
        LuksCloseDevice(Dev);
        return FALSE;
    }
    /* kept out of the page file, all of it: a key file may fill the buffer
       (best effort: the working set may not allow it) */
    SetProcessWorkingSetSize(GetCurrentProcess(), EXT4CTL_MAX_PASSPHRASE + EXT4CTL_WS_MIN_EXTRA,
                             EXT4CTL_MAX_PASSPHRASE + EXT4CTL_WS_MAX_EXTRA);
    VirtualLock(Pass, EXT4CTL_MAX_PASSPHRASE);
    if (!ReadPassphrase(A->KeyFile, A->Stdin, Pass, EXT4CTL_MAX_PASSPHRASE, &PassLength)) {
        Fail("no passphrase");
        VirtualFree(Pass, 0, MEM_RELEASE);
        LuksCloseDevice(Dev);
        return FALSE;
    }

    Start = GetTickCount64();
    Ok = LuksUnlockKey(Dev, V, Pass, PassLength, Key, &Keyslot);
    SecureZeroMemory(Pass, PassLength);
    VirtualFree(Pass, 0, MEM_RELEASE);
    LuksCloseDevice(Dev);
    if (!Ok) {
        Fail("no keyslot opens with this passphrase");
        return FALSE;
    }
    Say("keyslot %d opened (%s, %llu ms)", Keyslot, V->Kdf, GetTickCount64() - Start);
    return TRUE;
}

/* the key to the driver, which makes \Device\Ext4CryptN; FALSE after a message */
static BOOL
SendUnlock(const UNLOCK_ARGS *A, const LUKS_VOLUME *V, UINT8 Key[LUKS_MAX_KEY], ULONG *Index)
{
    EXT4_CRYPT_UNLOCK   Req;
    DWORD               Error;

    memset(&Req, 0, sizeof(Req));
    Req.Magic = EXT4_CRYPT_MAGIC;
    Req.Version = EXT4_CRYPT_VERSION;
    /* LUKS counts the IV in 512-byte sectors whatever the encryption sector
       size (cryptsetup never sets dm-crypt's iv_large_sectors for LUKS) */
    Req.Flags = A->ReadOnly ? EXT4_CRYPT_READ_ONLY : 0;
    Req.Cipher = LuksCipher(V);
    Req.KeyBytes = V->KeyBytes;
    Req.SectorSize = V->SectorSize;
    Req.Letter = A->Letter;
    Req.PayloadOffset = V->PayloadOffset;
    Req.PayloadSize = V->PayloadSize;
    Req.IvOffset = V->IvOffset;
    wcscpy_s(Req.Device, ARRAYSIZE(Req.Device), A->Name);
    strcpy_s(Req.Uuid, sizeof(Req.Uuid), V->Uuid);
    memcpy(Req.Key, Key, V->KeyBytes);
    SecureZeroMemory(Key, LUKS_MAX_KEY);

    Error = DriverControl(IOCTL_APP_CRYPT_UNLOCK, &Req, sizeof(Req), &Req, sizeof(Req));
    SecureZeroMemory(&Req.Key, sizeof(Req.Key));
    if (Error != NO_ERROR) {
        if (Error != EXT4CTL_NO_DRIVER) {
            Fail(Error == ERROR_ALREADY_EXISTS || Error == ERROR_FILE_EXISTS ?
                 "%ls is unlocked already" : "the driver refused %ls (error %lu)", A->Name, Error);
        }
        return FALSE;
    }
    *Index = Req.Index;
    return TRUE;
}

int
CmdUnlock(int argc, WCHAR **argv)
{
    UNLOCK_ARGS A;
    LUKS_DEVICE Dev;
    LUKS_VOLUME V;
    UINT8       Key[LUKS_MAX_KEY];
    ULONG       Index;
    WCHAR       Device[64], Fs[32] = L"", Letter;
    int         Code;

    Code = ParseUnlockArgs(argc, argv, &A);
    if (Code != 0) {
        return Code;
    }
    if (!ProbeForUnlock(&Dev, A.Name, &V) || !VolumeKey(&Dev, &V, &A, Key) ||
        !SendUnlock(&A, &V, Key, &Index)) {
        SecureZeroMemory(Key, sizeof(Key));
        return 1;
    }

    CryptDevice(Index, Device, ARRAYSIZE(Device));
    Letter = WaitForMount(Device, Fs, ARRAYSIZE(Fs));
    if (Letter) {
        Say("%ls unlocked as %lc: (%ls, \\Device\\Ext4Crypt%lu)", A.Name, Letter, Fs, Index);
    } else {
        Say("%ls unlocked as \\Device\\Ext4Crypt%lu; no mounted drive letter yet", A.Name, Index);
    }
    return 0;
}

/* ---------------------------------------------------------------- lock, status */

int
CmdLock(int argc, WCHAR **argv)
{
    EXT4_CRYPT_LOCK     Req;
    EXT4_CRYPT_QUERY    Q;
    DWORD               Error;
    BOOL                Found = FALSE;
    ULONG               i;

    if (argc < 1) {
        Usage();
        return 2;
    }
    if (!CryptQuery(&Q)) {
        return 1;
    }

    memset(&Req, 0, sizeof(Req));
    Req.Magic = EXT4_CRYPT_MAGIC;
    Req.Flags = argc > 1 && _wcsicmp(argv[1], L"--force") == 0 ? EXT4_CRYPT_FORCE : 0;
    for (i = 0; i < Q.Count && !Found; i++) {
        WCHAR Device[64];
        CryptDevice(Q.Entries[i].Index, Device, ARRAYSIZE(Device));
        if (LetterNames(argv[0], Device) ||
            (iswdigit(argv[0][0]) && Q.Entries[i].Index == (ULONG)_wtoi(argv[0]))) {
            Req.Index = Q.Entries[i].Index;
            Found = TRUE;
        }
    }
    if (!Found) {
        Fail("%ls is not an unlocked volume (see ext4ctl status)", argv[0]);
        return 1;
    }

    Error = DriverControl(IOCTL_APP_CRYPT_LOCK, &Req, sizeof(Req), &Req, sizeof(Req));
    if (Error != NO_ERROR) {
        if (Error != EXT4CTL_NO_DRIVER) {
            Fail(Error == ERROR_BUSY ? "the volume is in use; close its files or use --force" :
                 "lock failed (error %lu)", Error);
        }
        return 1;
    }
    Say("\\Device\\Ext4Crypt%lu locked, the key is gone", Req.Index);
    return 0;
}

int
CmdStatus(void)
{
    EXT4_CRYPT_QUERY Q;
    ULONG            i;

    if (!CryptQuery(&Q)) {
        return 1;
    }
    if (Q.Count == 0) {
        Say("no unlocked LUKS volumes");
        return 0;
    }
    for (i = 0; i < Q.Count; i++) {
        const EXT4_CRYPT_ENTRY *E = &Q.Entries[i];
        WCHAR                   Device[64], L;

        CryptDevice(E->Index, Device, ARRAYSIZE(Device));
        L = DeviceLetter(Device);
        Say("%lu  %lc%s  %ls  UUID %s  %.1f G at %llu, %lu-byte sectors, AES-%lu-XTS%s",
            E->Index, L ? L : L'-', L ? ":" : " ", E->Device, E->Uuid,
            E->PayloadSize / EXT4CTL_GIB, E->PayloadOffset, E->SectorSize,
            E->KeyBytes * 8 / 2,                /* XTS: two AES keys of half the bits each */
            (E->Flags & EXT4_CRYPT_READ_ONLY) ? ", read-only" : "");
    }
    LvStatus();
    return 0;
}
