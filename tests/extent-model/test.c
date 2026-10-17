/*
 * test.c - the production extent tree (src/ext4/ExtentCore.c) built and cut
 * at random against a flat map of every logical block, every rule checked.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * A volume lives in memory: every block free, a node of the tree, or data
 * of the file. Files are written the way the driver writes them - holes
 * filled with new extents (next to the last one, or anywhere: merges and
 * fragments), preallocated (unwritten) ranges, written parts of those
 * converted - and truncated, at offsets low and at the top of the 32-bit
 * range, while new nodes fail now and then (a full volume). After a change
 * the tree is walked whole:
 *   every node sound (N1) - checked by the code's own rules and by a
 *   statement of Linux's, each block of the tree reached once, none empty;
 *   the extents exactly the flat map (block, start, kind) and nothing else;
 *   the data blocks the volume holds for the file exactly those, the nodes
 *   exactly the tree's (nothing leaked, nothing freed twice);
 *   every lookup (N2) the map's answer, holes to the next mapped block.
 * First, the node check against Linux's statement of it on nodes made at
 * the edge of each of its tests; last, damaged trees: answers or refusals,
 * never a loop or a wild read.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/ext4/ExtentCore.c"

#define VOLUME      (1u << 20)          /* blocks of the volume */
#define FIRST_DATA  16u                 /* below it: the volume's own metadata */
#define SPAN        (1u << 18)          /* logical blocks the map covers */

enum { FREE = 0, DATA = 1, NODE = 2 };

static unsigned char   Owner[VOLUME];
static unsigned char  *Data[VOLUME];    /* a node's contents */
static unsigned char   Checked[VOLUME];
static unsigned int    Seen[VOLUME / 8 + 1];
static unsigned int    Map[SPAN];       /* physical block of Base + i, 0: a hole */
static unsigned char   Unw[SPAN];
static unsigned int    Base, BlockSize, NodesHeld, DataHeld, FailNew, Outstanding;
static unsigned char   RootBytes[EX_ROOT_BYTES];
static EX_TREE         Tree;
static unsigned long long Rng, Checks, Ops, Inserts, Converts, Truncates, Lookups, NewFailures, MaxDepth;

static unsigned int Rand(void)
{
    Rng ^= Rng >> 12;
    Rng ^= Rng << 25;
    Rng ^= Rng >> 27;
    return (unsigned int)((Rng * 2685821657736338717ull) >> 32);
}

static void Fail(const char *What)
{
    printf("FAIL: %s (op %llu, base %u, bs %u, depth %u)\n", What, Ops, Base, BlockSize, Depth(RootBytes));
    exit(1);
}

#define CHECK(c, what) do { Checks++; if (!(c)) Fail(what); } while (0)

/* ---- the volume ---- */

static int Get(void *Ctx, unsigned long long Block, unsigned char **D, void **Handle, int *IsChecked)
{
    (void)Ctx;
    if (Block >= VOLUME || Owner[Block] != NODE)
        return EX_EIO;          /* (Valid keeps the code away from such blocks: never here) */
    *D = Data[Block];
    *Handle = (void *)(size_t)(Block + 1);
    *IsChecked = Checked[Block];
    Outstanding++;
    return EX_OK;
}

static void SetChecked(void *Ctx, void *Handle) { (void)Ctx; Checked[(size_t)Handle - 1] = 1; }
static void PutNode(void *Ctx, void *Handle) { (void)Ctx; (void)Handle; Outstanding--; }
static int DirtyNode(void *Ctx, void *Handle) { (void)Ctx; (void)Handle; return 0; }

