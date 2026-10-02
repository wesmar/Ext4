/**
 * indirect_map.c - ext2/ext3 indirect block maps: lookup and growth.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"

NTSTATUS
Ext2ExpandLast(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_MCB            Mcb,
    IN ULONG                Base,
    IN ULONG                Layer,
    IN __u32 **             Data,
    IN PULONG               Hint,
    IN __u32 *               Block,
    IN OUT PULONG           Number
)
{
    __u32 *     pData = NULL;
    ULONG       NewBlock = 0;
    ULONG       i;
    NTSTATUS    Status = STATUS_SUCCESS;

    if (Layer > 0 || IsMcbDirectory(Mcb)) {

        /* allocate buffer for new block */
        pData = (__u32 *) Ext2AllocatePool(
                    PagedPool,
                    BLOCK_SIZE,
                    EXT2_DATA_MAGIC
                );
        if (!pData) {
            DEBUG(DL_ERR, ( "Ext2ExpandBlock: failed to allocate memory for Data.\n"));
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto errorout;
        }
        RtlZeroMemory(pData, BLOCK_SIZE);
        INC_MEM_COUNT(PS_BLOCK_DATA, pData, BLOCK_SIZE);
    }

    /* allocate block from disk */
    Status = Ext2NewBlock(
                 IrpContext,
                 Vcb,
                 (Mcb->Inode->i_ino - 1) / INODES_PER_GROUP,
                 *Hint,
                 &NewBlock,
                 Number
             );
    *Block = NewBlock;

    if (!NT_SUCCESS(Status)) {
        goto errorout;
    }

    /* increase inode i_blocks */
    Mcb->Inode->i_blocks += (*Number << (BLOCK_BITS - 9));

    if (Layer == 0) {

        if (IsMcbDirectory(Mcb)) {
            /* for directory we need initialize its entry structure */
            PEXT2_DIR_ENTRY2 pEntry;
            pEntry = (PEXT2_DIR_ENTRY2) pData;
            pEntry->rec_len = (USHORT)(BLOCK_SIZE);
            ASSERT(*Number == 1);
            Ext2SaveBlock(IrpContext, Vcb, *Block, (PVOID)pData);
        }

        /* add the new run to the block map; on failure the maps are dropped */
        Ext2AddBlockExtent(Vcb, Mcb, Base, (*Block), *Number);

    } else {

        /* zero the content of all meta blocks */
        for (i = 0; i < *Number; i++) {
            Ext2SaveBlock(IrpContext, Vcb, *Block + i, (PVOID)pData);
            /* add block to meta extents */
            /* on failure the block maps are dropped and rebuilt later */
            Ext2AddMcbMetaExts(Vcb, Mcb, *Block + i, 1);
        }
    }

errorout:

    if (NT_SUCCESS(Status)) {
        *Hint = *Block + *Number;
        if (Data) {
            *Data = pData;
            ASSERT(*Number == 1);
        } else {
            if (pData) {
                Ext2FreePool(pData, EXT2_DATA_MAGIC);
                DEC_MEM_COUNT(PS_BLOCK_DATA, pData, BLOCK_SIZE);
            }
        }
    } else {
        if (pData) {
            Ext2FreePool(pData, EXT2_DATA_MAGIC);
            DEC_MEM_COUNT(PS_BLOCK_DATA, pData, BLOCK_SIZE);
        }
        if (*Block) {
            Ext2FreeBlock(IrpContext, Vcb, *Block, *Number);
            Mcb->Inode->i_blocks -= (*Number << (BLOCK_BITS - 9));
            *Block = 0;
        }
    }

    return Status;
}

