/**
 * luks_device.c - the volume a LUKS header is read from: open, close, read at any offset.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "luks_internal.h"

BOOL
LuksOpenDevice(LUKS_DEVICE *Dev, const WCHAR *Name)
{
    WCHAR                   Path[EXT4_CRYPT_DEVICE_CHARS + 32];
    GET_LENGTH_INFORMATION  Length;
    DWORD                   Bytes;

    memset(Dev, 0, sizeof(*Dev));
    wcsncpy_s(Dev->NtName, ARRAYSIZE(Dev->NtName), Name, _TRUNCATE);
    swprintf_s(Path, ARRAYSIZE(Path), L"\\\\?\\GLOBALROOT%ls", Name);
    Dev->Handle = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);
    if (Dev->Handle == INVALID_HANDLE_VALUE) {
        Dev->Handle = NULL;
        return FALSE;
    }
    if (DeviceIoControl(Dev->Handle, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0,
                        &Length, sizeof(Length), &Bytes, NULL)) {
        Dev->Size = (ULONGLONG)Length.Length.QuadPart;
    }
    return TRUE;
}

void
LuksCloseDevice(LUKS_DEVICE *Dev)
{
    if (Dev->Handle) {
        CloseHandle(Dev->Handle);
    }
    memset(Dev, 0, sizeof(*Dev));
}

/* any range: raw volume reads must be whole sectors, so read around it */
BOOL
LuksReadAt(LUKS_DEVICE *Dev, ULONGLONG Offset, void *Buffer, ULONG Length)
{
    ULONGLONG   First = Offset & ~(ULONGLONG)(LUKS_SECTOR - 1);
    ULONG       Head = (ULONG)(Offset - First);
    ULONG       Span = (Head + Length + LUKS_SECTOR - 1) & ~(ULONG)(LUKS_SECTOR - 1);
    UINT8      *Aligned;
    OVERLAPPED  Ov;
    DWORD       Got = 0;
    BOOL        Ok;

    Aligned = (UINT8 *)VirtualAlloc(NULL, Span, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (Aligned == NULL) {
        return FALSE;
    }
    memset(&Ov, 0, sizeof(Ov));
    Ov.Offset = (DWORD)First;
    Ov.OffsetHigh = (DWORD)(First >> 32);
    Ok = ReadFile(Dev->Handle, Aligned, Span, &Got, &Ov) && Got == Span;
    if (Ok) {
        memcpy(Buffer, Aligned + Head, Length);
    }
    SecureZeroMemory(Aligned, Span);
    VirtualFree(Aligned, 0, MEM_RELEASE);
    return Ok;
}
