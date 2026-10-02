/**
 * driver.c - ext4.sys from user mode: its control device, the volumes it serves, their letters.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4ctl.h"

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

/* ---------------------------------------------------------------- the driver */

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

/*
 * One request to the driver: NO_ERROR, the request's own error, or
 * EXT4CTL_NO_DRIVER when the driver could not be opened (said already).
 */
DWORD
DriverControl(DWORD Code, void *In, DWORD InBytes, void *Out, DWORD OutBytes)
{
    HANDLE  h = OpenDriver();
    DWORD   Bytes, Error = NO_ERROR;

    if (h == NULL) {
        return EXT4CTL_NO_DRIVER;
    }
    if (!DeviceIoControl(h, Code, In, InBytes, Out, OutBytes, &Bytes, NULL)) {
        Error = GetLastError();
    }
    CloseHandle(h);
    return Error;
}

/* the unlocked LUKS volumes; FALSE after a message */
BOOL
CryptQuery(EXT4_CRYPT_QUERY *Q)
{
    DWORD Error;

    memset(Q, 0, sizeof(*Q));
    Q->Magic = EXT4_CRYPT_MAGIC;
    Error = DriverControl(IOCTL_APP_CRYPT_QUERY, Q, sizeof(*Q), Q, sizeof(*Q));
    if (Error != NO_ERROR) {
        if (Error != EXT4CTL_NO_DRIVER) {
            Fail("query failed (error %lu)", Error);
        }
        return FALSE;
    }
    return TRUE;
}

/* the open LVM volumes: NO_ERROR, or why not (unsaid) */
DWORD
LvQuery(EXT4_LV_QUERY *Q)
{
    memset(Q, 0, sizeof(*Q));
    Q->Magic = EXT4_CRYPT_MAGIC;
    return DriverControl(IOCTL_APP_LV_QUERY, Q, sizeof(*Q), Q, sizeof(*Q));
}

/* ---------------------------------------------------------------- letters */

/* the drive letter leading to Device (\Device\Ext4Crypt3 ...), or 0 */
WCHAR
DeviceLetter(const WCHAR *Device)
{
    WCHAR Target[512], Drive[3] = L"A:";

    for (Drive[0] = L'A'; Drive[0] <= L'Z'; Drive[0]++) {
        if (QueryDosDeviceW(Drive, Target, ARRAYSIZE(Target)) && _wcsicmp(Target, Device) == 0) {
            return Drive[0];
        }
    }
    return 0;
}

/* an "X:" argument whose letter leads to Device */
BOOL
LetterNames(const WCHAR *Arg, const WCHAR *Device)
{
    WCHAR Target[512], Drive[3] = L"A:";

    if (!iswalpha(Arg[0]) || Arg[1] != L':') {
        return FALSE;
    }
    Drive[0] = towupper(Arg[0]);
    return QueryDosDeviceW(Drive, Target, ARRAYSIZE(Target)) && _wcsicmp(Target, Device) == 0;
}

/*
 * The file system mounts on a worker after the device appears, and the
 * letter follows: wait for both, a bounded time. The letter, or 0; Fs gets
 * the file system's name.
 */
WCHAR
WaitForMount(const WCHAR *Device, WCHAR *Fs, DWORD FsChars)
{
    ULONGLONG Start;

    for (Start = GetTickCount64(); GetTickCount64() - Start < EXT4CTL_LETTER_WAIT_MS;
         Sleep(EXT4CTL_LETTER_POLL_MS)) {
        WCHAR L = DeviceLetter(Device);
        if (L) {
            WCHAR Root[4] = { L, L':', L'\\', 0 };
            if (GetVolumeInformationW(Root, NULL, 0, NULL, NULL, NULL, Fs, FsChars)) {
                return L;
            }
        }
    }
    return 0;
}
