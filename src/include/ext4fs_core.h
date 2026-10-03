/**
 * ext4fs_core.h - in-memory objects: IRP contexts, FCB/CCB, MCB name cache, ICB table, extent lists, reapers.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _EXT4_EXT4FS_CORE_H_
#define _EXT4_EXT4FS_CORE_H_

VOID
Ext2FcbReaperThread(
    PVOID   Context
);

VOID
Ext2McbReaperThread(
    PVOID   Context
);

VOID
Ext2bhReaperThread(
    PVOID   Context
);

PEXT2_IRP_CONTEXT
Ext2AllocateIrpContext (IN PDEVICE_OBJECT   DeviceObject,
                        IN PIRP             Irp );

VOID
Ext2FreeIrpContext (IN PEXT2_IRP_CONTEXT IrpContext);

PEXT2_FCB
Ext2AllocateFcb (
    IN PEXT2_VCB   Vcb,
    IN PEXT2_MCB   Mcb
);

PEXT2_FCB
Ext2ReferDcb (
    IN PEXT2_VCB   Vcb,
    IN PEXT2_MCB   Mcb
);

VOID
Ext2UnlinkFcb(IN PEXT2_FCB Fcb);

VOID
Ext2FreeFcb (IN PEXT2_FCB Fcb);

VOID
Ext2DropIdleFcbs (IN PEXT2_VCB Vcb);

VOID
Ext2ReleaseFcb (IN PEXT2_FCB Fcb);

VOID
Ext2InsertFcb(PEXT2_VCB Vcb, PEXT2_FCB Fcb);

PEXT2_CCB
Ext2AllocateCcb (ULONG Flags, PEXT2_MCB Mcb, PEXT2_MCB SymLink);

VOID
Ext2FreeMcb (
    IN PEXT2_VCB        Vcb,
    IN PEXT2_MCB        Mcb
);

VOID
Ext2InitializeIcbTable (IN PEXT2_VCB Vcb);

BOOLEAN
Ext2AttachIcb (
    IN PEXT2_VCB        Vcb,
    IN PEXT2_MCB        Mcb,
    IN ULONG            Ino
);

VOID
Ext2DetachIcb (
    IN PEXT2_VCB        Vcb,
    IN PEXT2_MCB        Mcb
);

VOID
Ext2UnhashIcb (
    IN PEXT2_VCB        Vcb,
    IN PEXT2_ICB        Icb
);

BOOLEAN
Ext2PeekCachedInode (
    IN PEXT2_VCB        Vcb,
    IN ULONG            Ino,
    OUT struct inode   *Inode
);

VOID
Ext2NameUnlinked (
    IN PEXT2_VCB        Vcb,
    IN PEXT2_MCB        Mcb
);

BOOLEAN
Ext2DropLink (
    IN PEXT2_VCB        Vcb,
    IN struct inode    *Inode
);

PEXT2_MCB
Ext2NextPendingName (
    IN PEXT2_VCB        Vcb,
    IN PEXT2_ICB        Icb
);

PEXT2_MCB
Ext2LookupMcbByInode (
    IN PEXT2_VCB        Vcb,
    IN ULONG            Ino
);

VOID
Ext2FreeCcb (IN PEXT2_VCB Vcb, IN PEXT2_CCB Ccb);

PEXT2_INODE
Ext2AllocateInode (PEXT2_VCB  Vcb);

VOID
Ext2DestroyInode (IN PEXT2_VCB Vcb, IN PEXT2_INODE inode);

PEXT2_EXTENT
Ext2AllocateExtent();

VOID
Ext2FreeExtent (IN PEXT2_EXTENT Extent);

ULONG
Ext2CountExtents(IN PEXT2_EXTENT Chain);

VOID
Ext2JointExtents(
    IN PEXT2_EXTENT Chain,
    IN PEXT2_EXTENT Extent
);

VOID
Ext2DestroyExtentChain(IN PEXT2_EXTENT Chain);

NTSTATUS
Ext2BuildExtents(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_MCB            Mcb,
    IN ULONGLONG            Offset,
    IN ULONG                Size,
    IN BOOLEAN              bAlloc,
    OUT PEXT2_EXTENT *      Chain
);

BOOLEAN
Ext2ListExtents(PEXT4_RUN_MAP  Extents);

VOID
Ext2CheckExtent(
    PEXT4_RUN_MAP  Zone,
    LONGLONG    Vbn,
    LONGLONG    Lbn,
    LONGLONG    Length,
    BOOLEAN     bAdded
);

VOID
Ext2ClearAllExtents(PEXT4_RUN_MAP  Zone);

VOID
Ext2InvalidateZone(IN PEXT2_MCB Mcb);

BOOLEAN
Ext2AddVcbExtent (
    IN PEXT2_VCB Vcb,
    IN LONGLONG  Vbn,
    IN LONGLONG  Length
);

BOOLEAN
Ext2RemoveVcbExtent (
    IN PEXT2_VCB Vcb,
    IN LONGLONG  Vbn,
    IN LONGLONG  Length
);

BOOLEAN
Ext2LookupVcbExtent (
    IN PEXT2_VCB    Vcb,
    IN LONGLONG     Vbn,
    OUT PLONGLONG   Lbn,
    OUT PLONGLONG   Length
);

BOOLEAN
Ext2AddMcbExtent (
    IN PEXT2_VCB Vcb,
    IN PEXT2_MCB Mcb,
    IN LONGLONG  Vbn,
    IN LONGLONG  Lbn,
    IN LONGLONG  Length
);

BOOLEAN
Ext2RemoveMcbExtent (
    IN PEXT2_VCB Vcb,
    IN PEXT2_MCB Mcb,
    IN LONGLONG  Vbn,
    IN LONGLONG  Length
);

BOOLEAN
Ext2LookupMcbExtent (
    IN PEXT2_VCB    Vcb,
    IN PEXT2_MCB    Mcb,
    IN LONGLONG     Vbn,
    OUT PLONGLONG   Lbn,
    OUT PLONGLONG   Length
);

BOOLEAN
Ext2AddMcbMetaExts (
    IN PEXT2_VCB Vcb,
    IN PEXT2_MCB Mcb,
    IN ULONGLONG Block,
    IN ULONG     Length
);

BOOLEAN
Ext2RemoveMcbMetaExts (
    IN PEXT2_VCB Vcb,
    IN PEXT2_MCB Mcb,
    IN ULONGLONG Block,
    IN ULONG     Length
);

BOOLEAN
Ext2AddBlockExtent(
    IN PEXT2_VCB    Vcb,
    IN PEXT2_MCB    Mcb,
    IN ULONGLONG    Start,
    IN ULONGLONG    Block,
    IN ULONG        Number
);

BOOLEAN
Ext2LookupBlockExtent(
    IN PEXT2_VCB    Vcb,
    IN PEXT2_MCB    Mcb,
    IN ULONG        Start,
    IN PULONGLONG   Block,
    IN PULONG       Mapped
);

BOOLEAN
Ext2RemoveBlockExtent(
    IN PEXT2_VCB    Vcb,
    IN PEXT2_MCB    Mcb,
    IN ULONGLONG    Start,
    IN ULONG        Number
);

NTSTATUS
Ext2InitializeZone(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_MCB            Mcb
);

BOOLEAN
Ext2BuildName(
    IN OUT PUNICODE_STRING  Target,
    IN PUNICODE_STRING      File,
    IN PUNICODE_STRING      Parent
);

PEXT2_MCB
Ext2AllocateMcb (
    IN PEXT2_VCB        Vcb,
    IN PUNICODE_STRING  FileName,
    IN PUNICODE_STRING  Parent,
    IN ULONG            FileAttr
);

PEXT2_MCB
Ext2SearchMcb(
    PEXT2_VCB           Vcb,
    PEXT2_MCB           Parent,
    PUNICODE_STRING     FileName
);

ULONG
Ext2HashMcbName(IN PUNICODE_STRING Name);

ULONG
Ext2RoundUpPow2(IN ULONG Value);

ULONG
Ext2StripeCount(IN ULONG Items);

PEXT2_LOCK_STRIPE
Ext2AllocateStripes(IN ULONG Count, OUT PVOID *Pool);

VOID
Ext2FreeStripes(IN PEXT2_LOCK_STRIPE Stripes, IN ULONG Count, IN PVOID Pool);

NTSTATUS
Ext2InitializeGroupLocks(IN PEXT2_VCB Vcb);

VOID
Ext2DestroyGroupLocks(IN PEXT2_VCB Vcb);

PERESOURCE
Ext2LockGroupBlocks(IN PEXT2_VCB Vcb, IN ULONG Group);

PERESOURCE
Ext2LockGroupInodes(IN PEXT2_VCB Vcb, IN ULONG Group);

VOID
Ext2SetGroupDescCsum(IN PEXT2_VCB Vcb, IN ULONG Group, IN struct ext4_group_desc *Desc);

/* bg_flags is the one descriptor field both halves change: atomically */
VOID
Ext2ClearGroupFlag(IN struct ext4_group_desc *Desc, IN USHORT Flag);

