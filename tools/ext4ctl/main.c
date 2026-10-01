/**
 * main.c - ext4ctl: LUKS volumes and the ext4.sys driver from the command line.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 *
 *   ext4ctl list                              partitions: LUKS headers, ext file systems
 *   ext4ctl unlock <volume> [options]         open a LUKS volume, mount it with a letter
 *       --letter X                            the drive letter wanted (default: first free)
 *       --read-only                           no writes to the volume
 *       --key-file <file>                     the whole file is the passphrase (cryptsetup)
 *       --passphrase-stdin                    one line from standard input (scripts)
 *   ext4ctl lock <X:|index> [--force]         dismount and close; --force with open files
 *   ext4ctl status                            the unlocked volumes and LVM volumes
 *   ext4ctl lv list <index>                   LVM volumes inside unlocked LUKS volume <index>
 *   ext4ctl lv open <index> <vg/lv> [--letter X]  map one read-only (linear or thin)
 *   ext4ctl lv close <X:|lv index> [--force]  unmap it
 *   ext4ctl selftest                          RFC 9106 / IEEE 1619 / PBKDF2 test vectors
 *
 * <volume> is a partition's device: 5, HarddiskVolume5, \Device\HarddiskVolume5
 * or \Device\Harddisk1\Partition2. Everything but selftest needs an
 * administrator. Works the same on Windows Server Core and over ssh.
 */

#include "ext4ctl.h"
#include <conio.h>

#define EXT4CTL_MAX_PASSPHRASE  (8 * 1024 * 1024)   /* cryptsetup's keyfile limit */
#define EXT4CTL_MAX_VOLUMES     256                 /* HarddiskVolume1 .. N scanned by list */

/* ---------------------------------------------------------------- output */

void
Fail(const char *Format, ...)
{
    va_list Args;
    va_start(Args, Format);
    fflush(stdout);                         /* keep the order when both go to one pipe */
    fputs("ext4ctl: ", stderr);
    vfprintf(stderr, Format, Args);
    fputc('\n', stderr);
    va_end(Args);
}

void
Say(const char *Format, ...)
{
    va_list Args;
    va_start(Args, Format);
    vprintf(Format, Args);
    putchar('\n');
    va_end(Args);
}

static void
Usage(void)
{
    puts("usage: ext4ctl list\n"
         "       ext4ctl unlock <volume> [--letter X] [--read-only]\n"
         "                               [--key-file <file> | --passphrase-stdin]\n"
         "       ext4ctl lock <X:|index> [--force]\n"
         "       ext4ctl status\n"
         "       ext4ctl lv list <index>\n"
         "       ext4ctl lv open <index> <vg/lv> [--letter X]   (read-only)\n"
         "       ext4ctl lv close <X:|lv index> [--force]\n"
         "       ext4ctl selftest\n"
         "<volume>: 5, HarddiskVolume5, \\Device\\HarddiskVolume5, \\Device\\Harddisk1\\Partition2\n"
         "          or UUID=<uuid> of the LUKS header (see ext4ctl list)");
}

/* ---------------------------------------------------------------- driver */

HANDLE
OpenDriver(void)
{
    HANDLE h = CreateFileW(L"\\\\.\\ext4", GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        Fail(e == ERROR_ACCESS_DENIED ? "the driver is for administrators (run elevated)" :
             "ext4.sys is not running (error %lu)", e);
        return NULL;
    }
    return h;
}

static BOOL
QueryDriver(EXT4_CRYPT_QUERY *Q)
{
    HANDLE  h = OpenDriver();
    DWORD   Bytes;
    BOOL    Ok;

    if (h == NULL) {
        return FALSE;
    }
    memset(Q, 0, sizeof(*Q));
    Q->Magic = EXT4_CRYPT_MAGIC;
    Ok = DeviceIoControl(h, IOCTL_APP_CRYPT_QUERY, Q, sizeof(*Q), Q, sizeof(*Q), &Bytes, NULL);
    if (!Ok) {
        Fail("query failed (error %lu)", GetLastError());
    }
    CloseHandle(h);
    return Ok;
}

