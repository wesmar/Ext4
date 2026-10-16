/**
 * ext4fs_common.h - helpers shared by every part of the driver.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _EXT4_EXT4FS_COMMON_H_
#define _EXT4_EXT4FS_COMMON_H_

/* Include this so we don't need the latest WDK to build the driver. */
#ifndef FSCTL_GET_RETRIEVAL_POINTER_BASE
#define FSCTL_GET_RETRIEVAL_POINTER_BASE    CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 141, METHOD_BUFFERED, FILE_ANY_ACCESS) /* RETRIEVAL_POINTER_BASE */
#endif

#ifndef FILE_SUPPORTS_EXTENDED_ATTRIBUTES
#define FILE_SUPPORTS_EXTENDED_ATTRIBUTES   0x00800000
#endif

/* memory allocation statistics */

/* the statistics block keeps the ULONG layout reported to user mode; its
   counters change atomically, and the Interlocked primitives take LONG */
#define PERF_COUNTER(c)     ((volatile LONG *)&(c))

/*
 * Object and IRP statistics (IOCTL_APP_QUERY_PERFSTAT).
 *
 * Debug builds keep all of them. A release build keeps exactly one, the
 * number of cached names: the name-cache reaper runs on it (high and low
 * water marks), so it is part of the driver, not of its diagnostics. The
 * rest - totals, sizes, per-IRP counts - were three to five interlocked
 * operations on shared cache lines for every allocation, free and IRP,
 * contended by every CPU. Slot is a constant at every call site and the
 * function is inlined, so a release build keeps one instruction for the
 * name cache and nothing at all elsewhere; the statistics it reports are
 * zero apart from that count.
 */
FORCEINLINE
VOID
Ext2TraceMemory(BOOLEAN _n, int _i, PVOID _p, LONG _s)
{
    UNREFERENCED_PARAMETER(_p);
#if EXT2_DEBUG
    if (_n) {
        InterlockedIncrement(PERF_COUNTER(Ext2Global->PerfStat.Current.Slot[_i]));
        InterlockedIncrement(PERF_COUNTER(Ext2Global->PerfStat.Total.Slot[_i]));
        InterlockedExchangeAdd(PERF_COUNTER(Ext2Global->PerfStat.Size.Slot[_i]), _s);
    } else {
        InterlockedDecrement(PERF_COUNTER(Ext2Global->PerfStat.Current.Slot[_i]));
        InterlockedExchangeAdd(PERF_COUNTER(Ext2Global->PerfStat.Size.Slot[_i]), -_s);
    }
#else
    UNREFERENCED_PARAMETER(_s);
    if (_i == PS_MCB) {
        if (_n) {
            InterlockedIncrement(PERF_COUNTER(Ext2Global->PerfStat.Current.Slot[_i]));
        } else {
            InterlockedDecrement(PERF_COUNTER(Ext2Global->PerfStat.Current.Slot[_i]));
        }
    }
#endif
}

FORCEINLINE
VOID
Ext2TraceIrpContext(BOOLEAN _n, PEXT2_IRP_CONTEXT IrpContext)
{
#if EXT2_DEBUG
    if (_n) {
        INC_MEM_COUNT(PS_IRP_CONTEXT, IrpContext, sizeof(EXT2_IRP_CONTEXT));
        InterlockedIncrement(PERF_COUNTER(Ext2Global->PerfStat.Irps[IrpContext->MajorFunction].Current));
    } else {
        DEC_MEM_COUNT(PS_IRP_CONTEXT, IrpContext, sizeof(EXT2_IRP_CONTEXT));
        InterlockedIncrement(PERF_COUNTER(Ext2Global->PerfStat.Irps[IrpContext->MajorFunction].Processed));
        InterlockedDecrement(PERF_COUNTER(Ext2Global->PerfStat.Irps[IrpContext->MajorFunction].Current));
    }
#else
    UNREFERENCED_PARAMETER(_n);
    UNREFERENCED_PARAMETER(IrpContext);
#endif
}

/* one directory query as the entries are filled in (Ext2FillEntry) */
typedef struct _EXT2_FILLDIR_CONTEXT {
    PEXT2_IRP_CONTEXT       efc_irp;
    PUCHAR                  efc_buf;
    ULONG                   efc_size;
    ULONG                   efc_start;      /* bytes filled */
    ULONG                   efc_prev;       /* the last entry filled */
    NTSTATUS                efc_status;     /* why the fill stopped, if it failed */
    FILE_INFORMATION_CLASS  efc_fi;
    BOOLEAN                 efc_single;
    BOOLEAN                 efc_visible;    /* an entry Windows may see was met */
    BOOLEAN                 efc_all;        /* the pattern is "*": every name matches */
    BOOLEAN                 efc_wild;       /* the pattern has wildcards */
    UNICODE_STRING          efc_name;       /* the entry's name, one buffer per query */
} EXT2_FILLDIR_CONTEXT, *PEXT2_FILLDIR_CONTEXT;

#endif /* _EXT4_EXT4FS_COMMON_H_ */
