/**
 * ialloc.c - inode allocator.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "linux\ext4.h"

NTSTATUS
Ext2NewInode(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN ULONG                GroupHint,
    IN ULONG                Type,
    OUT PULONG              Inode
)
{
    struct super_block     *sb = &Vcb->sb;
    PEXT2_GROUP_DESC        gd;
    struct buffer_head     *gb = NULL;
    struct buffer_head     *bh = NULL;
    ext4_fsblk_t            bitmap_blk;

    RTL_BITMAP      InodeBitmap;

    ULONG           Group, i, j;
    ULONG           Average, Length;

    ULONG           dwInode;

    NTSTATUS        Status = STATUS_DISK_FULL;

    *Inode = dwInode = 0XFFFFFFFF;

    ExAcquireResourceExclusiveLite(&Vcb->MetaInode, TRUE);

    if (GroupHint >= Vcb->sbi.s_groups_count)
        GroupHint = GroupHint % Vcb->sbi.s_groups_count;

repeat:

    if (bh)
        fini_bh(&bh);

    if (gb)
        fini_bh(&gb);

    Group = i = 0;
    gd = NULL;

    if (Type == EXT2_FT_DIR) {

        Average = Vcb->SuperBlock->s_free_inodes_count / Vcb->sbi.s_groups_count;

        for (j = 0; j < Vcb->sbi.s_groups_count; j++) {

            i = (j + GroupHint) % (Vcb->sbi.s_groups_count);
            gd = ext4_get_group_desc(sb, i, &gb);
            if (!gd) {
                    DbgBreak();
                Status = STATUS_INSUFFICIENT_RESOURCES;
                goto errorout;
            }

            if ((gd->bg_flags & cpu_to_le16(EXT4_BG_INODE_UNINIT)) ||
                (ext4_used_dirs_count(sb, gd) << 8 < 
                 ext4_free_inodes_count(sb, gd)) ) {
                Group = i + 1;
                break;
            }
            fini_bh(&gb);
        }

        if (!Group) {

            PEXT2_GROUP_DESC  desc = NULL;

            gd = NULL;

            /* get the group with the biggest vacancy */
            for (j = 0; j < Vcb->sbi.s_groups_count; j++) {

                struct buffer_head *gt = NULL;
                desc = ext4_get_group_desc(sb, j, &gt);
                if (!desc) {
                    DbgBreak();
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    goto errorout;
                }

                /* return the group if it's not initialized yet */
                if (desc->bg_flags & cpu_to_le16(EXT4_BG_INODE_UNINIT)) {
                    Group = j + 1;
                    gd = desc;

                    if (gb)
                        fini_bh(&gb);
                    gb = gt;
                    gt = NULL;
                    break;
                }

                if (!gd) {
                    if (ext4_free_inodes_count(sb, desc) > 0) {
                        Group = j + 1;
                        gd = desc;
                        if (gb)
                            fini_bh(&gb);
                        gb = gt;
                        gt = NULL;
                    }
                } else {
                    if (ext4_free_inodes_count(sb, desc) >
                        ext4_free_inodes_count(sb, gd)) {
                        Group = j + 1;
                        gd = desc;
                        if (gb)
                            fini_bh(&gb);
                        gb = gt;
                        gt = NULL;
                        break;
                    }
                }
                if (gt)
                    fini_bh(&gt);
            }
        }

    } else {

        /*
         * Try to place the inode in its parent directory (GroupHint)
         */

        gd = ext4_get_group_desc(sb, GroupHint, &gb);
        if (!gb) {
            DbgBreak();
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto errorout;
        }

        if (gd->bg_flags & cpu_to_le16(EXT4_BG_INODE_UNINIT) ||
            ext4_free_inodes_count(sb, gd)) {

            Group = GroupHint + 1;

        } else {

            /* this group is 100% cocucpied */
            fini_bh(&gb);
 
            i = GroupHint;

            /*
             * Use a quadratic hash to find a group with a free inode
             */

            for (j = 1; j < Vcb->sbi.s_groups_count; j <<= 1) {

                i = (i + j) % Vcb->sbi.s_groups_count;
                gd = ext4_get_group_desc(sb, i, &gb);
                if (!gd) {
                    DbgBreak();
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    goto errorout;
                }

                if (gd->bg_flags & cpu_to_le16(EXT4_BG_INODE_UNINIT) ||
                    ext4_free_inodes_count(sb, gd)) {
                    Group = i + 1;
                    break;
                }

                fini_bh(&gb);                
            }
        }

        if (!Group) {
            /*
             * That failed: try linear search for a free inode
             */
            i = GroupHint;
            for (j = 1; j < Vcb->sbi.s_groups_count; j++) {

                i = (i + 1) % Vcb->sbi.s_groups_count;
                gd = ext4_get_group_desc(sb, i, &gb);
                if (!gd) {
                    DbgBreak();
                    Status = STATUS_INSUFFICIENT_RESOURCES;
                    goto errorout;
                }

                if (gd->bg_flags & cpu_to_le16(EXT4_BG_INODE_UNINIT) ||
                    ext4_free_inodes_count(sb, gd)) {
                    Group = i + 1;
                    break;
                }

                fini_bh(&gb);
            }
        }
    }

    if (gd == NULL || Group == 0) {
        goto errorout;
    }

    /* finally we got the group, but is it valid ? */
    if (Group > Vcb->sbi.s_groups_count) {
        DbgBreak();
        goto errorout;
    }

    /* valid group number starts from 1, not 0 */
    Group -= 1;

    ASSERT(gd);
    bitmap_blk = ext4_inode_bitmap(sb, gd);
    /* check the block is valid or not */
    if (bitmap_blk == 0 || bitmap_blk >= TOTAL_BLOCKS) {
        DbgBreak();
        Status = STATUS_DISK_CORRUPT_ERROR;
        goto errorout;
    }

    if (gd->bg_flags & cpu_to_le16(EXT4_BG_INODE_UNINIT)) {
        bh = sb_getblk_zero(sb, bitmap_blk);
        if (!bh) {
            DbgBreak();
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto errorout;
        }
        ext4_init_inode_bitmap(sb, bh, Group, gd);
        set_buffer_uptodate(bh);
        gd->bg_flags &= cpu_to_le16(~EXT4_BG_INODE_UNINIT);
        ext4_inode_bitmap_csum_set(sb, Group, gd, bh, EXT4_INODES_PER_GROUP(sb) / 8);
        Ext2SaveGroup(IrpContext, Vcb, Group);
    } else {
        bh = sb_getblk(sb, bitmap_blk);
        if (!bh) {
            DbgBreak();
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

    if (Vcb->sbi.s_groups_count == 1) {
        Length = INODES_COUNT;
    } else {
        if (Group + 1 == Vcb->sbi.s_groups_count) {
            Length = INODES_COUNT % INODES_PER_GROUP;
            if (!Length) {
                /* INODES_COUNT is integer multiple of INODES_PER_GROUP */
                Length = INODES_PER_GROUP;
            }
        } else  {
            Length = INODES_PER_GROUP;
        }
    }

    RtlInitializeBitMap(&InodeBitmap, (PULONG)bh->b_data, Length);
    dwInode = RtlFindClearBits(&InodeBitmap, 1, 0);

    if (dwInode == 0xFFFFFFFF || dwInode >= Length) {

        RtlZeroMemory(&InodeBitmap, sizeof(RTL_BITMAP));
        if (ext4_free_inodes_count(sb, gd) > 0) {
            /* the descriptor overstated this group: the bitmap wins, and
               the phantom inodes leave the volume total too */
            LONGLONG Stale = ext4_free_inodes_count(sb, gd);

            ext4_free_inodes_set(sb, gd, 0);
            ext4_inode_bitmap_csum_set(sb, Group, gd, bh, EXT4_INODES_PER_GROUP(sb) / 8);
            Ext2SaveGroup(IrpContext, Vcb, Group);
            Ext2AdjustVcbStat(IrpContext, Vcb, 0, -Stale);
        }
        goto repeat;

    } else {

        __u32 count = 0;

        /* update unused inodes count */
        count = ext4_free_inodes_count(sb, gd) - 1;
        ext4_free_inodes_set(sb, gd, count);

        RtlSetBits(&InodeBitmap, dwInode, 1);

        /* set block bitmap dirty in cache */
        mark_buffer_dirty(bh);

        /* If we didn't allocate from within the initialized part of the inode
         * table then we need to initialize up to this inode. */
        if (EXT4_HAS_RO_COMPAT_FEATURE(sb, EXT4_FEATURE_RO_COMPAT_GDT_CSUM) ||
            EXT4_HAS_RO_COMPAT_FEATURE(sb, EXT4_FEATURE_RO_COMPAT_METADATA_CSUM)) {

            __u32 free;

            if (gd->bg_flags & cpu_to_le16(EXT4_BG_INODE_UNINIT)) {
                gd->bg_flags &= cpu_to_le16(~EXT4_BG_INODE_UNINIT);
                /* When marking the block group with
                 * ~EXT4_BG_INODE_UNINIT we don't want to depend
                 * on the value of bg_itable_unused even though
                 * mke2fs could have initialized the same for us.
                 * Instead we calculated the value below
                 */

                free = 0;
            } else {
                free = EXT3_INODES_PER_GROUP(sb) - ext4_itable_unused_count(sb, gd);
            }

            /*
             * Check the relative inode number against the last used
             * relative inode number in this group. if it is greater
             * we need to  update the bg_itable_unused count
             *
             */
            if (dwInode + 1 > free) {
                ext4_itable_unused_set(sb, gd,
                                       (EXT3_INODES_PER_GROUP(sb) - 1 - dwInode));
            }

            /* The first inode of a group whose block bitmap was never
               written (BLOCK_UNINIT, lazy mkfs) materialises that bitmap:
               once the flag is gone, readers trust the block on disk.
               Ext2NewBlock may be initialising the same bitmap for a block
               allocation right now, so the flag is tested again under
               MetaBlock, which serialises every block bitmap change. Lock
               order is MetaInode -> MetaBlock; the block allocator never
               takes MetaInode. */
            if (gd->bg_flags & cpu_to_le16(EXT4_BG_BLOCK_UNINIT)) {

                ExAcquireResourceExclusiveLite(&Vcb->MetaBlock, TRUE);

                if (gd->bg_flags & cpu_to_le16(EXT4_BG_BLOCK_UNINIT)) {

                    struct buffer_head *block_bitmap_bh;

                    block_bitmap_bh = sb_getblk_zero(sb, ext4_block_bitmap(sb, gd));
                    if (block_bitmap_bh) {
                        /* the descriptor of an uninitialised group already
                           counts its free blocks; should the bitmap built
                           here disagree, the bitmap wins and the volume
                           total moves by the difference */
                        LONGLONG Before = ext4_free_blks_count(sb, gd);

                        free = ext4_init_block_bitmap(sb, block_bitmap_bh, Group, gd);
                        ext4_block_bitmap_csum_set(sb, Group, gd, block_bitmap_bh);
                        set_buffer_uptodate(block_bitmap_bh);
                        /* journaled like any other bitmap change: without
                           it the flag below would be cleared on disk while
                           the bitmap block kept whatever mkfs left there */
                        mark_buffer_dirty(block_bitmap_bh);
                        brelse(block_bitmap_bh);
                        gd->bg_flags &= cpu_to_le16(~EXT4_BG_BLOCK_UNINIT);
                        ext4_free_blks_set(sb, gd, free);
                        Ext2SaveGroup(IrpContext, Vcb, Group);
                        Ext2AdjustVcbStat(IrpContext, Vcb, (LONGLONG)free - Before, 0);
                    }
                }

                ExReleaseResourceLite(&Vcb->MetaBlock);
            }
        }

        *Inode = dwInode + 1 + Group * INODES_PER_GROUP;

        /* update group_desc / super_block */
        if (Type == EXT2_FT_DIR) {
            ext4_used_dirs_set(sb, gd, ext4_used_dirs_count(sb, gd) + 1);
        }
        ext4_inode_bitmap_csum_set(sb, Group, gd, bh, EXT4_INODES_PER_GROUP(sb) / 8);
        Ext2SaveGroup(IrpContext, Vcb, Group);
        /* one inode taken */
        Ext2AdjustVcbStat(IrpContext, Vcb, 0, -1);
        Status = STATUS_SUCCESS;
    }

errorout:

    ExReleaseResourceLite(&Vcb->MetaInode);

    if (bh)
        fini_bh(&bh);

    if (gb)
        fini_bh(&gb);

    return Status;
}

NTSTATUS
Ext2UpdateGroupDirStat(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN ULONG                group
    )
{
    struct super_block     *sb = &Vcb->sb;
    PEXT2_GROUP_DESC        gd;
    struct buffer_head     *gb = NULL;
    NTSTATUS                status;

    ExAcquireResourceExclusiveLite(&Vcb->MetaInode, TRUE);

    /* get group desc */
    gd = ext4_get_group_desc(sb, group, &gb);
    if (!gd) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto errorout;
    }

    /* update group_desc and super_block */
    /* the directory count lives only in the descriptor: the superblock
       has nothing to follow */
    ext4_used_dirs_set(sb, gd, ext4_used_dirs_count(sb, gd) - 1);
    Ext2SaveGroup(IrpContext, Vcb, group);
    status = STATUS_SUCCESS;

errorout:

    ExReleaseResourceLite(&Vcb->MetaInode);

    if (gb)
        fini_bh(&gb);

    return status;
}