NTSTATUS
Ext2GetBlock(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_MCB            Mcb,
    IN ULONG                Base,
    IN ULONG                Layer,
    IN ULONG                Start,
    IN ULONG                SizeArray,
    IN __u32 *               BlockArray,
    IN BOOLEAN              bAlloc,
    IN OUT PULONG           Hint,
    OUT PULONG              Block,
    OUT PULONG              Number
)
{
    NTSTATUS    Status = STATUS_SUCCESS;
    struct buffer_head *bh = NULL;
    __u32 *      pData = NULL;
    ULONG       Slot = 0, i = 0;
    ULONG       Unit = 1;

    if (Layer == 0) {

        *Number = 1;
        if (BlockArray[0] == 0 && bAlloc) {

            /* now allocate new block */
            Status = Ext2ExpandLast(
                         IrpContext,
                         Vcb,
                         Mcb,
                         Base,
                         Layer,
                         NULL,
                         Hint,
                         &BlockArray[0],
                         Number
                     );

            if (!NT_SUCCESS(Status)) {
                goto errorout;
            }
        } else {
            /* check the block is valid or not */
            if (BlockArray[0] >= TOTAL_BLOCKS) {
                Status = STATUS_DISK_CORRUPT_ERROR;
                goto errorout;
            }
        }

        *Block = BlockArray[0];
        for (i=1; i < SizeArray; i++) {
            if (BlockArray[i] == BlockArray[i-1] + 1) {
                *Number = *Number + 1;
            } else {
                break;
            }
        }
        *Hint = BlockArray[*Number - 1];

    } else if (Layer <= 3) {

        /* check the block is valid or not */
        if (BlockArray[0] == 0 || BlockArray[0] >= TOTAL_BLOCKS) {
            Status = STATUS_DISK_CORRUPT_ERROR;
            goto errorout;
        }

        /* add block to meta extents */
        /* on failure the block maps are dropped and rebuilt later */
        Ext2AddMcbMetaExts(Vcb, Mcb, BlockArray[0], 1);

        /* the index block goes through the bh layer so that changes to
           it are journaled */
        bh = sb_getblk(&Vcb->sb, (sector_t)BlockArray[0]);
        if (bh && !buffer_uptodate(bh) && bh_submit_read(bh) < 0) {
            fini_bh(&bh);
        }
        if (!bh) {
            DEBUG(DL_ERR, ( "Ext2GetBlock: Failed to load block: %xh ...\n",
                            BlockArray[0] ));
            Status = STATUS_CANT_WAIT;
            goto errorout;
        }
        pData = (__u32 *)bh->b_data;

        if (Layer > 1) {
            Unit = Vcb->max_blocks_per_layer[Layer - 1];
        } else {
            Unit = 1;
        }

        Slot  = Start / Unit;
        Start = Start % Unit;

        if (pData[Slot] == 0) {

            if (bAlloc) {

                /* we need allocate new block and zero all data in case
                   it's an in-direct block. Index stores the new block no. */
                ULONG   Count = 1;
                Status = Ext2ExpandLast(
                             IrpContext,
                             Vcb,
                             Mcb,
                             Base,
                             Layer,
                             NULL,
                             Hint,
                             &pData[Slot],
                             &Count
                         );

                if (!NT_SUCCESS(Status)) {
                    goto errorout;
                }

                /* refresh hint block */
                *Hint = pData[Slot];

                /* the index block is dirty */
                mark_buffer_dirty(bh);
                SetFlag(Vcb->Volume->Flags, FO_FILE_MODIFIED);

                /* save inode information here */
                Ext2SaveInode(IrpContext, Vcb, Mcb->Inode);

            } else {

                *Number = 1;

                if (Layer == 1) {
                    for (i = Slot + 1; i < BLOCK_SIZE/4; i++) {
                        if (pData[i] == 0) {
                            *Number = *Number + 1;
                        } else {
                            break;
                        }
                    }
                } else if (Layer == 2) {
                    *Number = BLOCK_SIZE/4 - Start;
                } else {
                    *Number = BLOCK_SIZE/4;
                }

                goto errorout;
            }
        }

        /* transfer to next recursion call */
        Status = Ext2GetBlock(
                     IrpContext,
                     Vcb,
                     Mcb,
                     Base,
                     Layer - 1,
                     Start,
                     BLOCK_SIZE/4 - Slot,
                     &pData[Slot],
                     bAlloc,
                     Hint,
                     Block,
                     Number
                 );

        if (!NT_SUCCESS(Status)) {
            goto errorout;
        }
    }

errorout:

    /* release the index block */
    if (bh) {
        fini_bh(&bh);
    }

    if (!NT_SUCCESS(Status)) {
        *Block = 0;
    }

    return Status;
}

