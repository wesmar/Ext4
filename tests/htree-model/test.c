/*
 * test.c - the production directory code (src/ext4/DirBlock.c, DirIndex.c,
 * NameHash.c) building directories name by name, every rule checked.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * A directory lives in memory; new blocks come full of garbage, as a block
 * of the volume may. Names are added and removed at random - long and
 * short, by every hash version, with a "weak" hash that gives few values
 * (collisions: leaves continuing one hash) - until the index grows levels
 * and, without large_dir, until it is full. After a change the directory
 * is walked whole:
 *   the root and every node sound (I1, I2), their hashes ascending;
 *   every block reached once, every leaf at the same depth;
 *   every live name inside its leaf's hash range, the continued ranges
 *   included; every entry sound (L1), the tails in place;
 *   the names exactly the ones added and not removed; each found by a
 *   lookup (P1-P2), with its inode; absent names not found;
 *   a walk over the leaves (P2, as a listing) meets every name once.
 * Then damaged indexes: the probe answers or refuses, never loops. First of
 * all, the entry check (L1) against Linux's statement of it, on entries
 * made at the edge of each of its tests.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/ext4/NameHash.c"
#include "../../src/ext4/DirBlock.c"
#include "../../src/ext4/DirIndex.c"

#define MAX_BLOCKS  80000
#define MAX_NAMES   120000

typedef struct {
    unsigned char   Name[256];
    unsigned int    Length;
    unsigned int    Inode;
    int             Live;
} NAME;

static unsigned char   *Blocks[MAX_BLOCKS];
static unsigned int     Count, BlockSize, Weak, Seed[4];
static int              Outstanding, Allocated;
static unsigned long long Checks, Adds, Deletes, Lookups, Splits;
static NAME            *Names;
static unsigned int     NameCount;
static unsigned long long Rng;
static DX_DIR           Dir;
static int              Indexed;

static unsigned int Rand(void)
{
    Rng ^= Rng >> 12;
    Rng ^= Rng << 25;
    Rng ^= Rng >> 27;
    return (unsigned int)((Rng * 2685821657736338717ull) >> 32);
}

static void Fail(const char *What)
{
    printf("FAIL: %s (blocks %u, names %u, bs %u, csum %u, levels %u, weak %u)\n",
           What, Count, NameCount, BlockSize, Dir.G.Csum, Dir.MaxLevels, Weak);
    exit(1);
}

#define CHECK(c, what) do { Checks++; if (!(c)) Fail(what); } while (0)

static int Get(void *Ctx, unsigned int Block, int Index, unsigned char **Data, void **Handle)
{
    (void)Ctx; (void)Index;
    if (Block >= Count)
        return DB_ECORRUPT;
    *Data = Blocks[Block];
    *Handle = (void *)(size_t)(Block + 1);
    Outstanding++;
    return 0;
}

static void Put(void *Ctx, void *Handle)
{
    (void)Ctx; (void)Handle;
    Outstanding--;
}

static int Append(void *Ctx, unsigned int *Block, unsigned char **Data, void **Handle)
{
    unsigned int i;
    (void)Ctx;
    if (Count == MAX_BLOCKS)
        return DB_ENOSPC;
    if (!Blocks[Count])
        Blocks[Count] = (unsigned char *)malloc(4096);     /* the largest block size used */
    for (i = 0; i < BlockSize; i++)     /* a block's previous life */
        Blocks[Count][i] = (unsigned char)Rand();
    *Block = Count;
    *Data = Blocks[Count];
    *Handle = (void *)(size_t)(Count + 1);
    Count++;
    Outstanding++;
    return 0;
}

static int Dirty(void *Ctx, void *Handle, int Index)
{
    (void)Ctx; (void)Handle; (void)Index;
    return 0;
}

static int Hash(void *Ctx, const unsigned char *Name, unsigned int Length, unsigned int Version,
                unsigned int *Major, unsigned int *Minor)
{
    (void)Ctx;
    if (NhHash(Name, Length, Version, Seed, Major, Minor))
        return DB_ECORRUPT;
    if (Weak)
        *Major &= 0xC0000000u;      /* four values: every leaf a collision */
    return 0;
}

static void *Allocate(void *Ctx, unsigned int Bytes) { (void)Ctx; Allocated++; return malloc(Bytes); }
static void Release(void *Ctx, void *Memory) { (void)Ctx; Allocated--; free(Memory); }

typedef struct { const unsigned char *Name; unsigned int Length; } WANT;

static int Match(void *Ctx, const unsigned char *Entry)
{
    WANT *w = (WANT *)Ctx;
    return Entry[DB_E_NAME_LEN] == w->Length && memcmp(Entry + DB_E_NAME, w->Name, w->Length) == 0;
}

