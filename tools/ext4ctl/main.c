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
 *
 *   cmd_crypt.c    list, unlock, lock, status      luks*.c   the LUKS formats
 *   lvm_cmd.c      lv list / open / close          lvm_*.c   LVM2 and dm-thin
 *   selftest.c     selftest                        driver.c  ext4.sys, letters, output
 */

#include "ext4ctl.h"

void
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
