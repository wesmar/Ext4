/**
 * xattr_inode.c - extended attribute values kept in inodes of their own
 * (the ea_inode feature): read, create, reference counting.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include <linux/module.h>
#include <linux/ext4_ext.h>
#include <linux/ext4_xattr.h>
#include "xattr_internal.h"

/*
 * The on-disk contract, as Linux keeps it (fs/ext4/xattr.c):
 *  - the entry holds e_value_inum, e_value_size, e_value_offs = 0, and
 *    e_hash = hash of the name and of the value inode's hash;
 *  - the value inode is a regular file with EXT4_EA_INODE_FL, one link,
 *    in no directory, its size the value's size;
 *  - its i_atime holds crc32c(s_csum_seed, value), the "value hash";
 *  - its reference count is i_ctime (high 32 bits) : i_version (low 32
 *    bits, l_i_version on disk) - how many entries point at it.
 */

__u32 ext4_xattr_inode_value_hash(PEXT2_VCB Vcb, const void *data, size_t size)
{
	return ext4_chksum(&Vcb->sbi, Vcb->sbi.s_csum_seed, data, (unsigned int)size);
}

/* the raw on-disk inode: the reference count lives where struct inode has no field */
static int ext4_xattr_inode_raw(PEXT2_VCB Vcb, __u32 ino, struct ext4_inode *raw,
				LONGLONG *offset)
{
	if (!Ext2GetInodeLba(Vcb, ino, offset))
		return -EIO;
	if (!Ext2LoadBuffer(NULL, Vcb, *offset, Vcb->InodeSize, raw))
		return -EIO;
	return 0;
}

static __u64 ext4_xattr_inode_get_ref(struct ext4_inode *raw)
{
	return ((__u64)le32_to_cpu(raw->i_ctime) << 32) |
	       le32_to_cpu(raw->osd1.linux1.l_i_version);
}

/* struct inode in, reference count set, checksum over the result, written */
static int ext4_xattr_inode_store(PEXT2_IRP_CONTEXT IrpContext, PEXT2_VCB Vcb,
				  struct inode *ei, __u64 ref)
{
	struct ext4_inode_info unused = {0};
	struct ext4_inode *raw;
	LONGLONG offset;
	int ret;

	raw = Ext2AllocatePool(NonPagedPool, Vcb->InodeSize, EXT2_INODE_MAGIC);
	if (!raw)
		return -ENOMEM;

	ret = ext4_xattr_inode_raw(Vcb, ei->i_ino, raw, &offset);
	if (!ret) {
		ei->i_ctime = (__u32)(ref >> 32);
		Ext2EncodeInode(raw, ei);
		raw->osd1.linux1.l_i_version = cpu_to_le32((__u32)ref);
		ext4_inode_csum_set(ei, raw, &unused);
		if (!Ext2SaveBuffer(IrpContext, Vcb, offset, Vcb->InodeSize, raw))
			ret = -EIO;
	}

	Ext2FreePool(raw, EXT2_INODE_MAGIC);
	return ret;
}

/* load and check a value inode: what the entry says it is, it must be */
static int ext4_xattr_inode_load(PEXT2_VCB Vcb, __u32 ino, size_t size,
				 struct inode *ei)
{
	RtlZeroMemory(ei, sizeof(*ei));
	ei->i_sb = &Vcb->sb;
	ei->i_ino = ino;

	if (ino < EXT4_FIRST_INO(&Vcb->sb) || ino > le32_to_cpu(Vcb->SuperBlock->s_inodes_count))
		return -EFSCORRUPTED;
	if (!Ext2LoadInode(Vcb, ei))
		return -EIO;
	if (!(ei->i_flags & EXT4_EA_INODE_FL) || !S_ISREG(ei->i_mode) ||
	    ei->i_nlink != 1 || (size_t)ei->i_size != size) {
		DbgPrint("ext4: xattr value inode %u is not one (flags %08x, mode %o, "
			 "links %u, size %I64u, expected %Iu)\n", ino, (ULONG)ei->i_flags,
			 ei->i_mode, ei->i_nlink, (ULONGLONG)ei->i_size, size);
		return -EFSCORRUPTED;
	}
	return 0;
}

