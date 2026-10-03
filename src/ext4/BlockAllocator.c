/**
 * BlockAllocator.c - block allocator.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "linux\ext4.h"

NTSTATUS
Ext2NewBlock(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN ULONG                GroupHint,
    IN ULONGLONG            BlockHint,
    OUT PULONGLONG          Block,
    IN OUT PULONG           Number,
    IN ULONGLONG            BlockLimit
)
{
    struct super_block      *sb = &Vcb->sb;
    PEXT2_GROUP_DESC        gd;
    struct buffer_head     *gb = NULL;
    struct buffer_head     *bh = NULL;
    ext4_fsblk_t            bitmap_blk;

    RTL_BITMAP              BlockBitmap;

    ULONG                   Group = 0;
    ULONG                   Index = 0xFFFFFFFF;
    ULONG                   dwHint = 0;
    ULONG                   Count = 0;
    ULONG                   Length = 0;
    ULONG                   GroupLength = 0;
    ULONG                   SearchGroups;
    ULONGLONG               Limit = min(TOTAL_BLOCKS, BlockLimit);

    NTSTATUS                Status = STATUS_DISK_FULL;
    PERESOURCE              Held = NULL;    /* the lock of Group (Ext2LockGroup) */

    *Block = 0;

    if (!*Number || Limit <= EXT2_FIRST_DATA_BLOCK) {
        return STATUS_INVALID_PARAMETER;
    }
    /* The caller's on-disk pointer width bounds the search, not the volume. */
    SearchGroups = (ULONG)((Limit - EXT2_FIRST_DATA_BLOCK - 1) / BLOCKS_PER_GROUP + 1);

    Ext2JournalJoin(Vcb);       /* before the lock: see Ext2JournalJoin */

    /* validate the hint group and hint block */
    if (GroupHint >= SearchGroups) {
        GroupHint = SearchGroups - 1;
    }

    /* The goal is the block after a file's last extent. When that extent
       ends at the last block of the volume, the goal lies past the end: it
       was taken as it was, its group did not exist, and the allocation
       failed with STATUS_INSUFFICIENT_RESOURCES although other groups had
       room - such a file could not grow at all. A goal outside the volume
       now leaves the search to start at the hint group. */
    if (BlockHint >= EXT2_FIRST_DATA_BLOCK && BlockHint < Limit) {
        GroupHint = (ULONG)((BlockHint - EXT2_FIRST_DATA_BLOCK) / BLOCKS_PER_GROUP);
        dwHint = (ULONG)((BlockHint - EXT2_FIRST_DATA_BLOCK) % BLOCKS_PER_GROUP);
    }

    Group = GroupHint;

