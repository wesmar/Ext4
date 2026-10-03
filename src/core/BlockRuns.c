/**
 * BlockRuns.c - block run lists: dirty VCB extents, MCB block maps, I/O extent chains.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "core_internal.h"

PEXT2_EXTENT
Ext2AllocateExtent ()
{
    PEXT2_EXTENT Extent;

    Extent = (PEXT2_EXTENT)ExAllocateFromNPagedLookasideList(
                 &(Ext2Global->Ext2ExtLookasideList));
    if (!Extent) {
        return NULL;
    }

    RtlZeroMemory(Extent, sizeof(EXT2_EXTENT));
    INC_MEM_COUNT(PS_EXTENT, Extent, sizeof(EXT2_EXTENT));

    return Extent;
}

VOID
Ext2FreeExtent (IN PEXT2_EXTENT Extent)
{
    ASSERT(Extent != NULL);
    ExFreeToNPagedLookasideList(&(Ext2Global->Ext2ExtLookasideList), Extent);
    DEC_MEM_COUNT(PS_EXTENT, Extent, sizeof(EXT2_EXTENT));
}

ULONG
Ext2CountExtents(IN PEXT2_EXTENT Chain)
{
    ULONG        count = 0;
    PEXT2_EXTENT List = Chain;

    while (List) {
        count += 1;
        List = List->Next;
    }

    return count;
}

VOID
Ext2JointExtents(
    IN PEXT2_EXTENT Chain,
    IN PEXT2_EXTENT Extent
)
{
    PEXT2_EXTENT List = Chain;

    while (List->Next) {
        List = List->Next;
    }

    List->Next = Extent;
}

VOID
Ext2DestroyExtentChain(IN PEXT2_EXTENT Chain)
{
    PEXT2_EXTENT Extent = NULL, List = Chain;

    while (List) {
        Extent = List->Next;
        Ext2FreeExtent(List);
        List = Extent;
    }
}

BOOLEAN
Ext2ListExtents(PEXT4_RUN_MAP  Extents)
{
    if (Extents == NULL || !Extents->Initialized) {
        return FALSE;
    }

    if (Ext4RunMapCount(Extents) != 0) {

        LONGLONG            DirtyVba;
        LONGLONG            DirtyLba;
        LONGLONG            DirtyLength;
        int                 i, n = 0;

        for (i = 0; Ext4RunMapNext(
                    Extents, i, &DirtyVba,
                    &DirtyLba, &DirtyLength); i++)  {
            if (DirtyVba > 0 && DirtyLba != -1) {
                DEBUG(DL_EXT, ("Vba:%I64xh Lba:%I64xh Len:%I64xh.\n", DirtyVba, DirtyLba, DirtyLength));
                n++;
            }
        }

        return n ? TRUE : FALSE;
    }

    return FALSE;
}

VOID
Ext2CheckExtent(
    PEXT4_RUN_MAP  Zone,
    LONGLONG    Vbn,
    LONGLONG    Lbn,
    LONGLONG    Length,
    BOOLEAN     bAdded
)
{
    UNREFERENCED_PARAMETER(Zone);
    UNREFERENCED_PARAMETER(Vbn);
    UNREFERENCED_PARAMETER(Lbn);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(bAdded);
#if EXT2_DEBUG
    LONGLONG    DirtyLbn;
    LONGLONG    DirtyLen;
    LONGLONG    RunStart;
    LONGLONG    RunLength;
    ULONG       Index;
    BOOLEAN     bFound = FALSE;

    bFound = Ext4RunMapLookup(
                 Zone,
                 Vbn,
                 &DirtyLbn,
                 &DirtyLen,
                 &RunStart,
                 &RunLength,
                 NULL );

    if (!bAdded && (!bFound || DirtyLbn == -1)) {
        return;
    }

    if ( !bFound || (DirtyLbn == -1) ||
            (DirtyLbn != Lbn) ||
            (DirtyLen < Length)) {


        for (Index = 0; TRUE; Index++) {

            if (!Ext4RunMapNext(
                        Zone,
                        Index,
                        &Vbn,
                        &Lbn,
                        &Length)) {
                break;
            }

            DEBUG(DL_EXT, ("Index = %xh Vbn = %I64xh Lbn = %I64xh Len = %I64xh\n",
                           Index, Vbn, Lbn, Length ));
        }
    }
#endif
}

VOID
Ext2ClearAllExtents(PEXT4_RUN_MAP  Zone)
{
    Ext4RunMapClear(Zone);
}

/*
 * The block maps of an inode (Extents, MetaExts) cache what is on disk and
 * Ext2InitializeZone rebuilds them. The fast truncate frees exactly the
 * blocks they list, so a map that missed an update must not be trusted:
 * both are dropped and rebuilt from disk when next needed.
 */