/* the value's blocks, one run at a time: extents, or the direct block map */
static int ext4_xattr_inode_map(void *icb, struct inode *ei, ext4_lblk_t lblk,
				unsigned long count, int create, ext4_fsblk_t *pblk)
{
	struct buffer_head bh = {0};
	int ret;

	if (ei->i_flags & EXT4_EXTENTS_FL) {
		ret = ext4_ext_get_blocks(icb, NULL, ei, lblk, count, &bh, create, 0);
		if (ret <= 0)
			return ret ? ret : -EFSCORRUPTED;
		*pblk = bh.b_blocknr;
		return ret;
	}
	/* e2fsprogs on a volume without extents: direct blocks only (a 64 KiB
	   value fits there at 4 KiB blocks) */
	if (create || lblk >= EXT2_NDIR_BLOCKS || ei->i_block[lblk] == 0)
		return -EFSCORRUPTED;
	*pblk = ei->i_block[lblk];
	return 1;
}

int ext4_xattr_inode_read(struct ext4_xattr_ref *ref, __u32 ino, void *buf,
			  size_t size, __u32 *hash)
{
	PEXT2_VCB Vcb = ref->fs;
	struct inode ei;
	size_t done = 0;
	ext4_lblk_t lblk = 0;
	int ret;

	ret = ext4_xattr_inode_load(Vcb, ino, size, &ei);
	if (ret)
		return ret;
	*hash = ei.i_atime;

	while (done < size) {
		ext4_fsblk_t pblk;
		unsigned long want = (unsigned long)((size - done + BLOCK_SIZE - 1) >> BLOCK_BITS);
		int n = ext4_xattr_inode_map(ref->IrpContext, &ei, lblk, want, 0, &pblk);
		int i;

		if (n < 0)
			return n;
		for (i = 0; i < n && done < size; i++) {
			struct buffer_head *bh = extents_bread(&Vcb->sb, pblk + i);
			size_t chunk = min(size - done, (size_t)BLOCK_SIZE);

			if (!bh)
				return -EIO;
			memcpy((char *)buf + done, bh->b_data, chunk);
			extents_brelse(bh);
			done += chunk;
		}
		lblk += n;
	}

	if (ext4_xattr_inode_value_hash(Vcb, buf, size) != *hash) {
		DbgPrint("ext4: xattr value inode %u: the value does not match its hash\n", ino);
		return -EFSCORRUPTED;
	}
	return 0;
}

/*
 * A new value inode with one reference, near the inode it serves. The data
 * goes through the buffer heads like any metadata block, so the journal
 * carries it with the entry that points at it (Linux writes it the same
 * way, ext4_xattr_inode_write).
 */
int ext4_xattr_inode_create(struct ext4_xattr_ref *ref, const void *data,
			    size_t size, __u32 *ino, __u32 *hash)
{
	PEXT2_IRP_CONTEXT IrpContext = ref->IrpContext;
	PEXT2_VCB Vcb = ref->fs;
	struct inode *owner = ref->inode_ref->Inode;
	struct ext3_super_block *es = EXT3_SB(&Vcb->sb)->s_es;
	struct inode ei = {0};
	LARGE_INTEGER SysTime;
	ULONG iNo = 0;
	size_t done = 0;
	ext4_lblk_t lblk = 0;
	NTSTATUS Status;
	int ret;

	if (!EXT3_HAS_INCOMPAT_FEATURE(&Vcb->sb, EXT4_FEATURE_INCOMPAT_EXTENTS))
		return -ENOSPC;	/* the value then does not fit, as without ea_inode */

	Status = Ext2NewInode(IrpContext, Vcb, (owner->i_ino - 1) / INODES_PER_GROUP,
			      EXT2_FT_REG_FILE, &iNo);
	if (!NT_SUCCESS(Status))
		return Ext2LinuxError(Status);

	KeQuerySystemTime(&SysTime);
	Ext2ClearInode(IrpContext, Vcb, iNo);
	ei.i_sb = &Vcb->sb;
	ei.i_ino = iNo;
	ei.i_mode = S_IFREG | S_IRUSR | S_IWUSR;
	ei.i_uid = owner->i_uid;
	ei.i_gid = owner->i_gid;
	ei.i_nlink = 1;
	ei.i_generation = owner->i_generation ^ iNo;
	Ext2SetInodeTime(&SysTime, &ei.i_mtime, &ei.i_mtime_extra);
	Ext2SetInodeTime(&SysTime, &ei.i_crtime, &ei.i_crtime_extra);
	if (Vcb->InodeSize > EXT2_GOOD_OLD_INODE_SIZE && le16_to_cpu(es->s_want_extra_isize))
		ei.i_extra_isize = le16_to_cpu(es->s_want_extra_isize);
	ei.i_flags = EXT4_EA_INODE_FL | EXT4_EXTENTS_FL;
	ext4_ext_tree_init(IrpContext, NULL, &ei);

	while (done < size) {
		ext4_fsblk_t pblk;
		unsigned long want = (unsigned long)((size - done + BLOCK_SIZE - 1) >> BLOCK_BITS);
		int n = ext4_xattr_inode_map(IrpContext, &ei, lblk, want, 1, &pblk);
		int i;

		if (n < 0) {
			ret = n;
			goto fail;
		}
		for (i = 0; i < n && done < size; i++) {
			struct buffer_head *bh = extents_bwrite(&Vcb->sb, pblk + i);
			size_t chunk = min(size - done, (size_t)BLOCK_SIZE);

			if (!bh) {
				ret = -ENOMEM;
				goto fail;
			}
			memcpy(bh->b_data, (const char *)data + done, chunk);
			if (chunk < BLOCK_SIZE)
				memset(bh->b_data + chunk, 0, BLOCK_SIZE - chunk);
			extents_mark_buffer_dirty(bh);
			extents_brelse(bh);
			done += chunk;
		}
		lblk += n;
	}

	ei.i_size = size;
	*hash = ext4_xattr_inode_value_hash(Vcb, data, size);
	ei.i_atime = *hash;
	ei.i_atime_extra = 0;
	ret = ext4_xattr_inode_store(IrpContext, Vcb, &ei, 1);
	if (ret)
		goto fail;

	*ino = iNo;
	return 0;

fail:
	ext4_ext_truncate(IrpContext, &ei, 0);
	Ext2FreeInode(IrpContext, Vcb, iNo, EXT2_FT_REG_FILE);
	return ret;
}

