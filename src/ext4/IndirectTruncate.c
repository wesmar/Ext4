/**
 * IndirectTruncate.c - ext2/ext3 indirect block maps: truncation.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"

BOOLEAN
Ext2IsBlockEmpty(__u32 * BlockArray, ULONG SizeArray)
{
    ULONG i = 0;
    for (i=0; i < SizeArray; i++) {
        if (BlockArray[i]) {
            break;
        }
    }
    return (i == SizeArray);
}

NTSTATUS
Ext2TruncateBlock(
    IN PEXT2_IRP_CONTEXT IrpContext,
    IN PEXT2_VCB         Vcb,
    IN PEXT2_MCB         Mcb,
    IN ULONG             Base,
    IN ULONG             Start,
    IN ULONG             Layer,
    IN ULONG             SizeArray,
    IN __u32 *            BlockArray,
    IN PULONG            Extra
)
{
    NTSTATUS    Status = STATUS_SUCCESS;
    ULONG       i = 0;
    ULONG       Slot = 0;
    ULONG       Skip = 0;

    struct buffer_head *bh = NULL;
    __u32 *      pData = NULL;

    ASSERT(Mcb != NULL);

    for (i = 0; i < SizeArray; i++) {

        if (Layer == 0) {

            ULONG   Number = 1;

            while (Extra &&  SizeArray > i + 1 && Number < *Extra) {

                if (BlockArray[SizeArray - i - 1] ==
                    BlockArray[SizeArray - i - 2] + 1) {

                    BlockArray[SizeArray - i - 1] = 0;
                    Number++;
                    SizeArray--;

                } else {
                    break;
                }
            }

            if (BlockArray[SizeArray - i - 1]) {

                Status = Ext2FreeBlock(IrpContext, Vcb, BlockArray[SizeArray - i - 1], Number);
                if (NT_SUCCESS(Status)) {
                    ASSERT(Mcb->Inode->i_blocks >= (Number << (BLOCK_BITS - 9)));
                    if (Mcb->Inode->i_blocks < (Number << (BLOCK_BITS - 9))) {
                        Mcb->Inode->i_blocks = 0;
                    } else {
                        Mcb->Inode->i_blocks -= (Number << (BLOCK_BITS - 9));
                    }
                    BlockArray[SizeArray - i - 1] = 0;
                }
            }

            if (Extra) {

                /* dec blocks count */
                ASSERT(*Extra >= Number);
                *Extra = *Extra - Number;

                /* remove block mapping frm Mcb Extents */
                if (!Ext2RemoveBlockExtent(Vcb, Mcb, Base + SizeArray - 1 - i, Number)) {
                    ClearLongFlag(Mcb->Icb->Flags, ICB_ZONE_INITED);
                    Ext2ClearAllExtents(&Mcb->Icb->Extents);
                }
            }

        } else {

            ASSERT(Layer <= 3);

            if (BlockArray[SizeArray - i - 1] >= TOTAL_BLOCKS) {
                BlockArray[SizeArray - i - 1] = 0;
            }

            if (i == 0) {
                if (Layer > 1) {
                    Slot  = Start / Vcb->max_blocks_per_layer[Layer - 1];
                    Start = Start % Vcb->max_blocks_per_layer[Layer - 1];
                } else {
                    Slot  = Start;
                    Start = (BLOCK_SIZE / 4) - 1;
                }
            } else {
                Slot = Start = (BLOCK_SIZE / 4) - 1;
            }

            Skip = (SizeArray - i - 1) * Vcb->max_blocks_per_layer[Layer];

            if (BlockArray[SizeArray - i - 1]) {

                bh = sb_getblk(&Vcb->sb, (sector_t)BlockArray[SizeArray - i - 1]);
                if (bh && !buffer_uptodate(bh) && bh_submit_read(bh) < 0) {
                    fini_bh(&bh);
                }
                if (!bh) {
                    DEBUG(DL_ERR, ( "Ext2TruncateBlock: failed to load block %xh ...\n",
                                    BlockArray[SizeArray - i - 1]));
                    Status = STATUS_CANT_WAIT;
                    goto errorout;
                }
                pData = (__u32 *)bh->b_data;

                Status = Ext2TruncateBlock(
                             IrpContext,
                             Vcb,
                             Mcb,
                             Base + Skip,
                             Start,
                             Layer - 1,
                             Slot + 1,
                             &pData[0],
                             Extra
                         );

                if (!NT_SUCCESS(Status)) {
                    break;
                }

                mark_buffer_dirty(bh);

                if (*Extra || Ext2IsBlockEmpty(pData, BLOCK_SIZE/4)) {

                    Ext2TruncateBlock(
                                 IrpContext,
                                 Vcb,
                                 Mcb,
                                 Base + Skip,    /* base */
                                 0,              /* start */
                                 0,              /* layer */
                                 1,
                                 &BlockArray[SizeArray - i - 1],
                                 NULL
                             );

                    /* on failure the block maps are dropped and rebuilt later */
                    Ext2RemoveMcbMetaExts(Vcb, Mcb, BlockArray[SizeArray - i - 1], 1);
                }

                if (pData) {
                    fini_bh(&bh);
                    pData = NULL;
                }

            } else {

                if (Layer > 1) {
                    if (*Extra > Slot * Vcb->max_blocks_per_layer[Layer - 1] + Start + 1) {
                        *Extra -= (Slot * Vcb->max_blocks_per_layer[Layer - 1] + Start + 1);
                    } else {
                        *Extra  = 0;
                    }
                } else {
                    if (*Extra > Slot + 1) {
                        *Extra -= (Slot + 1);
                    } else {
                        *Extra  = 0;
                    }
                }

                if (!Ext2RemoveBlockExtent(Vcb, Mcb, Base + Skip, (Start + 1))) {
                    ClearLongFlag(Mcb->Icb->Flags, ICB_ZONE_INITED);
                    Ext2ClearAllExtents(&Mcb->Icb->Extents);
                }
            }
        }

        if (Extra && *Extra == 0) {
            break;
        }
    }

