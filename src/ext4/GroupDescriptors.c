/**
 * GroupDescriptors.c - group descriptor fields, backup layout, bitmap initialisation, free counts.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "linux\ext4.h"

static inline int test_root(ext3_group_t a, ext3_group_t b)
{
    ext3_group_t num = b;

    while (a > num)
        num *= b;
    return num == a;
}

#define ext4_set_bit(n, p) set_bit((int)(n), (unsigned long *)(p))

ext4_fsblk_t ext4_block_bitmap(struct super_block *sb,
                               struct ext4_group_desc *bg)
{
    return le32_to_cpu(bg->bg_block_bitmap_lo) |
           (EXT4_DESC_SIZE(sb) >= EXT4_MIN_DESC_SIZE_64BIT ?
            (ext4_fsblk_t)le32_to_cpu(bg->bg_block_bitmap_hi) << 32 : 0);
}

ext4_fsblk_t ext4_inode_bitmap(struct super_block *sb,
                               struct ext4_group_desc *bg)
{
    return le32_to_cpu(bg->bg_inode_bitmap_lo) |
           (EXT4_DESC_SIZE(sb) >= EXT4_MIN_DESC_SIZE_64BIT ?
            (ext4_fsblk_t)le32_to_cpu(bg->bg_inode_bitmap_hi) << 32 : 0);
}

ext4_fsblk_t ext4_inode_table(struct super_block *sb,
                              struct ext4_group_desc *bg)
{
    return le32_to_cpu(bg->bg_inode_table_lo) |
           (EXT4_DESC_SIZE(sb) >= EXT4_MIN_DESC_SIZE_64BIT ?
            (ext4_fsblk_t)le32_to_cpu(bg->bg_inode_table_hi) << 32 : 0);
}

__u32 ext4_free_blks_count(struct super_block *sb,
                           struct ext4_group_desc *bg)
{
    return le16_to_cpu(bg->bg_free_blocks_count_lo) |
           (EXT4_DESC_SIZE(sb) >= EXT4_MIN_DESC_SIZE_64BIT ?
            (__u32)le16_to_cpu(bg->bg_free_blocks_count_hi) << 16 : 0);
}

__u32 ext4_free_inodes_count(struct super_block *sb,
                             struct ext4_group_desc *bg)
{
    return le16_to_cpu(bg->bg_free_inodes_count_lo) |
           (EXT4_DESC_SIZE(sb) >= EXT4_MIN_DESC_SIZE_64BIT ?
            (__u32)le16_to_cpu(bg->bg_free_inodes_count_hi) << 16 : 0);
}

__u32 ext4_used_dirs_count(struct super_block *sb,
                           struct ext4_group_desc *bg)
{
    return le16_to_cpu(bg->bg_used_dirs_count_lo) |
           (EXT4_DESC_SIZE(sb) >= EXT4_MIN_DESC_SIZE_64BIT ?
            (__u32)le16_to_cpu(bg->bg_used_dirs_count_hi) << 16 : 0);
}

__u32 ext4_itable_unused_count(struct super_block *sb,
                               struct ext4_group_desc *bg)
{
    return le16_to_cpu(bg->bg_itable_unused_lo) |
           (EXT4_DESC_SIZE(sb) >= EXT4_MIN_DESC_SIZE_64BIT ?
            (__u32)le16_to_cpu(bg->bg_itable_unused_hi) << 16 : 0);
}

void ext4_block_bitmap_set(struct super_block *sb,
                           struct ext4_group_desc *bg, ext4_fsblk_t blk)
{
    bg->bg_block_bitmap_lo = cpu_to_le32((u32)blk);
    if (EXT4_DESC_SIZE(sb) >= EXT4_MIN_DESC_SIZE_64BIT)
        bg->bg_block_bitmap_hi = cpu_to_le32(blk >> 32);
}

void ext4_inode_bitmap_set(struct super_block *sb,
                           struct ext4_group_desc *bg, ext4_fsblk_t blk)
{
    bg->bg_inode_bitmap_lo  = cpu_to_le32((u32)blk);
    if (EXT4_DESC_SIZE(sb) >= EXT4_MIN_DESC_SIZE_64BIT)
        bg->bg_inode_bitmap_hi = cpu_to_le32(blk >> 32);
}

void ext4_inode_table_set(struct super_block *sb,
                          struct ext4_group_desc *bg, ext4_fsblk_t blk)
{
    bg->bg_inode_table_lo = cpu_to_le32((u32)blk);
    if (EXT4_DESC_SIZE(sb) >= EXT4_MIN_DESC_SIZE_64BIT)
        bg->bg_inode_table_hi = cpu_to_le32(blk >> 32);
}

void ext4_free_blks_set(struct super_block *sb,
                        struct ext4_group_desc *bg, __u32 count)
{
    bg->bg_free_blocks_count_lo = cpu_to_le16((__u16)count);
    if (EXT4_DESC_SIZE(sb) >= EXT4_MIN_DESC_SIZE_64BIT)
        bg->bg_free_blocks_count_hi = cpu_to_le16(count >> 16);
}

void ext4_free_inodes_set(struct super_block *sb,
                          struct ext4_group_desc *bg, __u32 count)
{
    bg->bg_free_inodes_count_lo = cpu_to_le16((__u16)count);
    if (EXT4_DESC_SIZE(sb) >= EXT4_MIN_DESC_SIZE_64BIT)
        bg->bg_free_inodes_count_hi = cpu_to_le16(count >> 16);
}

void ext4_used_dirs_set(struct super_block *sb,
                        struct ext4_group_desc *bg, __u32 count)
{
    bg->bg_used_dirs_count_lo = cpu_to_le16((__u16)count);
    if (EXT4_DESC_SIZE(sb) >= EXT4_MIN_DESC_SIZE_64BIT)
        bg->bg_used_dirs_count_hi = cpu_to_le16(count >> 16);
}

void ext4_itable_unused_set(struct super_block *sb,
                            struct ext4_group_desc *bg, __u32 count)
{
    bg->bg_itable_unused_lo = cpu_to_le16((__u16)count);
    if (EXT4_DESC_SIZE(sb) >= EXT4_MIN_DESC_SIZE_64BIT)
        bg->bg_itable_unused_hi = cpu_to_le16(count >> 16);
}

static int ext3_group_sparse(ext3_group_t group)
{
    if (group <= 1)
        return 1;
    if (!(group & 1))
        return 0;
    return (test_root(group, 7) || test_root(group, 5) ||
            test_root(group, 3));
}

/**
 *	ext4_bg_has_super - number of blocks used by the superblock in group
 *	@sb: superblock for filesystem
 *	@group: group number to check
 *
 *	Return the number of blocks used by the superblock (primary or backup)
 *	in this group.  Currently this will be only 0 or 1.
 */