VOID
Ext2InvalidateZone(IN PEXT2_MCB Mcb)
{
    ClearLongFlag(Mcb->Icb->Flags, ICB_ZONE_INITED);
    Ext2ClearAllExtents(&Mcb->Icb->Extents);
    Ext2ClearAllExtents(&Mcb->Icb->MetaExts);
}

/*
 * The dirty ranges of the volume stream: the paging write of the stream
 * writes only what is registered here (see VolumeWrite.c). Registration
 * fails only when no pool is left; the caller must then fail its write.
 */
BOOLEAN
Ext2AddVcbExtent (
    IN PEXT2_VCB Vcb,
    IN LONGLONG  Vbn,
    IN LONGLONG  Length
)
{
    LONGLONG    Offset;
    BOOLEAN     rc;

    Offset = Vbn & (~((LONGLONG)Vcb->IoUnitSize - 1));
    Length = (Vbn - Offset + Length + Vcb->IoUnitSize - 1) &
             ~((LONGLONG)Vcb->IoUnitSize - 1);

    ASSERT ((Offset & (Vcb->IoUnitSize - 1)) == 0);
    ASSERT ((Length & (Vcb->IoUnitSize - 1)) == 0);

    Offset = (Offset >> Vcb->IoUnitBits) + 1;
    Length = (Length >> Vcb->IoUnitBits);

    rc = Ext4RunMapAdd(&Vcb->Extents, Offset, Offset, Length);

    DEBUG(DL_EXT, ("Ext2AddVcbExtent: Vbn=%I64xh Length=%I64xh,"
                   " rc=%d Runs=%u\n", Offset, Length, rc,
                   Ext4RunMapCount(&Vcb->Extents)));

    if (rc) {
        Ext2CheckExtent(&Vcb->Extents, Offset, Offset, Length, TRUE);
    }

    return rc;
}

/* a range that stays registered only costs one more write of clean data */
BOOLEAN
Ext2RemoveVcbExtent (
    IN PEXT2_VCB Vcb,
    IN LONGLONG  Vbn,
    IN LONGLONG  Length
)
{
    LONGLONG    Offset;
    BOOLEAN     rc;

    Offset = Vbn & (~((LONGLONG)Vcb->IoUnitSize - 1));
    Length = (Length + Vbn - Offset + Vcb->IoUnitSize - 1) & (~((LONGLONG)Vcb->IoUnitSize - 1));

    ASSERT ((Offset & (Vcb->IoUnitSize - 1)) == 0);
    ASSERT ((Length & (Vcb->IoUnitSize - 1)) == 0);

    Offset = (Offset >> Vcb->IoUnitBits) + 1;
    Length = (Length >> Vcb->IoUnitBits);

    rc = Ext4RunMapRemove(&Vcb->Extents, Offset, Length);

    DEBUG(DL_EXT, ("Ext2RemoveVcbExtent: Vbn=%I64xh Length=%I64xh Runs=%u\n",
                   Offset, Length, Ext4RunMapCount(&Vcb->Extents)));
    if (rc) {
        Ext2CheckExtent(&Vcb->Extents, Offset, 0, Length, FALSE);
    }

    return rc;
}

