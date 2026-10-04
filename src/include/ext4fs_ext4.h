/**
 * ext4fs_ext4.h - on-disk format: superblock, groups, inodes, allocators, directories, extents, xattrs, checksums.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _EXT4_EXT4FS_EXT4_H_
#define _EXT4_EXT4FS_EXT4_H_

NTSTATUS
Ext2MapExtent(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_MCB            Mcb,
    IN ULONG                Index,
    IN BOOLEAN              Alloc,
    OUT PULONGLONG          Block,
    OUT PULONG              Number
    );

NTSTATUS
Ext2ExpandExtent(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB         Vcb,
    PEXT2_MCB         Mcb,
    ULONG             Start,
    ULONG             End,
    PLARGE_INTEGER    Size
    );

NTSTATUS
Ext2TruncateExtent(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB         Vcb,
    PEXT2_MCB         Mcb,
    PLARGE_INTEGER    Size
    );

static inline ext3_fsblk_t ext3_blocks_count(struct ext3_super_block *es)
{
    return ((ext3_fsblk_t)le32_to_cpu(es->s_blocks_count_hi) << 32) |
           le32_to_cpu(es->s_blocks_count);
}

static inline ext3_fsblk_t ext3_r_blocks_count(struct ext3_super_block *es)
{
    return ((ext3_fsblk_t)le32_to_cpu(es->s_r_blocks_count_hi) << 32) |
           le32_to_cpu(es->s_r_blocks_count);
}

static inline ext3_fsblk_t ext3_free_blocks_count(struct ext3_super_block *es)
{
    return ((ext3_fsblk_t)le32_to_cpu(es->s_free_blocks_count_hi) << 32) |
           le32_to_cpu(es->s_free_blocks_count);
}

static inline void ext3_blocks_count_set(struct ext3_super_block *es,
        ext3_fsblk_t blk)
{
    es->s_blocks_count = cpu_to_le32((u32)blk);
    es->s_blocks_count_hi = cpu_to_le32(blk >> 32);
}

static inline void ext3_free_blocks_count_set(struct ext3_super_block *es,
        ext3_fsblk_t blk)
{
    es->s_free_blocks_count = cpu_to_le32((u32)blk);
    es->s_free_blocks_count_hi = cpu_to_le32(blk >> 32);
}

static inline void ext3_r_blocks_count_set(struct ext3_super_block *es,
        ext3_fsblk_t blk)
{
    es->s_r_blocks_count = cpu_to_le32((u32)blk);
    es->s_r_blocks_count_hi = cpu_to_le32(blk >> 32);
}

blkcnt_t ext3_inode_blocks(struct ext3_inode *raw_inode,
                           struct inode *inode);

int ext3_inode_blocks_set(struct ext3_inode *raw_inode,
                          struct inode * inode);

ext4_fsblk_t ext4_block_bitmap(struct super_block *sb,
                               struct ext4_group_desc *bg);

ext4_fsblk_t ext4_inode_bitmap(struct super_block *sb,
                               struct ext4_group_desc *bg);

ext4_fsblk_t ext4_inode_table(struct super_block *sb,
                              struct ext4_group_desc *bg);

__u32 ext4_free_blks_count(struct super_block *sb,
                           struct ext4_group_desc *bg);

__u32 ext4_free_inodes_count(struct super_block *sb,
                             struct ext4_group_desc *bg);

__u32 ext4_used_dirs_count(struct super_block *sb,
                           struct ext4_group_desc *bg);

__u32 ext4_itable_unused_count(struct super_block *sb,
                               struct ext4_group_desc *bg);

void ext4_block_bitmap_set(struct super_block *sb,
                           struct ext4_group_desc *bg, ext4_fsblk_t blk);

void ext4_inode_bitmap_set(struct super_block *sb,
                           struct ext4_group_desc *bg, ext4_fsblk_t blk);

void ext4_inode_table_set(struct super_block *sb,
                          struct ext4_group_desc *bg, ext4_fsblk_t blk);

void ext4_free_blks_set(struct super_block *sb,
                        struct ext4_group_desc *bg, __u32 count);

void ext4_free_inodes_set(struct super_block *sb,
                          struct ext4_group_desc *bg, __u32 count);

void ext4_used_dirs_set(struct super_block *sb,
                        struct ext4_group_desc *bg, __u32 count);

void ext4_itable_unused_set(struct super_block *sb,
                            struct ext4_group_desc *bg, __u32 count);

int ext3_bg_has_super(struct super_block *sb, ext3_group_t group);

unsigned long ext4_bg_num_gdb(struct super_block *sb, ext4_group_t group);

ULONG Ext2GroupReservedBlocks(struct super_block *sb, ext4_group_t block_group);

/* Inode written; a failure becomes *Status unless that already reports one */
VOID Ext2SaveInodeStatus(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb,
                         IN struct inode *Inode, IN OUT PNTSTATUS Status);

