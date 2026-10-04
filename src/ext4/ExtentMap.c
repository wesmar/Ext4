/**
 * ExtentMap.c - extent mapped files: block lookup, allocation and truncation entry points.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "linux\ext4.h"
#include "extents_internal.h"


static inline int get_default_free_blocks_flags(struct inode *inode)
{
    UNREFERENCED_PARAMETER(inode);
	return 0;
}

NTSTATUS
Ext2MapExtent(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_MCB            Mcb,
    IN ULONG                Index,
    IN BOOLEAN              Alloc,
    OUT PULONGLONG          Block,
    OUT PULONG              Number
)
{
    EXT4_EXTENT_HEADER *eh;
    struct buffer_head bh_got = {0};
    int    flags, rc;
	ULONG max_blocks = 0;

    memset(&bh_got, 0, sizeof(struct buffer_head));

    /* i_block holds data, not an extent tree: whoever allocates for an
       inline inode has missed converting it (InlineData.c) */
    if (Alloc && Ext4IsInline(Mcb->Inode)) {
        DbgPrint("ext4: allocation for inline inode %u refused\n", Mcb->Inode->i_ino);
        return STATUS_INVALID_DEVICE_STATE;
    }

    eh = get_ext4_header(Mcb->Inode);

    if (eh->eh_magic != EXT4_EXT_MAGIC) {
        if (Alloc) {
            /* now initialize inode extent root node */
            rc = ext4_ext_tree_init(IrpContext, NULL, Mcb->Inode);
            if (rc != 0)
                return Ext2WinntError(rc);
        } else {
            /* return empty-mapping when inode extent isn't initialized */
            if (Block)
                *Block = 0;
            if (Number) {
                LONGLONG  _len = Mcb->Inode->i_size;
                if (Mcb->Icb->Fcb)
                    _len = Mcb->Icb->Fcb->Header.AllocationSize.QuadPart;
                *Number = (ULONG)((_len + BLOCK_SIZE - 1) >> BLOCK_BITS);
            }
            return STATUS_SUCCESS;
        }
    }

    /* IrpContext is NULL when called during journal initialization */
    if (IsMcbDirectory(Mcb) || IrpContext == NULL ||
        IrpContext->MajorFunction == IRP_MJ_WRITE || !Alloc){
        flags = EXT4_GET_BLOCKS_IO_CONVERT_EXT;
		max_blocks = EXT_INIT_MAX_LEN;
    } else {
        flags = EXT4_GET_BLOCKS_IO_CREATE_EXT;
		max_blocks = EXT_UNWRITTEN_MAX_LEN;
    }
    
    if (Alloc) {
        /* allocate or initialize at most the run the caller asked for
           (never 0: that made ext4_ext_get_blocks split extents at their
           own start) */
        if (Number && *Number) {
            if (max_blocks > *Number) {
                max_blocks = *Number;
            }
        } else {
            max_blocks = 1;
        }
    }

    if ((rc = ext4_ext_get_blocks(
                            IrpContext,
                            NULL,
                            Mcb->Inode,
                            Index,
                            max_blocks,
                            &bh_got,
                            Alloc,
                            flags)) < 0) {
        DEBUG(DL_ERR, ("Block insufficient resources, err: %d\n", rc));
        return Ext2WinntError(rc);
    }
    if (Alloc && !Ext2SaveInode(IrpContext, Vcb, Mcb->Inode))
        return STATUS_UNEXPECTED_IO_ERROR;
    if (Number)
        *Number = rc ? rc : 1;
    if (Block)
        *Block = bh_got.b_blocknr;

    return STATUS_SUCCESS;
}