/* one entry fewer points at the value inode: gone with its last reference */
int ext4_xattr_inode_dec_ref(struct ext4_xattr_ref *ref, __u32 ino)
{
	PEXT2_IRP_CONTEXT IrpContext = ref->IrpContext;
	PEXT2_VCB Vcb = ref->fs;
	struct ext4_inode *raw;
	struct inode ei;
	LARGE_INTEGER SysTime;
	LONGLONG offset;
	__u64 count;
	int ret;

	RtlZeroMemory(&ei, sizeof(ei));
	ei.i_sb = &Vcb->sb;
	ei.i_ino = ino;
	if (!Ext2LoadInode(Vcb, &ei))
		return -EIO;
	if (!(ei.i_flags & EXT4_EA_INODE_FL))
		return -EFSCORRUPTED;	/* never free what is not ours to free */

	raw = Ext2AllocatePool(NonPagedPool, Vcb->InodeSize, EXT2_INODE_MAGIC);
	if (!raw)
		return -ENOMEM;
	ret = ext4_xattr_inode_raw(Vcb, ino, raw, &offset);
	count = ext4_xattr_inode_get_ref(raw);
	Ext2FreePool(raw, EXT2_INODE_MAGIC);
	if (ret)
		return ret;

	if (count > 1)
		return ext4_xattr_inode_store(IrpContext, Vcb, &ei, count - 1);

	/* the last reference: blocks, then the inode */
	if (ei.i_flags & EXT4_EXTENTS_FL) {
		ret = ext4_ext_truncate(IrpContext, &ei, 0);
		if (ret)
			return ret;
	} else {
		int i;

		/* only direct blocks were ever read from such an inode */
		for (i = EXT2_NDIR_BLOCKS; i < EXT4_N_BLOCKS; i++) {
			if (ei.i_block[i])
				return -EFSCORRUPTED;
		}
		for (i = 0; i < EXT2_NDIR_BLOCKS; i++) {
			if (ei.i_block[i]) {
				Ext2FreeBlock(IrpContext, Vcb, ei.i_block[i], 1);
				ei.i_block[i] = 0;
			}
		}
		ei.i_blocks = 0;
	}
	KeQuerySystemTime(&SysTime);
	ei.i_nlink = 0;
	ei.i_size = 0;
	ei.i_dtime = (__u32)Ext2UnixTime(&SysTime);
	ret = ext4_xattr_inode_store(IrpContext, Vcb, &ei, 0);
	if (ret)
		return ret;
	Ext2FreeInode(IrpContext, Vcb, ino, EXT2_FT_REG_FILE);
	return 0;
}