BOOLEAN
Ext2LookupVcbExtent (
    IN PEXT2_VCB    Vcb,
    IN LONGLONG     Vbn,
    OUT PLONGLONG   Lbn,
    OUT PLONGLONG   Length
)
{
    LONGLONG    offset;
    BOOLEAN     rc;

    offset = Vbn & (~((LONGLONG)Vcb->IoUnitSize - 1));
    ASSERT ((offset & (Vcb->IoUnitSize - 1)) == 0);
    offset = (offset >> Vcb->IoUnitBits) + 1;

    rc = Ext4RunMapLookup(
             &(Vcb->Extents),
             offset,
             Lbn,
             Length,
             NULL,
             NULL,
             NULL
         );

    if (rc) {

        if (Lbn && ((*Lbn) != -1)) {
            ASSERT((*Lbn) > 0);
            (*Lbn) = (((*Lbn) - 1) << Vcb->IoUnitBits);
            (*Lbn) += ((Vbn) & (Vcb->IoUnitSize - 1));
        }

        if (Length && *Length) {
            (*Length) <<= Vcb->IoUnitBits;
            (*Length)  -= ((Vbn) & (Vcb->IoUnitSize - 1));
        }
    }

    return rc;
}

BOOLEAN
Ext2AddMcbExtent (
    IN PEXT2_VCB Vcb,
    IN PEXT2_MCB Mcb,
    IN LONGLONG  Vbn,
    IN LONGLONG  Lbn,
    IN LONGLONG  Length
)
{
    UCHAR       Bits = (UCHAR)BLOCK_BITS;
    BOOLEAN     rc;

    ASSERT ((Vbn & ((LONGLONG)BLOCK_SIZE - 1)) == 0);
    ASSERT ((Lbn & ((LONGLONG)BLOCK_SIZE - 1)) == 0);
    ASSERT ((Length & ((LONGLONG)BLOCK_SIZE - 1)) == 0);

    Vbn = (Vbn >> Bits) + 1;
    Lbn =  (Lbn >> Bits) + 1;
    Length = (Length >> Bits);

    rc = Ext4RunMapAdd(&Mcb->Icb->Extents, Vbn, Lbn, Length);

    DEBUG(DL_EXT, ("Ext2AddMcbExtent: Vbn=%I64xh Lbn=%I64xh Length=%I64xh,"
                   " rc=%d Runs=%u\n", Vbn, Lbn, Length, rc,
                   Ext4RunMapCount(&Mcb->Icb->Extents)));

    if (rc) {
        Ext2CheckExtent(&Mcb->Icb->Extents, Vbn, Lbn, Length, TRUE);
    } else {
        Ext2InvalidateZone(Mcb);
    }

    return rc;
}

BOOLEAN
Ext2RemoveMcbExtent (
    IN PEXT2_VCB Vcb,
    IN PEXT2_MCB Mcb,
    IN LONGLONG  Vbn,
    IN LONGLONG  Length
)
{
    UCHAR       Bits = (UCHAR)BLOCK_BITS;
    BOOLEAN     rc;

    ASSERT ((Vbn & ((LONGLONG)BLOCK_SIZE - 1)) == 0);
    ASSERT ((Length & ((LONGLONG)BLOCK_SIZE - 1)) == 0);

    Vbn = (Vbn >> Bits) + 1;
    Length = (Length >> Bits);

    rc = Ext4RunMapRemove(&Mcb->Icb->Extents, Vbn, Length);

    DEBUG(DL_EXT, ("Ext2RemoveMcbExtent: Vbn=%I64xh Length=%I64xh Runs=%u\n",
                   Vbn, Length, Ext4RunMapCount(&Mcb->Icb->Extents)));
    if (rc) {
        Ext2CheckExtent(&Mcb->Icb->Extents, Vbn, 0, Length, FALSE);
    } else {
        Ext2InvalidateZone(Mcb);
    }

    return rc;
}