/* the drive letter leading to \Device\Ext4Crypt<Index>, or 0 */
static WCHAR
LetterOf(ULONG Index)
{
    WCHAR   Device[64], Target[512], Drive[3] = L"A:";

    swprintf_s(Device, ARRAYSIZE(Device), EXT4_CRYPT_DEVICE_PREFIX L"%lu", Index);
    for (Drive[0] = L'A'; Drive[0] <= L'Z'; Drive[0]++) {
        if (QueryDosDeviceW(Drive, Target, ARRAYSIZE(Target)) && _wcsicmp(Target, Device) == 0) {
            return Drive[0];
        }
    }
    return 0;
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

        swprintf_s(Name, ARRAYSIZE(Name), L"\\Device\\HarddiskVolume%lu", n);
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
    const WCHAR *p;

    if (_wcsnicmp(Arg, L"UUID=", 5) == 0) {
        return VolumeByUuid(Arg + 5, Out, OutChars);
    }
    for (p = Arg; *p >= L'0' && *p <= L'9'; p++) {
    }
    if (*Arg && *p == 0) {
        return swprintf_s(Out, OutChars, L"\\Device\\HarddiskVolume%ls", Arg) > 0;
    }
    if (_wcsnicmp(Arg, L"\\Device\\", 8) == 0) {
        return wcscpy_s(Out, OutChars, Arg) == 0;
    }
    if (_wcsnicmp(Arg, L"\\\\.\\", 4) == 0 || _wcsnicmp(Arg, L"\\\\?\\", 4) == 0) {
        Arg += 4;
    }
    return swprintf_s(Out, OutChars, L"\\Device\\%ls", Arg) > 0;
}