NTSTATUS
Ext2DoExtentExpand(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_MCB            Mcb,
    IN ULONG                Index,
    IN OUT PULONGLONG       Block,
    IN OUT PULONG           Number
)
{
    EXT4_EXTENT_HEADER *eh;
    struct buffer_head bh_got;
    int    rc, flags;

    if (!Number || !*Number)
        return STATUS_INVALID_PARAMETER;

    if (IsMcbDirectory(Mcb) || IrpContext == NULL ||
        IrpContext->MajorFunction == IRP_MJ_WRITE) {
        flags = EXT4_GET_BLOCKS_IO_CONVERT_EXT;
    } else {
        flags = EXT4_GET_BLOCKS_IO_CREATE_EXT;
    }

    memset(&bh_got, 0, sizeof(struct buffer_head));

    if (Ext4IsInline(Mcb->Inode)) {
        DbgPrint("ext4: allocation for inline inode %u refused\n", Mcb->Inode->i_ino);
        return STATUS_INVALID_DEVICE_STATE;
    }

    eh = get_ext4_header(Mcb->Inode);

    if (eh->eh_magic != EXT4_EXT_MAGIC) {
        rc = ext4_ext_tree_init(IrpContext, NULL, Mcb->Inode);
        if (rc != 0)
            return Ext2WinntError(rc);
    }

    if ((rc = ext4_ext_get_blocks( IrpContext, NULL, Mcb->Inode, Index,
                                  *Number, &bh_got, 1, flags)) < 0) {
        DEBUG(DL_ERR, ("Expand Block insufficient resources, Number: %u,"
                       " err: %d\n", *Number, rc));
        return Ext2WinntError(rc);
    }

    if (Number)
        *Number = rc ? rc : 1;
    if (Block)
        *Block = bh_got.b_blocknr;

    if (!Ext2SaveInode(IrpContext, Vcb, Mcb->Inode))
        return STATUS_UNEXPECTED_IO_ERROR;

    return STATUS_SUCCESS;
}

NTSTATUS
Ext2ExpandExtent(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB         Vcb,
    PEXT2_MCB         Mcb,
    ULONG             Start,
    ULONG             End,
    PLARGE_INTEGER    Size
    )
{
    ULONG Count = 0, Number = 0;
    ULONGLONG Block = 0;
    NTSTATUS Status = STATUS_SUCCESS;

    if (End <= Start)
        return Status;

    while (End > Start + Count) {

        Number = End - Start - Count;
        Status = Ext2DoExtentExpand(IrpContext, Vcb, Mcb, Start + Count,
                                    &Block, &Number);
        /* keep the reason: a full volume is STATUS_DISK_FULL (ENOSPC), which
           applications report as such, not a lack of system resources */
        if (!NT_SUCCESS(Status)) {
            break;
        }
        if (Number == 0) {
            Status = STATUS_DISK_FULL;
            break;
        }

        if (Block && IsZoneInited(Mcb)) {
            if (!Ext2AddBlockExtent(Vcb, Mcb, Start + Count, Block, Number)) {
                ClearLongFlag(Mcb->Icb->Flags, ICB_ZONE_INITED);
                Ext2ClearAllExtents(&Mcb->Icb->Extents);
            }
        }
        Count += Number;
    }

    Size->QuadPart = ((LONGLONG)(Start + Count)) << BLOCK_BITS;

    /* save inode whatever it succeeds to expand or not */
    if (!Ext2SaveInode(IrpContext, Vcb, Mcb->Inode) && NT_SUCCESS(Status))
        Status = STATUS_UNEXPECTED_IO_ERROR;

    return Status;
}

NTSTATUS
Ext2TruncateExtent(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB         Vcb,
    PEXT2_MCB         Mcb,
    PLARGE_INTEGER    Size
    )
{
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG    Wanted;
    int      err;

    /* the first logical block that goes; extents index blocks by 32 bits */
    Wanted = (ULONG)((Size->QuadPart + BLOCK_SIZE - 1) >> BLOCK_BITS);

    /* Failing part way, the tree has lost some of the blocks past Wanted
       and nobody knows which: every cached run goes, not only those past
       Wanted - a run left behind would map a freed block. Size stays as
       asked: an allocation reported short only makes a later extension
       find blocks that are already there. */
    err = ext4_ext_truncate(IrpContext, Mcb->Inode, Wanted);
    if (err != 0) {
        Status = Ext2WinntError(err);
    }
    if (err != 0 || !Ext2RemoveBlockExtent(Vcb, Mcb, Wanted, MAXULONG - Wanted)) {
        ClearLongFlag(Mcb->Icb->Flags, ICB_ZONE_INITED);
        Ext2ClearAllExtents(&Mcb->Icb->Extents);
    }

    if (Mcb->Inode->i_size > (loff_t)(Size->QuadPart))
        Mcb->Inode->i_size = (loff_t)(Size->QuadPart);

    /* Save modifications on i_blocks field and i_size field of the inode. */
    if (!Ext2SaveInode(IrpContext, Vcb, Mcb->Inode) && NT_SUCCESS(Status))
        Status = STATUS_UNEXPECTED_IO_ERROR;

    return Status;
}

