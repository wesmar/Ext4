/**
 * DirBlock.c - the blocks of an ext4 directory: the entries of a leaf, and
 * the hash index (htree) above the leaves.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * A leaf is a chain of entries, each record long enough for its name
 * (8 bytes and the name, rounded up to 4) and possibly more: the room after
 * the name is free space, and an entry with inode 0 is free altogether. The
 * records cover the block exactly, up to the checksum tail.
 *
 *  L1  An entry is sound when its record is at least 12 bytes, a multiple
 *      of 4, long enough for its name, ends within the region and not
 *      fewer than 12 bytes before its end, and names an inode the volume
 *      has (Linux __ext4_check_dir_entry).
 *  L2  A name goes into the first entry whose free room holds its record:
 *      a free entry is taken whole, a live one gives up what follows its
 *      own record. A deleted entry's record joins the one before it; the
 *      first entry of a block only loses its inode.
 *  L3  A full leaf splits by hash: its live entries sorted by hash, the
 *      upper ones move to a new block - as many as fit in half a block,
 *      or half of them by count when they do not fill half a block - so
 *      that both end up at most about half full. The new block's first
 *      hash enters the index, marked "continued" when the entry before it
 *      has the same hash (the two blocks then share it, and a lookup reads
 *      both).
 *
 * The index (Linux fs/ext4/namei.c, the "htree" of the disk format):
 *
 *  I1  An array of entries is (limit, count, block of the first child),
 *      then (hash, block) pairs in ascending hash order; a child takes the
 *      hashes from its own entry's hash (the first child: from the bottom)
 *      up to the next entry's. The limit is what the block holds; the count
 *      is 1 to the limit.
 *  I2  The root names the hash version, the levels below it (below 2, or
 *      3 with large_dir) and no unknown flag.
 *  I3  A lookup takes, at every level, the last entry whose hash is not
 *      above the name's (DxFind).
 *
 * Tested by tests/htree-model.ps1: directories built name by name with this
 * code, every rule checked after every change.
 */

#include "DirBlock.h"

static void Copy(unsigned char *To, const unsigned char *From, unsigned int Length)
{
    unsigned int i;

    /* forward: also when To lies below From in the same block */
    for (i = 0; i < Length; i++)
        To[i] = From[i];
}

static void Zero(unsigned char *To, unsigned int Length)
{
    unsigned int i;

    for (i = 0; i < Length; i++)
        To[i] = 0;
}

unsigned int DbRecLen(const DB_GEOMETRY *G, const unsigned char *Entry)
{
    unsigned int length = DbGet16(Entry + DB_E_REC_LEN);

    if (G->BlockSize >= 65536 && (length == 0 || length == 65535))
        return 65536;
    return length;
}

void DbSetRecLen(const DB_GEOMETRY *G, unsigned char *Entry, unsigned int Length)
{
    (void)G;
    DbPut16(Entry + DB_E_REC_LEN, Length >= 65536 ? 65535 : Length);
}

/* L1 */
int DbCheck(const DB_GEOMETRY *G, const unsigned char *Block, unsigned int Size, unsigned int Offset)
{
    const unsigned char *e = Block + Offset;
    unsigned int         length, next;

    if (Offset + DB_E_NAME > G->BlockSize)
        return DB_ECORRUPT;
    length = DbRecLen(G, e);
    next = Offset + length;
    if (length < DB_REC_LEN(1) || (length & 3) || length < DB_REC_LEN(e[DB_E_NAME_LEN]))
        return DB_ECORRUPT;
    if (next > Size || (next != Size && next + DB_REC_LEN(1) > Size))
        return DB_ECORRUPT;
    if (DbGet32(e + DB_E_INODE) > G->InodesCount)
        return DB_ECORRUPT;
    return DB_OK;
}

/* L2 */
int DbFindSpace(const DB_GEOMETRY *G, const unsigned char *Block, unsigned int Size, unsigned int Length,
                DB_MATCH Match, void *Context, unsigned int *Offset)
{
    unsigned int need = DB_REC_LEN(Length), at = 0;

    if (Size < need)
        return DB_ENOSPC;
    while (at <= Size - need) {
        const unsigned char *e = Block + at;
        unsigned int         live, room;

        if (DbCheck(G, Block, Size, at))
            return DB_ECORRUPT;
        live = DbGet32(e + DB_E_INODE) != 0;
        if (live && Match && Match(Context, e))
            return DB_EEXIST;
        room = DbRecLen(G, e) - (live ? DB_REC_LEN(e[DB_E_NAME_LEN]) : 0);
        if (room >= need) {
            *Offset = at;
            return DB_OK;
        }
        at += DbRecLen(G, e);
    }
    return DB_ENOSPC;
}