NTSTATUS
Ext2ExpandBlock(
    IN PEXT2_IRP_CONTEXT IrpContext,
    IN PEXT2_VCB         Vcb,
    IN PEXT2_MCB         Mcb,
    IN ULONG             Base,
    IN ULONG             Layer,
    IN ULONG             Start,
    IN ULONG             SizeArray,
    IN __u32 *            BlockArray,
    IN PULONG            Hint,
    IN PULONG            Extra
)
{
    ULONG       i = 0;
    ULONG       j;
    ULONG       Slot;
    __u32       Block = 0;

    struct buffer_head *bh = NULL;
    __u32 *      pData = NULL;
    ULONG       Skip = 0;

    ULONG       Number;
    ULONG       Wanted;

    NTSTATUS    Status = STATUS_SUCCESS;

    if (Layer == 1) {

        /*
         * try to make all leaf block continuous to avoid fragments
         */

        Number = min(SizeArray, ((*Extra + (Start & (BLOCK_SIZE/4 - 1))) * 4 / BLOCK_SIZE));
        Wanted = 0;
        DEBUG(DL_BLK, ("Ext2ExpandBlock: SizeArray=%xh Extra=%xh Start=%xh %xh\n",
                       SizeArray, *Extra, Start, Number ));

        for (i=0; i < Number; i++) {
            if (BlockArray[i] == 0) {
                Wanted += 1;
            }
        }

        i = 0;
        while (Wanted > 0) {

            Number = Wanted;
            Status = Ext2ExpandLast(
                         IrpContext,
                         Vcb,
                         Mcb,
                         Base,
                         Layer,
                         NULL,
                         Hint,
                         &Block,
                         &Number
                     );
            if (!NT_SUCCESS(Status)) {
                goto errorout;
            }

            ASSERT(Number > 0);
            Wanted -= Number;
            while (Number) {
                if (BlockArray[i] == 0) {
                    BlockArray[i] = Block++;
                    Number--;
                }
                i++;
            }
        }

    } else if (Layer == 0) {

        /*
         * bulk allocation for inode data blocks
         */

        i = 0;

        while (*Extra && i < SizeArray) {

            Wanted = 0;
            Number = 1;

            for (j = i; j < SizeArray && j < i + *Extra; j++) {

                if (BlockArray[j] >= TOTAL_BLOCKS) {
                    BlockArray[j] = 0;
                }

                if (BlockArray[j] == 0) {
                    Wanted += 1;
                } else {
                    break;
                }
            }

            if (Wanted == 0) {

                /* add block extent into Mcb */
                ASSERT(BlockArray[i] != 0);
                if (!Ext2AddBlockExtent(Vcb, Mcb, Base + i, BlockArray[i], 1)) {
                    ClearLongFlag(Mcb->Icb->Flags, ICB_ZONE_INITED);
                    Ext2ClearAllExtents(&Mcb->Icb->Extents);
                }

            } else {

                Number = Wanted;
                Status = Ext2ExpandLast(
                             IrpContext,
                             Vcb,
                             Mcb,
                             Base + i,
                             0,
                             NULL,
                             Hint,
                             &Block,
                             &Number
                         );
                if (!NT_SUCCESS(Status)) {
                    goto errorout;
                }

                ASSERT(Number > 0);
                for (j = 0; j < Number; j++) {
                    BlockArray[i + j] = Block++;
                }
            }

            *Extra -= Number;
            i += Number;
        }

        goto errorout;
    }


    /*
     * only for meta blocks allocation
     */

    for (i = 0; *Extra && i < SizeArray; i++) {

        if (Layer <= 3) {

            if (BlockArray[i] >= TOTAL_BLOCKS) {
                BlockArray[i] = 0;
            }

            if (BlockArray[i] == 0) {
                Number = 1;
                Status = Ext2ExpandLast(
                             IrpContext,
                             Vcb,
                             Mcb,
                             Base,
                             Layer,
                             &pData,
                             Hint,
                             &BlockArray[i],
                             &Number
                         );
                if (!NT_SUCCESS(Status)) {
                    goto errorout;
                }

            } else {

                bh = sb_getblk(&Vcb->sb, (sector_t)BlockArray[i]);
                if (bh && !buffer_uptodate(bh) && bh_submit_read(bh) < 0) {
                    fini_bh(&bh);
                }
                if (!bh) {
                    DEBUG(DL_ERR, ( "Ext2ExpandInode: failed to load block %xh...\n",
                                    BlockArray[i]));
                    Status = STATUS_CANT_WAIT;
                    goto errorout;
                }
                pData = (__u32 *)bh->b_data;

                /* add block to meta extents */
                /* on failure the block maps are dropped and rebuilt later */
                Ext2AddMcbMetaExts(Vcb, Mcb,  BlockArray[i], 1);
            }

            Skip = Vcb->max_blocks_per_layer[Layer] * i;

            if (i == 0) {
                if (Layer > 1) {
                    Slot  = Start / Vcb->max_blocks_per_layer[Layer - 1];
                    Start = Start % Vcb->max_blocks_per_layer[Layer - 1];
                    Skip += Slot * Vcb->max_blocks_per_layer[Layer - 1];
                } else {
                    Slot  = Start;
                    Start = 0;
                    Skip += Slot;
                }
            } else {
                Start = 0;
                Slot  = 0;
            }

            Status = Ext2ExpandBlock(
                         IrpContext,
                         Vcb,
                         Mcb,
                         Base + Skip,
                         Layer - 1,
                         Start,
                         BLOCK_SIZE/4 - Slot,
                         &pData[Slot],
                         Hint,
                         Extra
                     );

            if (bh) {
                mark_buffer_dirty(bh);
            } else {
                Ext2SaveBlock(IrpContext, Vcb, BlockArray[i], (PVOID)pData);
            }

            if (pData) {
                if (bh) {
                    fini_bh(&bh);
                } else {
                    Ext2FreePool(pData, EXT2_DATA_MAGIC);
                    DEC_MEM_COUNT(PS_BLOCK_DATA, pData, BLOCK_SIZE);
                }
                pData = NULL;
            }

            if (!NT_SUCCESS(Status)) {
                break;
            }
        }
    }

errorout:

    return Status;
}