static int
CmdList(void)
{
    ULONG   n, Found = 0;

    Say("%-28s %10s  %s", "volume", "size", "contents");
    for (n = 1; n <= EXT4CTL_MAX_VOLUMES; n++) {
        WCHAR       Name[64];
        LUKS_DEVICE Dev;
        LUKS_VOLUME V;
        UINT8       Super[2];
        int         Luks;

        swprintf_s(Name, ARRAYSIZE(Name), L"\\Device\\HarddiskVolume%lu", n);
        if (!LuksOpenDevice(&Dev, Name)) {
            continue;
        }
        Found++;
        Luks = LuksProbe(&Dev, &V);
        if (Luks == 1) {
            printf("%-28ls %8.1f G  LUKS%d %s, %lu-byte key, %lu-byte sectors, kdf %s, %lu keyslot(s), UUID %s%s%s\n",
                   Name, Dev.Size / 1073741824.0, V.Version, V.Cipher, V.KeyBytes, V.SectorSize,
                   V.Kdf, V.Keyslots, V.Uuid, V.Label[0] ? ", label " : "", V.Label);
        } else if (LuksReadAt(&Dev, EXT2_SUPER_MAGIC_OFFSET, Super, sizeof(Super)) &&
                   (Super[0] | (Super[1] << 8)) == EXT2_SUPER_MAGIC) {
            printf("%-28ls %8.1f G  ext2/3/4\n", Name, Dev.Size / 1073741824.0);
        }
        LuksCloseDevice(&Dev);
    }
    if (Found == 0) {
        Fail("no volume could be opened (run elevated)");
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- passphrase */

static BOOL
ReadPassphraseConsole(char *Out, ULONG Room, ULONG *Length)
{
    HANDLE  In = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                             NULL, OPEN_EXISTING, 0, NULL);
    DWORD   Mode = 0, Got = 0;
    WCHAR   Wide[1024];
    int     n;

    if (In == INVALID_HANDLE_VALUE || !GetConsoleMode(In, &Mode)) {
        if (In != INVALID_HANDLE_VALUE) CloseHandle(In);
        return FALSE;
    }
    fputs("Passphrase: ", stderr);
    SetConsoleMode(In, (Mode | ENABLE_LINE_INPUT) & ~ENABLE_ECHO_INPUT);
    BOOL Ok = ReadConsoleW(In, Wide, ARRAYSIZE(Wide) - 1, &Got, NULL);
    SetConsoleMode(In, Mode);
    CloseHandle(In);
    fputc('\n', stderr);
    if (!Ok) {
        return FALSE;
    }
    while (Got > 0 && (Wide[Got - 1] == L'\n' || Wide[Got - 1] == L'\r')) {
        Got--;
    }
    /* cryptsetup hashes the passphrase bytes as the terminal gave them: UTF-8 */
    n = WideCharToMultiByte(CP_UTF8, 0, Wide, (int)Got, Out, (int)Room, NULL, NULL);
    SecureZeroMemory(Wide, sizeof(Wide));
    if (n <= 0 && Got > 0) {
        return FALSE;
    }
    *Length = (ULONG)n;
    return TRUE;
}

static BOOL
ReadPassphraseStdin(char *Out, ULONG Room, ULONG *Length)
{
    ULONG n = 0;
    int   c;

    while ((c = getchar()) != EOF && c != '\n') {
        if (n + 1 >= Room) {
            return FALSE;
        }
        Out[n++] = (char)c;
    }
    if (n > 0 && Out[n - 1] == '\r') {
        n--;
    }
    *Length = n;
    return TRUE;
}

static BOOL
ReadKeyFile(const WCHAR *Path, char *Out, ULONG Room, ULONG *Length)
{
    HANDLE  h = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD   Got = 0;
    BOOL    Ok;

    if (h == INVALID_HANDLE_VALUE) {
        Fail("cannot open key file %ls (error %lu)", Path, GetLastError());
        return FALSE;
    }
    Ok = ReadFile(h, Out, Room, &Got, NULL);
    CloseHandle(h);
    *Length = Got;
    return Ok;
}

/* ---------------------------------------------------------------- unlock / lock */

static int
CmdUnlock(int argc, WCHAR **argv)
{
    WCHAR               Name[EXT4_CRYPT_DEVICE_CHARS];
    const WCHAR        *KeyFile = NULL;
    BOOL                Stdin = FALSE, ReadOnly = FALSE;
    WCHAR               Letter = 0;
    LUKS_DEVICE         Dev;
    LUKS_VOLUME         V;
    char               *Pass;
    ULONG               PassLength = 0;
    UINT8               Key[LUKS_MAX_KEY];
    EXT4_CRYPT_UNLOCK   Req;
    int                 Keyslot = -1, i;
    ULONGLONG           Start;
    HANDLE              Driver;
    DWORD               Bytes;
    BOOL                Ok;

    if (argc < 1 || !VolumeName(argv[0], Name, ARRAYSIZE(Name))) {
        Usage();
        return 2;
    }
    for (i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--letter") == 0 && i + 1 < argc) {
            Letter = towupper(argv[++i][0]);
        } else if (_wcsicmp(argv[i], L"--read-only") == 0) {
            ReadOnly = TRUE;
        } else if (_wcsicmp(argv[i], L"--key-file") == 0 && i + 1 < argc) {
            KeyFile = argv[++i];
        } else if (_wcsicmp(argv[i], L"--passphrase-stdin") == 0) {
            Stdin = TRUE;
        } else {
            Usage();
            return 2;
        }
    }
    if (Letter && (Letter < L'A' || Letter > L'Z')) {
        Fail("--letter wants A..Z");
        return 2;
    }

    if (!LuksOpenDevice(&Dev, Name)) {
        Fail("cannot open %ls (error %lu)", Name, GetLastError());
        return 1;
    }
    if (LuksProbe(&Dev, &V) != 1) {
        Fail("%ls holds no readable LUKS header", Name);
        LuksCloseDevice(&Dev);
        return 1;
    }

    Pass = (char *)VirtualAlloc(NULL, EXT4CTL_MAX_PASSPHRASE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (Pass == NULL) {
        LuksCloseDevice(&Dev);
        return 1;
    }
    VirtualLock(Pass, 4096);
    Ok = KeyFile ? ReadKeyFile(KeyFile, Pass, EXT4CTL_MAX_PASSPHRASE, &PassLength) :
         Stdin   ? ReadPassphraseStdin(Pass, EXT4CTL_MAX_PASSPHRASE, &PassLength) :
                   ReadPassphraseConsole(Pass, EXT4CTL_MAX_PASSPHRASE, &PassLength) ||
                   ReadPassphraseStdin(Pass, EXT4CTL_MAX_PASSPHRASE, &PassLength);
    if (!Ok) {
        Fail("no passphrase");
        VirtualFree(Pass, 0, MEM_RELEASE);
        LuksCloseDevice(&Dev);
        return 1;
    }

    Start = GetTickCount64();
    Ok = LuksUnlockKey(&Dev, &V, Pass, PassLength, Key, &Keyslot);
    SecureZeroMemory(Pass, PassLength);
    VirtualFree(Pass, 0, MEM_RELEASE);
    LuksCloseDevice(&Dev);
    if (!Ok) {
        Fail("no keyslot opens with this passphrase");
        return 1;
    }
    Say("keyslot %d opened (%s, %llu ms)", Keyslot, V.Kdf, GetTickCount64() - Start);

    memset(&Req, 0, sizeof(Req));
    Req.Magic = EXT4_CRYPT_MAGIC;
    Req.Version = EXT4_CRYPT_VERSION;
    /* LUKS counts the IV in 512-byte sectors whatever the encryption sector
       size (cryptsetup never sets dm-crypt's iv_large_sectors for LUKS) */
    Req.Flags = ReadOnly ? EXT4_CRYPT_READ_ONLY : 0;
    Req.Cipher = _stricmp(V.Cipher, "aes-xts-plain") == 0 ? EXT4_CIPHER_AES_XTS_PLAIN :
                                                            EXT4_CIPHER_AES_XTS_PLAIN64;
    Req.KeyBytes = V.KeyBytes;
    Req.SectorSize = V.SectorSize;
    Req.Letter = Letter;
    Req.PayloadOffset = V.PayloadOffset;
    Req.PayloadSize = V.PayloadSize;
    Req.IvOffset = V.IvOffset;
    wcscpy_s(Req.Device, ARRAYSIZE(Req.Device), Name);
    strcpy_s(Req.Uuid, sizeof(Req.Uuid), V.Uuid);
    memcpy(Req.Key, Key, V.KeyBytes);
    SecureZeroMemory(Key, sizeof(Key));

    Driver = OpenDriver();
    Ok = Driver && DeviceIoControl(Driver, IOCTL_APP_CRYPT_UNLOCK, &Req, sizeof(Req),
                                   &Req, sizeof(Req), &Bytes, NULL);
    if (!Ok && Driver) {
        DWORD e = GetLastError();
        Fail(e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS ?
             "%ls is unlocked already" : "the driver refused %ls (error %lu)", Name, e);
    }
    SecureZeroMemory(&Req.Key, sizeof(Req.Key));
    if (Driver) CloseHandle(Driver);
    if (!Ok) {
        return 1;
    }

    /* the file system mounts on a worker right after; the letter follows */
    for (Start = GetTickCount64(); GetTickCount64() - Start < EXT4CTL_LETTER_WAIT_MS; ) {
        WCHAR L = LetterOf(Req.Index);
        if (L) {
            WCHAR Root[4] = { L, L':', L'\\', 0 }, Fs[32] = L"";
            if (GetVolumeInformationW(Root, NULL, 0, NULL, NULL, NULL, Fs, ARRAYSIZE(Fs))) {
                Say("%ls unlocked as %lc: (%ls, \\Device\\Ext4Crypt%lu)", Name, L, Fs, Req.Index);
                return 0;
            }
        }
        Sleep(EXT4CTL_LETTER_POLL_MS);
    }
    Say("%ls unlocked as \\Device\\Ext4Crypt%lu; no mounted drive letter yet", Name, Req.Index);
    return 0;
}

static int
CmdLock(int argc, WCHAR **argv)
{
    EXT4_CRYPT_LOCK Req;
    EXT4_CRYPT_QUERY Q;
    HANDLE          Driver;
    DWORD           Bytes;
    BOOL            Ok, Force = FALSE, Found = FALSE;
    ULONG           i;

    if (argc < 1) {
        Usage();
        return 2;
    }
    if (argc > 1 && _wcsicmp(argv[1], L"--force") == 0) {
        Force = TRUE;
    }
    if (!QueryDriver(&Q)) {
        return 1;
    }

    memset(&Req, 0, sizeof(Req));
    Req.Magic = EXT4_CRYPT_MAGIC;
    Req.Flags = Force ? EXT4_CRYPT_FORCE : 0;
    for (i = 0; i < Q.Count && !Found; i++) {
        WCHAR L = LetterOf(Q.Entries[i].Index);
        if ((iswalpha(argv[0][0]) && argv[0][1] == L':' && L == towupper(argv[0][0])) ||
            (iswdigit(argv[0][0]) && Q.Entries[i].Index == (ULONG)_wtoi(argv[0]))) {
            Req.Index = Q.Entries[i].Index;
            Found = TRUE;
        }
    }
    if (!Found) {
        Fail("%ls is not an unlocked volume (see ext4ctl status)", argv[0]);
        return 1;
    }

    Driver = OpenDriver();
    Ok = Driver && DeviceIoControl(Driver, IOCTL_APP_CRYPT_LOCK, &Req, sizeof(Req),
                                   &Req, sizeof(Req), &Bytes, NULL);
    if (!Ok && Driver) {
        DWORD e = GetLastError();
        Fail(e == ERROR_BUSY ? "the volume is in use; close its files or use --force" :
             "lock failed (error %lu)", e);
    }
    if (Driver) CloseHandle(Driver);
    if (Ok) {
        Say("\\Device\\Ext4Crypt%lu locked, the key is gone", Req.Index);
    }
    return Ok ? 0 : 1;
}

static int
CmdStatus(void)
{
    EXT4_CRYPT_QUERY Q;
    ULONG            i;

    if (!QueryDriver(&Q)) {
        return 1;
    }
    if (Q.Count == 0) {
        Say("no unlocked LUKS volumes");
        return 0;
    }

    for (i = 0; i < Q.Count; i++) {
        const EXT4_CRYPT_ENTRY *E = &Q.Entries[i];
        WCHAR L = LetterOf(E->Index);
        Say("%lu  %lc%s  %ls  UUID %s  %.1f G at %llu, %lu-byte sectors, AES-%lu-XTS%s",
            E->Index, L ? L : L'-', L ? ":" : " ", E->Device, E->Uuid,
            E->PayloadSize / 1073741824.0, E->PayloadOffset, E->SectorSize, E->KeyBytes * 4,
            (E->Flags & EXT4_CRYPT_READ_ONLY) ? ", read-only" : "");
    }
    LvStatus();
    return 0;
}

/* ---------------------------------------------------------------- selftest */

static BOOL
Hex(const char *Text, UINT8 *Out, ULONG Length)
{
    ULONG i;
    for (i = 0; i < Length; i++) {
        unsigned v;
        if (sscanf_s(Text + 2 * i, "%2x", &v) != 1) return FALSE;
        Out[i] = (UINT8)v;
    }
    return TRUE;
}

static BOOL
Check(const char *Name, const UINT8 *Got, const char *WantHex, ULONG Length)
{
    UINT8 Want[64];
    BOOL  Ok = Hex(WantHex, Want, Length) && memcmp(Got, Want, Length) == 0;
    Say("  %s  %s", Ok ? "ok  " : "FAIL", Name);
    return Ok;
}

static int
CmdSelfTest(void)
{
    UINT8           Out[64], Pwd[32], Salt[16], Secret[8], Ad[12], Key[64], Data[512], Scratch[512];
    ARGON2_INPUT    In;
    EXT4_XTS        Xts;
    int             Failed = 0;

    /* RFC 9106, 5.3: Argon2id, t=3, m=32 KiB, p=4 */
    memset(Pwd, 1, sizeof(Pwd)); memset(Salt, 2, sizeof(Salt));
    memset(Secret, 3, sizeof(Secret)); memset(Ad, 4, sizeof(Ad));
    memset(&In, 0, sizeof(In));
    In.Password = Pwd;  In.PasswordLength = sizeof(Pwd);
    In.Salt = Salt;     In.SaltLength = sizeof(Salt);
    In.Secret = Secret; In.SecretLength = sizeof(Secret);
    In.Data = Ad;       In.DataLength = sizeof(Ad);
    In.Passes = 3; In.MemoryKiB = 32; In.Lanes = 4;
    In.Type = ARGON2_ID;
    Failed += !(Argon2(&In, Out, 32) && Check("Argon2id (RFC 9106 5.3)", Out,
        "0d640df58d78766c08c037a34a8b53c9d01ef0452d75b65eb52520e96b01e659", 32));
    /* RFC 9106, 5.2: Argon2i, same input */
    In.Type = ARGON2_I;
    Failed += !(Argon2(&In, Out, 32) && Check("Argon2i  (RFC 9106 5.2)", Out,
        "c814d9d1dc7f37aa13f0d77f2494bda1c8de6b016dd388d29952a4c4672b6ce8", 32));

    /* RFC 6070: PBKDF2-HMAC-SHA1 "password"/"salt", 4096 iterations */
    Failed += !(CryptoPbkdf2(BCRYPT_SHA1_ALGORITHM, "password", 8, "salt", 4, 4096, Out, 20) &&
                Check("PBKDF2-SHA1 (RFC 6070)", Out, "4b007901b765489abead49d926f721d065a429c1", 20));

    /* IEEE 1619-2007 vector 1: XTS-AES-128, keys and data zero, unit 0 */
    memset(Key, 0, sizeof(Key));
    memset(Data, 0, sizeof(Data));
    Failed += !(CryptoAesEcb() && NT_SUCCESS(Ext4XtsInit(&Xts, CryptoAesEcb(), Key, 32)) &&
                NT_SUCCESS(Ext4XtsUnit(&Xts, TRUE, 0, Data, 32, Scratch)) &&
                Check("XTS-AES-128 (IEEE 1619 #1)", Data,
                      "917cf69ebd68b2ec9b9fe9a3eadda692cd43d2f59598ed858c02c2652fbf922e", 32));
    Ext4XtsFree(&Xts);

    /* and back */
    Failed += !(NT_SUCCESS(Ext4XtsInit(&Xts, CryptoAesEcb(), Key, 32)) &&
                NT_SUCCESS(Ext4XtsUnit(&Xts, FALSE, 0, Data, 32, Scratch)) &&
                Check("XTS-AES-128 decrypt", Data,
                      "0000000000000000000000000000000000000000000000000000000000000000", 32));
    Ext4XtsFree(&Xts);

    Say("selftest: %d failed", Failed);
    return Failed ? 1 : 0;
}

/* ---------------------------------------------------------------- lv */

static int
CmdLv(int argc, WCHAR **argv)
{
    WCHAR Letter = 0;

    if (argc >= 2 && _wcsicmp(argv[0], L"list") == 0 && iswdigit(argv[1][0])) {
        return CmdLvList((ULONG)_wtoi(argv[1]));
    }
    if (argc >= 3 && _wcsicmp(argv[0], L"open") == 0 && iswdigit(argv[1][0])) {
        if (argc >= 5 && _wcsicmp(argv[3], L"--letter") == 0 && iswalpha(argv[4][0])) {
            Letter = towupper(argv[4][0]);
        }
        return CmdLvOpen((ULONG)_wtoi(argv[1]), argv[2], Letter);
    }
    if (argc >= 2 && _wcsicmp(argv[0], L"close") == 0) {
        return CmdLvClose(argv[1], argc >= 3 && _wcsicmp(argv[2], L"--force") == 0);
    }
    Usage();
    return 2;
}

/* ---------------------------------------------------------------- main */

int
wmain(int argc, WCHAR **argv)
{
    if (argc < 2) {
        Usage();
        return 2;
    }
    if (_wcsicmp(argv[1], L"list") == 0)     return CmdList();
    if (_wcsicmp(argv[1], L"unlock") == 0)   return CmdUnlock(argc - 2, argv + 2);
    if (_wcsicmp(argv[1], L"lock") == 0)     return CmdLock(argc - 2, argv + 2);
    if (_wcsicmp(argv[1], L"status") == 0)   return CmdStatus();
    if (_wcsicmp(argv[1], L"selftest") == 0) return CmdSelfTest();
    if (_wcsicmp(argv[1], L"lv") == 0)       return CmdLv(argc - 2, argv + 2);
    Usage();
    return 2;
}
