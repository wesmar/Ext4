/**
 * DirIndex.c - walking and growing an indexed (htree) directory.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * A path runs from the root (block 0) through the index nodes to the leaf a
 * hash belongs to (DirBlock.c, I1-I3). The walks, as Linux's fs/ext4/namei.c
 * makes them:
 *
 *  P1  A probe checks every block it reads (I1, I2) and refuses a path that
 *      comes back to a block it went through.
 *  P2  The next leaf is the next entry of the deepest level that has one,
 *      the levels below it read afresh from their first entry. A lookup
 *      goes on only into a leaf that continues its hash (DX_HASH_CONTINUED
 *      with the same hash); a listing goes on to every leaf.
 *  P3  A name goes into its leaf when the leaf has room (L2). A full leaf
 *      is split (L3) and the new leaf entered after its own entry in the
 *      index; when that index block is full too, the lowest full block
 *      whose parent has room is split in two halves by count, its upper
 *      half to a new node entered in the parent - or, when every block up
 *      to the root is full, the root's entries move to a new node under it,
 *      one level more (up to 2 levels of nodes with large_dir, 1 without:
 *      then the directory is full). After a change above the leaf's own
 *      index block the path is probed again.
 *  P4  A one-block directory that fills up becomes indexed: block 0 keeps
 *      '.' and '..' and becomes the root; the rest moves to a new leaf,
 *      which is split at once as a full leaf is.
 *
 * Tested by tests/htree-model.ps1.
 */

#include "DirIndex.h"

/* a block not sound, told to the caller (it records it) */
static int Corrupt(const DX_DIR *D, unsigned int Block, int Index)
{
    if (D->Io.Corrupt)
        D->Io.Corrupt(D->Io.Context, Block, Index);
    return DB_ECORRUPT;
}

static unsigned int HashVersion(const DX_DIR *D, unsigned int OnDisk)
{
    return OnDisk <= 2 ? OnDisk + D->UnsignedDelta : OnDisk;
}

void DxRelease(const DX_DIR *D, DX_PATH *Path)
{
    unsigned int i;

    for (i = 0; i < DX_LEVELS_MAX; i++) {
        if (Path->Frame[i].Handle)
            D->Io.Put(D->Io.Context, Path->Frame[i].Handle);
        Path->Frame[i].Handle = 0;
    }
    Path->Depth = 0;
}

static void ClearPath(DX_PATH *Path)
{
    unsigned int i;

    for (i = 0; i < DX_LEVELS_MAX; i++) {
        Path->Frame[i].Handle = 0;
        Path->Frame[i].Data = 0;
        Path->Frame[i].Entries = 0;
        Path->Frame[i].At = 0;
    }
    Path->Depth = 0;
    Path->Version = 0;
    Path->Hash = 0;
    Path->Minor = 0;
}

/* P1 */
int DxProbe(const DX_DIR *D, const unsigned char *Name, unsigned int Length, unsigned int Hash,
            DX_PATH *Path)
{
    DX_ROOT         root;
    unsigned int    level, i, seen[DX_LEVELS_MAX];
    int             err;

    ClearPath(Path);
    err = D->Io.Get(D->Io.Context, 0, 1, &Path->Frame[0].Data, &Path->Frame[0].Handle);
    if (err)
        return err;
    Path->Depth = 1;
    err = DxReadRoot(&D->G, Path->Frame[0].Data, D->MaxLevels, &root);
    if (err)
        err = Corrupt(D, 0, 1);
    if (!err) {
        Path->Version = HashVersion(D, root.Version);
        if (Name)
            err = D->Io.Hash(D->Io.Context, Name, Length, Path->Version, &Path->Hash, &Path->Minor);
        else
            Path->Hash = Hash;
    }
    if (err) {
        DxRelease(D, Path);
        return err;
    }
    Path->Frame[0].Entries = root.Entries;
    seen[0] = 0;

    for (level = 0; ; level++) {
        DX_FRAME           *f = &Path->Frame[level];
        const unsigned char *e = f->Data + f->Entries;
        unsigned int        child;

        f->At = DxFind(e, Path->Hash);
        child = DxBlock(e, f->At);
        for (i = 0; i <= level; i++) {
            if (seen[i] == child) {
                DxRelease(D, Path);
                return Corrupt(D, child, 1);    /* the index comes back on itself */
            }
        }
        if (level == root.Levels)
            return DB_OK;
        f = &Path->Frame[level + 1];
        err = D->Io.Get(D->Io.Context, child, 1, &f->Data, &f->Handle);
        if (!err) {
            Path->Depth = level + 2;
            if (DxReadNode(&D->G, f->Data, &f->Entries))
                err = Corrupt(D, child, 1);
        }
        if (err) {
            DxRelease(D, Path);
            return err;
        }
        seen[level + 1] = child;
    }
}

