/**
 * cmd_file.c - ext4ctl mount, unmount, trim: ext4 volumes in image files, and
 * free blocks given back to the device under a volume.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 */

#include "ext4ctl.h"

#define EXT4CTL_TRIM_DEFAULT    65536       /* NTFS frees a sparse file 64 KiB at a time */

static void
FileDevice(ULONG Index, WCHAR *Device, DWORD Chars)
{
    swprintf(Device, Chars, EXT4_FILE_DEVICE_PREFIX L"%lu", Index);
}

/* the file's NT name: \??\C:\..., \??\UNC\server\share\... */
static BOOL
FileNtPath(const WCHAR *Name, WCHAR *Path, DWORD Chars)
{
    WCHAR   Full[EXT4_FILE_PATH_CHARS];
    DWORD   Length = GetFullPathNameW(Name, ARRAYSIZE(Full), Full, NULL);

    if (Length == 0 || Length >= ARRAYSIZE(Full)) {
        Fail("%ls: no such path, or too long", Name);
        return FALSE;
    }
    if (Full[0] == L'\\' && Full[1] == L'\\' && Full[2] != L'?' && Full[2] != L'.') {
        swprintf(Path, Chars, L"\\??\\UNC\\%ls", Full + 2);
    } else if (Full[0] == L'\\' && Full[1] == L'\\') {
        swprintf(Path, Chars, L"\\??\\%ls", Full + 4);
    } else {
        swprintf(Path, Chars, L"\\??\\%ls", Full);
    }
    return TRUE;
}

int
CmdMount(int argc, WCHAR **argv)
{
    EXT4_FILE_OPEN  Req;
    WCHAR           Device[64], Fs[32], L;
    DWORD           Error;
    int             i;

    if (argc < 1) {
        Usage();
        return 2;
    }
    memset(&Req, 0, sizeof(Req));
    Req.Magic = EXT4_CRYPT_MAGIC;
    Req.Version = EXT4_CRYPT_VERSION;
    for (i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--read-only") == 0) {
            Req.Flags |= EXT4_CRYPT_READ_ONLY;
        } else if (_wcsicmp(argv[i], L"--letter") == 0 && i + 1 < argc && iswalpha(argv[i + 1][0])) {
            Req.Letter = towupper(argv[++i][0]);
        } else {
            Usage();
            return 2;
        }
    }
    if (!FileNtPath(argv[0], Req.Path, ARRAYSIZE(Req.Path))) {
        return 1;
    }

    Error = DriverControl(IOCTL_APP_FILE_OPEN, &Req, sizeof(Req), &Req, sizeof(Req));
    if (Error != NO_ERROR) {
        if (Error == ERROR_INVALID_FUNCTION) {
            Fail("%ls lies on a volume of ext4.sys itself; such a file cannot hold one", argv[0]);
        } else if (Error == ERROR_SHARING_VIOLATION) {
            Fail("%ls is open for writing, or mounted already", argv[0]);
        } else if (Error != EXT4CTL_NO_DRIVER) {
            Fail("%ls cannot be mounted (error %lu)", argv[0], Error);
        }
        return 1;
    }

    FileDevice(Req.Index, Device, ARRAYSIZE(Device));
    L = WaitForMount(Device, Fs, ARRAYSIZE(Fs));
    if (L) {
        Say("%ls is %lc: (%ls, %.1f G, index %lu)", argv[0], L, Fs, Req.Size / EXT4CTL_GIB, Req.Index);
    } else {
        Say("%ls is %ls; no mounted drive letter yet", argv[0], Device);
    }
    if (!Req.Sparse && !(Req.Flags & EXT4_CRYPT_READ_ONLY)) {
        Say("the file is not sparse: ext4ctl trim cannot give anything back from it");
    }
    return 0;
}