static int New(void *Ctx, unsigned long long Goal, unsigned long long *Block, unsigned char **D, void **Handle)
{
    unsigned int b = (unsigned int)(Goal < VOLUME && Goal >= FIRST_DATA ? Goal : FIRST_DATA + Rand() % (VOLUME - FIRST_DATA)), i;

    (void)Ctx;
    if (FailNew && Rand() % FailNew == 0) {
        NewFailures++;
        return EX_ENOSPC;
    }
    for (i = 0; i < VOLUME && Owner[b] != FREE; i++)
        b = b + 1 < VOLUME ? b + 1 : FIRST_DATA;
    if (Owner[b] != FREE)
        return EX_ENOSPC;
    Owner[b] = NODE;
    Data[b] = (unsigned char *)malloc(BlockSize);
    for (i = 0; i < BlockSize; i++)
        Data[b][i] = (unsigned char)Rand();     /* a new block holds what it held */
    Checked[b] = 0;
    NodesHeld++;
    *Block = b;
    *D = Data[b];
    *Handle = (void *)(size_t)(b + 1);
    Outstanding++;
    return EX_OK;
}

static int FreeBlocks(void *Ctx, unsigned long long Block, unsigned int Count, int Node)
{
    unsigned int i;

    (void)Ctx;
    CHECK(Block + Count <= VOLUME && Count > 0, "a free past the volume");
    for (i = 0; i < Count; i++) {
        CHECK(Owner[Block + i] == (Node ? NODE : DATA), "a block freed that the file does not hold");
        Owner[Block + i] = FREE;
        if (Node) {
            free(Data[Block + i]);
            Data[Block + i] = 0;
            NodesHeld--;
        } else {
            DataHeld--;
        }
    }
    return EX_OK;
}

static int Valid(void *Ctx, unsigned long long Block, unsigned int Count)
{
    (void)Ctx;
    return Block >= FIRST_DATA && Block + Count <= VOLUME && Block + Count > Block;
}

/* ---- the file, as the driver writes it ---- */

/* a run of free data blocks near Goal (or anywhere): its start, *Count cut to it */
static unsigned int Allocate(unsigned int Goal, unsigned int *Count, int Near)
{
    unsigned int b = Near && Goal >= FIRST_DATA && Goal < VOLUME ? Goal : FIRST_DATA + Rand() % (VOLUME - FIRST_DATA);
    unsigned int n = 0, i;

    for (i = 0; i < VOLUME && Owner[b] != FREE; i++)
        b = b + 1 < VOLUME ? b + 1 : FIRST_DATA;
    if (Owner[b] != FREE)
        return 0;
    while (n < *Count && b + n < VOLUME && Owner[b + n] == FREE)
        n++;
    *Count = n;
    for (i = 0; i < n; i++)
        Owner[b + i] = DATA;
    DataHeld += n;
    return b;
}

static void ReleaseData(unsigned int Start, unsigned int Count)
{
    unsigned int i;

    for (i = 0; i < Count; i++)
        Owner[Start + i] = FREE;
    DataHeld -= Count;
}

/* N3 after an insert or a conversion of Block..End-1: where the extent now
   holding it begins at Block, the one before it in the leaf could not take
   it; where it ends at End, the one after it could not (extents a split
   left mergeable elsewhere are no concern of this change, as in Linux) */
static void CheckMerged(unsigned int Block, unsigned long long End)
{
    EX_PATH    p;
    EX_FRAME  *leaf;
    EX_EXTENT  x, y;
    unsigned int at;

    if (Find(&Tree, Block, &p) != EX_OK)
        return;
    leaf = &p.Frame[p.Depth];
    at = (unsigned int)leaf->At;
    ReadExtent(Entry(leaf->Node, at), &x);
    if (at > 0 && x.Block == Block) {
        ReadExtent(Entry(leaf->Node, at - 1), &y);
        CHECK(!Mergeable(&y, &x), "an extent left mergeable with the one before it");
    }
    if (at + 1 < Entries(leaf->Node) && x.Block + (unsigned long long)x.Length == End) {
        ReadExtent(Entry(leaf->Node, at + 1), &y);
        CHECK(!Mergeable(&x, &y), "an extent left mergeable with the one after it");
    }
    Release(&Tree, &p);
}