NTSTATUS
Ext2MapIndirect(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_MCB            Mcb,
    IN ULONG                Index,
    IN BOOLEAN              bAlloc,
    OUT PULONG              pBlock,
    OUT PULONG              Number
)
{
    ULONG   Layer;
    ULONG   Slot;

    ULONG   Base = Index;

    NTSTATUS Status = STATUS_SUCCESS;

    *pBlock = 0;
    *Number = 0;

    for (Layer = 0; Layer < EXT2_BLOCK_TYPES; Layer++) {

        if (Index < Vcb->max_blocks_per_layer[Layer]) {

            ULONG   dwRet = 0, dwHint = 0, dwArray = 0;
            __u32   dwBlk = 0;

            Slot = (Layer==0) ? (Index):(Layer + EXT2_NDIR_BLOCKS - 1);
            dwBlk = Mcb->Inode->i_block[Slot];

            if (dwBlk == 0) {

                if (!bAlloc) {

                    *Number = 1;
                    goto errorout;

                } else {

                    if (Slot) {
                        dwHint = Mcb->Inode->i_block[Slot - 1];
                    }

                    /* allocate and zero block if necessary */
                    *Number = 1;
                    Status = Ext2ExpandLast(
                                 IrpContext,
                                 Vcb,
                                 Mcb,
                                 Base,
                                 Layer,
                                 NULL,
                                 &dwHint,
                                 &dwBlk,
                                 Number
                             );

                    if (!NT_SUCCESS(Status)) {
                        goto errorout;
                    }

                    /* save the it into inode*/
                    Mcb->Inode->i_block[Slot] = dwBlk;

                    /* save the inode */
                    if (!Ext2SaveInode(IrpContext, Vcb, Mcb->Inode)) {
                        Status = STATUS_UNSUCCESSFUL;
                        goto errorout;
                    }
                }
            }

            if (Layer == 0) 
                dwArray = Vcb->max_blocks_per_layer[Layer] - Index;
            else
                dwArray = 1;

            /* querying block number of the index-th file block */
            Status = Ext2GetBlock(
                         IrpContext,
                         Vcb,
                         Mcb,
                         Base,
                         Layer,
                         Index,
                         dwArray,
                        &Mcb->Inode->i_block[Slot],
                         bAlloc,
                         &dwHint,
                         &dwRet,
                         Number
                     );

            if (NT_SUCCESS(Status)) {
                *pBlock = dwRet;
            }

            break;
        }

        Index -= Vcb->max_blocks_per_layer[Layer];
    }

errorout:

    return Status;
}