int
CmdUnmount(int argc, WCHAR **argv)
{
    static EXT4_FILE_QUERY  Q;              /* too large for a stack */
    EXT4_FILE_CLOSE         Req;
    DWORD                   Error;
    BOOL                    Found = FALSE;
    ULONG                   i;

    if (argc < 1) {
        Usage();
        return 2;
    }
    memset(&Q, 0, sizeof(Q));
    Q.Magic = EXT4_CRYPT_MAGIC;
    Error = DriverControl(IOCTL_APP_FILE_QUERY, &Q, sizeof(Q), &Q, sizeof(Q));
    if (Error != NO_ERROR) {
        if (Error != EXT4CTL_NO_DRIVER) {
            Fail("query failed (error %lu)", Error);
        }
        return 1;
    }

    memset(&Req, 0, sizeof(Req));
    Req.Magic = EXT4_CRYPT_MAGIC;
    Req.Flags = argc > 1 && _wcsicmp(argv[1], L"--force") == 0 ? EXT4_CRYPT_FORCE : 0;
    for (i = 0; i < Q.Count && !Found; i++) {
        WCHAR Device[64];
        FileDevice(Q.Entries[i].Index, Device, ARRAYSIZE(Device));
        if (LetterNames(argv[0], Device) ||
            (iswdigit(argv[0][0]) && Q.Entries[i].Index == (ULONG)_wtoi(argv[0]))) {
            Req.Index = Q.Entries[i].Index;
            Found = TRUE;
        }
    }
    if (!Found) {
        Fail("%ls is not a mounted file (see ext4ctl status)", argv[0]);
        return 1;
    }

    Error = DriverControl(IOCTL_APP_FILE_CLOSE, &Req, sizeof(Req), &Req, sizeof(Req));
    if (Error != NO_ERROR) {
        if (Error != EXT4CTL_NO_DRIVER) {
            Fail(Error == ERROR_BUSY ? "the volume is in use; close its files or use --force" :
                 "unmount failed (error %lu)", Error);
        }
        return 1;
    }
    Say("%ls unmounted, the file is flushed and free", argv[0]);
    return 0;
}

/* the files that are mounted, for ext4ctl status; how many */
ULONG
FileStatus(void)
{
    static EXT4_FILE_QUERY  Q;
    ULONG                   i;

    memset(&Q, 0, sizeof(Q));
    Q.Magic = EXT4_CRYPT_MAGIC;
    if (DriverControl(IOCTL_APP_FILE_QUERY, &Q, sizeof(Q), &Q, sizeof(Q)) != NO_ERROR) {
        return 0;
    }
    for (i = 0; i < Q.Count; i++) {
        const EXT4_FILE_ENTRY  *E = &Q.Entries[i];
        WCHAR                   Device[64], L;

        FileDevice(E->Index, Device, ARRAYSIZE(Device));
        L = DeviceLetter(Device);
        Say("file %lu  %lc%s  %ls  %.1f G%s%s", E->Index, L ? L : L'-', L ? ":" : " ", E->Path + 4,
            E->Size / EXT4CTL_GIB, E->Sparse ? ", sparse" : "",
            (E->Flags & EXT4_CRYPT_READ_ONLY) ? ", read-only" : "");
    }
    return Q.Count;
}

int
CmdTrim(int argc, WCHAR **argv)
{
    EXT4_TRIM   Trim;
    WCHAR       Volume[8];
    HANDLE      h;
    DWORD       Bytes;

    if (argc < 1 || !iswalpha(argv[0][0])) {
        Usage();
        return 2;
    }
    memset(&Trim, 0, sizeof(Trim));
    Trim.MinLength = EXT4CTL_TRIM_DEFAULT;
    if (argc >= 3 && _wcsicmp(argv[1], L"--min-bytes") == 0) {
        Trim.MinLength = _wcstoui64(argv[2], NULL, 10);
    }

    swprintf(Volume, ARRAYSIZE(Volume), L"\\\\.\\%lc:", argv[0][0]);
    h = CreateFileW(Volume, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        Fail("%ls cannot be opened (error %lu)", argv[0], GetLastError());
        return 1;
    }
    if (!DeviceIoControl(h, FSCTL_EXT4_TRIM, &Trim, sizeof(Trim), &Trim, sizeof(Trim), &Bytes, NULL)) {
        DWORD Error = GetLastError();

        CloseHandle(h);
        if (Error == ERROR_NOT_SUPPORTED) {
            Fail("the device under %ls takes no trims (a file that is not sparse does not)", argv[0]);
        } else if (Error == ERROR_INVALID_FUNCTION) {
            Fail("%ls is not a volume of ext4.sys", argv[0]);
        } else {
            Fail("trim failed (error %lu)", Error);
        }
        return 1;
    }
    CloseHandle(h);
    Say("%ls: %.2f G trimmed in %llu runs", argv[0], Trim.Bytes / EXT4CTL_GIB, Trim.Runs);
    return 0;
}