BOOLEAN
Ext2LookupMcbExtent (
    IN PEXT2_VCB    Vcb,
    IN PEXT2_MCB    Mcb,
    IN LONGLONG     Vbn,
    OUT PLONGLONG   Lbn,
    OUT PLONGLONG   Length
)
{
    LONGLONG    offset;
    BOOLEAN     rc;

    offset = Vbn & (~((LONGLONG)BLOCK_SIZE - 1));
    ASSERT ((offset & (BLOCK_SIZE - 1)) == 0);
    offset = (offset >> BLOCK_BITS) + 1;

    rc = Ext4RunMapLookup(
             &(Mcb->Icb->Extents),
             offset,
             Lbn,
             Length,
             NULL,
             NULL,
             NULL
         );

    if (rc) {

        if (Lbn && ((*Lbn) != -1)) {
            ASSERT((*Lbn) > 0);
            (*Lbn) = (((*Lbn) - 1) << BLOCK_BITS);
            (*Lbn) += ((Vbn) & ((LONGLONG)BLOCK_SIZE - 1));
        }

        if (Length && *Length) {
            (*Length) <<= BLOCK_BITS;
            (*Length)  -= ((Vbn) & ((LONGLONG)BLOCK_SIZE - 1));
        }
    }

    return rc;
}

BOOLEAN
Ext2AddMcbMetaExts (
    IN PEXT2_VCB Vcb,
    IN PEXT2_MCB Mcb,
    IN ULONGLONG Block,
    IN ULONG     Length
)
{
    LONGLONG    Lbn = (LONGLONG)Block + 1;
    BOOLEAN     rc;

    UNREFERENCED_PARAMETER(Vcb);

    rc = Ext4RunMapAdd(&Mcb->Icb->MetaExts, Lbn, Lbn, Length);

    DEBUG(DL_EXT, ("Ext2AddMcbMetaExts: Block: %I64xh-%xh rc=%d Runs=%u\n", Block,
                   Length, rc, Ext4RunMapCount(&Mcb->Icb->MetaExts)));

    if (rc) {
        Ext2CheckExtent(&Mcb->Icb->MetaExts, Lbn, Lbn, Length, TRUE);
    } else {
        Ext2InvalidateZone(Mcb);
    }

    return rc;
}

BOOLEAN
Ext2RemoveMcbMetaExts (
    IN PEXT2_VCB Vcb,
    IN PEXT2_MCB Mcb,
    IN ULONGLONG Block,
    IN ULONG     Length
)
{
    LONGLONG    Lbn = (LONGLONG)Block + 1;
    BOOLEAN     rc;

    UNREFERENCED_PARAMETER(Vcb);

    rc = Ext4RunMapRemove(&Mcb->Icb->MetaExts, Lbn, Length);

    DEBUG(DL_EXT, ("Ext2RemoveMcbMetaExts: Block: %I64xh-%xh Runs=%u\n", Block,
                    Length, Ext4RunMapCount(&Mcb->Icb->MetaExts)));
    if (rc) {
        Ext2CheckExtent(&Mcb->Icb->MetaExts, Lbn, 0, Length, FALSE);
    } else {
        Ext2InvalidateZone(Mcb);
    }

    return rc;
}

BOOLEAN
Ext2AddBlockExtent(
    IN PEXT2_VCB    Vcb,
    IN PEXT2_MCB    Mcb,
    IN ULONGLONG    Start,
    IN ULONGLONG    Block,
    IN ULONG        Number
)
{
    LONGLONG    Vbn = 0;
    LONGLONG    Lbn = 0;
    LONGLONG    Length = 0;

    Vbn = ((LONGLONG) Start) << BLOCK_BITS;
    Lbn = ((LONGLONG) Block) << BLOCK_BITS;
    Length = ((LONGLONG)Number << BLOCK_BITS);

    if (Mcb) {
        return Ext2AddMcbExtent(Vcb, Mcb, Vbn, Lbn, Length);

    }

    ASSERT(Start == Block);
    return Ext2AddVcbExtent(Vcb, Vbn, Length);
}

BOOLEAN
Ext2LookupBlockExtent(
    IN PEXT2_VCB    Vcb,
    IN PEXT2_MCB    Mcb,
    IN ULONG        Start,
    IN PULONGLONG   Block,
    IN PULONG       Mapped
)
{
    LONGLONG    Vbn = 0;
    LONGLONG    Lbn = 0;
    LONGLONG    Length = 0;

    BOOLEAN     rc = FALSE;

    Vbn = ((LONGLONG) Start) << BLOCK_BITS;

    if (Mcb) {
        rc = Ext2LookupMcbExtent(Vcb, Mcb, Vbn, &Lbn, &Length);
    } else {
        rc = Ext2LookupVcbExtent(Vcb, Vbn, &Lbn, &Length);
    }

    if (rc) {
        *Mapped = (ULONG)(Length >> BLOCK_BITS);
        if (Lbn != -1 && Length > 0) {
            *Block = (ULONGLONG)(Lbn >> BLOCK_BITS);
        } else {
            *Block = 0;
        }
    }

    return rc;
}