/* no data block, nor block of its map: at most the external xattr block */
BOOLEAN Ext2InodeHoldsNoData(struct inode *inode);

/* the metadata of the whole volume as sorted block ranges (MetadataMap.c):
   superblock and descriptor copies, bitmaps and inode tables of every
   group, wherever flex_bg placed them; built once at mount */
NTSTATUS Ext2BuildMetadataMap(IN PEXT2_VCB Vcb);
VOID     Ext2FreeMetadataMap(IN PEXT2_VCB Vcb);
BOOLEAN  Ext2MetadataOverlaps(IN PEXT2_VCB Vcb, IN ULONGLONG Block, IN ULONGLONG Count);

/* the reach of an on-disk block pointer: an allocation never goes past it */
#define EXT2_EXTENT_BLOCK_LIMIT     (1ULL << 48)    /* ext4 extent: 32 + 16 bits */
#define EXT2_INDIRECT_BLOCK_LIMIT   (1ULL << 32)    /* ext2/ext3 block map: 32 bits */

/* Ext2NewBlock: the new blocks' contents go through the journal (directory,
   index, xattr and indirect blocks). Such a block may be one a transaction
   not yet committed freed: its new contents reach it only through a
   checkpoint after the commit, so a crash before leaves the old owner's
   data in place. File data is written in place and must wait. */
#define EXT2_ALLOC_JOURNALED        0x00000001

/* the block bitmap of a lazily initialised group written out (BlockAllocator.c) */
NTSTATUS
Ext2MaterializeBlockBitmap(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN ULONG Group);

/* blocks for Inode, counted in its i_blocks; and given back */
NTSTATUS
Ext2AllocateInodeBlocks(IN PEXT2_IRP_CONTEXT IrpContext, IN struct inode *Inode,
                        IN ULONGLONG Goal, IN ULONG Flags, OUT PULONGLONG Block,
                        IN OUT PULONG Count);
NTSTATUS
Ext2ReleaseInodeBlocks(IN PEXT2_IRP_CONTEXT IrpContext, IN struct inode *Inode,
                       IN ULONGLONG Block, IN ULONG Count);

/* blocks freed by transactions not yet committed (FreedBlocks.c), all under
   the BlockLock of the group's stripe: at most two transactions are
   uncommitted at a time, one slot each */
#define EXT2_FREED_SLOTS        2
#define EXT2_FREED_COMMIT_SHARE 8       /* ask for a commit at once when more than
                                           1/8 of the free space waits for one */

/* the overlay of Group for the allocator (NULL slots: none), committed slots
   of the stripe emptied first */
VOID    Ext2FreedBlocksOf(IN PEXT2_VCB Vcb, IN ULONG Group, OUT const ULONG64 *Busy[EXT2_FREED_SLOTS]);

/* bits [Index, Index + Count) of Group freed, after the bitmap change was
   filed in the journal; FALSE: no memory for the overlay, reusable at once */
BOOLEAN Ext2DeferFreedBlocks(IN PEXT2_VCB Vcb, IN ULONG Group, IN ULONG Index, IN ULONG Count);

VOID    Ext2DestroyFreedBlocks(IN PEXT2_VCB Vcb);

/* at the top of a request that will grow file data from Allocated to Wanted
   bytes, before it takes any lock: commit first when only blocks still
   waiting for a commit could cover it */