/* a new directory: one block, '.' and '..' */
static void NewDirectory(void)
{
    unsigned char *b;
    unsigned int   block;
    void          *h;

    Count = 0;
    Indexed = 0;
    Append(0, &block, &b, &h);
    Put(0, h);
    memset(b, 0, BlockSize);
    DbPut32(b, 2);
    DbSetRecLen(&Dir.G, b, 12);
    b[DB_E_NAME_LEN] = 1;
    b[DB_E_TYPE] = 2;
    b[DB_E_NAME] = '.';
    DbPut32(b + 12, 2);
    DbSetRecLen(&Dir.G, b + 12, DbLeafSize(&Dir.G) - 12);
    b[12 + DB_E_NAME_LEN] = 2;
    b[12 + DB_E_TYPE] = 2;
    b[12 + DB_E_NAME] = '.';
    b[12 + DB_E_NAME + 1] = '.';
    DbInitTail(&Dir.G, b);
}

static int Add(NAME *N)
{
    WANT w;
    int  err;

    w.Name = N->Name;
    w.Length = N->Length;
    if (Indexed)
        return DxAdd(&Dir, N->Name, N->Length, N->Inode, 1, Match, &w);
    {
        unsigned int at;
        err = DbFindSpace(&Dir.G, Blocks[0], DbLeafSize(&Dir.G), N->Length, Match, &w, &at);
        if (err == DB_OK) {
            DbInsert(&Dir.G, Blocks[0], at, N->Inode, 1, N->Name, N->Length);
            return 0;
        }
        if (err != DB_ENOSPC)
            return err;
        Outstanding++;
        {
            int converted;
            err = DxMakeIndexed(&Dir, (void *)1, Blocks[0], Rand() % 3, N->Name, N->Length, N->Inode, 1, &converted);
            CHECK(converted == !err, "a conversion half done");
        }
        Outstanding--;
        if (!err)
            Indexed = 1;
        return err;
    }
}

static int Find(const unsigned char *Name, unsigned int Length, unsigned int *Inode)
{
    WANT           w;
    void          *h;
    unsigned char *data;
    unsigned int   at;
    int            err;

    w.Name = Name;
    w.Length = Length;
    Lookups++;
    if (!Indexed) {
        err = DbSearch(&Dir.G, Blocks[0], DbLeafSize(&Dir.G), Match, &w, &at);
        if (!err)
            *Inode = DbGet32(Blocks[0] + at);
        return err;
    }
    err = DxLookup(&Dir, Name, Length, Match, &w, &h, &data, &at);
    if (!err) {
        *Inode = DbGet32(data + at);
        Put(0, h);
    }
    return err;
}

static int Remove(NAME *N)
{
    WANT           w;
    void          *h;
    unsigned char *data;
    unsigned int   at;
    int            err;

    w.Name = N->Name;
    w.Length = N->Length;
    if (!Indexed) {
        err = DbSearch(&Dir.G, Blocks[0], DbLeafSize(&Dir.G), Match, &w, &at);
        return err ? err : DbDelete(&Dir.G, Blocks[0], DbLeafSize(&Dir.G), at);
    }
    err = DxLookup(&Dir, N->Name, N->Length, Match, &w, &h, &data, &at);
    if (err)
        return err;
    err = DbDelete(&Dir.G, data, DbLeafSize(&Dir.G), at);
    Put(0, h);
    return err;
}

/* ---- the whole directory, walked ---- */

static unsigned char *Seen;
static unsigned int   Live;

static void WalkLeaf(unsigned int Block, unsigned long long Low, unsigned long long High, unsigned int Version)
{
    unsigned char *b = Blocks[Block];
    unsigned int   size = DbLeafSize(&Dir.G), at;

    for (at = 0; at < size; at += DbRecLen(&Dir.G, b + at)) {
        CHECK(DbCheck(&Dir.G, b, size, at) == DB_OK, "a leaf entry is not sound");
        /* names carry their type only where the volume has file types */
        CHECK(DbGet32(b + at) <= 2 || b[at + DB_E_TYPE] == (Dir.G.FileTypes ? 1 : 0), "an entry's file type");
        if (DbGet32(b + at) && b[at + DB_E_NAME_LEN]) {
            unsigned int major, minor;
            Hash(0, b + at + DB_E_NAME, b[at + DB_E_NAME_LEN], Version, &major, &minor);
            CHECK(major >= Low && major < High, "a name outside its leaf's hash range");
            Live++;
        }
    }
    if (Dir.G.Csum) {
        unsigned char *t = b + BlockSize - DB_TAIL_SIZE;
        CHECK(DbGet32(t) == 0 && DbRecLen(&Dir.G, t) == DB_TAIL_SIZE && t[DB_E_TYPE] == DB_TAIL_TYPE,
              "the leaf's tail");
    }
}

