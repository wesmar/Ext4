/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef XATTR_MODEL_STUB_H
#define XATTR_MODEL_STUB_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
typedef uint8_t __u8;
typedef uint16_t __u16, __le16;
typedef uint32_t __u32, __le32, ULONG;
typedef uint64_t __u64, ULONGLONG;
typedef int32_t __s32;
typedef uintptr_t ULONG_PTR;
typedef int BOOL;
typedef void *PEXT2_IRP_CONTEXT;
typedef struct { __u32 i_ino; } MODEL_RUNTIME_INODE;
typedef struct { MODEL_RUNTIME_INODE *Inode; } MODEL_MCB, *PEXT2_MCB;
typedef struct {
    unsigned char legacy[128];
    __le16 i_extra_isize;
    unsigned char extra[126];
} MODEL_INODE, *PEXT2_INODE;
typedef struct { BOOL ea_inode; } MODEL_SUPER;
typedef struct { ULONG InodeSize, BlockSize; MODEL_SUPER sb; } MODEL_FS, *PEXT2_VCB;
struct buffer_head { char *b_data; __u64 b_blocknr; };
#define TRUE 1
#define FALSE 0
#define ASSERT assert
#define EFSCORRUPTED 990
#define EFSBADCRC 991
#define GFP_NOFS 0
#define EXT4_GOOD_OLD_INODE_SIZE 128
#define __attribute__(x)
#define container_of(p,t,m) ((t *)((char *)(p) - offsetof(t,m)))
#ifndef min
#define min(a,b) ((a)<(b)?(a):(b))
#endif
#define le16_to_cpu(x) ((__u16)(x))
#define le32_to_cpu(x) ((__u32)(x))
#define cpu_to_le16(x) ((__le16)(x))
#define cpu_to_le32(x) ((__le32)(x))
static unsigned PoolCalls, PoolLive, FailAt;
static unsigned ChecksumCalls, DiagnosticCalls;
static BOOL ChecksumValid;
/* The parser model injects the verifier result; it does not model CRC32C. */
static BOOL ext4_xattr_block_csum_verify(const MODEL_RUNTIME_INODE *inode,
    const struct buffer_head *bh)
{
    assert(inode && inode->i_ino == 42 && bh && bh->b_blocknr == 123);
    ChecksumCalls++;
    return ChecksumValid;
}
static void DbgPrint(const char *format, ...)
{
    assert(format);
    DiagnosticCalls++;
}
static size_t LargestAllocation;
static void *ModelAllocate(size_t size)
{
    void *p;
    PoolCalls++;
    assert(size && size <= (1u << 24));
    if (size > LargestAllocation) LargestAllocation = size;
    if (FailAt && PoolCalls == FailAt) return NULL;
    p = malloc(size);
    assert(p);
    PoolLive++;
    return p;
}
static void ModelFree(void *p)
{
    assert(p && PoolLive);
    PoolLive--;
    free(p);
}
#define kmalloc(size, flags) ModelAllocate(size)
#define kfree(p) ModelFree(p)
static void *kzalloc(size_t size, int flags)
{
    void *p;
    (void)flags;
    p = ModelAllocate(size);
    if (p) memset(p, 0, size);
    return p;
}
static int ext4_has_feature_ea_inode(const MODEL_SUPER *sb)
{
    return sb->ea_inode;
}
#include <linux/list.h>
#include <linux/rbtree.h>
#endif
