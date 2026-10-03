/**
 * Utility.c - small helpers: log2, time conversion, dot names.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"

ULONG
Ext2Log2(ULONG Value)
{
    ULONG Order = 0;

    ASSERT(Value > 0);

    while (Value) {
        Order++;
        Value >>= 1;
    }

    return (Order - 1);
}

/*
 * Time stamps. Windows counts 100 ns ticks since 1601, ext4 counts seconds
 * since 1970 with 64-bit arithmetic here (RtlTimeToSecondsSince1970 stops
 * in 2106 and treats the value as 32 bits).
 *
 * Superblock stamps keep 8 more bits of seconds in *_hi bytes
 * (Ext2SetSuperTime). Inode stamps keep a signed 32-bit base and an
 * *_extra word: (nanoseconds << 2) | epoch, the epoch being bits 32-33 of
 * the seconds - the encoding of ext4_encode_extra_time.
 */

LONGLONG
Ext2UnixTime(IN PLARGE_INTEGER SysTime)
{
    return SysTime->QuadPart / TICKSPERSEC - SECS_1601_TO_1970;
}

LARGE_INTEGER
Ext2SystemTime(IN LONGLONG UnixSeconds)
{
    LARGE_INTEGER SysTime;

    SysTime.QuadPart = UnixSeconds * TICKSPERSEC + TICKS_1601_TO_1970;
    return SysTime;
}

VOID
Ext2SetInodeTime(
    IN PLARGE_INTEGER SysTime,
    OUT __u32 *i_time,
    OUT __u32 *i_time_extra)
{
    LONGLONG    Seconds = Ext2UnixTime(SysTime);
    ULONG       Epoch = (ULONG)((ULONGLONG)(Seconds - (LONG)Seconds) >> 32) & EXT4_EPOCH_MASK;
    ULONG       Nanoseconds = (ULONG)(SysTime->QuadPart % TICKSPERSEC) * 100;

    *i_time = (__u32)Seconds;
    *i_time_extra = Epoch | (Nanoseconds << EXT4_EPOCH_BITS);
}

LARGE_INTEGER
Ext2GetInodeTime(
    IN __u32 i_time,
    IN __u32 i_time_extra)
{
    LONGLONG        Seconds = (LONG)i_time + ((LONGLONG)(i_time_extra & EXT4_EPOCH_MASK) << 32);
    ULONG           Nanoseconds = (i_time_extra & EXT4_NSEC_MASK) >> EXT4_EPOCH_BITS;
    LARGE_INTEGER   SysTime = Ext2SystemTime(Seconds);

    SysTime.QuadPart += Nanoseconds / 100;
    return SysTime;
}

BOOLEAN Ext2IsDot(PUNICODE_STRING name)
{
    return (name->Length == 2 && name->Buffer[0] == L'.');
}

BOOLEAN Ext2IsDotDot(PUNICODE_STRING name)
{
    return (name->Length == 4 && name->Buffer[0] == L'.' &&
            name->Buffer[1] == L'.');
}