/* High is exclusive; a range continued into the next child ends one past
   the shared hash */
static void WalkIndex(unsigned int Block, unsigned int Entries, unsigned int Depth, unsigned int Levels,
                      unsigned long long Low, unsigned long long High, unsigned int Version)
{
    unsigned char *e = Blocks[Block] + Entries;
    unsigned int   n = DxCount(e), i;

    for (i = 0; i < n; i++) {
        unsigned long long lo = i ? (DxHash(e, i) & ~1u) : Low, hi;
        unsigned int       child = DxBlock(e, i);

        if (i + 1 < n) {
            unsigned int next = DxHash(e, i + 1);
            hi = (next & 1) ? (unsigned long long)(next & ~1u) + 1 : next;
            CHECK((next & ~1u) >= (i ? (DxHash(e, i) & ~1u) : 0), "index hashes out of order");
        } else {
            hi = High;
        }
        CHECK(lo >= Low && hi <= High, "a child's range outside its parent's");
        CHECK(child > 0 && child < Count && !Seen[child], "a block reached twice, or none");
        Seen[child] = 1;
        if (Depth == Levels) {
            WalkLeaf(child, lo, hi, Version);
        } else {
            unsigned int entries;
            CHECK(DxReadNode(&Dir.G, Blocks[child], &entries) == DB_OK, "a node is not sound");
            /* one empty entry spanning the node, as every reader expects */
            CHECK(DbGet32(Blocks[child]) == 0 && DbRecLen(&Dir.G, Blocks[child]) == BlockSize &&
                  Blocks[child][DB_E_NAME_LEN] == 0, "a node's empty entry");
            WalkIndex(child, entries, Depth + 1, Levels, lo, hi, Version);
        }
    }
}

static void CheckAll(int LookAll)
{
    unsigned int i, expect = 0, inode;

    Live = 0;
    if (!Indexed) {
        WalkLeaf(0, 0, 0x100000000ull, 0);
        Live -= 2;      /* '.' and '..' */
    } else {
        DX_ROOT root;
        DX_PATH path;
        unsigned int leaves = 0, leaf, i2;
        int more;

        Seen = (unsigned char *)calloc(Count, 1);
        CHECK(DxReadRoot(&Dir.G, Blocks[0], Dir.MaxLevels, &root) == DB_OK, "the root is not sound");
        /* '.' and '..', the second spanning the rest of the block */
        CHECK(DbRecLen(&Dir.G, Blocks[0]) == 12 && DbRecLen(&Dir.G, Blocks[0] + 12) == BlockSize - 12,
              "the root's '.' and '..'");
        Seen[0] = 1;
        WalkIndex(0, root.Entries, 0, root.Levels, 0, 0x100000000ull, root.Version + Dir.UnsignedDelta);
        for (i2 = 0; i2 < Count; i2++)
            CHECK(Seen[i2], "a block the index does not reach");
        free(Seen);

        /* the listing's walk: every leaf once, in order */
        CHECK(DxProbe(&Dir, 0, 0, 0, &path) == DB_OK, "a probe from the start");
        leaf = DxLeaf(&path);
        do {
            leaves++;
            leaf = DxLeaf(&path);
            (void)leaf;
            more = DxNextLeaf(&Dir, &path, DX_NEXT_ALWAYS, 0);
            CHECK(more >= 0, "the listing's walk failed");
        } while (more == 1);
        DxRelease(&Dir, &path);
        CHECK(Outstanding == 0, "a block held after the walk");
        (void)leaves;
    }
    for (i = 0; i < NameCount; i++)
        expect += Names[i].Live;
    CHECK(Live == expect, "the names are not the ones added");

    if (LookAll) {
        for (i = 0; i < NameCount; i++) {
            int err = Find(Names[i].Name, Names[i].Length, &inode);
            if (Names[i].Live)
                CHECK(err == DB_OK && inode == Names[i].Inode, "a name not found");
            else
                CHECK(err == DB_ENOENT, "a removed name found");
        }
    }
    CHECK(Outstanding == 0, "a block held");
    CHECK(Allocated == 0, "memory not released");
}

/* every name once: a name made again is the same record (open addressing) */
#define TABLE_SLOTS 262144
static int *Table;

static unsigned int NameKey(const unsigned char *Name, unsigned int Length)
{
    unsigned int h = 2166136261u, i;
    for (i = 0; i < Length; i++)
        h = (h ^ Name[i]) * 16777619u;
    return h & (TABLE_SLOTS - 1);
}

