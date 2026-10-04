/**
 * InodeAllocator.c - inode allocation and release, one group at a time.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * A group is chosen from the descriptors as they are - another thread may
 * be allocating meanwhile - and the choice is checked under the group's
 * InodeLock against the bitmap (core\LockStripes.c). A group found full
 * there has its count corrected from the bitmap and the choice is made
 * again; every such round zeroes one group's count, so the search ends.
 *
 * The descriptor counts are recounted from the bitmap after every change,
 * as the block allocator does: a count that had drifted is right again,
 * and the volume total moves by the exact difference.
 *
 * Inodes below s_first_ino (the root, the journal, resize, ...) are never
 * handed out or given back, whatever a bitmap says.
 */

#include "ext4fs.h"
#include "linux\ext4.h"

/* ---------------------------------------------------------------- geometry */

/* inodes in Group: a full group, or what the inode count leaves of the last one */
static __inline ULONG
Ext2GroupInodes(IN PEXT2_VCB Vcb, IN ULONG Group)
{
    ULONGLONG First = (ULONGLONG)Group * INODES_PER_GROUP;

    return (ULONG)min((ULONGLONG)INODES_PER_GROUP, (ULONGLONG)INODES_COUNT - First);
}

static __inline BOOLEAN
Ext2GroupHasInodes(IN struct super_block *sb, IN struct ext4_group_desc *gd)
{
    return (gd->bg_flags & cpu_to_le16(EXT4_BG_INODE_UNINIT)) ||
           ext4_free_inodes_count(sb, gd) > 0;
}

/* ---------------------------------------------------------------- choice */

/*
 * A directory: the first group from the hint with few directories for its
 * free inodes (or never used); else an uninitialised group or the first
 * one with more free inodes than the best so far.
 */
static ULONG
Ext2ChooseDirectoryGroup(IN PEXT2_VCB Vcb, IN ULONG GroupHint)
{
    struct super_block     *sb = &Vcb->sb;
    ULONG                   Count = Vcb->sbi.s_groups_count;
    ULONG                   Best = MAXULONG, j;
    ULONG                   BestFree = 0;

    for (j = 0; j < Count; j++) {
        ULONG                   g = (j + GroupHint) % Count;
        struct ext4_group_desc *gd = ext4_get_group_desc(sb, g, NULL);

        if (gd == NULL) {
            return MAXULONG;
        }
        if ((gd->bg_flags & cpu_to_le16(EXT4_BG_INODE_UNINIT)) ||
            ((ULONGLONG)ext4_used_dirs_count(sb, gd) << 8) < ext4_free_inodes_count(sb, gd)) {
            return g;
        }
    }

    for (j = 0; j < Count; j++) {
        struct ext4_group_desc *gd = ext4_get_group_desc(sb, j, NULL);
        ULONG                   Free;

        if (gd == NULL) {
            return MAXULONG;
        }
        if (gd->bg_flags & cpu_to_le16(EXT4_BG_INODE_UNINIT)) {
            return j;
        }
        Free = ext4_free_inodes_count(sb, gd);
        if (Best == MAXULONG) {
            if (Free > 0) {
                Best = j;
                BestFree = Free;
            }
        } else if (Free > BestFree) {
            return j;
        }
    }
    return Best;
}

/* a file: its directory's group, else a quadratic probe from it, else every group in turn */
static ULONG
Ext2ChooseFileGroup(IN PEXT2_VCB Vcb, IN ULONG GroupHint)
{
    struct super_block     *sb = &Vcb->sb;
    ULONG                   Count = Vcb->sbi.s_groups_count;
    struct ext4_group_desc *gd = ext4_get_group_desc(sb, GroupHint, NULL);
    ULONG                   g, j;

    if (gd == NULL) {
        return MAXULONG;
    }
    if (Ext2GroupHasInodes(sb, gd)) {
        return GroupHint;
    }
    for (g = GroupHint, j = 1; j < Count; j <<= 1) {
        g = (g + j) % Count;
        gd = ext4_get_group_desc(sb, g, NULL);
        if (gd == NULL) {
            return MAXULONG;
        }
        if (Ext2GroupHasInodes(sb, gd)) {
            return g;
        }
    }
    for (g = GroupHint, j = 1; j < Count; j++) {
        g = (g + 1) % Count;
        gd = ext4_get_group_desc(sb, g, NULL);
        if (gd == NULL) {
            return MAXULONG;
        }
        if (Ext2GroupHasInodes(sb, gd)) {
            return g;
        }
    }
    return MAXULONG;
}