/* Block..Block+Count-1 of the span: holes filled (unwritten if asked),   unwritten parts converted when written */
static void Write(unsigned int Block, unsigned int Count, int Unwritten, int Near)
{
    unsigned int end = Block + Count;

    while (Block < end) {
        EX_EXTENT x;
        unsigned long long next, goal;
        int err = ExMap(&Tree, Base + Block, &x, &next, &goal);

        Lookups++;
        if (err == EX_OK) {
            unsigned int upto = x.Block - Base + x.Length < end ? x.Block - Base + x.Length : end;

            if (x.Unwritten && !Unwritten) {
                unsigned int i;
                err = ExConvert(&Tree, Base + Block, upto - Block);
                Converts++;
                if (err == EX_OK) {
                    for (i = Block; i < upto; i++)
                        Unw[i] = 0;
                    CheckMerged(Base + Block, Base + (unsigned long long)upto);
                } else {
                    CHECK(err == EX_ENOSPC, "a conversion failed otherwise than for room");
                    return;
                }
            }
            Block = upto;
            continue;
        }
        CHECK(err == EX_ENOENT, "a lookup failed");
        {
            unsigned int room = (unsigned int)((next - Base < end ? next - Base : end) - Block), start, i;
            unsigned int count = room < (Unwritten ? EX_UNWRITTEN_MAX : EX_INIT_MAX) ? room : (Unwritten ? EX_UNWRITTEN_MAX : EX_INIT_MAX);
            EX_EXTENT n;

            CHECK(room > 0, "a hole of no blocks");
            start = Allocate((unsigned int)goal, &count, Near);
            if (!start)
                return;
            n.Block = Base + Block;
            n.Length = count;
            n.Start = start;
            n.Unwritten = Unwritten;
            {
                /* N4: a level comes only under a full root */
                unsigned int depth = Depth(RootBytes), full = Entries(RootBytes) == Max(RootBytes);
                err = ExInsert(&Tree, &n, 1);
                CHECK(err || Depth(RootBytes) <= depth || full, "a level added under a root with room");
            }
            Inserts++;
            if (err) {
                CHECK(err == EX_ENOSPC, "an insert failed otherwise than for room");
                ReleaseData(start, count);
                return;
            }
            for (i = 0; i < count; i++) {
                Map[Block + i] = start + i;
                Unw[Block + i] = (unsigned char)Unwritten;
            }
            CheckMerged(Base + Block, Base + (unsigned long long)Block + count);
            Block += count;
        }
    }
}

static void Truncate(unsigned int From)
{
    unsigned int i;
    int err = ExTruncate(&Tree, Base + From);

    Truncates++;
    CHECK(err == EX_OK, "a truncation failed");
    for (i = From; i < SPAN; i++)
        Map[i] = 0;
}

/* ---- the whole tree, walked ---- */

static unsigned int Walked, WalkedNodes;

/* N1 as Linux states it (__ext4_ext_check, ext4_valid_extent_entries) */
static int RefNode(const unsigned char *N, unsigned int Depth, int Root, unsigned int Key0)
{
    unsigned int entries = N[2] | N[3] << 8, max = N[4] | N[5] << 8, depth = N[6] | N[7] << 8, i;
    unsigned int room = Root ? 4 : (BlockSize - 12) / 12;
    unsigned long long cur = 0;

    if ((N[0] | N[1] << 8) != 0xF30A || depth != Depth || max == 0 || max > room || entries > max)
        return 1;
    if (entries == 0 && depth > 0)
        return 1;
    if (depth > 5)
        return 1;
    for (i = 0; i < entries; i++) {
        const unsigned char *e = N + 12 + 12 * i;
        unsigned int lblk = e[0] | e[1] << 8 | e[2] << 16 | (unsigned int)e[3] << 24;

        if (!Root && i == 0 && lblk != Key0)
            return 1;               /* the parent's key */
        if (depth == 0) {
            unsigned int len = e[4] | e[5] << 8;
            unsigned long long start = (unsigned long long)(e[6] | e[7] << 8) << 32 |
                                       (e[8] | e[9] << 8 | e[10] << 16 | (unsigned int)e[11] << 24);
            len = len > 32768 ? len - 32768 : len;
            if (len == 0 || lblk + (unsigned long long)len - 1 > 0xFFFFFFFFull || !Valid(0, start, len) || lblk < cur)
                return 1;
            cur = lblk + (unsigned long long)len;
        } else {
            unsigned long long leaf = (unsigned long long)(e[8] | e[9] << 8) << 32 |
                                      (e[4] | e[5] << 8 | e[6] << 16 | (unsigned int)e[7] << 24);
            if (!Valid(0, leaf, 1) || lblk < cur)
                return 1;
            cur = lblk + 1ull;
        }
    }
    return 0;
}