/* the record of this name, or -1; *Slot: where it is or would go */
static int Record(const unsigned char *Name, unsigned int Length, unsigned int *Slot)
{
    unsigned int s = NameKey(Name, Length);
    for (; Table[s] >= 0; s = (s + 1) & (TABLE_SLOTS - 1)) {
        NAME *n = &Names[Table[s]];
        if (n->Length == Length && memcmp(n->Name, Name, Length) == 0) {
            *Slot = s;
            return Table[s];
        }
    }
    *Slot = s;
    return -1;
}

static void MakeName(NAME *N, unsigned int Index, int Long)
{
    unsigned int i;

    N->Length = Long ? 200 + Rand() % 56 : 1 + Rand() % (Rand() % 4 ? 24 : 255);
    for (i = 0; i < N->Length; i++)
        N->Name[i] = (unsigned char)(1 + Rand() % 255);
    if (N->Length <= 2 && N->Name[0] == '.')
        N->Name[0] = 'x';
    N->Inode = 11 + Index;
    N->Live = 0;
}

/* one directory: Ops changes, a full check every Every of them */
static void Run(unsigned int Bs, unsigned int Csum, unsigned int Levels, unsigned int Delta, unsigned int WeakHash,
                unsigned int Ops, unsigned int Every, int LongNames, int Fill)
{
    unsigned int op, i;

    BlockSize = Bs;
    Weak = WeakHash;
    Dir.G.BlockSize = Bs;
    Dir.G.Csum = Csum;
    Dir.G.InodesCount = 0xFFFFFFFFu;
    Dir.G.FileTypes = Rand() & 1;
    Dir.MaxLevels = Levels;
    Dir.UnsignedDelta = Delta;
    for (i = 0; i < 4; i++)
        Seed[i] = Rand() % 3 ? Rand() : 0;
    NameCount = 0;
    memset(Table, 0xFF, sizeof(int) * TABLE_SLOTS);
    NewDirectory();

    for (op = 0; op < Ops; op++) {
        unsigned int r = Rand() % 100;
        int err;

        if (NameCount < MAX_NAMES && (Fill || r < 65 || NameCount == 0)) {
            NAME *n = &Names[NameCount];
            unsigned int inode, slot;
            int old;
            MakeName(n, NameCount, LongNames);
            /* looked up first, as the driver does: a name may be in a leaf
               after the one its hash leads to (a continued hash) */
            if (Find(n->Name, n->Length, &inode) == DB_OK)
                continue;
            old = Record(n->Name, n->Length, &slot);
            if (old >= 0) {
                /* a name removed before comes back, as the same record */
                CHECK(!Names[old].Live, "a live name the lookup did not find");
                err = Add(&Names[old]);
                CHECK(err == DB_OK, "an add failed");
                Names[old].Live = 1;
                Adds++;
                continue;
            }
            {
                /* a level comes only when every index block of the path is
                   full, the root first of all (P3) */
                DX_ROOT before, after;
                unsigned int full = Indexed && DxReadRoot(&Dir.G, Blocks[0], Dir.MaxLevels, &before) == DB_OK &&
                                    DxCount(Blocks[0] + before.Entries) == DxLimit(Blocks[0] + before.Entries);
                unsigned int levels = Indexed ? Blocks[0][DX_ROOT_INFO + DX_INFO_LEVELS] : 0;

                err = Add(n);
                if (err == DB_OK && Indexed && DxReadRoot(&Dir.G, Blocks[0], Dir.MaxLevels, &after) == DB_OK &&
                    after.Levels > levels)
                    CHECK(full, "a level added under a root with room");
            }
            CHECK(err != DB_EEXIST, "a name the lookup did not find exists");
            if (err == DB_ENOSPC && Fill) {
                CHECK(Levels == 2, "a large_dir index full");
                break;              /* the index is full: what the rules allow */
            }
            CHECK(err == DB_OK, "an add failed");
            n->Live = 1;
            Table[slot] = (int)NameCount;
            NameCount++;
            Adds++;
        } else if (r < 90) {
            NAME *n = &Names[Rand() % NameCount];
            err = Remove(n);
            if (n->Live) {
                CHECK(err == DB_OK, "a remove failed");
                n->Live = 0;
                Deletes++;
            } else {
                CHECK(err == DB_ENOENT, "a removed name removed again");
            }
        } else {
            NAME *n = &Names[Rand() % NameCount];
            unsigned int inode;
            err = Find(n->Name, n->Length, &inode);
            CHECK(n->Live ? (err == DB_OK && inode == n->Inode) : err == DB_ENOENT, "a lookup");
        }
        if ((op + 1) % Every == 0)
            CheckAll(1);
    }
    CheckAll(1);
    Splits += Count;
}

/* a node that names itself below: the probe refuses the path (Linux: "tree
   cycle"), whatever the node looks like */