VOID    Ext2WaitForFreedBlocks(IN PEXT2_VCB Vcb, IN LONGLONG Allocated, IN LONGLONG Wanted);

unsigned ext4_init_inode_bitmap(struct super_block *sb, struct buffer_head *bh,
                                ext4_group_t block_group,
                                struct ext4_group_desc *gdp);

unsigned ext4_init_block_bitmap(struct super_block *sb, struct buffer_head *bh,
                                ext4_group_t block_group, struct ext4_group_desc *gdp);

struct ext4_group_desc * ext4_get_group_desc(struct super_block *sb,
                    ext4_group_t block_group, struct buffer_head **bh);

ext4_fsblk_t ext4_count_free_blocks(struct super_block *sb);

unsigned long ext4_count_free_inodes(struct super_block *sb);

int ext4_check_descriptors(struct super_block *sb);

NTSTATUS
Ext2LoadSuper(
    IN PEXT2_VCB      Vcb,
    IN BOOLEAN        bVerify,
    OUT PEXT2_SUPER_BLOCK * Sb
);

BOOLEAN
Ext2SaveSuper(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb
);

ULONGLONG
Ext2FreeBlocks(IN PEXT2_VCB Vcb);

ULONG
Ext2FreeInodes(IN PEXT2_VCB Vcb);

VOID
Ext2SyncSuperTotals(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb);

VOID
Ext2LockSuper(IN PEXT2_VCB Vcb);

VOID
Ext2UnlockSuper(IN PEXT2_VCB Vcb);

VOID
Ext2SetSuperRoCompat(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN ULONG                Feature
);

BOOLEAN
Ext2RefreshSuper(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb
);

BOOLEAN
Ext2LoadGroupBH(IN PEXT2_VCB Vcb);

BOOLEAN
Ext2LoadGroup(IN PEXT2_VCB Vcb);

VOID
Ext2DropGroupBH(IN PEXT2_VCB Vcb);

VOID
Ext2PutGroup(IN PEXT2_VCB Vcb);

VOID
Ext2DropBH(IN PEXT2_VCB Vcb);

VOID
Ext2DrainBH(IN PEXT2_VCB Vcb);

NTSTATUS
Ext2FlushVcb(IN PEXT2_VCB Vcb);

BOOLEAN
Ext2SaveGroup(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN ULONG                Group
);

BOOLEAN
Ext2RefreshGroup(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb
);

BOOLEAN
Ext2GetInodeLba (
    IN PEXT2_VCB Vcb,
    IN ULONG inode,
    OUT PLONGLONG offset
);

BOOLEAN
Ext2LoadInode (
    IN PEXT2_VCB Vcb,
    IN struct inode *Inode
);

/* struct inode onto the on-disk inode (the fields it has; the rest stays) */
void Ext2EncodeInode(struct ext4_inode *dst, struct inode *src);

/* InlineData.c: inline_data, files and directories inside their inode */
BOOLEAN Ext4IsInline(struct inode *inode);
loff_t Ext4DirSize(struct inode *dir);
struct buffer_head *Ext4InlineDirBlock(struct ext2_icb *icb, struct inode *dir,
                                       unsigned long block, int *err);
NTSTATUS Ext4InlineRead(PEXT2_IRP_CONTEXT IrpContext, PEXT2_VCB Vcb, PEXT2_MCB Mcb,
                        ULONGLONG Offset, PVOID Buffer, ULONG Size, PULONG BytesRead);
NTSTATUS Ext4UninlineFile(PEXT2_IRP_CONTEXT IrpContext, PEXT2_VCB Vcb,
                          PEXT2_MCB Mcb, BOOLEAN KeepData);
NTSTATUS Ext4UninlineDir(PEXT2_IRP_CONTEXT IrpContext, PEXT2_VCB Vcb, PEXT2_MCB Mcb);

BOOLEAN
Ext2ClearInode (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB Vcb,
    IN ULONG inode
);

BOOLEAN
Ext2SaveInode (
    IN PEXT2_IRP_CONTEXT IrpContext,
    IN PEXT2_VCB Vcb,
    IN struct inode *Inode
);