static void WalkNode(const unsigned char *N, unsigned int Depth, int Root, unsigned int Key0)
{
    unsigned int entries = Entries(N), i;

    CHECK(CheckNode(&Tree, N, Depth, Root, Key0) == EX_OK, "a node is not sound");
    CHECK(RefNode(N, Depth, Root, Key0) == 0, "a node is not sound as Linux states it");
    CHECK(Root || entries > 0, "an empty node left in the tree");
    for (i = 0; i < entries; i++) {
        const unsigned char *e = N + EX_HEADER + i * EX_ENTRY;

        if (Depth == 0) {
            EX_EXTENT x;
            unsigned int k;

            ReadExtent(e, &x);
            CHECK(x.Length <= (x.Unwritten ? EX_UNWRITTEN_MAX : EX_INIT_MAX), "an extent longer than its kind holds");
            for (k = 0; k < x.Length; k++) {
                unsigned int l = x.Block + k - Base;
                CHECK(x.Block + k >= Base && l < SPAN, "an extent outside the span written");
                CHECK(Map[l] == x.Start + k && Unw[l] == (unsigned char)x.Unwritten, "an extent unlike the map");
                CHECK(Owner[x.Start + k] == DATA, "an extent on a block not the file's");
                CHECK(!(Seen[(x.Start + k) / 32] & (1u << ((x.Start + k) % 32))), "a block in two extents");
                Seen[(x.Start + k) / 32] |= 1u << ((x.Start + k) % 32);
            }
            Walked += x.Length;
        } else {
            unsigned long long child = Child(e);

            CHECK(child < VOLUME && Owner[child] == NODE, "an index to a block not a node");
            CHECK(!(Seen[child / 32] & (1u << (child % 32))), "a node reached twice");
            Seen[child / 32] |= 1u << (child % 32);
            WalkedNodes++;
            WalkNode(Data[child], Depth - 1, 0, Get32(e));
        }
    }
}

static void CheckAll(int LookAll)
{
    unsigned int i, mapped = 0, n;

    memset(Seen, 0, sizeof(Seen));
    Walked = WalkedNodes = 0;
    WalkNode(RootBytes, Depth(RootBytes), 1, 0);
    for (i = 0; i < SPAN; i++)
        mapped += Map[i] != 0;
    CHECK(Walked == mapped, "the map holds blocks the tree does not");
    CHECK(DataHeld == mapped, "data blocks held that the tree does not map");
    CHECK(WalkedNodes == NodesHeld, "nodes held that the tree does not reach");
    CHECK(Outstanding == 0, "a node held after a change");
    if (Depth(RootBytes) > MaxDepth)
        MaxDepth = Depth(RootBytes);

    /* N2: lookups, every block or some, the holes' lengths */
    for (n = 0; n < (LookAll ? SPAN : 2000u); n++) {
        unsigned int l = LookAll ? n : Rand() % SPAN, k;
        EX_EXTENT x;
        unsigned long long next, goal;
        int err = ExMap(&Tree, Base + l, &x, &next, &goal);

        Lookups++;
        if (Map[l]) {
            CHECK(err == EX_OK && x.Start + (Base + l - x.Block) == Map[l] && x.Unwritten == Unw[l], "a lookup unlike the map");
        } else {
            CHECK(err == EX_ENOENT, "a hole found mapped");
            for (k = l + 1; k < SPAN && !Map[k]; k++)
                ;
            /* nothing is mapped outside the span: past its last extent, no end */
            CHECK(next == (k < SPAN ? Base + (unsigned long long)k : EX_END), "a hole's end unlike the map");
        }
    }
    CHECK(Outstanding == 0, "a node held after a lookup");
}