Again:

    if (bh)
        fini_bh(&bh);

    if (gb)
        fini_bh(&gb);

    /* one group at a time, under its own lock */
    if (Held) {
        Ext2UnlockGroup(Held);
    }
    Held = Ext2LockGroupBlocks(Vcb, Group);

    gd = ext4_get_group_desc(sb, Group, &gb);
    if (!gd) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto errorout;
    }

    bitmap_blk = ext4_block_bitmap(sb, gd);

    if (gd->bg_flags & cpu_to_le16(EXT4_BG_BLOCK_UNINIT)) {
        bh = sb_getblk_zero(sb, bitmap_blk);
        if (!bh) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto errorout;
        }
        /* lazy mkfs never wrote this bitmap: build it from the group
           layout and journal it at once. Clearing BLOCK_UNINIT publishes
           the on-disk block as the truth, whether or not the allocation
           below ends up in this group. */
        ext4_init_block_bitmap(sb, bh, Group, gd);
        set_buffer_uptodate(bh);
        Ext2ClearGroupFlag(gd, EXT4_BG_BLOCK_UNINIT);
        ext4_block_bitmap_csum_set(sb, Group, gd, bh);
        mark_buffer_dirty(bh);
        Ext2SaveGroup(IrpContext, Vcb, Group);
    } else {
        bh = sb_getblk(sb, bitmap_blk);
        if (!bh) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto errorout;
        }
    }

    if (!buffer_uptodate(bh)) {
	    int err = bh_submit_read(bh);
	    if (err < 0) {
		    DbgPrint("bh_submit_read error! err: %d\n", err);
		    Status = Ext2WinntError(err);
		    goto errorout;
	    }
    }

    if (ext4_free_blks_count(sb, gd)) {

        GroupLength = (ULONG)min((ULONGLONG)BLOCKS_PER_GROUP,
            TOTAL_BLOCKS - EXT2_FIRST_DATA_BLOCK - (ULONGLONG)Group * BLOCKS_PER_GROUP);
        Length = (ULONG)min((ULONGLONG)GroupLength,
            Limit - EXT2_FIRST_DATA_BLOCK - (ULONGLONG)Group * BLOCKS_PER_GROUP);

        /* initialize bitmap buffer */
        RtlInitializeBitMap(&BlockBitmap, (PULONG)bh->b_data, Length);

        /* try to find a clear bit range */
        Index = RtlFindClearBits(&BlockBitmap, *Number, dwHint);

        /* no run of the full length from the goal on: take the longest run at or after the goal, else anywhere in the group */
        if (Index == 0xFFFFFFFF) {

            /* search clear bits from the hint block */
            Count = RtlFindNextForwardRunClear(&BlockBitmap, dwHint, &Index);
            if (dwHint != 0 && Count == 0) {
                /* search clear bits from the very beginning */
                Count = RtlFindNextForwardRunClear(&BlockBitmap, 0, &Index);
            }

            if (Count == 0) {

                LONGLONG Before = ext4_free_blks_count(sb, gd);
                LONGLONG After;

                /* A pointer-width boundary may expose only part of this group. */
                BlockBitmap.SizeOfBitMap = GroupLength;
                After = RtlNumberOfClearBits(&BlockBitmap);
                ext4_free_blks_set(sb, gd, (__u32)After);
                ext4_block_bitmap_csum_set(sb, Group, gd, bh);
                Ext2SaveGroup(IrpContext, Vcb, Group);
                Ext2AdjustVcbStat(IrpContext, Vcb, After - Before, 0);

                dwHint = 0;
                Group = (Group + 1) % SearchGroups;
                if (Group == GroupHint) {
                    goto errorout;
                }
                goto Again;

            } else {

                /* we got free blocks */
                if (Count <= *Number) {
                    *Number = Count;
                }
            }
        }

    } else {

        /* try next group */
        dwHint = 0;
        Group = (Group + 1) % SearchGroups;
        if (Group != GroupHint) {
            goto Again;
        }

        Index = 0xFFFFFFFF;
    }

    if (Index < Length) {

        LONGLONG Before = ext4_free_blks_count(sb, gd);
        LONGLONG After;
        ULONGLONG Candidate = Index + EXT2_FIRST_DATA_BLOCK +
                              (ULONGLONG)Group * BLOCKS_PER_GROUP;

        if (*Number > Length - Index || Candidate >= Limit ||
            *Number > Limit - Candidate) {
            Status = STATUS_DISK_CORRUPT_ERROR;
            goto errorout;
        }
        /* A corrupt bitmap must not hand out the group's own metadata. */
        if ((ext4_block_bitmap(sb, gd) >= Candidate &&
             ext4_block_bitmap(sb, gd) - Candidate < *Number) ||
            (ext4_inode_bitmap(sb, gd) >= Candidate &&
             ext4_inode_bitmap(sb, gd) - Candidate < *Number) ||
            (Candidate < ext4_inode_table(sb, gd) + Vcb->sbi.s_itb_per_group &&
             ext4_inode_table(sb, gd) < Candidate + *Number)) {
            Status = STATUS_DISK_CORRUPT_ERROR;
            goto errorout;
        }

        /* mark block bits as allocated */
        RtlSetBits(&BlockBitmap, Index, *Number);

        /* set block bitmap dirty in cache */
        mark_buffer_dirty(bh);

        /* The group count is taken from the bitmap, not decremented: one
           4 KiB popcount per allocation, and a descriptor that had drifted
           is right again afterwards. The volume total moves by exactly
           the change made here. */
        BlockBitmap.SizeOfBitMap = GroupLength;
        After = RtlNumberOfClearBits(&BlockBitmap);
        ext4_free_blks_set(sb, gd, (__u32)After);
        ext4_block_bitmap_csum_set(sb, Group, gd, bh);
        Ext2SaveGroup(IrpContext, Vcb, Group);
        Ext2AdjustVcbStat(IrpContext, Vcb, After - Before, 0);

        /* validate the new allocated block number */
        *Block = Candidate;

        /* Always remove dirty MCB to prevent Volume's lazy writing.
           Metadata blocks will be re-added during modifications.*/
        if (!Ext2RemoveBlockExtent(Vcb, NULL, *Block, *Number)) {
            Ext2RemoveBlockExtent(Vcb, NULL, *Block, *Number);  /* once more: out of pool */
        }

        DEBUG(DL_INF, ("Ext2NewBlock:  Block %I64xh - %I64x allocated.\n",
                       *Block, *Block + *Number));
        Status = STATUS_SUCCESS;
    }

errorout:

    if (Held) {
        Ext2UnlockGroup(Held);
    }

    if (bh)
        fini_bh(&bh);

    if (gb)
        fini_bh(&gb);

    return Status;
}