BOOLEAN
Ext2LoadInodeXattr(IN PEXT2_VCB Vcb,
	IN struct inode *Inode,
	IN PEXT2_INODE InodeXattr);

BOOLEAN
Ext2SaveInodeXattr(IN PEXT2_IRP_CONTEXT IrpContext,
	IN PEXT2_VCB Vcb,
	IN struct inode *Inode,
	IN PEXT2_INODE InodeXattr);

BOOLEAN
Ext2LoadBlock (
    IN PEXT2_VCB Vcb,
    IN ULONGLONG dwBlk,
    IN PVOID     Buffer
);

BOOLEAN
Ext2SaveBlock (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN ULONGLONG            dwBlk,
    IN PVOID                Buf
);

BOOLEAN
Ext2LoadBuffer(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN LONGLONG             Offset,
    IN ULONG                Size,
    IN PVOID                Buf
);

BOOLEAN
Ext2ZeroBuffer(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN LONGLONG             Offset,
    IN ULONG                Size
);

BOOLEAN
Ext2SaveBuffer(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN LONGLONG             Offset,
    IN ULONG                Size,
    IN PVOID                Buf
);

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
);

VOID
Ext2AdjustVcbStat(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN LONGLONG             BlockDelta,
    IN LONGLONG             InodeDelta
);

NTSTATUS
Ext2NewBlock(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN ULONG                GroupHint,
    IN ULONGLONG            BlockHint,
    OUT PULONGLONG          Block,
    IN OUT PULONG           Number,
    IN ULONGLONG            BlockLimit,
    IN ULONG                Flags           /* EXT2_ALLOC_* */
);

NTSTATUS
Ext2FreeBlock(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN ULONGLONG            Block,
    IN ULONG                Number
);

NTSTATUS
Ext2NewInode(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN ULONG                GroupHint,
    IN ULONG                Type,
    OUT PULONG              Inode
);

NTSTATUS
Ext2UpdateGroupDirStat(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN ULONG                Group
);

NTSTATUS
Ext2FreeInode(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN ULONG                Inode,
    IN ULONG                Type
);

NTSTATUS
Ext2AddEntry (
    IN PEXT2_IRP_CONTEXT   IrpContext,
    IN PEXT2_VCB           Vcb,
    IN PEXT2_FCB           Dcb,
    IN struct inode       *Inode,
    IN PUNICODE_STRING     FileName,
    OUT struct dentry    **dentry
);

NTSTATUS
Ext2SetFileType (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_FCB            Dcb,
    IN PEXT2_MCB            Mcb,
    IN umode_t              mode
);

NTSTATUS
Ext2RemoveEntry (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_FCB            Dcb,
    IN PEXT2_MCB            Mcb,
    OUT PBOOLEAN            LastName OPTIONAL
);

NTSTATUS
Ext2SetParentEntry (
    IN PEXT2_IRP_CONTEXT   IrpContext,
    IN PEXT2_VCB           Vcb,
    IN PEXT2_FCB           Dcb,
    IN ULONG               OldParent,
    IN ULONG               NewParent );


struct ext3_dir_entry_2 *ext3_next_entry(struct ext3_dir_entry_2 *p);

int ext3_check_dir_entry (const char * function, struct inode * dir,
                          struct ext3_dir_entry_2 * de,
                          struct buffer_head * bh,
                          unsigned long offset);

loff_t ext3_max_size(int blkbits, int has_huge_files);

loff_t ext3_max_bitmap_size(int bits, int has_huge_files);

ext3_fsblk_t descriptor_loc(struct super_block *sb,
                            ext3_fsblk_t logical_sb_block, unsigned int nr);

struct ext4_group_desc * ext4_get_group_desc(struct super_block *sb,
                    ext4_group_t block_group, struct buffer_head **bh);

int ext4_check_descriptors(struct super_block *sb);

struct buffer_head *ext3_append(struct ext2_icb *icb, struct inode *inode,
                                            ext3_lblk_t *block, int *err);

void ext3_set_de_type(struct super_block *sb,
                      struct ext3_dir_entry_2 *de,
                      umode_t mode);