int ext3_bg_has_super(struct super_block *sb, ext3_group_t group)
{
    struct ext3_super_block *es = EXT4_SB(sb)->s_es;

    if (group == 0)
        return 1;
    if (ext4_has_feature_sparse_super2(sb))
        return group == le32_to_cpu(es->s_backup_bgs[0]) ||
               group == le32_to_cpu(es->s_backup_bgs[1]);
    if (EXT4_HAS_RO_COMPAT_FEATURE(sb,
                                   EXT3_FEATURE_RO_COMPAT_SPARSE_SUPER) &&
            !ext3_group_sparse(group))
        return 0;
    return 1;
}

static unsigned long ext4_bg_num_gdb_meta(struct super_block *sb,
        ext4_group_t group)
{
    unsigned long metagroup = group / EXT4_DESC_PER_BLOCK(sb);
    ext4_group_t first = metagroup * EXT4_DESC_PER_BLOCK(sb);
    ext4_group_t last = first + EXT4_DESC_PER_BLOCK(sb) - 1;

    if (group == first || group == first + 1 || group == last)
        return 1;
    return 0;
}

static unsigned long ext4_bg_num_gdb_nometa(struct super_block *sb,
        ext4_group_t group)
{
    if (!ext3_bg_has_super(sb, group))
        return 0;
    return ext4_has_feature_meta_bg(sb) ?
        le32_to_cpu(EXT4_SB(sb)->s_es->s_first_meta_bg) : EXT4_SB(sb)->s_gdb_count;
}