/*
 * L4: a name looked for, as Linux's ext4_search_dir looks: a walk by the
 * records with the least checking (this runs for every name of every block
 * read), the full check of L1 on the entry that matches; a record of
 * length 0 stops the walk as corrupt.
 */
int DbSearch(const DB_GEOMETRY *G, const unsigned char *Block, unsigned int Size,
             DB_MATCH Match, void *Context, unsigned int *Offset)
{
    unsigned int at = 0;

    while (at + DB_E_NAME_LEN < Size) {
        const unsigned char *e = Block + at;
        unsigned int         length;

        if (at + DB_E_NAME + e[DB_E_NAME_LEN] <= Size && DbGet32(e + DB_E_INODE) && Match(Context, e)) {
            if (DbCheck(G, Block, Size, at))
                return DB_ECORRUPT;
            *Offset = at;
            return DB_OK;
        }
        length = DbRecLen(G, e);
        if (length == 0)
            return DB_ECORRUPT;
        at += length;
    }
    return DB_ENOENT;
}

void DbInsert(const DB_GEOMETRY *G, unsigned char *Block, unsigned int Offset, unsigned int Inode,
              unsigned int Type, const unsigned char *Name, unsigned int Length)
{
    unsigned char *e = Block + Offset;

    if (DbGet32(e + DB_E_INODE)) {
        /* a live entry keeps its own record; the new one takes the rest */
        unsigned int own = DB_REC_LEN(e[DB_E_NAME_LEN]), whole = DbRecLen(G, e);
        unsigned char *rest = e + own;

        DbSetRecLen(G, rest, whole - own);
        DbSetRecLen(G, e, own);
        e = rest;
    }
    DbPut32(e + DB_E_INODE, Inode);
    e[DB_E_NAME_LEN] = (unsigned char)Length;
    e[DB_E_TYPE] = G->FileTypes ? (unsigned char)Type : 0;
    Copy(e + DB_E_NAME, Name, Length);
}

int DbDelete(const DB_GEOMETRY *G, unsigned char *Block, unsigned int Size, unsigned int Offset)
{
    unsigned int at = 0, before = 0, first = 1;

    while (at < Size) {
        unsigned int length;

        if (DbCheck(G, Block, Size, at))
            return DB_ECORRUPT;
        length = DbRecLen(G, Block + at);
        if (at == Offset) {
            if (!first) {
                DbSetRecLen(G, Block + before, DbRecLen(G, Block + before) + length);
                Zero(Block + at, length);
            } else {
                DbPut32(Block + at + DB_E_INODE, 0);
                Zero(Block + at + DB_E_NAME_LEN, length - DB_E_NAME_LEN);
            }
            return DB_OK;
        }
        before = at;
        first = 0;
        at += length;
    }
    return DB_ENOENT;
}

void DbInitTail(const DB_GEOMETRY *G, unsigned char *Block)
{
    unsigned char *t;

    if (!G->Csum)
        return;
    t = Block + G->BlockSize - DB_TAIL_SIZE;
    Zero(t, DB_TAIL_SIZE);
    DbSetRecLen(G, t, DB_TAIL_SIZE);
    t[DB_E_TYPE] = (unsigned char)DB_TAIL_TYPE;
}

void DbInitLeaf(const DB_GEOMETRY *G, unsigned char *Block)
{
    Zero(Block, G->BlockSize);
    DbSetRecLen(G, Block, DbLeafSize(G));
    DbInitTail(G, Block);
}

int DbMap(const DB_GEOMETRY *G, const unsigned char *Block, DB_HASH Hash, void *Context, DB_MAP *Map)
{
    unsigned int size = DbLeafSize(G), at = 0, count = 0;

    while (at < size) {
        const unsigned char *e = Block + at;

        if (DbCheck(G, Block, size, at))
            return DB_ECORRUPT;
        if (e[DB_E_NAME_LEN] && DbGet32(e + DB_E_INODE)) {
            int err = Hash(Context, e + DB_E_NAME, e[DB_E_NAME_LEN], &Map[count].Hash);
            if (err)
                return err;
            Map[count].Offset = at;
            Map[count].Size = DbRecLen(G, e);
            count++;
        }
        at += DbRecLen(G, e);
    }
    return (int)count;
}

