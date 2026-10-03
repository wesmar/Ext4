/**
 * GroupCache.c - group descriptor cache: load, store, refresh, validation.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "linux\ext4.h"

VOID
Ext2DropGroupBH(IN PEXT2_VCB Vcb)
{
    struct ext4_sb_info *sbi = &Vcb->sbi;
    unsigned long i;

    if (NULL == Vcb->sbi.s_gd) {
        return;
    }

    for (i = 0; i < Vcb->sbi.s_gdb_count; i++) {
        if (Vcb->sbi.s_gd[i].bh) {
            InterlockedDecrement(&sbi->s_gd[i].bh->b_jpin);
            fini_bh(&sbi->s_gd[i].bh);
            Vcb->sbi.s_gd[i].bh = NULL;
        }
    }
}

VOID
Ext2PutGroup(IN PEXT2_VCB Vcb)
{

    if (NULL == Vcb->sbi.s_gd) {
        return;
    }

    Ext2DropGroupBH(Vcb);

    kfree(Vcb->sbi.s_gd);
    Vcb->sbi.s_gd = NULL;

    ClearLongFlag(Vcb->Flags, VCB_GD_LOADED);
}

BOOLEAN
Ext2LoadGroupBH(IN PEXT2_VCB Vcb)
{
    struct super_block  *sb = &Vcb->sb;
    struct ext4_sb_info *sbi = &Vcb->sbi;
    unsigned long i;
    BOOLEAN rc = FALSE;

    __try {

        ExAcquireResourceExclusiveLite(&Vcb->sbi.s_gd_lock, TRUE);
        ASSERT (NULL != sbi->s_gd);

        for (i = 0; i < sbi->s_gdb_count; i++) {
            ASSERT (sbi->s_gd[i].block);
            if (sbi->s_gd[i].bh)
                continue;
            sbi->s_gd[i].bh = sb_getblk(sb, sbi->s_gd[i].block);
            if (!sbi->s_gd[i].bh) {
                DEBUG(DL_ERR, ("Ext2LoadGroupBH: can't read group descriptor %d\n", i));
                __leave;
            }
            /* held for the life of the mount: not a modification in
               progress as far as the journal checkpoint is concerned */
            InterlockedIncrement(&sbi->s_gd[i].bh->b_jpin);
            sbi->s_gd[i].gd = (struct ext4_group_desc *)sbi->s_gd[i].bh->b_data;
        }

        rc = TRUE;

    } __finally {

        ExReleaseResourceLite(&Vcb->sbi.s_gd_lock);
    }

    return rc;
}

BOOLEAN
Ext2LoadGroup(IN PEXT2_VCB Vcb)
{
    struct super_block  *sb = &Vcb->sb;
    struct ext4_sb_info *sbi = &Vcb->sbi;
    ext3_fsblk_t sb_block = 1;
    unsigned long i;
    BOOLEAN rc = FALSE;

    __try {

        ExAcquireResourceExclusiveLite(&Vcb->sbi.s_gd_lock, TRUE);

        if (NULL == sbi->s_gd) {
            sbi->s_gd = kzalloc(sbi->s_gdb_count * sizeof(struct ext3_gd),
                                        GFP_KERNEL);
        }
        if (sbi->s_gd == NULL) {
            DEBUG(DL_ERR, ("Ext2LoadGroup: not enough memory.\n"));
            __leave;
        }

        if (BLOCK_SIZE != EXT3_MIN_BLOCK_SIZE) {
            sb_block = EXT4_MIN_BLOCK_SIZE / BLOCK_SIZE;
        }

        for (i = 0; i < sbi->s_gdb_count; i++) {
            sbi->s_gd[i].block =  descriptor_loc(sb, sb_block, i);
            if (!sbi->s_gd[i].block) {
                DEBUG(DL_ERR, ("Ext2LoadGroup: can't locate group descriptor %d\n", i));
                __leave;
            }
        }

        if (!Ext2LoadGroupBH(Vcb)) {
            DEBUG(DL_ERR, ("Ext2LoadGroup: Failed to load group descriptions !\n"));
            __leave;
        }

        if (!ext4_check_descriptors(sb)) {
            DEBUG(DL_ERR, ("Ext2LoadGroup: group descriptors corrupted !\n"));
            __leave;
        }

        SetLongFlag(Vcb->Flags, VCB_GD_LOADED);
        rc = TRUE;

    } __finally {

        if (!rc)
            Ext2PutGroup(Vcb);

        ExReleaseResourceLite(&Vcb->sbi.s_gd_lock);
    }

    return rc;
}

BOOLEAN
Ext2SaveGroup(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN ULONG                Group
)
{
    UNREFERENCED_PARAMETER(IrpContext);
    struct ext4_group_desc *gd;
    struct buffer_head     *gb = NULL;

    gd = ext4_get_group_desc(&Vcb->sb, Group, &gb);
    if (!gd)
        return 0;

    /* computed and stored as one step: the block and the inode half of
       the descriptor change under different locks (core\LockStripes.c) */
    Ext2SetGroupDescCsum(Vcb, Group, gd);
    mark_buffer_dirty(gb);
    fini_bh(&gb);

    return !IsVcbReadOnly(Vcb);
}