BOOLEAN
Ext2RemoveBlockExtent(
    IN PEXT2_VCB    Vcb,
    IN PEXT2_MCB    Mcb,
    IN ULONGLONG    Start,
    IN ULONG        Number
)
{
    LONGLONG    Vbn = 0;
    LONGLONG    Length = 0;
    BOOLEAN     rc;

    Vbn = ((LONGLONG) Start) << BLOCK_BITS;
    Length = ((LONGLONG)Number << BLOCK_BITS);

    if (Mcb) {
        rc = Ext2RemoveMcbExtent(Vcb, Mcb, Vbn, Length);
    } else {
        rc = Ext2RemoveVcbExtent(Vcb, Vbn, Length);
    }

    return rc;
}

NTSTATUS
Ext2InitializeZone(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_MCB            Mcb
)
{
    NTSTATUS    Status = STATUS_SUCCESS;

    ULONG       Start = 0;
    ULONG       End;
    ULONGLONG   Block;
    ULONG       Mapped;

    Ext2ClearAllExtents(&Mcb->Icb->Extents);
    Ext2ClearAllExtents(&Mcb->Icb->MetaExts);

    ASSERT(Mcb != NULL);
    End = (ULONG)((Mcb->Inode->i_size + BLOCK_SIZE - 1) >> BLOCK_BITS);

    while (Start < End) {

        Block = Mapped = 0;

        /* mapping file offset to ext2 block */
        if (INODE_HAS_EXTENT(Mcb->Inode)) {
            Status = Ext2MapExtent(
                         IrpContext,
                         Vcb,
                         Mcb,
                         Start,
                         FALSE,
                         &Block,
                         &Mapped
                     );
        } else {
            Status = Ext2MapIndirect(
                         IrpContext,
                         Vcb,
                         Mcb,
                         Start,
                         FALSE,
                         &Block,
                         &Mapped
                     );
        }

        if (!NT_SUCCESS(Status)) {
            goto errorout;
        }

        /* a block number past the volume end is not a block: an inline
           symlink keeps its target text where the map would be. Treat it as a hole. */
        if (Block >= TOTAL_BLOCKS) {
            Block = 0;
        }

        if (Block) {
            if (!Ext2AddBlockExtent(Vcb, Mcb, Start, Block, Mapped)) {
                ClearLongFlag(Mcb->Icb->Flags, ICB_ZONE_INITED);
                Ext2ClearAllExtents(&Mcb->Icb->Extents);
                Status = STATUS_INSUFFICIENT_RESOURCES;
                goto errorout;
            }
            DEBUG(DL_MAP, ("Ext2InitializeZone %wZ: Block = %I64xh Mapped = %xh\n",
                           &Mcb->FullName, Block, Mapped));
        }

        /* Mapped is total number of continous blocks or NULL blocks */
        Start += Mapped;
    }

    /* set mcb zone as initialized */
    SetLongFlag(Mcb->Icb->Flags, ICB_ZONE_INITED);

errorout:

    if (!IsZoneInited(Mcb)) {
        Ext2ClearAllExtents(&Mcb->Icb->Extents);
        Ext2ClearAllExtents(&Mcb->Icb->MetaExts);
    }

    return Status;
}