/* by hash, entries of one hash in their order in the block */
static void SortMap(DB_MAP *Map, unsigned int Count)
{
    unsigned int i, j;

    for (i = 1; i < Count; i++) {
        DB_MAP m = Map[i];
        for (j = i; j > 0 && Map[j - 1].Hash > m.Hash; j--)
            Map[j] = Map[j - 1];
        Map[j] = m;
    }
}

/* L3 */
void DbSplit(const DB_GEOMETRY *G, unsigned char *Old, unsigned char *New, DB_MAP *Map,
             unsigned int Count, unsigned int *Split)
{
    unsigned int size = DbLeafSize(G), half = G->BlockSize / 2;
    unsigned int moved = 0, bytes = 0, split, out = 0, last = 0, at, kept = 0, to = 0;
    int          stopped = -1, i;

    SortMap(Map, Count);
    /* from the top: as many as stay within half a block */
    for (i = (int)Count - 1; i >= 0; i--) {
        if (bytes + Map[i].Size / 2 > half) {
            stopped = i;
            break;
        }
        bytes += Map[i].Size;
        moved++;
    }
    split = stopped > 0 ? Count - moved : Count / 2;
    if (split >= Count)
        split = Count - 1;
    *Split = Map[split].Hash;
    if (split > 0 && Map[split - 1].Hash == Map[split].Hash)
        *Split |= DX_HASH_CONTINUED;

    /* the upper entries to the new block, each in a record of its own size */
    Zero(New, G->BlockSize);
    for (at = split; at < Count; at++) {
        unsigned char *from = Old + Map[at].Offset;
        unsigned int   own = DB_REC_LEN(from[DB_E_NAME_LEN]);

        Copy(New + out, from, own);
        DbSetRecLen(G, New + out, own);
        DbPut32(from + DB_E_INODE, 0);
        last = out;
        out += own;
    }
    DbSetRecLen(G, New + last, size - last);

    /* the old block packed: its live entries close together from the start */
    for (at = 0; at < size; ) {
        unsigned char *e = Old + at;
        unsigned int   length = DbRecLen(G, e), own = DB_REC_LEN(e[DB_E_NAME_LEN]);

        if (DbGet32(e + DB_E_INODE) && e[DB_E_NAME_LEN]) {
            if (at != to)
                Copy(Old + to, e, own);
            DbSetRecLen(G, Old + to, own);
            kept = to;
            to += own;
        }
        at += length;
    }
    DbSetRecLen(G, Old + kept, size - kept);
    DbInitTail(G, Old);
    DbInitTail(G, New);
}

/* ---- the index ---- */

unsigned int DxRootLimit(const DB_GEOMETRY *G, unsigned int InfoLength)
{
    unsigned int fixed = DB_REC_LEN(1) + DB_REC_LEN(2) + InfoLength + (G->Csum ? DX_TAIL_SIZE : 0);
    return G->BlockSize > fixed ? (G->BlockSize - fixed) / DX_ENTRY_SIZE : 0;
}

unsigned int DxNodeLimit(const DB_GEOMETRY *G)
{
    return (G->BlockSize - DB_REC_LEN(0) - (G->Csum ? DX_TAIL_SIZE : 0)) / DX_ENTRY_SIZE;
}

static int EntriesSound(const DB_GEOMETRY *G, const unsigned char *E, unsigned int Offset, unsigned int Limit)
{
    unsigned int count = DxCount(E);

    if (Limit == 0 || DxLimit(E) != Limit || count == 0 || count > Limit ||
        Offset + Limit * DX_ENTRY_SIZE > G->BlockSize)
        return DB_ECORRUPT;
    return DB_OK;
}

/* I1, I2 */
int DxReadRoot(const DB_GEOMETRY *G, const unsigned char *Block, unsigned int MaxLevels, DX_ROOT *Root)
{
    const unsigned char *info = Block + DX_ROOT_INFO;
    unsigned int         length = info[DX_INFO_LENGTH];

    if (info[DX_INFO_VERSION] > 2 || (info[DX_INFO_FLAGS] & 1) || info[DX_INFO_LEVELS] >= MaxLevels)
        return DB_ECORRUPT;
    Root->Version = info[DX_INFO_VERSION];
    Root->Levels = info[DX_INFO_LEVELS];
    Root->Entries = DX_ROOT_INFO + length;
    if (Root->Entries + DX_ENTRY_SIZE > G->BlockSize)
        return DB_ECORRUPT;
    return EntriesSound(G, Block + Root->Entries, Root->Entries, DxRootLimit(G, length));
}