static void NewTree(unsigned int Bs, unsigned int TreeBase)
{
    unsigned int i;

    for (i = 0; i < VOLUME; i++) {
        if (Data[i])
            free(Data[i]);
        Data[i] = 0;
        Owner[i] = FREE;
        Checked[i] = 0;
    }
    memset(Map, 0, sizeof(Map));
    memset(Unw, 0, sizeof(Unw));
    NodesHeld = DataHeld = 0;
    BlockSize = Bs;
    Base = TreeBase;
    Tree.Root = RootBytes;
    Tree.BlockSize = Bs;
    Tree.Goal = FIRST_DATA + Rand() % 1000;
    ExInit(&Tree);
}

/* one file: Ops changes, a full check every Every */
static void Run(unsigned int Bs, unsigned int TreeBase, unsigned int Ops0, unsigned int Every, int Fragmented, unsigned int FailRate)
{
    unsigned int op;

    NewTree(Bs, TreeBase);
    FailNew = FailRate;
    for (op = 0; op < Ops0; op++) {
        unsigned int r = Rand() % 100;

        Ops++;
        if (Fragmented) {
            /* single blocks two apart, anywhere: no merges, a deep tree of
               full leaves; then the blocks between them, at random - splits
               in the middle of full leaves, extents going into the next */
            unsigned int l = op < Ops0 / 2 ? (op * 2) % SPAN : (Rand() % (Ops0 / 2)) * 2 + 1;
            Write(l % SPAN, 1, Rand() % 4 == 0, 0);
        } else if (r < 45) {
            unsigned int l = Rand() % SPAN, n = 1 + (Rand() % 4 ? Rand() % 64 : Rand() % 4000);
            Write(l, l + n <= SPAN ? n : SPAN - l, 0, Rand() % 3 != 0);
        } else if (r < 70) {
            unsigned int l = Rand() % SPAN, n = 1 + (Rand() % 4 ? Rand() % 256 : Rand() % 40000);
            Write(l, l + n <= SPAN ? n : SPAN - l, 1, Rand() % 2);
        } else if (r < 85) {
            /* written into what was preallocated */
            unsigned int l = Rand() % SPAN, n = 1 + Rand() % 300;
            Write(l, l + n <= SPAN ? n : SPAN - l, 0, 1);
        } else if (r < 92) {
            Truncate(Rand() % 3 ? Rand() % SPAN : SPAN - 1 - Rand() % 1000);
        } else {
            /* appends, next to the last extent */
            unsigned int l, n = 1 + Rand() % 100;
            for (l = SPAN; l > 0 && !Map[l - 1]; l--)
                ;
            if (l < SPAN)
                Write(l, n < SPAN - l ? n : SPAN - l, 0, 1);
        }
        if ((op + 1) % Every == 0)
            CheckAll(0);
    }
    FailNew = 0;
    CheckAll(1);
    /* and everything goes, down to an empty root */
    Truncate(Rand() % 2 ? 0 : Rand() % 64);
    CheckAll(0);
    Truncate(0);
    CheckAll(0);
    CHECK(Depth(RootBytes) == 0 && Entries(RootBytes) == 0 && NodesHeld == 0, "a truncation to nothing left a tree");
}

/* N3 at its edges: neighbours touching logically and physically whose
   lengths sum to one below, at and one above each kind's limit - merged
   exactly when the sum fits; then written in pieces and as a whole */
static void Lengths(void)
{
    static const unsigned int firsts[] = { 1, 767, 768, 769, 16384, 32000 };
    unsigned int kind, f, delta;

    for (kind = 0; kind < 2; kind++) {
        unsigned int limit = kind ? EX_UNWRITTEN_MAX : EX_INIT_MAX;

        for (f = 0; f < sizeof(firsts) / sizeof(firsts[0]); f++) {
            for (delta = 0; delta < 3; delta++) {
                unsigned int a = firsts[f], b = limit - 1 + delta - a, i;
                EX_EXTENT x;

                if (a >= limit)
                    continue;
                NewTree(1024, Rand() % 1000);
                x.Block = Base;
                x.Length = a;
                x.Start = 100000;
                x.Unwritten = (int)kind;
                CHECK(ExInsert(&Tree, &x, 1) == EX_OK, "an edge extent refused");
                x.Block = Base + a;
                x.Length = b;
                x.Start = 100000 + a;
                CHECK(ExInsert(&Tree, &x, 1) == EX_OK, "an edge extent refused");
                for (i = 0; i < a + b; i++) {
                    Owner[100000 + i] = DATA;
                    Map[i] = 100000 + i;
                    Unw[i] = (unsigned char)kind;
                }
                DataHeld = a + b;
                CheckAll(0);
                CHECK((Entries(RootBytes) == 1) == (a + b <= limit), "neighbours merged unlike N3");
                if (kind) {
                    /* written in two pieces, then the rest: every block written */
                    Write(a / 2, a + b - a / 2 > 3 ? 3 : a + b - a / 2, 0, 1);
                    Write(0, a + b, 0, 1);
                    CheckAll(0);
                }
                Truncate(0);
                CheckAll(0);
            }
        }
    }
}