static void Cycle(void)
{
    DX_ROOT  root;
    DX_PATH  path;
    unsigned int i, tried = 0;

    if (!Indexed || DxReadRoot(&Dir.G, Blocks[0], Dir.MaxLevels, &root) || root.Levels < 2)
        return;
    for (i = 0; i < NameCount && tried < 200; i += 97) {
        unsigned int node, at, keep;
        unsigned char *e;

        if (DxProbe(&Dir, Names[i].Name, Names[i].Length, 0, &path))
            continue;
        node = DxBlock(path.Frame[0].Data + path.Frame[0].Entries, path.Frame[0].At);
        e = path.Frame[1].Data + path.Frame[1].Entries;
        at = path.Frame[1].At;
        DxRelease(&Dir, &path);
        keep = DbGet32(e + at * DX_ENTRY_SIZE + 4);
        DbPut32(e + at * DX_ENTRY_SIZE + 4, node);
        CHECK(DxProbe(&Dir, Names[i].Name, Names[i].Length, 0, &path) == DB_ECORRUPT, "a cycle in the index followed");
        CHECK(Outstanding == 0, "a cycle refused, a block held");
        DbPut32(e + at * DX_ENTRY_SIZE + 4, keep);
        tried++;
    }
    CHECK(tried > 0, "no cycle tried");
}

/* a leaf of free entries too short for the name: full without a name in
   it, which no split can mend - refused as damage, nothing changed */
static void EmptyFull(void)
{
    NAME          n;
    WANT          w;
    DX_PATH       path;
    unsigned char *leaf, keep[4096];
    unsigned int  at, block;

    if (!Indexed)
        return;
    MakeName(&n, NameCount, 1);
    w.Name = n.Name;
    w.Length = n.Length;
    if (DxProbe(&Dir, n.Name, n.Length, 0, &path))
        return;
    block = DxLeaf(&path);
    DxRelease(&Dir, &path);
    leaf = Blocks[block];
    memcpy(keep, leaf, BlockSize);
    /* 12-byte free entries, the last one to the region's end: sound (L1),
       and no room in any of them for a long name */
    for (at = 0; at + 24 <= DbLeafSize(&Dir.G); at += 12) {
        memset(leaf + at, 0, 12);
        DbSetRecLen(&Dir.G, leaf + at, 12);
    }
    memset(leaf + at, 0, DbLeafSize(&Dir.G) - at);
    DbSetRecLen(&Dir.G, leaf + at, DbLeafSize(&Dir.G) - at);
    /* (an index block above it may split first, as in Linux: that change is sound) */
    CHECK(DxAdd(&Dir, n.Name, n.Length, n.Inode, 1, Match, &w) == DB_ECORRUPT, "a full leaf without names split");
    CHECK(Outstanding == 0 && Allocated == 0, "a full leaf without names leaked");
    memcpy(leaf, keep, BlockSize);
}

/* damaged indexes: answers or refusals, never a loop or a wild read */
static void Damage(unsigned int Rounds)
{
    unsigned int r, i;

    for (r = 0; r < Rounds; r++) {
        Run(1024, Rand() & 1, 3, 0, Rand() & 1, 600, 600, 0, 0);
        if (!Indexed)
            continue;
        for (i = 0; i < 40; i++) {
            unsigned int   block = Rand() % Count, at = Rand() % BlockSize, entries, inode;
            unsigned char *keep = (unsigned char *)malloc(BlockSize);
            NAME          *n = &Names[Rand() % NameCount];

            memcpy(keep, Blocks[block], BlockSize);
            if (Rand() & 1)
                Blocks[block][at] = (unsigned char)Rand();
            else if (block == 0 || DxReadNode(&Dir.G, Blocks[block], &entries) == DB_OK)
                /* a child pointer bent: back to the root, past the end, anywhere */
                DbPut32(Blocks[block] + (block ? DX_NODE_ENTRIES : DX_ROOT_INFO + DX_INFO_SIZE) + 4 +
                        DX_ENTRY_SIZE * (Rand() % 3), Rand() % 3 ? 0 : Rand() % (Count + 2));
            Find(n->Name, n->Length, &inode);
            CHECK(Outstanding == 0 && Allocated == 0, "a damaged index leaked");
            memcpy(Blocks[block], keep, BlockSize);
            free(keep);
        }
    }
}