int DxReadNode(const DB_GEOMETRY *G, const unsigned char *Block, unsigned int *Entries)
{
    *Entries = DX_NODE_ENTRIES;
    return EntriesSound(G, Block + DX_NODE_ENTRIES, DX_NODE_ENTRIES, DxNodeLimit(G));
}

/* I3 */
unsigned int DxFind(const unsigned char *E, unsigned int Hash)
{
    unsigned int lo = 1, hi = DxCount(E);    /* the answer is in [lo - 1, hi) */

    while (lo < hi) {
        unsigned int mid = lo + (hi - lo) / 2;
        if (DxHash(E, mid) > Hash)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo - 1;
}

void DxInsert(unsigned char *E, unsigned int At, unsigned int Hash, unsigned int Block)
{
    unsigned int count = DxCount(E), i;

    for (i = count; i > At + 1; i--)
        Copy(E + i * DX_ENTRY_SIZE, E + (i - 1) * DX_ENTRY_SIZE, DX_ENTRY_SIZE);
    DbPut32(E + (At + 1) * DX_ENTRY_SIZE, Hash);
    DbPut32(E + (At + 1) * DX_ENTRY_SIZE + 4, Block);
    DbPut16(E + 2, count + 1);
}

void DxInitNode(const DB_GEOMETRY *G, unsigned char *Block, const unsigned char *E, unsigned int Count)
{
    unsigned char *entries = Block + DX_NODE_ENTRIES;

    Zero(Block, G->BlockSize);
    DbSetRecLen(G, Block, G->BlockSize);        /* one empty entry spanning the block */
    Copy(entries, E, Count * DX_ENTRY_SIZE);
    DbPut16(entries, DxNodeLimit(G));
    DbPut16(entries + 2, Count);
}

/* where the entries after '.' and '..' begin - the root keeps those two -
   or 0: the block is not sound, or holds nothing else to move */
static unsigned int RootStart(const DB_GEOMETRY *G, const unsigned char *Block0)
{
    unsigned int size = DbLeafSize(G), dot = DbRecLen(G, Block0), start, at;

    if (dot != DB_REC_LEN(1) || DbCheck(G, Block0, size, 0) ||
        Block0[DB_E_NAME_LEN] != 1 || Block0[DB_E_NAME] != '.')
        return 0;
    if (DbCheck(G, Block0, size, dot) || Block0[dot + DB_E_NAME_LEN] != 2)
        return 0;
    start = dot + DbRecLen(G, Block0 + dot);
    for (at = start; at < size; at += DbRecLen(G, Block0 + at))
        if (DbCheck(G, Block0, size, at))
            return 0;
    return start < size ? start : 0;
}

int DxCanIndex(const DB_GEOMETRY *G, const unsigned char *Block0)
{
    return RootStart(G, Block0) ? DB_OK : DB_ECORRUPT;
}

int DxMakeRoot(const DB_GEOMETRY *G, unsigned char *Block0, unsigned char *Leaf,
               unsigned int LeafBlock, unsigned int Version)
{
    unsigned int size = DbLeafSize(G), dot = DB_REC_LEN(1), start = RootStart(G, Block0), length, at;
    unsigned char *info, *entries;

    if (!start)
        return DB_ECORRUPT;
    length = size - start;

    /* the entries after '..' move to the leaf, the last one stretched to
       the leaf's end (the move left it short by what the root keeps) */
    Zero(Leaf, G->BlockSize);
    Copy(Leaf, Block0 + start, length);
    for (at = 0; at + DbRecLen(G, Leaf + at) < length; at += DbRecLen(G, Leaf + at))
        ;
    DbSetRecLen(G, Leaf + at, size - at);
    DbInitTail(G, Leaf);

    /* the root */
    Zero(Block0 + start, G->BlockSize - start);
    DbSetRecLen(G, Block0 + dot, G->BlockSize - dot);
    info = Block0 + DX_ROOT_INFO;
    Zero(info, DX_INFO_SIZE);
    info[DX_INFO_VERSION] = (unsigned char)Version;
    info[DX_INFO_LENGTH] = DX_INFO_SIZE;
    entries = info + DX_INFO_SIZE;
    DbPut16(entries, DxRootLimit(G, DX_INFO_SIZE));
    DbPut16(entries + 2, 1);
    DbPut32(entries + 4, LeafBlock);
    return DB_OK;
}