VOID
Ext2UnlockGroup(IN PERESOURCE Lock);

NTSTATUS
Ext2InitializeNameCache(IN PEXT2_VCB Vcb);

VOID
Ext2DestroyNameCache(IN PEXT2_VCB Vcb);

PERESOURCE
Ext2AcquireNameStripe(
    IN PEXT2_VCB        Vcb,
    IN PEXT2_MCB        Parent,
    IN ULONG            NameHash,
    IN BOOLEAN          Exclusive
);

VOID
Ext2ReleaseNameStripe(IN PERESOURCE Stripe);

PEXT2_MCB
Ext2FindMcbLocked(
    IN PEXT2_VCB        Vcb,
    IN PEXT2_MCB        Parent,
    IN PUNICODE_STRING  FileName,
    IN ULONG            Hash
);

PEXT2_MCB
Ext2ReferParent(IN PEXT2_VCB Vcb, IN PEXT2_MCB Mcb);

PEXT2_MCB
Ext2ReferDirectory(IN PEXT2_VCB Vcb, IN PEXT2_MCB Dir);

PEXT2_MCB
Ext2ReferLinkTarget(IN PEXT2_VCB Vcb, IN PEXT2_MCB Mcb);

BOOLEAN
Ext2IsLinkDangling(IN PEXT2_VCB Vcb, IN PEXT2_MCB Mcb);