/* L1 as Linux states it (__ext4_check_dir_entry), each test in its order */
static int RefCheck(unsigned int Inodes, const unsigned char *Block, unsigned int Size, unsigned int Offset)
{
    const unsigned char *de = Block + Offset;
    unsigned int rlen = de[4] | de[5] << 8, next = Offset + rlen;

    if (rlen < 12)
        return 1;                       /* smaller than minimal */
    if (rlen % 4)
        return 1;
    if (rlen < ((8u + de[6] + 3) & ~3u))
        return 1;                       /* too small for name_len */
    if (next > Size)
        return 1;                       /* overrun */
    if (next > Size - 12 && next != Size)
        return 1;                       /* too close to block end */
    if ((de[0] | de[1] << 8 | de[2] << 16 | (unsigned int)de[3] << 24) > Inodes)
        return 1;                       /* inode out of bounds */
    return 0;
}

/* a value near one that matters, or anything */
static unsigned int Near(unsigned int Value)
{
    unsigned int r = Rand() % 8;
    return r < 6 ? Value + r - 3 : Rand();
}

/* DbCheck against the statement of L1: entries made at the edges of every test */
static void Entries(unsigned int Rounds)
{
    unsigned char block[4096];
    unsigned int r;

    for (r = 0; r < Rounds; r++) {
        unsigned int bs = 1024u << (Rand() % 3), size = bs - (Rand() & 1 ? DB_TAIL_SIZE : 0);
        unsigned int at = (Rand() % (size / 4)) * 4, rlen, inodes = 1 + Rand() % 100000, inode, i;
        DB_GEOMETRY g;

        g.BlockSize = bs;
        g.Csum = size != bs;
        g.InodesCount = inodes;
        g.FileTypes = 1;
        for (i = 0; i < bs; i++)
            block[i] = (unsigned char)Rand();
        if (at + DB_E_NAME > size)
            at = size - DB_E_NAME;
        switch (Rand() % 6) {
        case 0:  rlen = Near(12); break;
        case 1:  rlen = Near(size - at); break;
        case 2:  rlen = Near(size - at - 12); break;
        case 3:  rlen = Near((8u + block[at + DB_E_NAME_LEN] + 3) & ~3u); break;
        case 4:  rlen = (Rand() % (size - at + 8)) & ~3u; break;
        default: rlen = Rand() & 0xFFFF; break;
        }
        rlen &= 0xFFFF;
        inode = Rand() % 3 ? Near(inodes) : Rand() % 2 ? 0 : Rand();
        DbPut16(block + at + DB_E_REC_LEN, rlen);
        DbPut32(block + at + DB_E_INODE, inode);
        CHECK((DbCheck(&g, block, size, at) != DB_OK) == RefCheck(inodes, block, size, at),
              "an entry judged unlike L1 says");
    }
}

/* L2 as Linux states it (ext4_find_dest_de): the first entry, ending no
   later than the region allows, whose free room holds the record - unless
   a live entry of the same name comes first (one after it goes unseen:
   the caller looked the name up) */
static int RefFindSpace(const unsigned char *Block, unsigned int Size, unsigned int Length,
                        const unsigned char *Name, unsigned int *Offset)
{
    unsigned int need = (8u + Length + 3) & ~3u, at = 0;

    while (at + need <= Size) {
        const unsigned char *de = Block + at;
        unsigned int rlen = de[4] | de[5] << 8, nlen = (8u + de[6] + 3) & ~3u;
        unsigned int live = (de[0] | de[1] | de[2] | de[3]) != 0;

        if (live && Name && de[6] == Length && memcmp(de + 8, Name, Length) == 0)
            return DB_EEXIST;
        if ((live ? rlen - nlen : rlen) >= need) {
            *Offset = at;
            return DB_OK;
        }
        at += rlen;
    }
    return DB_ENOSPC;
}

/* the next entry of a test's walk: a record too short fails, never loops */
static unsigned int Step(const DB_GEOMETRY *G, const unsigned char *Block, unsigned int At)
{
    unsigned int length = DbRecLen(G, Block + At);

    CHECK(length >= 12, "a record too short in a leaf the code made");
    return At + length;
}

static int Fnv(void *Ctx, const unsigned char *Name, unsigned int Length, unsigned int *Hash)
{
    unsigned int h = 2166136261u, i;

    (void)Ctx;
    for (i = 0; i < Length; i++)
        h = (h ^ Name[i]) * 16777619u;
    *Hash = h & ~1u;
    return 0;
}

/* DbFindSpace against L2's statement, on leaves filled and thinned at random
   (sound ones: L1 holds for every entry) */
