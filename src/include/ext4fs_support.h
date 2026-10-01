/**
 * ext4fs_support.h - debug output, pool wrappers, disk I/O, character sets, status translation, helpers.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _EXT4_EXT4FS_SUPPORT_H_
#define _EXT4_EXT4FS_SUPPORT_H_

PMDL
Ext2CreateMdl (
    IN PVOID Buffer,
    IN ULONG Length,
    IN LOCK_OPERATION Operation
);

VOID
Ext2DestroyMdl (IN PMDL Mdl);

NTSTATUS
Ext2LockUserBuffer (
    IN PIRP             Irp,
    IN ULONG            Length,
    IN LOCK_OPERATION   Operation);

PVOID
Ext2GetUserBuffer (IN PIRP Irp);

NTSTATUS
Ext2ReadWriteBlocks(
    IN PEXT2_IRP_CONTEXT IrpContext,
    IN PEXT2_VCB        Vcb,
    IN PEXT2_EXTENT     Extent,
    IN ULONG            Length
    );

NTSTATUS
Ext2ReadSync(
    IN PEXT2_VCB        Vcb,
    IN ULONGLONG        Offset,
    IN ULONG            Length,
    OUT PVOID           Buffer );

NTSTATUS
Ext2ReadDisk(
    IN PEXT2_VCB       Vcb,
    IN ULONGLONG       Offset,
    IN ULONG           Size,
    IN PVOID           Buffer,
    IN BOOLEAN         bVerify  );

NTSTATUS
Ext2WriteDiskSync(
    IN PEXT2_VCB       Vcb,
    IN ULONGLONG       Offset,
    IN ULONG           Length,
    IN PVOID           Buffer,
    IN BOOLEAN         WriteThrough );

NTSTATUS
Ext2FlushDisk(
    IN PEXT2_VCB       Vcb );

/* a mark to take once writes have completed ... */
LONG64
Ext2FlushMark(
    IN PEXT2_VCB       Vcb );

/* ... and whether a flush issued after it has completed since */
BOOLEAN
Ext2FlushedSince(
    IN PEXT2_VCB       Vcb,
    IN LONG64          Mark );

NTSTATUS
Ext2DiskIoControl (
    IN PDEVICE_OBJECT   DeviceOjbect,
    IN ULONG            IoctlCode,
    IN PVOID            InputBuffer,
    IN ULONG            InputBufferSize,
    IN OUT PVOID        OutputBuffer,
    IN OUT PULONG       OutputBufferSize );

NTSTATUS
Ext2DiskShutDown(PEXT2_VCB Vcb);

#define DL_VIT 0x00000001

#define DL_ERR 0x00000002

#define DL_DBG 0x00000004

#define DL_INF 0x00000008

#define DL_FUN 0x00000010

#define DL_LOW 0x00000020

#define DL_REN 0x00000040   /* renaming operation */

#define DL_RES 0x00000080   /* entry reference managment */

#define DL_BLK 0x00000100   /* data block allocation / free */

#define DL_CP  0x00000200   /* code pages (create, querydir) */

#define DL_EXT 0x00000400   /* mcb extents */

#define DL_MAP 0x00000800   /* retrieval points */

#define DL_JNL 0x00001000   /* dump journal operations */

#define DL_HTI 0x00002000   /* htree index */

#define DL_WRN 0x00004000   /* warning */

#define DL_BH  0x00008000   /* buffer head */

#define DL_PNP 0x00010000   /* pnp */

#define DL_IO  0x00020000   /* file i/o */

#define DL_DEFAULT (DL_ERR|DL_VIT)

#if EXT2_DEBUG
extern  ULONG DebugFilter;

VOID
__cdecl
Ext2NiPrintf(
    PCHAR DebugMessage,
    ...
);

#define DEBUG(_DL, arg)    do {if ((_DL) & DebugFilter) Ext2Printf arg;} while(0)
#define DEBUGNI(_DL, arg)  do {if ((_DL) & DebugFilter) Ext2NiPrintf arg;} while(0)