/* ---------------------------------------------------------------- one group */

/* the inode bitmap of Group; one lazy mkfs never wrote is built and published first */
static NTSTATUS
Ext2LoadInodeBitmap(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN ULONG Group,
                    IN struct ext4_group_desc *gd, OUT struct buffer_head **Bh)
{
    struct super_block *sb = &Vcb->sb;
    ext4_fsblk_t        Where = ext4_inode_bitmap(sb, gd);

    if (gd->bg_flags & cpu_to_le16(EXT4_BG_INODE_UNINIT)) {
        *Bh = sb_getblk_zero(sb, Where);
        if (*Bh == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        ext4_init_inode_bitmap(sb, *Bh, Group, gd);
        set_buffer_uptodate(*Bh);
        Ext2ClearGroupFlag(gd, EXT4_BG_INODE_UNINIT);
        ext4_inode_bitmap_csum_set(sb, Group, gd, *Bh, EXT4_INODES_PER_GROUP(sb) / 8);
        return Ext2SaveGroup(IrpContext, Vcb, Group) ? STATUS_SUCCESS : STATUS_UNEXPECTED_IO_ERROR;
    }

    *Bh = sb_getblk(sb, Where);
    if (*Bh == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (!buffer_uptodate(*Bh)) {
        int err = bh_submit_read(*Bh);
        if (err < 0) {
            return Ext2WinntError(err);
        }
    }
    return STATUS_SUCCESS;
}

/* the descriptor's free count from the bitmap, its checksums, the volume total */
static NTSTATUS
Ext2RecountGroupInodes(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN ULONG Group,
                       IN struct ext4_group_desc *gd, IN struct buffer_head *bh)
{
    struct super_block *sb = &Vcb->sb;
    RTL_BITMAP          Bitmap;
    LONGLONG            Before = ext4_free_inodes_count(sb, gd);
    LONGLONG            After;

    RtlInitializeBitMap(&Bitmap, (PULONG)bh->b_data, Ext2GroupInodes(Vcb, Group));
    After = RtlNumberOfClearBits(&Bitmap);
    ext4_free_inodes_set(sb, gd, (__u32)After);
    ext4_inode_bitmap_csum_set(sb, Group, gd, bh, EXT4_INODES_PER_GROUP(sb) / 8);
    if (!Ext2SaveGroup(IrpContext, Vcb, Group)) {
        return STATUS_UNEXPECTED_IO_ERROR;
    }
    Ext2AdjustVcbStat(IrpContext, Vcb, 0, After - Before);
    return STATUS_SUCCESS;
}

/*
 * With group checksums the descriptor says how much of the inode table is
 * in use (bg_itable_unused counts the never used inodes at its end); an
 * inode past that mark moves it.
 */
static VOID
Ext2MarkInodeTableUsed(IN PEXT2_VCB Vcb, IN struct ext4_group_desc *gd, IN ULONG Index)
{
    struct super_block *sb = &Vcb->sb;
    ULONG               Used;

    if (!EXT4_HAS_RO_COMPAT_FEATURE(sb, EXT4_FEATURE_RO_COMPAT_GDT_CSUM) &&
        !EXT4_HAS_RO_COMPAT_FEATURE(sb, EXT4_FEATURE_RO_COMPAT_METADATA_CSUM)) {
        return;
    }
    Used = EXT3_INODES_PER_GROUP(sb) - ext4_itable_unused_count(sb, gd);
    if (Index + 1 > Used) {
        ext4_itable_unused_set(sb, gd, EXT3_INODES_PER_GROUP(sb) - 1 - Index);
    }
}

/*
 * Take an inode of Group, its InodeLock held: STATUS_SUCCESS with *Inode,
 * STATUS_DISK_FULL when the group has none (its count corrected), anything
 * else stops the allocation.
 */
static NTSTATUS
Ext2TakeInodeInLockedGroup(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN ULONG Group,
                           IN ULONG Type, OUT PULONG Inode, OUT struct buffer_head **Gb,
                           OUT struct buffer_head **Bh)
{
    struct super_block     *sb = &Vcb->sb;
    struct ext4_group_desc *gd;
    RTL_BITMAP              Bitmap;
    ULONG                   Index;
    ULONGLONG               Ino;
    NTSTATUS                Status;

    gd = ext4_get_group_desc(sb, Group, Gb);
    if (gd == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Status = Ext2LoadInodeBitmap(IrpContext, Vcb, Group, gd, Bh);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    RtlInitializeBitMap(&Bitmap, (PULONG)(*Bh)->b_data, Ext2GroupInodes(Vcb, Group));
    Index = RtlFindClearBits(&Bitmap, 1, 0);
    if (Index == MAXULONG) {
        /* the descriptor promised a free inode the bitmap does not have */
        Status = Ext2RecountGroupInodes(IrpContext, Vcb, Group, gd, *Bh);
        return NT_SUCCESS(Status) ? STATUS_DISK_FULL : Status;
    }
    Ino = (ULONGLONG)Group * INODES_PER_GROUP + Index + 1;
    if (Ino < EXT4_FIRST_INO(sb)) {
        return STATUS_DISK_CORRUPT_ERROR;       /* a reserved inode marked free */
    }

    RtlSetBits(&Bitmap, Index, 1);
    mark_buffer_dirty(*Bh);
    Ext2MarkInodeTableUsed(Vcb, gd, Index);

    /* the first inode of a group whose block bitmap lazy mkfs never wrote
       makes that bitmap real; inode before block lock */
    if (gd->bg_flags & cpu_to_le16(EXT4_BG_BLOCK_UNINIT)) {
        Status = Ext2MaterializeBlockBitmap(IrpContext, Vcb, Group);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }
    if (Type == EXT2_FT_DIR) {
        ext4_used_dirs_set(sb, gd, ext4_used_dirs_count(sb, gd) + 1);
    }
    Status = Ext2RecountGroupInodes(IrpContext, Vcb, Group, gd, *Bh);
    if (NT_SUCCESS(Status)) {
        *Inode = (ULONG)Ino;
    }
    return Status;
}

static NTSTATUS
Ext2TakeInodeInGroup(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN ULONG Group,
                     IN ULONG Type, OUT PULONG Inode)
{
    PERESOURCE          Held = Ext2LockGroupInodes(Vcb, Group);
    struct buffer_head *gb = NULL, *bh = NULL;
    NTSTATUS            Status;

    Status = Ext2TakeInodeInLockedGroup(IrpContext, Vcb, Group, Type, Inode, &gb, &bh);
    if (bh != NULL) {
        fini_bh(&bh);
    }
    if (gb != NULL) {
        fini_bh(&gb);
    }
    Ext2UnlockGroup(Held);
    return Status;
}

/* ---------------------------------------------------------------- the interface */

NTSTATUS
Ext2NewInode(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN ULONG                GroupHint,
    IN ULONG                Type,
    OUT PULONG              Inode
)
{
    ULONG       Attempt;
    NTSTATUS    Status = STATUS_DISK_FULL;

    *Inode = 0;
    Ext2JournalJoin(Vcb);       /* before the lock: see Ext2JournalJoin */
    if (IsVcbReadOnly(Vcb)) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }
    GroupHint %= Vcb->sbi.s_groups_count;

    /* a round that finds its group full corrects that group's count to
       what its bitmap says, so the next choice is another group */
    for (Attempt = 0; Attempt <= Vcb->sbi.s_groups_count && Status == STATUS_DISK_FULL; Attempt++) {
        ULONG Group = (Type == EXT2_FT_DIR) ? Ext2ChooseDirectoryGroup(Vcb, GroupHint)
                                            : Ext2ChooseFileGroup(Vcb, GroupHint);
        if (Group == MAXULONG) {
            return STATUS_DISK_FULL;
        }
        Status = Ext2TakeInodeInGroup(IrpContext, Vcb, Group, Type, Inode);
    }
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
    struct ext4_group_desc *gd;
    struct buffer_head     *gb = NULL;
    NTSTATUS                Status = STATUS_SUCCESS;
    PERESOURCE              Held;

    Ext2JournalJoin(Vcb);       /* before the lock: see Ext2JournalJoin */
    Held = Ext2LockGroupInodes(Vcb, group);

    /* the directory count lives only in the descriptor: the superblock
       has nothing to follow */
    gd = ext4_get_group_desc(sb, group, &gb);
    if (gd == NULL) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
    } else if (ext4_used_dirs_count(sb, gd) == 0) {
        Status = STATUS_DISK_CORRUPT_ERROR;     /* one more directory gone than counted */
    } else {
        ext4_used_dirs_set(sb, gd, ext4_used_dirs_count(sb, gd) - 1);
        if (!Ext2SaveGroup(IrpContext, Vcb, group)) {
            Status = STATUS_UNEXPECTED_IO_ERROR;
        }
    }

    Ext2UnlockGroup(Held);
    if (gb != NULL) {
        fini_bh(&gb);
    }
    return Status;
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
    struct ext4_group_desc *gd;
    struct buffer_head     *gb = NULL;
    struct buffer_head     *bh = NULL;
    RTL_BITMAP              Bitmap;
    ULONG                   Group, Index;
    NTSTATUS                Status;
    PERESOURCE              Held;

    DEBUG(DL_INF, ("Ext2FreeInode: inode %xh\n", Inode));

    /* reserved inodes are never freed, nonexistent ones cannot be */
    if (Inode < EXT4_FIRST_INO(sb) || Inode > INODES_COUNT) {
        return STATUS_DISK_CORRUPT_ERROR;
    }
    Group = (Inode - 1) / INODES_PER_GROUP;
    Index = (Inode - 1) % INODES_PER_GROUP;

    Ext2JournalJoin(Vcb);       /* before the lock: see Ext2JournalJoin */
    if (IsVcbReadOnly(Vcb)) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }
    Held = Ext2LockGroupInodes(Vcb, Group);

    gd = ext4_get_group_desc(sb, Group, &gb);
    if (gd == NULL) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
    } else if (gd->bg_flags & cpu_to_le16(EXT4_BG_INODE_UNINIT)) {
        Status = STATUS_DISK_CORRUPT_ERROR;     /* nothing was ever allocated there */
    } else {
        Status = Ext2LoadInodeBitmap(IrpContext, Vcb, Group, gd, &bh);
    }

    if (NT_SUCCESS(Status)) {
        RtlInitializeBitMap(&Bitmap, (PULONG)bh->b_data, Ext2GroupInodes(Vcb, Group));
        if (!RtlCheckBit(&Bitmap, Index)) {
            Status = STATUS_DISK_CORRUPT_ERROR; /* freed already */
        } else {
            RtlClearBits(&Bitmap, Index, 1);
            mark_buffer_dirty(bh);
            if (Type == EXT2_FT_DIR && ext4_used_dirs_count(sb, gd) > 0) {
                ext4_used_dirs_set(sb, gd, ext4_used_dirs_count(sb, gd) - 1);
            }
            Status = Ext2RecountGroupInodes(IrpContext, Vcb, Group, gd, bh);
        }
    }

    Ext2UnlockGroup(Held);
    if (bh != NULL) {
        fini_bh(&bh);
    }
    if (gb != NULL) {
        fini_bh(&gb);
    }
    return Status;
}