static void Space(unsigned int Rounds)
{
    unsigned char block[4096];
    unsigned int r;

    for (r = 0; r < Rounds; r++) {
        unsigned int bs = 1024u << (Rand() % 3), i, at, want, got = 0, length;
        DB_GEOMETRY g;
        int err, ref;

        g.BlockSize = bs;
        g.Csum = Rand() & 1;
        g.InodesCount = 0xFFFFFFFFu;
        g.FileTypes = 1;
        DbInitLeaf(&g, block);
        for (i = 0; i < 400; i++) {
            unsigned char name[255];
            unsigned int k;

            length = 1 + Rand() % (Rand() % 4 ? 40 : 255);
            for (k = 0; k < length; k++)
                name[k] = (unsigned char)(1 + Rand() % 255);
            if (Rand() % 3 == 0) {
                /* a live entry goes: any one of them */
                unsigned int pick = Rand() % 64, n = 0;
                for (at = 0; at < DbLeafSize(&g); at = Step(&g, block, at))
                    if (DbGet32(block + at) && n++ == pick)
                        break;
                if (at < DbLeafSize(&g))
                    DbDelete(&g, block, DbLeafSize(&g), at);
            } else if (DbFindSpace(&g, block, DbLeafSize(&g), length, 0, 0, &at) == DB_OK) {
                DbInsert(&g, block, at, 11 + i, 1, name, length);
            }
        }
        /* a free entry anywhere, the last one too, as other code leaves one:
           a live entry whose inode went to 0 */
        if (Rand() % 2) {
            unsigned int pick = Rand() % 64, n = 0;
            for (at = 0; at < DbLeafSize(&g); at = Step(&g, block, at))
                if (DbGet32(block + at) && n++ == pick)
                    break;
            if (at >= DbLeafSize(&g))
                for (at = 0; Step(&g, block, at) < DbLeafSize(&g); at = Step(&g, block, at))
                    ;
            DbPut32(block + at, 0);
        }
        length = 1 + Rand() % 255;
        err = DbFindSpace(&g, block, DbLeafSize(&g), length, 0, 0, &got);
        ref = RefFindSpace(block, DbLeafSize(&g), length, 0, &want);
        CHECK(err == ref && (err != DB_OK || got == want), "room found unlike L2 says");

        /* a name there is found, and refused as a new one; once its inode is
           0 - with the name left in place, as other code leaves it - it is
           neither */
        for (at = 0; at < DbLeafSize(&g) && !DbGet32(block + at); at = Step(&g, block, at))
            ;
        if (at < DbLeafSize(&g)) {
            WANT w;
            unsigned char name[255];

            w.Length = block[at + DB_E_NAME_LEN];
            memcpy(name, block + at + DB_E_NAME, w.Length);
            w.Name = name;
            CHECK(DbSearch(&g, block, DbLeafSize(&g), Match, &w, &got) == DB_OK && got == at, "a name not found in its leaf");
            err = DbFindSpace(&g, block, DbLeafSize(&g), w.Length, Match, &w, &got);
            ref = RefFindSpace(block, DbLeafSize(&g), w.Length, name, &want);
            CHECK(err == ref && (err != DB_OK || got == want), "a name there added again unlike L2 says");
            {
                static DB_MAP map[4096 / 12 + 1];
                int before = DbMap(&g, block, Fnv, 0, map), after;

                DbPut32(block + at, 0);
                after = DbMap(&g, block, Fnv, 0, map);
                CHECK(before > 0 && after == before - 1, "a free entry mapped for a split");
            }
            err = DbSearch(&g, block, DbLeafSize(&g), Match, &w, &got);
            CHECK(err == DB_ENOENT || (err == DB_OK && got != at), "a free entry found by its old name");
        }
    }
}

/* I1, I2 as Linux states them (dx_probe, dx_root_limit, dx_node_limit) */
static unsigned int RefRootLimit(unsigned int Bs, unsigned int Csum, unsigned int Info)
{
    return (Bs - 12 - 12 - Info - (Csum ? 8 : 0)) / 8;
}

static unsigned int RefNodeLimit(unsigned int Bs, unsigned int Csum)
{
    return (Bs - 8 - (Csum ? 8 : 0)) / 8;
}

static int RefRoot(unsigned int Bs, unsigned int Csum, unsigned int MaxLevels, const unsigned char *B)
{
    unsigned int info = B[24 + 5], at = 24 + info, limit, count;

    if (B[24 + 4] > 2 || (B[24 + 7] & 1) || B[24 + 6] >= MaxLevels)
        return 1;                               /* hash, flags, depth */
    if (24 + info + 4 > Bs)
        return 1;                               /* (Linux reads past the block) */
    limit = B[at] | B[at + 1] << 8;
    count = B[at + 2] | B[at + 3] << 8;
    if (Bs < 24 + info + (Csum ? 8 : 0) || limit != RefRootLimit(Bs, Csum, info))
        return 1;
    return count == 0 || count > limit;
}

static int RefNode(unsigned int Bs, unsigned int Csum, const unsigned char *B)
{
    unsigned int limit = B[8] | B[9] << 8, count = B[10] | B[11] << 8;

    return limit != RefNodeLimit(Bs, Csum) || count == 0 || count > limit;
}

