/* SPDX-License-Identifier: GPL-2.0-only
 * Compile the production parser, disk layouts and tree; stub only kernel services.
 */
#include "ext4fs.h"
#include "../../src/linux/rbtree.c"
#include "../../src/ext4/AttributeStorage.c"

static unsigned Checks, EaReads;
static int EaReadError;
static void Check(int condition)
{
    Checks++;
    assert(condition);
}
int ext4_xattr_inode_read(struct ext4_xattr_ref *ref, __u32 ino, void *buf,
    size_t size, __u32 *hash)
{
    (void)ref;
    assert(ino == 42 && size <= EXT4_XATTR_SIZE_MAX);
    EaReads++;
    if (EaReadError) return EaReadError;
    memset(buf, 0x6b, size);
    *hash = 123;
    return 0;
}
typedef struct {
    MODEL_FS fs;
    MODEL_INODE inode;
    MODEL_RUNTIME_INODE runtime_inode;
    MODEL_MCB mcb;
    char block[4096];
    struct buffer_head bh;
    struct ext4_xattr_ref ref;
} FIXTURE;

static void Init(FIXTURE *f, BOOL in_inode)
{
    assert(!PoolLive);
    memset(f, 0, sizeof(*f));
    PoolCalls = FailAt = EaReads = 0;
    EaReadError = 0;
    ChecksumCalls = DiagnosticCalls = 0;
    ChecksumValid = TRUE;
    LargestAllocation = 0;
    f->fs.InodeSize = sizeof(f->inode);
    f->fs.BlockSize = sizeof(f->block);
    f->inode.i_extra_isize = 32;
    f->bh.b_data = f->block;
    f->bh.b_blocknr = 123;
    f->runtime_inode.i_ino = 42;
    f->mcb.Inode = &f->runtime_inode;
    f->ref.inode_ref = &f->mcb;
    f->ref.fs = &f->fs;
    f->ref.OnDiskInode = &f->inode;
    f->ref.block_bh = &f->bh;
    f->ref.block_loaded = !in_inode;
    f->ref.inode_size_rem = ext4_xattr_inode_room(&f->ref);
    f->ref.block_size_rem = sizeof(f->block) - sizeof(struct ext4_xattr_header) - 4;
    INIT_LIST_HEAD(&f->ref.ordered_list);
    if (in_inode) {
        EXT4_XATTR_IHDR(&f->inode)->h_magic = cpu_to_le32(EXT4_XATTR_MAGIC);
    } else {
        struct ext4_xattr_header *h = EXT4_XATTR_BHDR(&f->bh);
        h->h_magic = cpu_to_le32(EXT4_XATTR_MAGIC);
        h->h_blocks = cpu_to_le32(1);
    }
}
static char *Base(FIXTURE *f, BOOL in_inode)
{
    return in_inode ? (char *)EXT4_XATTR_IFIRST(EXT4_XATTR_IHDR(&f->inode)) : f->block;
}
static size_t Capacity(FIXTURE *f, BOOL in_inode)
{
    return in_inode ? sizeof(f->inode) - (size_t)(Base(f, TRUE) - (char *)&f->inode) : sizeof(f->block);
}
static struct ext4_xattr_entry *Entry(FIXTURE *f, BOOL in_inode)
{
    return in_inode ? (struct ext4_xattr_entry *)Base(f, TRUE) : EXT4_XATTR_BFIRST(&f->bh);
}
static void Put(struct ext4_xattr_entry *e, __u32 size, __u16 offset, __u32 ino)
{
    memset(e, 0, sizeof(*e) + 4);
    e->e_name_index = EXT4_XATTR_INDEX_USER;
    e->e_name_len = 1;
    EXT4_XATTR_NAME(e)[0] = 'a';
    e->e_value_size = cpu_to_le32(size);
    e->e_value_offs = cpu_to_le16(offset);
    e->e_value_block = cpu_to_le32(ino);
    memset(EXT4_XATTR_NEXT(e), 0, 4);
}
static void Cleanup(FIXTURE *f)
{
    while (f->ref.ordered_list.next != &f->ref.ordered_list) {
        struct ext4_xattr_item *i = container_of(f->ref.ordered_list.next, struct ext4_xattr_item, list_node);
        ext4_xattr_item_remove(&f->ref, i);
        ext4_xattr_item_free(i);
    }
    Check(PoolLive == 0);
}
static void Reject(FIXTURE *f)
{
    Check(ext4_xattr_fetch(&f->ref) == -EFSCORRUPTED);
    Check(PoolCalls == 0 && PoolLive == 0 && EaReads == 0);
    Check(!f->ref.root.rb_node);
}
static void TestLocal(BOOL in_inode)
{
    FIXTURE f;
    const __u32 bad[] = {0xfffffffcU, 0xfffffffdU, 0xfffffffeU, 0xffffffffU,
        EXT4_XATTR_SIZE_MAX + 1U, EXT4_XATTR_SIZE_MAX};
    struct ext4_xattr_entry *e;
    size_t cap;
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        Init(&f, in_inode);
        cap = Capacity(&f, in_inode);
        Put(Entry(&f, in_inode), bad[i], (__u16)(cap - 4), 0);
        Reject(&f);
    }
    Init(&f, in_inode);
    cap = Capacity(&f, in_inode);
    Put(Entry(&f, in_inode), 4, (__u16)(cap + 1), 0);
    Reject(&f);
    Init(&f, in_inode);
    cap = Capacity(&f, in_inode);
    Put(Entry(&f, in_inode), 1, (__u16)(cap - 1), 0);
    Reject(&f); /* The byte fits, but its format padding does not. */
    Init(&f, in_inode);
    e = Entry(&f, in_inode);
    Put(e, 4, (__u16)((char *)e - Base(&f, in_inode)), 0);
    Reject(&f); /* Value overlaps the names. */
    Init(&f, in_inode);
    e = Entry(&f, in_inode);
    Put(e, 4, (__u16)((char *)EXT4_XATTR_NEXT(e) - Base(&f, in_inode)), 0);
    Reject(&f); /* Value overlaps the terminator. */
    Init(&f, in_inode);
    cap = Capacity(&f, in_inode);
    e = Entry(&f, in_inode);
    Put(e, 4, (__u16)(cap - 4), 0);
    memcpy(Base(&f, in_inode) + cap - 4, "data", 4);
    Check(ext4_xattr_fetch(&f.ref) == 0);
    struct ext4_xattr_item *item = container_of(f.ref.ordered_list.next, struct ext4_xattr_item, list_node);
    Check(item->data_size == 4 && !memcmp(item->data, "data", 4));
    Cleanup(&f);
    Init(&f, in_inode);
    Put(Entry(&f, in_inode), 0, UINT16_MAX, 0);
    Check(ext4_xattr_fetch(&f.ref) == 0);
    item = container_of(f.ref.ordered_list.next, struct ext4_xattr_item, list_node);
    Check(item->data_size == 0);
    Cleanup(&f);
    for (unsigned fail = 1; fail <= 2; fail++) {
        Init(&f, in_inode);
        cap = Capacity(&f, in_inode);
        Put(Entry(&f, in_inode), 4, (__u16)(cap - 4), 0);
        FailAt = fail;
        Check(ext4_xattr_fetch(&f.ref) == -ENOMEM);
        Check(!PoolLive && !f.ref.root.rb_node);
    }
    Init(&f, in_inode);
    cap = Capacity(&f, in_inode);
    e = Entry(&f, in_inode);
    Put(e, 4, (__u16)(cap - 4), 0);
    Put(EXT4_XATTR_NEXT(e), UINT32_MAX, (__u16)(cap - 8), 0);
    Reject(&f); /* A corrupt later entry prevents allocation of earlier items. */
    Init(&f, in_inode);
    e = Entry(&f, in_inode);
    memset(e, 0, sizeof(*e));
    e->e_name_len = UINT8_MAX;
    e->e_name_index = EXT4_XATTR_INDEX_USER;
    if (!in_inode) f.fs.BlockSize = 64;
    Reject(&f);
    Init(&f, in_inode);
    e = Entry(&f, in_inode);
    Put(e, 0, 0, 0);
    size_t names_end = (size_t)((char *)EXT4_XATTR_NEXT(e) - Base(&f, in_inode));
    Check(ext4_xattr_fetch_entries(&f.ref, e, Base(&f, in_inode), names_end, in_inode) == -EFSCORRUPTED);
    Check(PoolCalls == 0);
}
static void TestEa(BOOL in_inode)
{
    FIXTURE f;
    Init(&f, in_inode);
    Put(Entry(&f, in_inode), 4, 0, 42);
    Reject(&f); /* Feature absent. */
    Init(&f, in_inode);
    f.fs.sb.ea_inode = TRUE;
    Put(Entry(&f, in_inode), EXT4_XATTR_SIZE_MAX + 1U, 0, 42);
    Reject(&f);
    Init(&f, in_inode);
    f.fs.sb.ea_inode = TRUE;
    Put(Entry(&f, in_inode), EXT4_XATTR_SIZE_MAX, 0, 42);
    Check(ext4_xattr_fetch(&f.ref) == 0);
    Check(EaReads == 1 && LargestAllocation == EXT4_XATTR_SIZE_MAX);
    struct ext4_xattr_item *item = container_of(f.ref.ordered_list.next, struct ext4_xattr_item, list_node);
    Check(item->ea && item->ea_ino == 42 && ((unsigned char *)item->data)[item->data_size-1] == 0x6b);
    Cleanup(&f);
    Init(&f, in_inode);
    f.fs.sb.ea_inode = TRUE;
    Put(Entry(&f, in_inode), 4, 0, 42);
    EaReadError = -EIO;
    Check(ext4_xattr_fetch(&f.ref) == -EIO);
    Check(!PoolLive && !f.ref.root.rb_node);
}
static void TestComparator(void)
{
    struct ext4_xattr_item a = {0}, b = {0};
    a.name = "a"; b.name = "b";
    a.name_len = b.name_len = 1;
    Check(ext4_xattr_item_cmp(&a.node, &a.node) == 0);
    Check(ext4_xattr_item_cmp(&a.node, &b.node) < 0);
    Check(ext4_xattr_item_cmp(&b.node, &a.node) > 0);
    a.is_data = TRUE;
    Check(ext4_xattr_item_cmp(&a.node, &b.node) < 0);
    Check(ext4_xattr_item_cmp(&b.node, &a.node) > 0);
    a.is_data = FALSE;
    a.name_index = 2;
    Check(ext4_xattr_item_cmp(&a.node, &b.node) > 0);
    a.name_index = 0;
    a.name_len = 0;
    Check(ext4_xattr_item_cmp(&a.node, &b.node) < 0);
}
static void TestChecksumGate(void)
{
    FIXTURE f;
    Init(&f, FALSE);
    Put(Entry(&f, FALSE), 4, sizeof(f.block) - 4, 0);
    ChecksumValid = FALSE;
    Check(ext4_xattr_fetch(&f.ref) == -EFSBADCRC);
    Check(ChecksumCalls == 1 && DiagnosticCalls == 1);
    Check(PoolCalls == 0 && PoolLive == 0 && EaReads == 0);
    Check(!f.ref.root.rb_node && f.ref.ordered_list.next == &f.ref.ordered_list);

    Init(&f, FALSE);
    Put(Entry(&f, FALSE), 4, sizeof(f.block) - 4, 0);
    memcpy(f.block + sizeof(f.block) - 4, "data", 4);
    Check(ext4_xattr_fetch(&f.ref) == 0);
    Check(ChecksumCalls == 1 && DiagnosticCalls == 0);
    Cleanup(&f);

    Init(&f, TRUE);
    ChecksumValid = FALSE;
    Check(ext4_xattr_fetch(&f.ref) == 0);
    Check(ChecksumCalls == 0 && DiagnosticCalls == 0);
    Cleanup(&f); /* In-body xattrs rely on the inode checksum, not a block verifier. */
}
static void TestRanges(void)
{
    char buffer[1024];
    struct ext4_xattr_entry entry = {0};
    uint32_t rng = 0x413a20d7U;
    for (unsigned i = 0; i < 1000000; i++) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        __u32 size = (i & 1) ? rng : rng % 1100;
        __u16 offset = (__u16)(rng >> 16);
        size_t cap = rng % sizeof(buffer);
        size_t minimum = (rng >> 8) % 64;
        entry.e_value_size = size;
        entry.e_value_offs = offset;
        uint64_t padded = ((uint64_t)size + 3) & ~UINT64_C(3);
        int expected = !size || (size <= EXT4_XATTR_SIZE_MAX && offset >= minimum &&
            (uint64_t)offset + padded <= cap);
        void *p = ext4_xattr_entry_data(&entry, buffer, cap, minimum);
        Check((p != NULL) == expected);
        if (p) Check(p == buffer + (size ? offset : 0));
    }
}
int main(void)
{
    static_assert(offsetof(MODEL_INODE, i_extra_isize) == EXT4_GOOD_OLD_INODE_SIZE);
    TestLocal(TRUE); TestLocal(FALSE);
    TestEa(TRUE); TestEa(FALSE);
    TestComparator(); TestRanges(); TestChecksumGate();
    FIXTURE f;
    Init(&f, TRUE);
    f.inode.i_extra_isize = UINT16_MAX;
    Reject(&f);
    Init(&f, FALSE);
    f.fs.BlockSize = sizeof(struct ext4_xattr_header) - 1;
    Reject(&f);
    printf("XATTR MODEL: %u checks, 1000000 range cases: ALL PASSED\n", Checks);
    return 0;
}
