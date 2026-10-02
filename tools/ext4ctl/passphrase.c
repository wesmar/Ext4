/**
 * passphrase.c - the passphrase: typed at the console, a line of stdin, or a key file.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Bytes as cryptsetup hashes them: the terminal's text as UTF-8, a line of
 * standard input without its end, a key file whole. The caller's buffer is
 * locked and wiped; nothing here keeps a copy.
 */

#include "ext4ctl.h"

#define EXT4CTL_CONSOLE_CHARS   1024        /* a typed passphrase, in UTF-16 units */

static BOOL
ReadPassphraseConsole(char *Out, ULONG Room, ULONG *Length)
{
    HANDLE  In = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                             NULL, OPEN_EXISTING, 0, NULL);
    DWORD   Mode = 0, Got = 0;
    WCHAR   Wide[EXT4CTL_CONSOLE_CHARS];
    BOOL    Ok;
    int     n;

    if (In == INVALID_HANDLE_VALUE || !GetConsoleMode(In, &Mode)) {
        if (In != INVALID_HANDLE_VALUE) {
            CloseHandle(In);
        }
        return FALSE;
    }
    fputs("Passphrase: ", stderr);
    SetConsoleMode(In, (Mode | ENABLE_LINE_INPUT) & ~ENABLE_ECHO_INPUT);
    Ok = ReadConsoleW(In, Wide, ARRAYSIZE(Wide) - 1, &Got, NULL);
    SetConsoleMode(In, Mode);
    CloseHandle(In);
    fputc('\n', stderr);
    if (!Ok) {
        return FALSE;
    }
    /* a line that filled the buffer without its end was longer: refused,
       not cut (the rest would stay in the console, the start be used) */
    if (Got == ARRAYSIZE(Wide) - 1 && Wide[Got - 1] != L'\n') {
        SecureZeroMemory(Wide, sizeof(Wide));
        Fail("the passphrase is longer than %u characters", (unsigned)(ARRAYSIZE(Wide) - 3));
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
    HANDLE          h = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD           Got = 0;
    LARGE_INTEGER   Size;
    BOOL            Ok;

    if (h == INVALID_HANDLE_VALUE) {
        Fail("cannot open key file %ls (error %lu)", Path, GetLastError());
        return FALSE;
    }
    /* cryptsetup refuses a key file past its limit; the start of it is not
       the key, so neither is it here */
    if (!GetFileSizeEx(h, &Size) || (ULONGLONG)Size.QuadPart > Room) {
        Fail("key file %ls is larger than %lu bytes", Path, Room);
        CloseHandle(h);
        return FALSE;
    }
    Ok = ReadFile(h, Out, Room, &Got, NULL);
    CloseHandle(h);
    *Length = Got;
    return Ok;
}

BOOL
ReadPassphrase(const WCHAR *KeyFile, BOOL Stdin, char *Out, ULONG Room, ULONG *Length)
{
    if (KeyFile) {
        return ReadKeyFile(KeyFile, Out, Room, Length);
    }
    if (Stdin) {
        return ReadPassphraseStdin(Out, Room, Length);
    }
    /* no console (ssh, a pipe): a line of standard input */
    return ReadPassphraseConsole(Out, Room, Length) || ReadPassphraseStdin(Out, Room, Length);
}