/* the root and node checks against I1, I2's statement, at the edge of each */
static void Indexes(unsigned int Rounds)
{
    unsigned char block[4096];
    unsigned int r;

    for (r = 0; r < Rounds; r++) {
        unsigned int bs = 1024u << (Rand() % 3), csum = Rand() & 1, levels = 2 + (Rand() & 1), i, at, limit;
        unsigned int entries;
        DB_GEOMETRY g;
        DX_ROOT root;

        g.BlockSize = bs;
        g.Csum = csum;
        g.InodesCount = 0xFFFFFFFFu;
        g.FileTypes = 1;
        CHECK(DxRootLimit(&g, 8) == RefRootLimit(bs, csum, 8) && DxNodeLimit(&g) == RefNodeLimit(bs, csum),
              "an index limit unlike Linux's");
        for (i = 0; i < bs; i++)
            block[i] = (unsigned char)Rand();
        /* the root: version, length, levels, flags near the bounds */
        block[24 + 4] = (unsigned char)(Rand() % 4 ? Rand() % 3 : Near(2));
        block[24 + 5] = (unsigned char)(Rand() % 4 ? 8 : Near(8));
        block[24 + 6] = (unsigned char)(Rand() % 4 ? Rand() % levels : Near(levels));
        block[24 + 7] = (unsigned char)(Rand() % 4 ? 0 : Rand());
        at = 24 + block[24 + 5];
        if (at + 4 <= bs) {
            limit = Rand() % 4 ? RefRootLimit(bs, csum, block[24 + 5]) : Near(RefRootLimit(bs, csum, block[24 + 5]));
            DbPut16(block + at, limit);
            DbPut16(block + at + 2, Rand() % 3 ? Near(limit) : Rand() % 2 ? 0 : 1 + Rand() % (limit ? limit : 1));
        }
        CHECK((DxReadRoot(&g, block, levels, &root) != DB_OK) == RefRoot(bs, csum, levels, block),
              "a root judged unlike Linux's dx_probe");
        /* a node */
        limit = Rand() % 4 ? RefNodeLimit(bs, csum) : Near(RefNodeLimit(bs, csum));
        DbPut16(block + 8, limit);
        DbPut16(block + 10, Rand() % 3 ? Near(limit) : Rand() % 2 ? 0 : 1 + Rand() % (limit ? limit : 1));
        CHECK((DxReadNode(&g, block, &entries) != DB_OK) == RefNode(bs, csum, block),
              "a node judged unlike Linux's dx_probe");
    }
}

int main(int argc, char **argv)
{
    unsigned long long seed = argc > 1 ? strtoull(argv[1], NULL, 10) : 1;
    unsigned int k;

    Rng = seed * 0x9E3779B97F4A7C15ull + 7;
    Names = (NAME *)malloc(sizeof(NAME) * MAX_NAMES);
    Table = (int *)malloc(sizeof(int) * TABLE_SLOTS);
    Dir.Io.Context = 0;
    Dir.Io.Get = Get;
    Dir.Io.Put = Put;
    Dir.Io.Append = Append;
    Dir.Io.Dirty = Dirty;
    Dir.Io.Hash = Hash;
    Dir.Io.Allocate = Allocate;
    Dir.Io.Release = Release;
    Dir.Io.Corrupt = 0;

    Entries(2000000);
    Space(200000);
    Indexes(2000000);

    /* every block size, checksums or not, hash version and signedness,
       weak hashes: small directories checked after every change */
    for (k = 0; k < 96; k++)
        Run(1024u << (k % 3), (k / 3) & 1, 2 + ((k / 6) & 1), ((k / 12) & 1) ? 3 : 0, (k / 24) & 1,
            700, 1, (k / 48) & 1, 0);
    /* deeper: two and three levels, checked every 500 changes */
    for (k = 0; k < 8; k++)
        Run(1024, k & 1, 3, 0, (k / 2) & 1, 30000, 500, 1, 1);
    /* three levels: large_dir, long names in 1 KiB blocks */
    Run(1024, 1, 3, 0, 0, 70000, 10000, 1, 1);
    Cycle();
    EmptyFull();
    /* without large_dir until the index is full */
    Run(1024, 1, 2, 0, 0, MAX_NAMES, 5000, 1, 1);
    Run(1024, 0, 2, 3, 0, MAX_NAMES, 5000, 1, 1);
    Damage(60);

    printf("HTREE-MODEL: seed %llu: %llu adds, %llu removes, %llu lookups, %llu blocks built, %llu checks\n",
           seed, Adds, Deletes, Lookups, Splits, Checks);
    return 0;
}
