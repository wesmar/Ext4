/* SPDX-License-Identifier: GPL-2.0-only
 * Exercise the production run map and red-black tree, including allocation failure.
 */
#include "ext4fs.h"
#include "../../src/linux/rbtree.c"
#include "../../src/core/RunMap.c"
#include <time.h>
typedef uint64_t ULONGLONG;
typedef int32_t NTSTATUS;
typedef struct { ULONG BlockSize, max_data_blocks; } MODEL_VCB, *PEXT2_VCB;
struct inode { ULONG i_flags; };
#define MAXULONG UINT32_MAX
#define INODE_HAS_EXTENT(i) ((i)->i_flags & 0x80000)
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000d)
#define STATUS_FILE_TOO_LARGE ((NTSTATUS)0xc0000904)
#include "../../src/include/FileLimits.h"
#define WIDTH 512
static LONGLONG Oracle[WIDTH];
static uint32_t RandomState = 0x91e10da5;
static uint32_t Random(void)
{
    RandomState ^= RandomState << 13;
    RandomState ^= RandomState >> 17;
    RandomState ^= RandomState << 5;
    return RandomState;
}
static unsigned CheckTree(struct rb_node *node, struct rb_node *parent,
    LONGLONG *end, LONGLONG *physicalEnd, unsigned *count)
{
    EXT4_RUN *r;
    unsigned left, right;
    if (!node) return 1;
    assert(rb_parent(node) == parent);
    if (rb_is_red(node)) {
        assert(!node->rb_left || rb_is_black(node->rb_left));
        assert(!node->rb_right || rb_is_black(node->rb_right));
    }
    left = CheckTree(node->rb_left, node, end, physicalEnd, count);
    r = Run(node);
    assert(r->Length > 0 && r->Physical > 0 && r->Start >= *end);
    assert(r->Start != *end || r->Physical != *physicalEnd);
    *end = r->Start + r->Length;
    *physicalEnd = r->Physical + r->Length;
    (*count)++;
    right = CheckTree(node->rb_right, node, end, physicalEnd, count);
    assert(left == right);
    return left + (unsigned)rb_is_black(node);
}
static void Verify(PEXT4_RUN_MAP map)
{
    LONGLONG lastEnd = -1, physicalEnd = -1, start, physical, length;
    unsigned count = 0, index = 0, expected = 0;
    int i, end;
    if (map->Root.rb_node) assert(rb_is_black(map->Root.rb_node));
    CheckTree(map->Root.rb_node, NULL, &lastEnd, &physicalEnd, &count);
    assert(count == Ext4RunMapCount(map));
    for (i = 0; i < WIDTH; i = end) {
        end = i + 1;
        if (!Oracle[i]) continue;
        while (end < WIDTH && Oracle[end] == Oracle[i] + end - i) end++;
        assert(Ext4RunMapNext(map, index++, &start, &physical, &length));
        assert(start == i && physical == Oracle[i] && length == end - i);
        expected++;
    }
    assert(expected == count);
    assert(!Ext4RunMapNext(map, index, &start, &physical, &length));
    for (i = 0; i < WIDTH; i++) {
        BOOLEAN found = Ext4RunMapLookup(map, i, &physical, &length, NULL, NULL, NULL);
        if (Oracle[i]) {
            assert(found && physical == Oracle[i] && length > 0);
        } else {
            end = i;
            while (end < WIDTH && !Oracle[end]) end++;
            assert(found == (end < WIDTH));
            if (found) assert(physical == -1 && length == end - i);
        }
    }
    if (count) {
        /* Enumeration must also tolerate decreasing and repeated indices. */
        assert(Ext4RunMapNext(map, count - 1, &start, &physical, &length));
        assert(Ext4RunMapNext(map, 0, &start, &physical, &length));
    }
}
int main(int argc, char **argv)
{
    EXT4_RUN_MAP map;
    LONGLONG physical, length;
    unsigned iteration;
    assert(argc <= 2);
    if (argc == 2) RandomState = (uint32_t)strtoull(argv[1], NULL, 0);
    assert(RandomState != 0);
    printf("RUN MAP MODEL: seed=%u\n", RandomState);
    {
        MODEL_VCB volume;
        struct inode inode;
        unsigned bits, extent;
        for (bits = 10; bits <= 12; bits++) {
            ULONGLONG n = 1ULL << (bits - 2);
            volume.BlockSize = 1U << bits;
            volume.max_data_blocks = (ULONG)(12 + n + n*n + n*n*n);
            for (extent = 0; extent <= 1; extent++) {
                ULONGLONG limit;
                inode.i_flags = extent ? 0x80000 : 0;
                limit = Ext4FileSizeLimit(&volume, &inode);
                assert(limit == (extent ? (ULONGLONG)MAXULONG : volume.max_data_blocks) * volume.BlockSize);
                assert(Ext4CheckFileSize(&volume, &inode, (LONGLONG)limit) == STATUS_SUCCESS);
                assert(Ext4CheckFileSize(&volume, &inode, (LONGLONG)limit + 1) == STATUS_FILE_TOO_LARGE);
                assert(Ext4CheckFileSize(&volume, &inode, -1) == STATUS_INVALID_PARAMETER);
                assert(Ext4CheckFileSize(&volume, &inode, MAXLONGLONG) == STATUS_FILE_TOO_LARGE);
                assert(Ext4CheckWriteRange(&volume, &inode, (LONGLONG)limit - 1, 1) == STATUS_SUCCESS);
                assert(Ext4CheckWriteRange(&volume, &inode, (LONGLONG)limit, 1) == STATUS_FILE_TOO_LARGE);
                assert(Ext4CheckWriteRange(&volume, &inode, MAXLONGLONG, MAXULONG) == STATUS_FILE_TOO_LARGE);
                assert(Ext4CheckWriteRange(&volume, &inode, -1, 1) == STATUS_INVALID_PARAMETER);
            }
        }
        puts("FILE LIMIT MODEL: all block sizes, extent/indirect formats and overflow: ALL PASSED");
    }
    Ext4RunMapInitialize(&map);
    assert(Ext4RunMapAdd(&map, 0, (1LL << 48), 512));
    ModelFailAllocation = 1;
    assert(!Ext4RunMapRemove(&map, 100, 10));
    assert(Ext4RunMapLookup(&map, 105, &physical, &length, NULL, NULL, NULL));
    assert(physical == (1LL << 48) + 105 && length == 407);
    assert(Ext4RunMapAdd(&map, 100, (1LL << 48) + 100, 10));
    assert(!Ext4RunMapAdd(&map, 600, (1LL << 48), 10));
    assert(Ext4RunMapRemove(&map, 0, 512));
    ModelFailAllocation = 0;
    assert(Ext4RunMapAdd(&map, MAXLONGLONG - 10, MAXLONGLONG - 10, 10));
    assert(!Ext4RunMapAdd(&map, MAXLONGLONG - 9, MAXLONGLONG - 9, 10));
    assert(Ext4RunMapLookup(&map, MAXLONGLONG - 1, &physical, &length, NULL, NULL, NULL));
    assert(physical == MAXLONGLONG - 1 && length == 1);
    Ext4RunMapClear(&map);
    for (iteration = 0; iteration < 50000; iteration++) {
        unsigned start = Random() % WIDTH;
        unsigned n = 1 + Random() % (WIDTH - start);
        unsigned op = Random() % 3;
        unsigned i;
        BOOLEAN allowed = TRUE, result;
        LONGLONG base = (1LL << 44) + (Random() % 8) * WIDTH + start;
        ModelFailAllocation = (Random() % 11 == 0);
        if (op == 0) {
            for (i = 0; i < n; i++)
                if (Oracle[start + i] && Oracle[start + i] != base + i) allowed = FALSE;
            result = Ext4RunMapAdd(&map, start, base, n);
            assert(!result || allowed);
            if (allowed && !ModelFailAllocation) assert(result);
            if (result) for (i = 0; i < n; i++) Oracle[start + i] = base + i;
        } else {
            result = Ext4RunMapRemove(&map, start, n);
            if (!ModelFailAllocation) assert(result);
            if (result) for (i = 0; i < n; i++) Oracle[start + i] = 0;
        }
        Verify(&map);
    }
    ModelFailAllocation = 0;
    Ext4RunMapClear(&map);
    {
        clock_t began = clock();
        LONGLONG start;
        for (iteration = 0; iteration < 100000; iteration++)
            assert(Ext4RunMapAdd(&map, iteration * 2LL, (1LL << 48) + iteration * 3LL, 1));
        for (iteration = 0; iteration < 100000; iteration++) {
            assert(Ext4RunMapNext(&map, iteration, &start, &physical, &length));
            assert(start == iteration * 2LL && physical == (1LL << 48) + iteration * 3LL);
        }
        for (iteration = 0; iteration < 1000000; iteration++) {
            unsigned position = Random() % 100000;
            assert(Ext4RunMapLookup(&map, position * 2LL, &physical, &length, NULL, NULL, NULL));
            assert(physical == (1LL << 48) + position * 3LL && length == 1);
        }
        printf("Fragmentation: 100000 runs, 1000000 lookups, %.3f s\n",
            (double)(clock() - began) / CLOCKS_PER_SEC);
    }
    Ext4RunMapDestroy(&map);
    assert(!ModelPoolCount);
    puts("RUN MAP MODEL: 50000 operations, 64-bit addresses, RB invariants and OOM: ALL PASSED");
    return 0;
}