errorout:

    if (bh) {
        fini_bh(&bh);
    }

    return Status;
}

NTSTATUS
Ext2TruncateIndirectFast(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB         Vcb,
    PEXT2_MCB         Mcb
    )
{
    LONGLONG            Vba;
    LONGLONG            Lba;
    LONGLONG            Length;
    NTSTATUS            Status = STATUS_SUCCESS;
    int                 i;

    /* try to load all indirect blocks if mcb zone is not initialized */
    if (!IsZoneInited(Mcb)) {
        Status = Ext2InitializeZone(IrpContext, Vcb, Mcb);
        if (!NT_SUCCESS(Status)) {
            ClearLongFlag(Mcb->Icb->Flags, ICB_ZONE_INITED);
            goto errorout;
        }
    }

    ASSERT (IsZoneInited(Mcb));

    /* delete all data blocks here */
    if (Ext4RunMapCount(&Mcb->Icb->Extents) != 0) {
        for (i = 0; Ext4RunMapNext(&Mcb->Icb->Extents, i, &Vba, &Lba, &Length); i++) {
            /* ignore the non-existing runs */
            if (-1 == Lba || Vba == 0 || Length <= 0)
                continue;
            /* now do data block free */
            Ext2FreeBlock(IrpContext, Vcb, (ULONG)(Lba - 1), (ULONG)Length);
        }
    }

    /* delete all meta blocks here */
    if (Ext4RunMapCount(&Mcb->Icb->MetaExts) != 0) {
        for (i = 0; Ext4RunMapNext(&Mcb->Icb->MetaExts, i, &Vba, &Lba, &Length); i++) {
            /* ignore the non-existing runs */
            if (-1 == Lba || Vba == 0 || Length <= 0)
                continue;
            /* now do meta block free */
            Ext2FreeBlock(IrpContext, Vcb, (ULONG)(Lba - 1), (ULONG)Length);
        }
    }

    /* clear data and meta extents */
    Ext2ClearAllExtents(&Mcb->Icb->Extents);
    Ext2ClearAllExtents(&Mcb->Icb->MetaExts);
    ClearLongFlag(Mcb->Icb->Flags, ICB_ZONE_INITED);

    /* clear inode blocks & sizes */
    Mcb->Inode->i_blocks = 0;
    Mcb->Inode->i_size = 0;
    memset(&Mcb->Inode->i_block[0], 0, sizeof(__u32) * 15);

    /* the caller will do inode save */

errorout:

    return Status;
}

NTSTATUS
Ext2TruncateIndirect(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB         Vcb,
    PEXT2_MCB         Mcb,
    PLARGE_INTEGER    Size
)
{
    NTSTATUS Status = STATUS_SUCCESS;

    ULONG    Layer = 0;

    ULONG    Extra = 0;
    ULONG    Wanted = 0;
    ULONG    End;
    ULONG    Base;

    ULONG    SizeArray = 0;
    __u32 *   BlockArray = NULL;

    /* translate file size to block */
    End = Base = Vcb->max_data_blocks;
    Wanted = (ULONG)((Size->QuadPart + BLOCK_SIZE - 1) >> BLOCK_BITS);

    /* do fast deletion here */
    if (Wanted == 0) {
        Status = Ext2TruncateIndirectFast(IrpContext, Vcb, Mcb);
        if (NT_SUCCESS(Status))
            goto errorout;
    }

    /* calculate blocks to be freed */
    Extra = End - Wanted;

    for (Layer = EXT2_BLOCK_TYPES; Layer > 0 && Extra; Layer--) {

        if (Vcb->max_blocks_per_layer[Layer - 1] == 0) {
            continue;
        }

        Base -= Vcb->max_blocks_per_layer[Layer - 1];

        if (Layer - 1 == 0) {
            BlockArray = &Mcb->Inode->i_block[0];
            SizeArray = End;
            ASSERT(End == EXT2_NDIR_BLOCKS && Base == 0);
        } else {
            BlockArray = &Mcb->Inode->i_block[EXT2_NDIR_BLOCKS - 1 + Layer - 1];
            SizeArray = 1;
        }

        Status = Ext2TruncateBlock(
                     IrpContext,
                     Vcb,
                     Mcb,
                     Base,
                     End - Base - 1,
                     Layer - 1,
                     SizeArray,
                     BlockArray,
                     &Extra
                 );
        if (!NT_SUCCESS(Status)) {
            break;
        }

        End = Base;
    }

errorout:

    if (!NT_SUCCESS(Status)) {
        Size->QuadPart += ((ULONGLONG)Extra << BLOCK_BITS);
    }

    /* save inode */
    if (Mcb->Inode->i_size > (loff_t)(Size->QuadPart))
        Mcb->Inode->i_size = (loff_t)(Size->QuadPart);
    Ext2SaveInode(IrpContext, Vcb, Mcb->Inode);

    return Status;
}
