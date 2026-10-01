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
Ext2ListExtents(PLARGE_MCB  Extents);

VOID
Ext2CheckExtent(
    PLARGE_MCB  Zone,
    LONGLONG    Vbn,
    LONGLONG    Lbn,
    LONGLONG    Length,
    BOOLEAN     bAdded
);

VOID
Ext2ClearAllExtents(PLARGE_MCB  Zone);

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
    IN ULONG     Block,
    IN ULONG     Length
);

BOOLEAN
Ext2RemoveMcbMetaExts (
    IN PEXT2_VCB Vcb,
    IN PEXT2_MCB Mcb,
    IN ULONG     Block,
    IN ULONG     Length
);

BOOLEAN
Ext2AddBlockExtent(
    IN PEXT2_VCB    Vcb,
    IN PEXT2_MCB    Mcb,
    IN ULONG        Start,
    IN ULONG        Block,
    IN ULONG        Number
);

BOOLEAN
Ext2LookupBlockExtent(
    IN PEXT2_VCB    Vcb,
    IN PEXT2_MCB    Mcb,
    IN ULONG        Start,
    IN PULONG       Block,
    IN PULONG       Mapped
);

BOOLEAN
Ext2RemoveBlockExtent(
    IN PEXT2_VCB    Vcb,
    IN PEXT2_MCB    Mcb,
    IN ULONG        Start,
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

PEXT2_MCB
Ext2SearchMcbWithoutLock(
    PEXT2_MCB           Parent,
    PUNICODE_STRING     FileName
);

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
Ext2LinkHeadMcb(PEXT2_VCB Vcb, PEXT2_MCB Mcb);

VOID
Ext2UnlinkMcb(PEXT2_VCB Vcb, PEXT2_MCB Mcb);

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