static int ext4_ext_convert_to_initialized (
		void *icb,
		handle_t *handle,
		struct inode *inode,
		struct ext4_ext_path **ppath,
		ext4_lblk_t split,
		unsigned long blocks,
		int flags)
{
	int depth = ext_depth(inode), err;
	struct ext4_extent *ex = (*ppath)[depth].p_ext;

	assert (le32_to_cpu(ex->ee_block) <= split);

	if (split + blocks == le32_to_cpu(ex->ee_block) + 
						  ext4_ext_get_actual_len(ex)) {

		/* split and initialize right part */
		err = ext4_split_extent_at(icb, handle, inode, ppath, split,
								   EXT4_EXT_MARK_UNWRIT1, flags);

	} else if (le32_to_cpu(ex->ee_block) == split) {

		/* split and initialize left part */
		err = ext4_split_extent_at(icb, handle, inode, ppath, split + blocks,
								   EXT4_EXT_MARK_UNWRIT2, flags);

	} else {

		/* split 1 extent to 3 and initialize the 2nd. Both halves of
		 * the first split stay unwritten, so they must not merge back
		 * into one: PRE_IO keeps the insert from merging (Linux
		 * ext4_split_extent passes it for the same reason), otherwise
		 * the second split initializes everything right of @split */
		err = ext4_split_extent_at(icb, handle, inode, ppath, split + blocks,
								   EXT4_EXT_MARK_UNWRIT1 |
								   EXT4_EXT_MARK_UNWRIT2,
								   flags | EXT4_GET_BLOCKS_PRE_IO);
		if (0 == err) {
			/* the insert left the path at the new right-hand extent:
			 * look up the left one again before splitting it (as Linux
			 * ext4_split_extent does), or the split lands on the wrong
			 * extent and marks everything up to its end initialized */
			struct ext4_ext_path *path = ext4_find_extent(inode, split, ppath, 0);
			if (IS_ERR(path))
				return PTR_ERR(path);
			*ppath = path;
			err = ext4_split_extent_at(icb, handle, inode, ppath, split,
									   EXT4_EXT_MARK_UNWRIT1, flags);
		}
	}

	if (0 == err && !(flags & EXT4_GET_BLOCKS_PRE_IO)) {
		/* join the initialized run to initialized neighbours, as Linux
		 * ext4_ext_convert_to_initialized does: a file written in pieces
		 * into preallocated space must not end up one extent per piece */
		struct ext4_ext_path *path = ext4_find_extent(inode, split, ppath, 0);
		if (IS_ERR(path))
			return PTR_ERR(path);
		*ppath = path;
		depth = ext_depth(inode);
		ex = path[depth].p_ext;
		err = ext4_ext_get_access(icb, handle, inode, path + depth);
		if (0 == err) {
			ext4_ext_try_to_merge(icb, handle, inode, path, ex);
			err = ext4_ext_dirty(icb, handle, inode, path + path->p_depth);
		}
	}

	return err;
}