NTSTATUS
Ext2BuildExtents(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_MCB            Mcb,
    IN ULONGLONG            Offset,
    IN ULONG                Size,
    IN BOOLEAN              bAlloc,
    OUT PEXT2_EXTENT *      Chain
)
{
    ULONG       Start, End;
    ULONG       Total = 0;

    LONGLONG    Lba = 0;
    NTSTATUS    Status = STATUS_SUCCESS;

    PEXT2_EXTENT  Extent = NULL;
    PEXT2_EXTENT  List = *Chain = NULL;

    if (!IsZoneInited(Mcb)) {
        Status = Ext2InitializeZone(IrpContext, Vcb, Mcb);
    }

    if ((IrpContext && IrpContext->Irp) &&
            ((IrpContext->Irp->Flags & IRP_NOCACHE) ||
             (IrpContext->Irp->Flags & IRP_PAGING_IO))) {
        Size = (Size + SECTOR_SIZE - 1) & (~(SECTOR_SIZE - 1));
    }

    Start = (ULONG)(Offset >> BLOCK_BITS);
    End = (ULONG)((Size + Offset + BLOCK_SIZE - 1) >> BLOCK_BITS);

    if (End > (ULONG)((Mcb->Inode->i_size + BLOCK_SIZE - 1) >> BLOCK_BITS) ) {
        End = (ULONG)((Mcb->Inode->i_size + BLOCK_SIZE - 1) >> BLOCK_BITS);
    }

    while (Size > 0 && Start < End) {

        ULONG   Mapped = 0;
        ULONG   Length = 0;
        ULONGLONG Block = 0;

        BOOLEAN rc = FALSE;

        /* try to map file offset to ext2 block upon Extents cache */
        if (IsZoneInited(Mcb)) {
            rc = Ext2LookupBlockExtent(
                     Vcb,
                     Mcb,
                     Start,
                     &Block,
                     &Mapped);

            if (!rc) {
                /* we likely get a sparse file here */
                Mapped = 1;
                Block = 0;
            }
        }

        /* try to BlockMap in case failed to access Extents cache */
        if (!IsZoneInited(Mcb) || (bAlloc && Block == 0)) {

            if (bAlloc) {
                /* ask for the whole rest of this i/o at once, never more:
                   the block mapper stops at the end of the hole or of the
                   unwritten extent by itself, while a block initialized
                   beyond the i/o would never be written, i.e. would show
                   stale disk contents. A cache miss leaves Mapped at 1,
                   which used to allocate and convert block by block. */
                Mapped = End - Start;
            }

            Status = Ext2BlockMap(
                             IrpContext,
                             Vcb,
                             Mcb,
                             Start,
                             bAlloc,
                             &Block,
                             &Mapped
                     );
            if (!NT_SUCCESS(Status)) {
                break;
            }

            /* a block number past the volume end is not a block: an inline
               symlink keeps its target text where the map would be. Treat it as a hole. */
            if (Block >= TOTAL_BLOCKS) {
                Block = 0;
            }

            /* add new allocated blocks to Mcb zone */
            if (IsZoneInited(Mcb) && Block) {
                if (!Ext2AddBlockExtent(Vcb, Mcb, Start, Block, Mapped)) {
                    ClearLongFlag(Mcb->Icb->Flags, ICB_ZONE_INITED);
                    Ext2ClearAllExtents(&Mcb->Icb->Extents);
                }
            }
        }

        /* calculate i/o extent */
        Lba = ((LONGLONG)Block << BLOCK_BITS) + Offset - ((LONGLONG)Start << BLOCK_BITS);
        Length = (ULONG)(((LONGLONG)(Start + Mapped) << BLOCK_BITS) - Offset);
        if (Length > Size) {
            Length = Size;
        }

        if (0 == Length) {
            break;
        }

        Start += Mapped;
        Offset = (ULONGLONG)Start << BLOCK_BITS;

        if (Block != 0) {

            if (List && List->Lba + List->Length == Lba) {

                /* it's continuous upon previous Extent */
                List->Length += Length;

            } else {

                /* have to allocate a new Extent */
                Extent = Ext2AllocateExtent();
                if (!Extent) {
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    break;
                }

                Extent->Lba = Lba;
                Extent->Length = Length;
                Extent->Offset = Total;

                /* insert new Extent to chain */
                if (List) {
                    List->Next = Extent;
                    List = Extent;
                } else {
                    *Chain = List = Extent;
                }
            }
        }

        Total += Length;
        Size  -= Length;
    }

    return Status;
}