/* N4 at a leaf's edge: blocks two apart fill leaves to the brim; the block
   between the last of one leaf and the first of the next then has nowhere
   to go but a new leaf (both are full) - and once the next has room, into
   it */
static void NextLeaf(void)
{
    unsigned int per = (1024 - EX_HEADER) / EX_ENTRY, l;

    /* four full leaves: 0..2(per-1), 2per.., ... */
    NewTree(1024, 0);
    for (l = 0; l < 2 * per * 4; l += 2)
        Write(l, 1, 0, 0);
    CheckAll(0);
    CHECK(Depth(RootBytes) == 1 && Entries(RootBytes) == 4, "four full leaves");
    Write(2 * per - 1, 1, 0, 0);        /* between the first two */
    CheckAll(0);
    Write(4 * per - 1, 1, 0, 0);        /* between the next two */
    CheckAll(0);
}
/* N1 against Linux's statement: nodes made at the edge of each test */
static void Nodes(unsigned int Rounds)
{
    unsigned char n[4096];
    unsigned int r;

    for (r = 0; r < Rounds; r++) {
        unsigned int bs = 1024u << (Rand() % 3), root = Rand() % 4 == 0, depth = Rand() % 3, size = root ? 60 : bs;
        unsigned int room = root ? 4 : (bs - 12) / 12, entries, i, key0;
        unsigned long long cur;

        BlockSize = bs;
        Tree.BlockSize = bs;
        for (i = 0; i < size; i++)
            n[i] = (unsigned char)Rand();
        Put16(n, Rand() % 16 ? EX_MAGIC : Rand());
        Put16(n + EX_H_DEPTH, Rand() % 8 ? depth : Rand() % 8);
        Put16(n + EX_H_MAX, Rand() % 4 ? room : Rand() % 3 ? room + 1 - Rand() % 3 : 0);
        entries = Rand() % 6 ? Rand() % (room + 1) : Rand() % (room + 3);
        if (entries > room + 2) entries = room + 2;
        if (12 + entries * 12 > size) entries = (size - 12) / 12;
        Put16(n + EX_H_ENTRIES, entries);
        /* entries mostly sound, with one bent near its bound */
        cur = Rand() % 1000;
        key0 = (unsigned int)cur;
        for (i = 0; i < entries; i++) {
            unsigned char *e = n + 12 + 12 * i;
            if (depth == 0) {
                EX_EXTENT x;
                x.Block = (unsigned int)cur;
                x.Length = 1 + Rand() % 100;
                x.Unwritten = Rand() & 1;
                x.Start = FIRST_DATA + Rand() % (VOLUME - 200);
                WriteExtent(e, &x);
                cur += x.Length + Rand() % 3;
            } else {
                WriteIndex(e, (unsigned int)cur, FIRST_DATA + Rand() % VOLUME);
                cur += 1 + Rand() % 3;
            }
        }
        if (entries && Rand() % 2) {
            unsigned char *e = n + 12 + 12 * (Rand() % entries);
            switch (Rand() % 6) {
            case 0: Put32(e, Get32(e) - 1 - Rand() % 2); break;                    /* overlaps */
            case 1: if (depth == 0) Put16(e + 4, Rand() % 2 ? 0 : 32768); break;   /* empty */
            case 2: Put16(e + (depth == 0 ? 6 : 8), Rand() % 3); break;            /* start past the volume */
            case 3: Put32(e + (depth == 0 ? 8 : 4), Rand() % 20); break;           /* into metadata */
            case 4: Put32(e, 0xFFFFFFFFu - Rand() % 4); break;                     /* to 2^32 */
            default: break;
            }
        }
        if (Rand() % 8 == 0)
            key0 += Rand() % 3 - 1;
        CHECK((CheckNode(&Tree, n, depth, root, key0) != EX_OK) == RefNode(n, depth, root, key0),
              "a node judged unlike Linux's statement of N1");
    }
}