BOOLEAN
Ext2RefreshGroup(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb
)
{
    UNREFERENCED_PARAMETER(IrpContext);
    UNREFERENCED_PARAMETER(Vcb);
    return TRUE;
}

/**
 * ext4_get_group_desc() -- load group descriptor from disk
 * @sb:			super block
 * @block_group:	given block group
 * @bh:			pointer to the buffer head to store the block
 *			group descriptor
 */
struct ext4_group_desc * ext4_get_group_desc(struct super_block *sb,
                    ext4_group_t block_group, struct buffer_head **bh)
{
    struct ext4_group_desc *desc = NULL;
    struct ext4_sb_info *sbi = EXT4_SB(sb);
    PEXT2_VCB vcb = sb->s_priv;
    ext4_group_t group;
    ext4_group_t offset;

    if (bh)
        *bh = NULL;

    if (block_group >= sbi->s_groups_count) {
        ext4_error(sb, "ext4_get_group_desc",
                   "block_group >= groups_count - "
                   "block_group = %u, groups_count = %u",
                   block_group, sbi->s_groups_count);

        return NULL;
    }

    __try {

        group = block_group >> EXT4_DESC_PER_BLOCK_BITS(sb);
        offset = block_group & (EXT4_DESC_PER_BLOCK(sb) - 1);

        if (!sbi->s_gd) {
            if (!Ext2LoadGroup(vcb)) {
                __leave;
            }
        } else if ( !sbi->s_gd[group].block ||
                    !sbi->s_gd[group].bh) {
            if (!Ext2LoadGroupBH(vcb)) {
                __leave;
            }
        }

        desc = (struct ext4_group_desc *)((PCHAR)sbi->s_gd[group].gd +
                                          offset * EXT4_DESC_SIZE(sb));
        if (bh) {
            atomic_inc(&sbi->s_gd[group].bh->b_count);
            *bh = sbi->s_gd[group].bh;
        }
    } __finally {
        /* nothing to undo: the descriptor cache is never changed here */
    }

    return desc;
}

/* Called at mount-time, super-block is locked */
int ext4_check_descriptors(struct super_block *sb)
{
    struct ext4_sb_info *sbi = EXT4_SB(sb);
    ext4_fsblk_t first_block = le32_to_cpu(sbi->s_es->s_first_data_block);
    ext4_fsblk_t last_block;
    ext4_fsblk_t block_bitmap;
    ext4_fsblk_t inode_bitmap;
    ext4_fsblk_t inode_table;
    int flexbg_flag = 0;
    ext4_group_t i;

    if (EXT4_HAS_INCOMPAT_FEATURE(sb, EXT4_FEATURE_INCOMPAT_FLEX_BG))
        flexbg_flag = 1;

    DEBUG(DL_INF, ("Checking group descriptors"));

    for (i = 0; i < sbi->s_groups_count; i++) {

        struct buffer_head *bh = NULL;
        struct ext4_group_desc *gdp = ext4_get_group_desc(sb, i, &bh);

        if (!bh)
            continue;

        if (i == sbi->s_groups_count - 1 || flexbg_flag)
            last_block = ext3_blocks_count(sbi->s_es) - 1;
        else
            last_block = first_block +
                         (EXT3_BLOCKS_PER_GROUP(sb) - 1);

        block_bitmap = ext4_block_bitmap(sb, gdp);
        if (block_bitmap < first_block || block_bitmap > last_block) {
            printk(KERN_ERR "EXT4-fs: ext4_check_descriptors: "
                   "Block bitmap for group %u not in group "
                   "(block %llu)!\n", i, block_bitmap);
            __brelse(bh);
            return 0;
        }
        inode_bitmap = ext4_inode_bitmap(sb, gdp);
        if (inode_bitmap < first_block || inode_bitmap > last_block) {
            printk(KERN_ERR "EXT4-fs: ext4_check_descriptors: "
                   "Inode bitmap for group %u not in group "
                   "(block %llu)!\n", i, inode_bitmap);
            __brelse(bh);
            return 0;
        }
        inode_table = ext4_inode_table(sb, gdp);
        if (inode_table < first_block ||
                inode_table + sbi->s_itb_per_group - 1 > last_block) {
            printk(KERN_ERR "EXT4-fs: ext4_check_descriptors: "
                   "Inode table for group %u not in group "
                   "(block %llu)!\n", i, inode_table);
            __brelse(bh);
            return 0;
        }

        if (!ext4_group_desc_csum_verify(sb, i, gdp)) {
            printk(KERN_ERR "EXT4-fs: ext4_check_descriptors: "
                   "Checksum for group %u failed.\n", i);
        }

        if (!flexbg_flag)
            first_block += EXT4_BLOCKS_PER_GROUP(sb);

        __brelse(bh);
    }

    ext3_free_blocks_count_set(sbi->s_es, ext4_count_free_blocks(sb));
    sbi->s_es->s_free_inodes_count = cpu_to_le32(ext4_count_free_inodes(sb));
    return 1;
}