/**
 *	ext4_bg_num_gdb - number of blocks used by the group table in group
 *	@sb: superblock for filesystem
 *	@group: group number to check
 *
 *	Return the number of blocks used by the group descriptor table
 *	(primary or backup) in this group.  In the future there may be a
 *	different number of descriptor blocks in each group.
 */
unsigned long ext4_bg_num_gdb(struct super_block *sb, ext4_group_t group)
{
    unsigned long first_meta_bg =
        le32_to_cpu(EXT4_SB(sb)->s_es->s_first_meta_bg);
    unsigned long metagroup = group / EXT4_DESC_PER_BLOCK(sb);

    if (!EXT4_HAS_INCOMPAT_FEATURE(sb,EXT4_FEATURE_INCOMPAT_META_BG) ||
            metagroup < first_meta_bg)
        return ext4_bg_num_gdb_nometa(sb, group);

    return ext4_bg_num_gdb_meta(sb,group);

}

ext3_fsblk_t descriptor_loc(struct super_block *sb,
                            ext3_fsblk_t logical_sb_block, unsigned int nr)
{
    struct ext4_sb_info *sbi = EXT4_SB(sb);
    ext3_group_t bg, first_meta_bg;
    int has_super = 0;

    first_meta_bg = le32_to_cpu(sbi->s_es->s_first_meta_bg);

    if (!EXT4_HAS_INCOMPAT_FEATURE(sb, EXT4_FEATURE_INCOMPAT_META_BG) ||
            nr < first_meta_bg)
        return logical_sb_block + nr + 1;
    bg = sbi->s_desc_per_block * nr;
    if (ext3_bg_has_super(sb, bg))
        has_super = 1;
    return (has_super + ext3_group_first_block_no(sb, bg));
}

/*
 * The free inodes are managed by bitmaps.  A file system contains several
 * blocks groups.  Each group contains 1 bitmap block for blocks, 1 bitmap
 * block for inodes, N blocks for the inode table and data blocks.
 *
 * The file system contains group descriptors which are located after the
 * super block.  Each descriptor contains the number of the bitmap block and
 * the free blocks count in the block.
 */

/*
 * To avoid calling the atomic setbit hundreds or thousands of times, we only
 * need to use it within a single byte (to ensure we get endianness right).
 * We can use memset for the rest of the bitmap as there are no other users.
 */
void mark_bitmap_end(int start_bit, int end_bit, char *bitmap)
{
    int i;

    if (start_bit >= end_bit)
        return;

    DEBUG(DL_INF, ("mark end bits +%d through +%d used\n", start_bit, end_bit));
    for (i = start_bit; (unsigned)i < ((start_bit + 7) & ~7UL); i++)
        ext4_set_bit(i, bitmap);
    if (i < end_bit)
        memset(bitmap + (i >> 3), 0xff, (end_bit - i) >> 3);
}

/*
 * Initializes an uninitialized inode bitmap.
 *
 * The descriptor is not checked against its checksum here: descriptors are
 * verified when they are read from disk, and the caller may already have
 * changed this one in memory (the inode allocator updates the free count
 * and the flags before it builds the bitmaps). The old check of the
 * changed descriptor failed for every group a first inode went into, and
 * "marked the group used" - free blocks and inodes 0, bitmap all ones - so
 * each fresh group was lost to allocation and e2fsck found it wrong.
 */
unsigned ext4_init_inode_bitmap(struct super_block *sb, struct buffer_head *bh,
                                ext4_group_t block_group,
                                struct ext4_group_desc *gdp)
{
    UNREFERENCED_PARAMETER(block_group);

    mark_buffer_dirty(bh);