/* damaged trees: answers or refusals, never a loop or a wild read */
static void Damage(unsigned int Rounds)
{
    unsigned int r, i;

    for (r = 0; r < Rounds; r++) {
        Run(1024, Rand() % 1000, 400, 400, 0, 0);
        NewTree(1024, 0);
        for (i = 0; i < 3000; i++)
            Write((i * 3) % SPAN, 1 + Rand() % 2, Rand() & 1, 0);
        for (i = 0; i < 60; i++) {
            unsigned int b, at;
            unsigned char keep[4096], root[EX_ROOT_BYTES];
            EX_EXTENT x;
            unsigned long long next, goal;
            for (b = Rand() % VOLUME; Owner[b] != NODE; b = (b + 1) % VOLUME)
                ;
            memcpy(keep, Data[b], BlockSize);
            memcpy(root, RootBytes, EX_ROOT_BYTES);
            at = Rand() % BlockSize;
            if (Rand() % 4 == 0)
                RootBytes[Rand() % EX_ROOT_BYTES] = (unsigned char)Rand();
            else
                Data[b][at] = (unsigned char)Rand();
            memset(Checked, 0, sizeof(Checked));
            ExMap(&Tree, Rand() % (SPAN * 2), &x, &next, &goal);
            CHECK(Outstanding == 0, "a damaged tree held a node");
            memcpy(Data[b], keep, BlockSize);
            memcpy(RootBytes, root, EX_ROOT_BYTES);
        }
        memset(Checked, 0, sizeof(Checked));
        CheckAll(0);
    }
}

int main(int argc, char **argv)
{
    unsigned long long seed = argc > 1 ? strtoull(argv[1], NULL, 10) : 1;
    unsigned int k;

    Rng = seed * 0x9E3779B97F4A7C15ull + 11;
    Tree.Io.Context = 0;
    Tree.Io.Get = Get;
    Tree.Io.Checked = SetChecked;
    Tree.Io.Put = PutNode;
    Tree.Io.Dirty = DirtyNode;
    Tree.Io.New = New;
    Tree.Io.Free = FreeBlocks;
    Tree.Io.Valid = Valid;
    Tree.Io.Corrupt = 0;
    Tree.Root = RootBytes;

    Nodes(2000000);
    Lengths();
    NextLeaf();

    /* every block size, low and at the top of the 32-bit range, nodes
       failing now and then: small trees checked after every change */
    for (k = 0; k < 24; k++)
        Run(1024u << (k % 3), (k / 3) % 2 ? 0xFFFFFFFFu - SPAN + 1 : Rand() % 5000, 800, 1, 0, (k / 6) % 2 ? 7 : 0);
    /* larger, checked every 200 changes */
    for (k = 0; k < 6; k++)
        Run(1024u << (k % 3), k % 2 ? 0xFFFFFFFFu - SPAN + 1 : 0, 20000, 200, 0, k % 3 ? 0 : 50);
    /* deep: 60 000 single-block extents in 1 KiB nodes, three levels below the root */
    Run(1024, 0, 60000, 5000, 1, 0);
    Run(1024, 0, 60000, 5000, 1, 40);
    CHECK(MaxDepth >= 3, "no tree three levels deep");
    Damage(20);

    printf("EXTENT-MODEL: seed %llu: %llu ops, %llu inserts, %llu conversions, %llu truncations, %llu lookups, "
           "%llu nodes refused room, depth %llu, %llu checks\n",
           seed, Ops, Inserts, Converts, Truncates, Lookups, NewFailures, MaxDepth, Checks);
    return 0;
}
