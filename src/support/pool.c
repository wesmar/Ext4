/**
 * pool.c - pool allocation wrappers; checked builds add guard bytes and accounting.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"

#if EXT2_DEBUG

KSPIN_LOCK  Ext2MemoryLock;
ULONGLONG   Ext2TotalMemorySize = 0;
ULONG       Ext2TotalAllocates = 0;

PVOID
Ext2AllocatePool(
    IN POOL_TYPE PoolType,
    IN SIZE_T NumberOfBytes,
    IN ULONG Tag
)
{
    PUCHAR  Buffer =  ExAllocatePoolWithTag(
                          PoolType,
                          0x20 + NumberOfBytes,
                          Tag);
    if (Buffer) {
        KIRQL   Irql = 0;
        PULONG  Data = (PULONG)Buffer;
        Data[0] = (ULONG)NumberOfBytes;
        Data[1] = (ULONG)NumberOfBytes + 0x20;
        memset(Buffer + 0x08, 'S', 8);
        memset(Buffer + 0x10 + NumberOfBytes, 'E', 0x10);
        Buffer += 0x10;
        KeAcquireSpinLock(&Ext2MemoryLock, &Irql);
        Ext2TotalMemorySize = Ext2TotalMemorySize + NumberOfBytes;
        Ext2TotalAllocates += 1;
        KeReleaseSpinLock(&Ext2MemoryLock, Irql);
    }

    return Buffer;
}

VOID
Ext2FreePool(
    IN PVOID P,
    IN ULONG Tag
)
{
    PUCHAR  Buffer = (PUCHAR)P;
    PULONG  Data;
    ULONG   NumberOfBytes, i;
    KIRQL   Irql;

    Buffer -= 0x10;
    Data = (PULONG)(Buffer);
    NumberOfBytes = Data[0];
    if (Data[1] != NumberOfBytes + 0x20) {
        DbgBreak();
        return;
    }
    for (i=0x08; i < 0x10; i++) {
        if (Buffer[i] != 'S') {
            DbgBreak();
        }
        Buffer[i] = '-';
    }
    for (i=0; i < 0x10; i++) {
        if (Buffer[i + NumberOfBytes + 0x10] != 'E') {
            DbgBreak();
            return;
        }
        Buffer[i + NumberOfBytes + 0x10] = '-';
    }

    KeAcquireSpinLock(&Ext2MemoryLock, &Irql);
    Ext2TotalMemorySize = Ext2TotalMemorySize - NumberOfBytes;
    Ext2TotalAllocates -= 1;
    KeReleaseSpinLock(&Ext2MemoryLock, Irql);

    ExFreePoolWithTag(Buffer, Tag);
}

#else /* !EXT2_DEBUG */

PVOID
Ext2AllocatePool(
    IN POOL_TYPE PoolType,
    IN SIZE_T NumberOfBytes,
    IN ULONG Tag
)
{
    POOL_FLAGS PoolFlags;

    /* ExAllocatePool2 memory is no-execute already: both non-paged types
       map to the same flag (flags 0 would fail every allocation) */
    if (PoolType == PagedPool)
        PoolFlags = POOL_FLAG_PAGED;
    else if (PoolType == NonPagedPool || PoolType == NonPagedPoolNx)
        PoolFlags = POOL_FLAG_NON_PAGED;
    else
        PoolFlags = 0;

    return ExAllocatePool2(
               PoolFlags,
               NumberOfBytes,
               Tag);
}

VOID
Ext2FreePool(
    IN PVOID P,
    IN ULONG Tag
)
{
    ExFreePoolWithTag(P, Tag);
}

#endif /* EXT2_DEBUG */