    memset(bh->b_data, 0, (EXT4_INODES_PER_GROUP(sb) + 7) / 8);
    mark_bitmap_end(EXT4_INODES_PER_GROUP(sb), sb->s_blocksize * 8,
                    bh->b_data);
    ext4_itable_unused_set(sb, gdp, EXT4_INODES_PER_GROUP(sb));

    return EXT4_INODES_PER_GROUP(sb);
}

/*
 * Calculate the block group number and offset, given a block number
 */
void ext4_get_group_no_and_offset(struct super_block *sb, ext4_fsblk_t blocknr,
                                  ext4_group_t *blockgrpp, ext4_grpblk_t *offsetp)
{
    struct ext4_super_block *es = EXT4_SB(sb)->s_es;
    ext4_grpblk_t offset;

    blocknr = blocknr - le32_to_cpu(es->s_first_data_block);
    offset = do_div(blocknr, EXT4_BLOCKS_PER_GROUP(sb));
    if (offsetp)
        *offsetp = offset;
    if (blockgrpp)
        *blockgrpp = (ext4_grpblk_t)blocknr;

}

static int ext4_block_in_group(struct super_block *sb, ext4_fsblk_t block,
                               ext4_group_t block_group)
{
    ext4_group_t actual_group;
    ext4_get_group_no_and_offset(sb, block, &actual_group, NULL);
    if (actual_group == block_group)
        return 1;
    return 0;
}

static int ext4_group_used_meta_blocks(struct super_block *sb,
                                       ext4_group_t block_group)
{
    ext4_fsblk_t tmp;
    struct ext4_sb_info *sbi = EXT4_SB(sb);
    /* block bitmap, inode bitmap, and inode table blocks */
    int used_blocks = sbi->s_itb_per_group + 2;

    if (EXT4_HAS_INCOMPAT_FEATURE(sb, EXT4_FEATURE_INCOMPAT_FLEX_BG)) {
        struct ext4_group_desc *gdp;
        struct buffer_head *bh = NULL;

        gdp = ext4_get_group_desc(sb, block_group, &bh);
        if (!ext4_block_in_group(sb, ext4_block_bitmap(sb, gdp),
                                 block_group))
            used_blocks--;

        if (!ext4_block_in_group(sb, ext4_inode_bitmap(sb, gdp),
                                 block_group))
            used_blocks--;

        tmp = ext4_inode_table(sb, gdp);
        for (; tmp < ext4_inode_table(sb, gdp) +
                sbi->s_itb_per_group; tmp++) {
            if (!ext4_block_in_group(sb, tmp, block_group))
                used_blocks -= 1;
        }
        if (bh)
            fini_bh(&bh);
    }
    return used_blocks;
}

/* Initializes an uninitialized block bitmap if given, and returns the
 * number of blocks free in the group. */