/* P2 */
int DxNextLeaf(const DX_DIR *D, DX_PATH *Path, unsigned int Hash, unsigned int *StartHash)
{
    unsigned int level = Path->Depth - 1, down = 0, next;
    DX_FRAME    *f;
    int          err;

    for (;;) {
        f = &Path->Frame[level];
        if (++f->At < DxCount(f->Data + f->Entries))
            break;
        if (level == 0)
            return 0;
        down++;
        level--;
    }
    next = DxHash(f->Data + f->Entries, f->At);
    if (StartHash)
        *StartHash = next;
    if ((Hash & 1) == 0 && (next & ~DX_HASH_CONTINUED) != Hash)
        return 0;
    while (down--) {
        unsigned int   child = DxBlock(f->Data + f->Entries, f->At);
        unsigned char *data;
        void          *handle;

        err = D->Io.Get(D->Io.Context, child, 1, &data, &handle);
        if (err)
            return err;
        f = &Path->Frame[++level];
        D->Io.Put(D->Io.Context, f->Handle);
        f->Handle = handle;
        f->Data = data;
        f->At = 0;
        if (DxReadNode(&D->G, data, &f->Entries))
            return Corrupt(D, child, 1);
    }
    return 1;
}

int DxLookup(const DX_DIR *D, const unsigned char *Name, unsigned int Length, DB_MATCH Match,
             void *Context, void **Handle, unsigned char **Data, unsigned int *Offset)
{
    DX_PATH path;
    int     err;

    err = DxProbe(D, Name, Length, 0, &path);
    if (err)
        return err;
    for (;;) {
        unsigned char *leaf;
        void          *handle;

        err = D->Io.Get(D->Io.Context, DxLeaf(&path), 0, &leaf, &handle);
        if (err)
            break;
        err = DbSearch(&D->G, leaf, DbLeafSize(&D->G), Match, Context, Offset);
        if (err == DB_ECORRUPT)
            Corrupt(D, DxLeaf(&path), 0);
        if (err == DB_OK) {
            *Handle = handle;
            *Data = leaf;
            break;
        }
        D->Io.Put(D->Io.Context, handle);
        if (err != DB_ENOENT)
            break;
        err = DxNextLeaf(D, &path, path.Hash, 0);
        if (err <= 0) {
            err = err ? err : DB_ENOENT;
            break;
        }
    }
    DxRelease(D, &path);
    return err;
}

/* the names of a leaf hashed by the path's version (DbMap) */
typedef struct _DX_HASHING {
    const DX_DIR   *D;
    unsigned int    Version;
} DX_HASHING;

static int HashEntry(void *Context, const unsigned char *Name, unsigned int Length, unsigned int *Hash)
{
    DX_HASHING  *h = (DX_HASHING *)Context;
    unsigned int minor;

    return h->D->Io.Hash(h->D->Io.Context, Name, Length, h->Version, Hash, &minor);
}

/*
 * L3 and P3's last step: the full leaf (held, LeafHandle) split, the new
 * leaf entered in the path's deepest index block (it has room), the name
 * added to the half its hash belongs to. Releases the leaf.
 */