int ext4_ext_get_blocks(void *icb, handle_t *handle, struct inode *inode, ext4_lblk_t iblock,
		unsigned long max_blocks, struct buffer_head *bh_result,
		int create, int flags)
{
	struct ext4_ext_path *path = NULL;
	struct ext4_extent newex, *ex;
	ext4_fsblk_t goal, newblock;
	int err = 0, depth;
	unsigned long allocated = 0;
	ext4_lblk_t next;

	clear_buffer_new(bh_result);
	/*mutex_lock(&ext4_I(inode)->truncate_mutex);*/

	/* find extent for this block */
	path = ext4_find_extent(inode, iblock, NULL, 0);
	if (IS_ERR(path)) {
		err = PTR_ERR(path);
		path = NULL;
		goto out2;
	}

	depth = ext_depth(inode);

	/*
	 * consistent leaf must not be empty
	 * this situations is possible, though, _during_ tree modification
	 * this is why assert can't be put in ext4_ext_find_extent()
	 */
	BUG_ON(path[depth].p_ext == NULL && depth != 0);

	if ((ex = path[depth].p_ext)) {
		ext4_lblk_t ee_block = le32_to_cpu(ex->ee_block);
		ext4_fsblk_t ee_start = ext4_ext_pblock(ex);
		unsigned short ee_len  = ext4_ext_get_actual_len(ex);
		/* if found exent covers block, simple return it */
		if (iblock >= ee_block && iblock < ee_block + ee_len) {

			/* number of remain blocks in the extent */
			allocated = ee_len + ee_block - iblock;

			if (ext4_ext_is_unwritten(ex)) {
				if (create) {
					/* initialize only the blocks being written: the
					 * rest of the extent stays unwritten (reads as
					 * zeros) instead of exposing stale disk contents */
					if (allocated > max_blocks)
						allocated = max_blocks;
					newblock = iblock - ee_block + ee_start;
					err = ext4_ext_convert_to_initialized (
							icb, handle,
							inode,
							&path,
							iblock,
							allocated,
							flags);
					if (err)
						goto out2;

				} else {
					newblock = 0;
				}
			} else {
				newblock = iblock - ee_block + ee_start;
			}
			goto out;
		}
	}

	/*
	 * requested block isn't allocated yet
	 * we couldn't try to create block if create flag is zero
	 */
	if (!create) {
		goto out2;
	}

	/* find next allocated block so that we know how many
	 * blocks we can allocate without overlapping the next extent */
	next = ext4_ext_next_allocated_block(path);
	BUG_ON(next <= iblock);
	allocated = next - iblock;
	if (flags & EXT4_GET_BLOCKS_PRE_IO && max_blocks > EXT_UNWRITTEN_MAX_LEN)
		max_blocks = EXT_UNWRITTEN_MAX_LEN;
	if (allocated > max_blocks)
		allocated = max_blocks;

	/* allocate new block */
	goal = ext4_ext_find_goal(inode, path, iblock);

	newblock = ext4_new_data_blocks(icb, inode, goal, &allocated, &err);
	if (!newblock)
		goto out2;

	/* try to insert new extent into found leaf and return */
	newex.ee_block = cpu_to_le32(iblock);
	ext4_ext_store_pblock(&newex, newblock);
	newex.ee_len = cpu_to_le16(allocated);
	/* if it's fallocate, mark ex as unwritten */
	if (flags & EXT4_GET_BLOCKS_PRE_IO) {
		ext4_ext_mark_unwritten(&newex);
	}
	err = ext4_ext_insert_extent(icb, handle, inode, &path, &newex,
                                 flags & EXT4_GET_BLOCKS_PRE_IO);

	if (err) {
		/* Roll back exactly this allocation. ee_len also carries the
		 * unwritten flag (0x8000), which is not part of the block count.
		 * Blocks that cannot go back stop the journal. */
		(void)ext4_free_blocks(icb, handle, inode, NULL, newblock,
				(int)allocated, get_default_free_blocks_flags(inode));
		goto out2;
	}

	/* the tree's root, and i_blocks, live in the inode */
	err = ext4_mark_inode_dirty(icb, handle, inode);
	if (err)
		goto out2;

	/* previous routine could use block we allocated */
	if (ext4_ext_is_unwritten(&newex))
		newblock = 0;
	else
		newblock = ext4_ext_pblock(&newex);

	set_buffer_new(bh_result);

out:
	if (allocated > max_blocks)
		allocated = max_blocks;

	ext4_ext_show_leaf(inode, path);
	set_buffer_mapped(bh_result);
	bh_result->b_bdev = inode->i_sb->s_bdev;
	bh_result->b_blocknr = newblock;
out2:
	if (path) {
		ext4_ext_drop_refs(path);
		kfree(path);
	}
	/*mutex_unlock(&ext4_I(inode)->truncate_mutex);*/

	return err ? err : allocated;
}