unsigned ext4_init_block_bitmap(struct super_block *sb, struct buffer_head *bh,
                                ext4_group_t block_group, struct ext4_group_desc *gdp)
{
    int bit, bit_max;
    unsigned free_blocks, group_blocks;
    struct ext4_sb_info *sbi = EXT4_SB(sb);

    /* no checksum check of the descriptor: see ext4_init_inode_bitmap */
    if (bh) {
        mark_buffer_dirty(bh);
        memset(bh->b_data, 0, sb->s_blocksize);
    }

    /* Check for superblock and gdt backups in this group */
    bit_max = ext3_bg_has_super(sb, block_group);

    if (!EXT4_HAS_INCOMPAT_FEATURE(sb, EXT4_FEATURE_INCOMPAT_META_BG) ||
            block_group < le32_to_cpu(sbi->s_es->s_first_meta_bg) *
            sbi->s_desc_per_block) {
        if (bit_max) {
            bit_max += ext4_bg_num_gdb(sb, block_group);
            bit_max +=
                le16_to_cpu(sbi->s_es->s_reserved_gdt_blocks);
        }
    } else { /* For META_BG_BLOCK_GROUPS */
        bit_max += ext4_bg_num_gdb(sb, block_group);
    }

    if (block_group == sbi->s_groups_count - 1) {
        /*
         * Even though mke2fs always initialize first and last group
         * if some other tool enabled the EXT4_BG_BLOCK_UNINIT we need
         * to make sure we calculate the right free blocks
         */
        group_blocks = (unsigned int)(ext3_blocks_count(sbi->s_es) -
                                      le32_to_cpu(sbi->s_es->s_first_data_block) -
                                      ((ext4_fsblk_t)EXT4_BLOCKS_PER_GROUP(sb) * (sbi->s_groups_count - 1)));
    } else {
        group_blocks = EXT4_BLOCKS_PER_GROUP(sb);
    }

    free_blocks = group_blocks - bit_max;

    if (bh) {
        ext4_fsblk_t start, tmp;
        int flex_bg = 0;

        for (bit = 0; bit < bit_max; bit++)
            ext4_set_bit(bit, bh->b_data);

        start = ext3_group_first_block_no(sb, block_group);

        if (EXT4_HAS_INCOMPAT_FEATURE(sb,
                                      EXT4_FEATURE_INCOMPAT_FLEX_BG))
            flex_bg = 1;

        /* Set bits for block and inode bitmaps, and inode table */
        tmp = ext4_block_bitmap(sb, gdp);
        if (!flex_bg || ext4_block_in_group(sb, tmp, block_group))
            ext4_set_bit(tmp - start, bh->b_data);

        tmp = ext4_inode_bitmap(sb, gdp);
        if (!flex_bg || ext4_block_in_group(sb, tmp, block_group))
            ext4_set_bit(tmp - start, bh->b_data);

        tmp = ext4_inode_table(sb, gdp);
        for (; tmp < ext4_inode_table(sb, gdp) +
                sbi->s_itb_per_group; tmp++) {
            if (!flex_bg ||
                    ext4_block_in_group(sb, tmp, block_group))
                ext4_set_bit(tmp - start, bh->b_data);
        }
        /*
         * Also if the number of blocks within the group is
         * less than the blocksize * 8 ( which is the size
         * of bitmap ), set rest of the block bitmap to 1
         */
        mark_bitmap_end(group_blocks, sb->s_blocksize * 8, bh->b_data);
    }
    return free_blocks - ext4_group_used_meta_blocks(sb, block_group);
}

/**
 * ext4_count_free_blocks() -- count filesystem free blocks
 * @sb:		superblock
 *
 * Adds up the number of free blocks from each block group.
 */
ext4_fsblk_t ext4_count_free_blocks(struct super_block *sb)
{
    ext4_fsblk_t desc_count;
    struct ext4_group_desc *gdp;
    struct buffer_head *bh = NULL;
    ext4_group_t i;
    ext4_group_t ngroups = EXT4_SB(sb)->s_groups_count;

    desc_count = 0;
    smp_rmb();
    for (i = 0; i < ngroups; i++) {
        gdp = ext4_get_group_desc(sb, i, &bh);
        if (!bh)
            continue;
        desc_count += ext4_free_blks_count(sb, gdp);
        fini_bh(&bh);
    }

    return desc_count;
}

unsigned long ext4_count_free_inodes(struct super_block *sb)
{
    unsigned long desc_count;
    struct ext4_group_desc *gdp;
    struct buffer_head *bh = NULL;
    ext4_group_t i;

    desc_count = 0;
    for (i = 0; i < EXT4_SB(sb)->s_groups_count; i++) {
        gdp = ext4_get_group_desc(sb, i, &bh);
        if (!bh)
            continue;
        desc_count += ext4_free_inodes_count(sb, gdp);
        fini_bh(&bh);
    }
    return desc_count;
}

/* Called at mount-time, super-block is locked */
unsigned long ext4_count_dirs(struct super_block * sb)
{
    struct ext4_group_desc *gdp;
    struct buffer_head *bh = NULL;
    unsigned long count = 0;
    ext4_group_t i;

    for (i = 0; i < EXT4_SB(sb)->s_groups_count; i++) {
        gdp = ext4_get_group_desc(sb, i, &bh);
        if (!bh)
            continue;
        count += ext4_used_dirs_count(sb, gdp);
        fini_bh(&bh);
    }
    return count;
}