static int SplitLeaf(const DX_DIR *D, DX_PATH *Path, unsigned int LeafBlock, void *LeafHandle, unsigned char *Leaf,
                     const unsigned char *Name, unsigned int Length, unsigned int Inode, unsigned int Type)
{
    DX_FRAME       *f = &Path->Frame[Path->Depth - 1];
    DX_HASHING      hashing;
    DB_MAP         *map;
    unsigned char  *data;
    void           *handle;
    unsigned int    block, split, at, room = D->G.BlockSize / DB_REC_LEN(1) + 1;
    int             count, err;

    map = (DB_MAP *)D->Io.Allocate(D->Io.Context, room * (unsigned int)sizeof(DB_MAP));
    if (!map) {
        D->Io.Put(D->Io.Context, LeafHandle);
        return DB_ENOMEM;
    }
    hashing.D = D;
    hashing.Version = Path->Version;
    count = DbMap(&D->G, Leaf, HashEntry, &hashing, map);
    if (count <= 0) {
        D->Io.Release(D->Io.Context, map);
        D->Io.Put(D->Io.Context, LeafHandle);
        /* a full leaf without a name is not sound either */
        return count == DB_ECORRUPT || count == 0 ? Corrupt(D, LeafBlock, 0) : count;
    }
    err = D->Io.Append(D->Io.Context, &block, &data, &handle);
    if (err) {
        D->Io.Release(D->Io.Context, map);
        D->Io.Put(D->Io.Context, LeafHandle);
        return err;
    }
    DbSplit(&D->G, Leaf, data, map, (unsigned int)count, &split);
    D->Io.Release(D->Io.Context, map);
    DxInsert(f->Data + f->Entries, f->At, split, block);

    /* the half its hash belongs to takes the name */
    if (Path->Hash >= (split & ~DX_HASH_CONTINUED)) {
        err = DbFindSpace(&D->G, data, DbLeafSize(&D->G), Length, 0, 0, &at);
        if (!err)
            DbInsert(&D->G, data, at, Inode, Type, Name, Length);
    } else {
        err = DbFindSpace(&D->G, Leaf, DbLeafSize(&D->G), Length, 0, 0, &at);
        if (!err)
            DbInsert(&D->G, Leaf, at, Inode, Type, Name, Length);
    }
    if (!err)
        err = D->Io.Dirty(D->Io.Context, LeafHandle, 0);
    if (!err)
        err = D->Io.Dirty(D->Io.Context, handle, 0);
    if (!err)
        err = D->Io.Dirty(D->Io.Context, f->Handle, 1);
    D->Io.Put(D->Io.Context, handle);
    D->Io.Put(D->Io.Context, LeafHandle);
    return err;
}

/* P3: the index block at level Top, full, splits (or the root moves down) */
static int GrowIndex(const DX_DIR *D, DX_PATH *Path, unsigned int Top, int AddLevel)
{
    DX_FRAME       *f = &Path->Frame[Top];
    unsigned char  *e = f->Data + f->Entries, *data;
    unsigned int    count = DxCount(e), block;
    void           *handle;
    int             err;

    err = D->Io.Append(D->Io.Context, &block, &data, &handle);
    if (err)
        return err;
    if (AddLevel) {
        /* every entry of the root to a node under it */
        DxInitNode(&D->G, data, e, count);
        DbPut16(e + 2, 1);
        DbPut32(e + 4, block);
        f->Data[DX_ROOT_INFO + DX_INFO_LEVELS]++;
        err = D->Io.Dirty(D->Io.Context, f->Handle, 1);
        if (!err)
            err = D->Io.Dirty(D->Io.Context, handle, 1);
    } else {
        DX_FRAME     *parent = &Path->Frame[Top - 1];
        unsigned int  half = count / 2, hash = DxHash(e, half);
        void         *other = handle;

        DxInitNode(&D->G, data, e + half * DX_ENTRY_SIZE, count - half);
        DbPut16(e + 2, half);
        if (f->At >= half) {
            /* the path goes on in the new node */
            other = f->Handle;
            f->Handle = handle;
            f->Data = data;
            f->Entries = DX_NODE_ENTRIES;
            f->At -= half;
        }
        DxInsert(parent->Data + parent->Entries, parent->At, hash, block);
        err = D->Io.Dirty(D->Io.Context, other, 1);
        if (!err)
            err = D->Io.Dirty(D->Io.Context, parent->Handle, 1);
        if (!err)
            err = D->Io.Dirty(D->Io.Context, f->Handle, 1);
        handle = other;
    }
    D->Io.Put(D->Io.Context, handle);
    return err;
}