NTSTATUS
Ext2ExpandIndirect(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB         Vcb,
    PEXT2_MCB         Mcb,
    ULONG             Start,
    ULONG             End,
    PLARGE_INTEGER    Size
)
{
    NTSTATUS Status = STATUS_SUCCESS;

    ULONG    Layer = 0;
    ULONG    Extra = 0;
    ULONG    Hint = 0;
    ULONG    Slot = 0;
    ULONG    Base = 0;

    Extra = End - Start;

	/* exceeds the biggest file size (indirect) */
    if (End > Vcb->max_data_blocks) {
        return STATUS_INVALID_PARAMETER;
    }

    for (Layer = 0; Layer < EXT2_BLOCK_TYPES && Extra; Layer++) {

        if (Start >= Vcb->max_blocks_per_layer[Layer]) {

            Base  += Vcb->max_blocks_per_layer[Layer];
            Start -= Vcb->max_blocks_per_layer[Layer];

        } else {

            /* get the slot in i_block array */
            if (Layer == 0) {
                Base = Slot = Start;
            } else {
                Slot = Layer + EXT2_NDIR_BLOCKS - 1;
            }

            /* set block hint to avoid fragments */
            if (Hint == 0) {
                if (Mcb->Inode->i_block[Slot] != 0) {
                    Hint = Mcb->Inode->i_block[Slot];
                } else if (Slot > 1) {
                    Hint = Mcb->Inode->i_block[Slot-1];
                }
            }

            /* now expand this slot */
            Status = Ext2ExpandBlock(
                         IrpContext,
                         Vcb,
                         Mcb,
                         Base,
                         Layer,
                         Start,
                         (Layer == 0) ? (Vcb->max_blocks_per_layer[Layer] - Start) : 1,
                         &Mcb->Inode->i_block[Slot],
                         &Hint,
                         &Extra
                     );
            if (!NT_SUCCESS(Status)) {
                break;
            }

            Start = 0;
            if (Layer == 0) {
                Base = 0;
            }
            Base += Vcb->max_blocks_per_layer[Layer];
        }
    }

    Size->QuadPart = ((LONGLONG)(End - Extra)) << BLOCK_BITS;

    /* save inode whatever it succeeds to expand or not */
    Ext2SaveInode(IrpContext, Vcb, Mcb->Inode);

    return Status;
}