NTSTATUS
Ext2FreeBlock(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN ULONGLONG            Block,
    IN ULONG                Number
)
{
    struct super_block     *sb = &Vcb->sb;
    PEXT2_GROUP_DESC        gd;
    struct buffer_head     *gb = NULL;
    struct buffer_head     *bh = NULL;
    ext4_fsblk_t            bitmap_blk;

    RTL_BITMAP      BlockBitmap;

    ULONG           Group;
    ULONG           Index;
    ULONG           Length;
    ULONG           Count;
    LONGLONG        FreedDelta = 0;

    NTSTATUS        Status = STATUS_UNSUCCESSFUL;
    PERESOURCE      Held = NULL;    /* the lock of Group (Ext2LockGroup) */

    Ext2JournalJoin(Vcb);       /* before the lock: see Ext2JournalJoin */

    DEBUG(DL_INF, ("Ext2FreeBlock: Block %I64xh - %I64x to be freed.\n",
                   Block, Block + Number));

    if (!Number || Block < EXT2_FIRST_DATA_BLOCK || Block >= TOTAL_BLOCKS ||
        (ULONGLONG)Number > TOTAL_BLOCKS - Block) {
        Status = STATUS_INVALID_PARAMETER;
        goto errorout;
    }

    /* any journaled copy of these blocks must be revoked before they can
       be reused (invariant I3) */
    Ext2JournalRevokeBlocks(Vcb, Block, Number);

    Group = (ULONG)((Block - EXT2_FIRST_DATA_BLOCK) / BLOCKS_PER_GROUP);
    Index = (ULONG)((Block - EXT2_FIRST_DATA_BLOCK) % BLOCKS_PER_GROUP);

Again:

    if (gb)
        fini_bh(&gb);
    if (bh)
        fini_bh(&bh);

    if ( Block < EXT2_FIRST_DATA_BLOCK ||
         Block >= TOTAL_BLOCKS ||
         Group >= Vcb->sbi.s_groups_count) {

        Status = STATUS_SUCCESS;

    } else  {

        /* one group at a time, under its own lock */
        if (Held) {
            Ext2UnlockGroup(Held);
        }
        Held = Ext2LockGroupBlocks(Vcb, Group);

        gd = ext4_get_group_desc(sb, Group, &gb);
        if (!gd) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto errorout;
        }
        bitmap_blk = ext4_block_bitmap(sb, gd);

        /* check the block is valid or not */
        if (bitmap_blk >= TOTAL_BLOCKS) {
            Status = STATUS_DISK_CORRUPT_ERROR;
            goto errorout;
        }

        if (Group == Vcb->sbi.s_groups_count - 1) {

            Length = (ULONG)(TOTAL_BLOCKS - EXT2_FIRST_DATA_BLOCK -
                             (ULONGLONG)Group * BLOCKS_PER_GROUP);

        } else {
            Length = BLOCKS_PER_GROUP;
        }

        /* read the bitmap through the bh layer so the change is journaled */
        bh = sb_getblk(sb, bitmap_blk);
        if (!bh) {
            DEBUG(DL_ERR, ("Ext2FreeBlock: failed to load bitmap block %I64xh.\n",
                           bitmap_blk));
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto errorout;
        }
        if (!buffer_uptodate(bh)) {
            int err = bh_submit_read(bh);
            if (err < 0) {
                DEBUG(DL_ERR, ("Ext2FreeBlock: reading bitmap block %I64xh failed: %d\n",
                               bitmap_blk, err));
                Status = STATUS_UNEXPECTED_IO_ERROR;
                goto errorout;
            }
        }

        /* return the run to the group; a run crossing groups is finished by the loop below */
        RtlInitializeBitMap(&BlockBitmap, (PULONG)bh->b_data, Length);
        Count = min(Length - Index, Number);
        RtlClearBits(&BlockBitmap, Index, Count);

        /* update group description table; a block freed twice changes
           nothing, so the total follows the bitmap, not Count */
        {
            LONGLONG Before = ext4_free_blks_count(sb, gd);
            LONGLONG After = RtlNumberOfClearBits(&BlockBitmap);

            ext4_free_blks_set(sb, gd, (__u32)After);
            FreedDelta = After - Before;
        }

        ext4_block_bitmap_csum_set(sb, Group, gd, bh);

        /* the bitmap block is dirty */
        mark_buffer_dirty(bh);
        fini_bh(&bh);
        Ext2SaveGroup(IrpContext, Vcb, Group);

        /* remove dirty MCB to prevent Volume's lazy writing. */
        if (!Ext2RemoveBlockExtent(Vcb, NULL, Block, Count)) {
            Ext2RemoveBlockExtent(Vcb, NULL, Block, Count);     /* once more: out of pool */
        }

        /* the superblock total follows this group's change */
        Ext2AdjustVcbStat(IrpContext, Vcb, FreedDelta, 0);

        /* try next group to clear all remaining */
        Number -= Count;
        if (Number) {
            Group += 1;
            if (Group < Vcb->sbi.s_groups_count) {
                Index = 0;
                Block += Count;
                goto Again;
            } else {
                DEBUG(DL_ERR, ("Ext2FreeBlock: block number beyonds max group.\n"));
                goto errorout;
            }
        }
    }

    Status = STATUS_SUCCESS;

errorout:

    if (gb)
        fini_bh(&gb);
    if (bh)
        fini_bh(&bh);

    if (Held) {
        Ext2UnlockGroup(Held);
    }

    return Status;
}