int DxAdd(const DX_DIR *D, const unsigned char *Name, unsigned int Length, unsigned int Inode,
          unsigned int Type, DB_MATCH Match, void *Context)
{
    DX_PATH         path;
    unsigned char  *leaf;
    void           *handle;
    unsigned int    at;
    int             err;

    for (;;) {
        DX_FRAME     *f;
        unsigned int  top;
        int           addLevel = 1, again = 0;

        err = DxProbe(D, Name, Length, 0, &path);
        if (err)
            return err;
        err = D->Io.Get(D->Io.Context, DxLeaf(&path), 0, &leaf, &handle);
        if (err)
            break;
        err = DbFindSpace(&D->G, leaf, DbLeafSize(&D->G), Length, Match, Context, &at);
        if (err == DB_ECORRUPT)
            Corrupt(D, DxLeaf(&path), 0);
        if (err == DB_OK) {
            DbInsert(&D->G, leaf, at, Inode, Type, Name, Length);
            err = D->Io.Dirty(D->Io.Context, handle, 0);
            D->Io.Put(D->Io.Context, handle);
            break;
        }
        if (err != DB_ENOSPC) {
            D->Io.Put(D->Io.Context, handle);
            break;
        }

        f = &path.Frame[path.Depth - 1];
        if (DxCount(f->Data + f->Entries) == DxLimit(f->Data + f->Entries)) {
            /* the lowest full block whose parent has room */
            for (top = path.Depth - 1; top > 0; top--) {
                const DX_FRAME *p = &path.Frame[top - 1];
                if (DxCount(p->Data + p->Entries) < DxLimit(p->Data + p->Entries)) {
                    addLevel = 0;
                    break;
                }
                again = 1;
            }
            if (addLevel && path.Depth == D->MaxLevels) {
                D->Io.Put(D->Io.Context, handle);
                err = DB_ENOSPC;            /* the index is full */
                break;
            }
            err = GrowIndex(D, &path, top, addLevel);
            if (err || addLevel || again) {
                D->Io.Put(D->Io.Context, handle);
                DxRelease(D, &path);
                if (err)
                    return err;
                continue;                   /* the path changed: probe again */
            }
        }
        err = SplitLeaf(D, &path, DxLeaf(&path), handle, leaf, Name, Length, Inode, Type);
        break;
    }
    DxRelease(D, &path);
    return err;
}

/* P4 */
int DxMakeIndexed(const DX_DIR *D, void *Handle0, unsigned char *Block0, unsigned int Version,
                  const unsigned char *Name, unsigned int Length, unsigned int Inode, unsigned int Type,
                  int *Converted)
{
    DX_PATH         path;
    unsigned char  *leaf;
    void           *handle;
    unsigned int    block;
    int             err;

    *Converted = 0;
    if (DxCanIndex(&D->G, Block0))
        return Corrupt(D, 0, 0);
    err = D->Io.Append(D->Io.Context, &block, &leaf, &handle);
    if (err)
        return err;
    err = DxMakeRoot(&D->G, Block0, leaf, block, Version);
    *Converted = !err;      /* block 0 is a root over the new leaf from here on */
    if (!err)
        err = D->Io.Dirty(D->Io.Context, Handle0, 1);
    if (!err)
        err = D->Io.Dirty(D->Io.Context, handle, 0);
    ClearPath(&path);
    path.Version = HashVersion(D, Version);
    if (!err)
        err = D->Io.Hash(D->Io.Context, Name, Length, path.Version, &path.Hash, &path.Minor);
    if (err) {
        D->Io.Put(D->Io.Context, handle);
        return err;
    }
    path.Depth = 1;
    path.Frame[0].Data = Block0;
    path.Frame[0].Entries = DX_ROOT_INFO + DX_INFO_SIZE;
    path.Frame[0].At = 0;
    path.Frame[0].Handle = Handle0;     /* the caller's: not released here */
    return SplitLeaf(D, &path, block, handle, leaf, Name, Length, Inode, Type);
}
