/**
 * xattr_internal.h - definitions shared by the files split from ext4_xattr.c.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _EXT4_EXT4_XATTR_INTERNAL_H_
#define _EXT4_EXT4_XATTR_INTERNAL_H_

#define NAME_HASH_SHIFT 5
#define VALUE_HASH_SHIFT 16

/* Linux's ext4_xattr_hash_entry: the name as unsigned bytes (the signed
   variant is what old kernels wrote; e2fsck accepts both), then the
   value as little-endian words */
static inline __u32 ext4_xattr_hash_entry(const char *name, size_t name_len,
					  const __le32 *value, size_t value_count)
{
	__u32 hash = 0;

	while (name_len--) {
		hash = (hash << NAME_HASH_SHIFT) ^
		       (hash >> (8 * sizeof(hash) - NAME_HASH_SHIFT)) ^
		       (unsigned char)*name++;
	}
	while (value_count--) {
		hash = (hash << VALUE_HASH_SHIFT) ^
		       (hash >> (8 * sizeof(hash) - VALUE_HASH_SHIFT)) ^
		       le32_to_cpu(*value++);
	}
	return hash;
}

/* an entry whose value is in the block (or the inode body) at e_value_offs */
static inline void ext4_xattr_compute_hash(struct ext4_xattr_header *header,
					   struct ext4_xattr_entry *entry)
{
	const __le32 *value =
	    (const __le32 *)((char *)header + le16_to_cpu(entry->e_value_offs));

	entry->e_hash = cpu_to_le32(ext4_xattr_hash_entry(
		EXT4_XATTR_NAME(entry), entry->e_name_len, value,
		(le32_to_cpu(entry->e_value_size) + EXT4_XATTR_ROUND) >> EXT4_XATTR_PAD_BITS));
}

/* an entry whose value is in a value inode: its hash stands for the value */
static inline void ext4_xattr_compute_hash_ea(struct ext4_xattr_entry *entry,
					      __u32 ea_hash)
{
	__le32 value = cpu_to_le32(ea_hash);

	entry->e_hash = cpu_to_le32(ext4_xattr_hash_entry(
		EXT4_XATTR_NAME(entry), entry->e_name_len, &value, 1));
}

/* the block hash, from the hashes of its entries */
void ext4_xattr_rehash(struct ext4_xattr_header *header);

/* the largest value Linux keeps in a value inode */
#define EXT4_XATTR_SIZE_MAX (1 << 24)

/* Linux: a value this large goes to a value inode right away */
#define EXT4_XATTR_MIN_LARGE_EA_SIZE(b) \
	((b) - EXT4_XATTR_ROUND - sizeof(struct ext4_xattr_entry) - 4)

/* what a value inode's value costs the inodes naming it, in 512-byte units */
static inline __u64 ext4_xattr_ea_charge(struct ext4_xattr_ref *ref, size_t size)
{
	size_t bs = ref->fs->BlockSize;

	return (__u64)((size + bs - 1) / bs) * (bs >> 9);
}

/* what an entry takes in the inode body or the block */
static inline size_t ext4_xattr_space(size_t name_len, size_t data_size, BOOL ea)
{
	return EXT4_XATTR_LEN(name_len) + (ea ? 0 : EXT4_XATTR_SIZE(data_size));
}

static inline size_t ext4_xattr_item_space(struct ext4_xattr_item *item)
{
	return ext4_xattr_space(item->name_len, item->data_size, item->ea);
}

/* xattr_inode.c */
__u32 ext4_xattr_inode_value_hash(PEXT2_VCB Vcb, const void *data, size_t size);
int ext4_xattr_inode_read(struct ext4_xattr_ref *ref, __u32 ino, void *buf,
			  size_t size, __u32 *hash);
int ext4_xattr_inode_create(struct ext4_xattr_ref *ref, const void *data,
			    size_t size, __u32 *ino, __u32 *hash);
int ext4_xattr_inode_dec_ref(struct ext4_xattr_ref *ref, __u32 ino);

/* the value inode of an item that goes: dropped once the new set is written */
int ext4_xattr_drop_ea(struct ext4_xattr_ref *ref, struct ext4_xattr_item *item);

void ext4_xattr_item_insert(struct ext4_xattr_ref *xattr_ref,
				   struct ext4_xattr_item *item);

void ext4_xattr_item_remove(struct ext4_xattr_ref *xattr_ref,
				   struct ext4_xattr_item *item);

struct ext4_xattr_item *
ext4_xattr_item_alloc(__u8 name_index, const char *name, size_t name_len);

int ext4_xattr_item_alloc_data(struct ext4_xattr_item *item,
				      const void *orig_data, size_t data_size);

int ext4_xattr_item_resize_data(struct ext4_xattr_item *item,
				       size_t new_data_size);

void ext4_xattr_item_free(struct ext4_xattr_item *item);

__s32 ext4_xattr_inode_space(struct ext4_xattr_ref *xattr_ref);

__s32 ext4_xattr_block_space(struct ext4_xattr_ref *xattr_ref);

size_t ext4_xattr_inode_room(struct ext4_xattr_ref *xattr_ref);

int ext4_xattr_replace_item(struct ext4_xattr_ref *xattr_ref,
			    struct ext4_xattr_item *item,
			    const void *data, size_t data_size);

int ext4_xattr_fetch(struct ext4_xattr_ref *xattr_ref);

struct ext4_xattr_item *
ext4_xattr_lookup_item(struct ext4_xattr_ref *xattr_ref, __u8 name_index,
		       const char *name, size_t name_len);

struct ext4_xattr_item *
ext4_xattr_insert_item(struct ext4_xattr_ref *xattr_ref, __u8 name_index,
		       const char *name, size_t name_len, const void *data,
		       size_t data_size,
		       int *err);

struct ext4_xattr_item *
ext4_xattr_insert_item_ordered(struct ext4_xattr_ref *xattr_ref, __u8 name_index,
	const char *name, size_t name_len, const void *data,
	size_t data_size,
	int *err);

int ext4_xattr_remove_item(struct ext4_xattr_ref *xattr_ref,
				  __u8 name_index, const char *name,
				  size_t name_len);

int ext4_xattr_resize_item(struct ext4_xattr_ref *xattr_ref,
				  struct ext4_xattr_item *item,
				  size_t new_data_size);

int ext4_xattr_write_to_disk(struct ext4_xattr_ref *xattr_ref);

#endif /* _EXT4_EXT4_XATTR_INTERNAL_H_ */