__u32 ext3_current_time(struct inode *in);

void ext3_warning (struct super_block * sb, const char * function,
                   char * fmt, ...);

#define ext3_error ext3_warning

#define ext4_error ext3_error

void ext3_update_dx_flag(struct inode *inode);

int ext3_mark_inode_dirty(struct ext2_icb *icb, struct inode *in);

void ext3_inc_count(struct inode *inode);

void ext3_dec_count(struct inode *inode);

struct buffer_head *
            ext3_find_entry (struct ext2_icb *icb, struct dentry *dentry,
                             struct ext3_dir_entry_2 ** res_dir);

struct buffer_head *
            ext3_dx_find_entry(struct ext2_icb *, struct dentry *dentry,
                               struct ext3_dir_entry_2 **res_dir, int *err);

typedef int (*filldir_t)(void *, const char *, int, unsigned long, __u32, unsigned);

int ext3_dx_readdir(struct file *filp, filldir_t filldir, void * context);

struct buffer_head *ext3_bread(struct ext2_icb *icb, struct inode *inode,
                                           unsigned long block, int *err);

int add_dirent_to_buf(struct ext2_icb *icb, struct dentry *dentry,
                      struct inode *inode, struct ext3_dir_entry_2 *de,
                      struct buffer_head *bh);

struct ext3_dir_entry_2 *
            do_split(struct ext2_icb *icb, struct inode *dir,
                     struct buffer_head **bh,struct dx_frame *frame,
                     struct dx_hash_info *hinfo, int *error);

int ext3_add_entry(struct ext2_icb *icb, struct dentry *dentry, struct inode *inode);

int ext3_delete_entry(struct ext2_icb *icb, struct inode *dir,
                      struct ext3_dir_entry_2 *de_del,
                      struct buffer_head *bh);

int ext3_is_dir_empty(struct ext2_icb *icb, struct inode *inode);

NTSTATUS
Ext2MapIndirect(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_MCB            Mcb,
    IN ULONG                Index,
    IN BOOLEAN              bAlloc,
    OUT PULONGLONG          pBlock,
    OUT PULONG              Number
);

NTSTATUS
Ext2ExpandIndirect(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB         Vcb,
    PEXT2_MCB         Mcb,
    ULONG             Start,
    ULONG             End,
    PLARGE_INTEGER    Size
);

NTSTATUS
Ext2TruncateIndirect(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB         Vcb,
    PEXT2_MCB         Mcb,
    PLARGE_INTEGER    Size
);

#if _MSC_VER > 1900
int strncmp(const char* str1, const char* str2, size_t count);
char* strncpy(char* dest, const char* src, size_t count);
#endif

struct dentry * Ext2AllocateEntry();

VOID Ext2FreeEntry (IN struct dentry *de);

struct dentry *Ext2BuildEntry(PEXT2_VCB Vcb, PEXT2_MCB Dcb, PUNICODE_STRING FileName);

/* Where the data of an inode should go first: the first block of the
   inode's own group, as ext4_inode_to_goal_block does in Linux. Keeping
   data next to its inode keeps files contiguous and spreads the
   allocations over the groups instead of piling them up in group 0. */
static __inline ext4_fsblk_t ext4_inode_to_goal_block(struct inode *inode)
{
    PEXT2_VCB   Vcb = inode->i_sb->s_priv;
    ULONG       Group = (ULONG)((inode->i_ino - 1) / INODES_PER_GROUP);

    return (ext4_fsblk_t)Group * BLOCKS_PER_GROUP + EXT2_FIRST_DATA_BLOCK;
}

/* the caseless names of a large directory (DirectoryNameCache.c) */
BOOLEAN Ext4DirNameMayExist(struct ext2_icb *icb, struct inode *dir, const char *name, int len);
VOID    Ext4DirNameAdded(struct inode *dir, const char *name, int len);
VOID    Ext4DirNameRemoved(struct inode *dir);
VOID    Ext4DirNamesFree(PEXT2_ICB Icb);
#endif /* _EXT4_EXT4FS_EXT4_H_ */