#define Ext2CompleteRequest(Irp, bPrint, PriorityBoost) \
        Ext2DbgPrintComplete(Irp, bPrint); \
        IoCompleteRequest(Irp, PriorityBoost)

#else

#define DEBUG(_DL, arg) do {if ((_DL) & DL_ERR) DbgPrint arg;} while(0)

#define Ext2CompleteRequest(Irp, bPrint, PriorityBoost) \
        IoCompleteRequest(Irp, PriorityBoost)

#endif // EXT2_DEBUG

VOID
__cdecl
Ext2Printf(
    PCHAR DebugMessage,
    ...
);

extern ULONG ProcessNameOffset;

#define Ext2GetCurrentProcessName() ( \
    (PUCHAR) PsGetCurrentProcess() + ProcessNameOffset \
)

ULONG
Ext2GetProcessNameOffset (VOID);

VOID
Ext2DbgPrintCall (
    IN PDEVICE_OBJECT   DeviceObject,
    IN PIRP             Irp );

VOID
Ext2DbgPrintComplete (
    IN PIRP Irp,
    IN BOOLEAN bPrint
);

PCSTR
Ext2NtStatusToString (IN NTSTATUS Status );

PVOID Ext2AllocatePool(
    IN POOL_TYPE PoolType,
    IN SIZE_T NumberOfBytes,
    IN ULONG Tag
);

VOID
Ext2FreePool(
    IN PVOID P,
    IN ULONG Tag
);

ULONG
Ext2Log2(ULONG Value);

/* Windows time counts 100 ns ticks since 1601-01-01, ext4 time counts
   seconds since 1970-01-01; 1601 to 1970 is 369 years plus 89 leap days */
#define TICKSPERSEC         10000000LL
#define SECSPERDAY          86400LL
#define SECS_1601_TO_1970   ((369 * 365 + 89) * SECSPERDAY)
#define TICKS_1601_TO_1970  (SECS_1601_TO_1970 * TICKSPERSEC)

LONGLONG
Ext2UnixTime(IN PLARGE_INTEGER SysTime);

LARGE_INTEGER
Ext2SystemTime(IN LONGLONG UnixSeconds);

/* superblock time stamps keep 8 more bits in a *_hi byte (ext4_update_tstamp) */
#define Ext2SetSuperTime(Sb, Field, Seconds) do {                \
        LONGLONG _s = (Seconds);                                  \
        (Sb)->Field = (__u32)_s;                                  \
        (Sb)->Field##_hi = (__u8)((ULONGLONG)_s >> 32);           \
    } while (0)

#define Ext2GetSuperTime(Sb, Field) \
    Ext2SystemTime((LONGLONG)(Sb)->Field + ((LONGLONG)(Sb)->Field##_hi << 32))

VOID
Ext2SetInodeTime(
    IN PLARGE_INTEGER SysTime,
    OUT __u32 *i_time,
    OUT __u32 *i_time_extra);

LARGE_INTEGER
Ext2GetInodeTime(
    IN __u32 i_time,
    IN __u32 i_time_extra);

ULONG
Ext2OEMToUnicodeSize(
    IN PEXT2_VCB        Vcb,
    IN PANSI_STRING     Oem
);

NTSTATUS
Ext2OEMToUnicode(
    IN PEXT2_VCB           Vcb,
    IN OUT PUNICODE_STRING Oem,
    IN POEM_STRING         Unicode
);

ULONG
Ext2UnicodeToOEMSize(
    IN PEXT2_VCB        Vcb,
    IN PUNICODE_STRING  Unicode
);

NTSTATUS
Ext2UnicodeToOEM (
    IN PEXT2_VCB        Vcb,
    IN OUT POEM_STRING  Oem,
    IN PUNICODE_STRING  Unicode
);

int Ext2LinuxError (NTSTATUS Status);

NTSTATUS Ext2WinntError(int rc);

BOOLEAN Ext2IsDot(PUNICODE_STRING name);

BOOLEAN Ext2IsDotDot(PUNICODE_STRING name);

#endif /* _EXT4_EXT4FS_SUPPORT_H_ */