VOID
Ext2InsertMcb(
    PEXT2_VCB Vcb,
    PEXT2_MCB Parent,
    PEXT2_MCB Child
);

BOOLEAN
Ext2RemoveMcb(
    PEXT2_VCB Vcb,
    PEXT2_MCB Mcb
);

VOID
Ext2CleanupAllMcbs(
    PEXT2_VCB Vcb
);

VOID
Ext2LinkTailMcb(PEXT2_VCB Vcb, PEXT2_MCB Mcb);

VOID
Ext2MoveMcbToHead(PEXT2_VCB Vcb, PEXT2_MCB Mcb);

ULONG
Ext2FirstUnusedMcb(
    PEXT2_VCB   Vcb,
    BOOLEAN     Wait,
    ULONG       Number,
    PLIST_ENTRY Reaped
);

VOID
Ext2ReaperThread(
    PVOID   Context
);

NTSTATUS
Ext2StartReaper(PEXT2_REAPER, EXT2_REAPER_RELEASE);

VOID
Ext2StopReaper(PEXT2_REAPER Reaper);

VOID
Ext2ReaperKick(IN PEXT2_REAPER Reaper, IN BOOLEAN Now);

VOID
Ext2FcbGone(IN PEXT2_VCB Vcb);

VOID
Ext2McbReaperWake(VOID);

#endif /* _EXT4_EXT4FS_CORE_H_ */