NTSTATUS
Ext2FreeInode(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN ULONG                Inode,
    IN ULONG                Type
)
{
    struct super_block     *sb = &Vcb->sb;
    PEXT2_GROUP_DESC        gd;
    struct buffer_head     *gb = NULL;
    struct buffer_head     *bh = NULL;
    ext4_fsblk_t            bitmap_blk;

    RTL_BITMAP      InodeBitmap;
    ULONG           Group;
    ULONG           Length;

    ULONG           dwIno;
    BOOLEAN         bModified = FALSE;
    LONGLONG        InodeDelta = 0;

    NTSTATUS        Status = STATUS_UNSUCCESSFUL;

    ExAcquireResourceExclusiveLite(&Vcb->MetaInode, TRUE);

    Group = (Inode - 1) / INODES_PER_GROUP;
    dwIno = (Inode - 1) % INODES_PER_GROUP;

    DEBUG(DL_INF, ( "Ext2FreeInode: Inode: %xh (Group/Off = %xh/%xh)\n",
                    Inode, Group, dwIno));

    if (Group >= Vcb->sbi.s_groups_count)  {
        DbgBreak();
        goto errorout;
    }

    gd = ext4_get_group_desc(sb, Group, &gb);
    if (!gd) {
        DbgBreak();
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto errorout;
    }

    bitmap_blk = ext4_inode_bitmap(sb, gd);
    bh = sb_getblk(sb, bitmap_blk);
    if (!bh) {
        DbgBreak();
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto errorout;
    }
    if (!buffer_uptodate(bh)) {
        int err = bh_submit_read(bh);
        if (err < 0) {
            DbgPrint("bh_submit_read error! err: %d\n", err);
            Status = Ext2WinntError(err);
            goto errorout;
        }
    }

    if (Group == Vcb->sbi.s_groups_count - 1) {

        Length = INODES_COUNT % INODES_PER_GROUP;
        if (!Length) {
            /* s_inodes_count is integer multiple of s_inodes_per_group */
            Length = INODES_PER_GROUP;
        }
    } else {
        Length = INODES_PER_GROUP;
    }

    RtlInitializeBitMap(&InodeBitmap, (PULONG)bh->b_data, Length);

    if (RtlCheckBit(&InodeBitmap, dwIno) == 0) {
        DbgBreak();
        Status = STATUS_SUCCESS;
    } else {
        RtlClearBits(&InodeBitmap, dwIno, 1);
        bModified = TRUE;
    }

    if (bModified) {
        LONGLONG Before = ext4_free_inodes_count(sb, gd);
        LONGLONG After = RtlNumberOfClearBits(&InodeBitmap);

        /* update group free inodes */
        ext4_free_inodes_set(sb, gd, (__u32)After);
        InodeDelta = After - Before;

        /* set inode block dirty and add to vcb dirty range */
        mark_buffer_dirty(bh);

        /* update group_desc and super_block */
        if (Type == EXT2_FT_DIR) {
            ext4_used_dirs_set(sb, gd,
                               ext4_used_dirs_count(sb, gd) - 1);
        }
        ext4_inode_bitmap_csum_set(sb, Group, gd, bh, EXT4_INODES_PER_GROUP(sb) / 8);
        Ext2SaveGroup(IrpContext, Vcb, Group);
        Ext2AdjustVcbStat(IrpContext, Vcb, 0, InodeDelta);
        Status = STATUS_SUCCESS;
    }

errorout:

    ExReleaseResourceLite(&Vcb->MetaInode);

    if (bh)
        fini_bh(&bh);

    if (gb)
        fini_bh(&gb);

    return Status;
}
