/**
 * ExtentCore.c - the extent tree of an ext4 inode.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * The root lives in the inode (four entries); below it, nodes of a block
 * each. A node is a header (magic 0xF30A, entries, max, depth - the levels
 * below it) and its entries: in a leaf (depth 0) extents - logical block,
 * length, physical start, a length past 32768 marking the extent unwritten
 * (allocated, read as zeros) - and above it index entries - the first
 * logical block below, the child node. The rules, as Linux reads and keeps
 * them (fs/ext4/extents.c):
 *
 *  N1  A node is sound when its magic and depth are right, its max is not
 *      0 nor more than the room it has (4 in the root, (block - 12) / 12
 *      in a block), its entries not more than its max, and an index node is
 *      not empty. Extents are not empty, end by 2^32, and go up without
 *      overlapping; index keys go strictly up; every block named lies in the
 *      volume and is none of its metadata. A node's first key is the key
 *      its parent's entry holds for it.
 *  N2  A lookup takes, at every level, the last entry whose key is not
 *      above the block (the first entry when all are). A hole runs to the
 *      next mapped block.
 *  N3  Two extents merge when they touch logically and physically, are of
 *      one kind (written or unwritten) and stay within that kind's length.
 *  N4  A new extent goes into its leaf, merged with a neighbour there when
 *      it can be; into the next leaf when its own is full and the extent
 *      lies past its last one and the next has room; otherwise the tree
 *      grows: the deepest node of the path with room gets a new child, the
 *      levels below it split at the path (the entries right of it move to
 *      new nodes, which begin at the first key moved - or at the new
 *      extent's block, when nothing moves); with no room up to the root,
 *      the root's entries move to a new node under it, one level more. A
 *      changed first key goes up to the parents.
 *  N5  A depth-1 tree whose leaf fits in the root again after a merge moves
 *      back into it.
 *  N6  Truncation frees every block from a point on, from the last extent
 *      backwards: an extent past it whole, one across it shortened (still
 *      of its kind); a node left empty goes with its entry in the parent;
 *      a root left empty is a leaf again.
 *
 * Tested by tests/extent-model.ps1: trees built and cut at random against a
 * flat map of every block, every rule checked after every change.
 */

#include "ExtentCore.h"

typedef struct _EX_FRAME {
    unsigned char      *Node;
    void               *Handle;     /* 0: the root */
    unsigned long long  Block;
    int                 At;         /* the entry the path goes through; -1: none */
} EX_FRAME;

typedef struct _EX_PATH {
    unsigned int    Depth;          /* Frame[Depth] is the leaf */
    EX_FRAME        Frame[EX_MAX_DEPTH + 1];
} EX_PATH;

static unsigned int Get16(const unsigned char *p) { return p[0] | (unsigned int)p[1] << 8; }
static unsigned int Get32(const unsigned char *p)
{
    return p[0] | (unsigned int)p[1] << 8 | (unsigned int)p[2] << 16 | (unsigned int)p[3] << 24;
}
static void Put16(unsigned char *p, unsigned int v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void Put32(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static void Move(unsigned char *To, const unsigned char *From, unsigned int Length)
{
    unsigned int i;

    if (To < From) {
        for (i = 0; i < Length; i++)
            To[i] = From[i];
    } else {
        for (i = Length; i > 0; i--)
            To[i - 1] = From[i - 1];
    }
}

static void Zero(unsigned char *To, unsigned int Length)
{
    unsigned int i;

    for (i = 0; i < Length; i++)
        To[i] = 0;
}

static unsigned int Entries(const unsigned char *N) { return Get16(N + EX_H_ENTRIES); }
static unsigned int Max(const unsigned char *N) { return Get16(N + EX_H_MAX); }
static unsigned int Depth(const unsigned char *N) { return Get16(N + EX_H_DEPTH); }
static unsigned char *Entry(unsigned char *N, unsigned int i) { return N + EX_HEADER + i * EX_ENTRY; }
static unsigned int Key(const unsigned char *N, unsigned int i) { return Get32(N + EX_HEADER + i * EX_ENTRY); }
static void SetEntries(unsigned char *N, unsigned int Count) { Put16(N + EX_H_ENTRIES, Count); }

static void ReadExtent(const unsigned char *E, EX_EXTENT *X)
{
    unsigned int length = Get16(E + EX_E_LEN);

    X->Block = Get32(E + EX_E_BLOCK);
    X->Unwritten = length > EX_INIT_MAX;
    X->Length = length > EX_INIT_MAX ? length - EX_INIT_MAX : length;
    X->Start = (unsigned long long)Get16(E + EX_E_START_HI) << 32 | Get32(E + EX_E_START_LO);
}

static void WriteExtent(unsigned char *E, const EX_EXTENT *X)
{
    Put32(E + EX_E_BLOCK, X->Block);
    Put16(E + EX_E_LEN, X->Unwritten ? X->Length + EX_INIT_MAX : X->Length);
    Put16(E + EX_E_START_HI, (unsigned int)(X->Start >> 32));
    Put32(E + EX_E_START_LO, (unsigned int)X->Start);
}

static unsigned long long Child(const unsigned char *I)
{
    return (unsigned long long)Get16(I + EX_I_LEAF_HI) << 32 | Get32(I + EX_I_LEAF_LO);
}

static void WriteIndex(unsigned char *I, unsigned int Block, unsigned long long Child)
{
    Put32(I + EX_I_BLOCK, Block);
    Put32(I + EX_I_LEAF_LO, (unsigned int)Child);
    Put16(I + EX_I_LEAF_HI, (unsigned int)(Child >> 32));
    Put16(I + 10, 0);
}

static void InitNode(unsigned char *N, unsigned int Size, unsigned int Max, unsigned int Depth)
{
    Zero(N, Size);
    Put16(N + EX_H_MAGIC, EX_MAGIC);
    Put16(N + EX_H_MAX, Max);
    Put16(N + EX_H_DEPTH, Depth);
}

/* N1: Key is the parent's key for the node (a block's, not the root's) */
static int CheckNode(const EX_TREE *T, const unsigned char *N, unsigned int Depth, int Root, unsigned int Key0)
{
    unsigned int entries = Entries(N), max = Max(N), room = Root ? EX_ROOT_MAX : ExBlockMax(T), i;
    unsigned long long next = 0;    /* the lowest block the next entry may begin at */

    if (Get16(N + EX_H_MAGIC) != EX_MAGIC || Get16(N + EX_H_DEPTH) != Depth || Depth > EX_MAX_DEPTH ||
        max == 0 || max > room || entries > max || (entries == 0 && Depth > 0))
        return EX_ECORRUPT;
    if (entries && !Root && Key(N, 0) != Key0)
        return EX_ECORRUPT;
    for (i = 0; i < entries; i++) {
        const unsigned char *e = N + EX_HEADER + i * EX_ENTRY;

        if (Depth == 0) {
            EX_EXTENT x;

            ReadExtent(e, &x);
            if (x.Length == 0 || x.Block < next || x.Block + (unsigned long long)x.Length > EX_END ||
                !T->Io.Valid(T->Io.Context, x.Start, x.Length))
                return EX_ECORRUPT;
            next = x.Block + (unsigned long long)x.Length;
        } else {
            unsigned int k = Get32(e + EX_I_BLOCK);

            if (k < next || !T->Io.Valid(T->Io.Context, Child(e), 1))
                return EX_ECORRUPT;
            next = k + 1ull;
        }
    }
    return EX_OK;
}

int ExCheckRoot(const EX_TREE *T)
{
    if (CheckNode(T, T->Root, Depth(T->Root), 1, 0)) {
        if (T->Io.Corrupt)
            T->Io.Corrupt(T->Io.Context, 0);
        return EX_ECORRUPT;
    }
    return EX_OK;
}

void ExInit(EX_TREE *T)
{
    InitNode(T->Root, EX_ROOT_BYTES, EX_ROOT_MAX, 0);
}

static void Release(const EX_TREE *T, EX_PATH *P)
{
    unsigned int i;

    for (i = 1; i <= EX_MAX_DEPTH; i++) {
        if (P->Frame[i].Handle)
            T->Io.Put(T->Io.Context, P->Frame[i].Handle);
        P->Frame[i].Handle = 0;
    }
}

/* N2: the last entry whose key is not above Block, the first when all are */
static int Search(const unsigned char *N, unsigned int Block)
{
    unsigned int lo = 1, hi = Entries(N);       /* the answer is in [lo - 1, hi) */

    if (hi == 0)
        return -1;
    while (lo < hi) {
        unsigned int mid = lo + (hi - lo) / 2;
        if (Key(N, mid) > Block)
            hi = mid;
        else
            lo = mid + 1;
    }
    return (int)lo - 1;
}

/* the path to Block, every node read checked (N1) */
static int Find(const EX_TREE *T, unsigned int Block, EX_PATH *P)
{
    unsigned int level, depth;
    int err;

    for (level = 0; level <= EX_MAX_DEPTH; level++) {
        P->Frame[level].Node = 0;
        P->Frame[level].Handle = 0;
        P->Frame[level].Block = 0;
        P->Frame[level].At = -1;
    }
    err = ExCheckRoot(T);
    if (err)
        return err;
    depth = Depth(T->Root);
    P->Depth = depth;
    P->Frame[0].Node = T->Root;
    for (level = 0; ; level++) {
        EX_FRAME           *f = &P->Frame[level], *c;
        unsigned long long  child;
        int                 checked = 0;

        f->At = Search(f->Node, Block);
        if (level == depth)
            return EX_OK;
        child = Child(Entry(f->Node, (unsigned int)f->At));
        c = &P->Frame[level + 1];
        err = T->Io.Get(T->Io.Context, child, &c->Node, &c->Handle, &checked);
        if (err) {
            c->Handle = 0;
            Release(T, P);
            return err;
        }
        c->Block = child;
        if (!checked) {
            if (CheckNode(T, c->Node, depth - level - 1, 0, Key(f->Node, (unsigned int)f->At))) {
                if (T->Io.Corrupt)
                    T->Io.Corrupt(T->Io.Context, child);
                Release(T, P);
                return EX_ECORRUPT;
            }
            T->Io.Checked(T->Io.Context, c->Handle);
        }
    }
}

static int Dirty(const EX_TREE *T, const EX_FRAME *F)
{
    return T->Io.Dirty(T->Io.Context, F->Handle);
}

/* the first mapped block after Block, which the path's leaf entry does not
   hold (N2) */
static unsigned long long NextBlock(const EX_PATH *P, unsigned int Block)
{
    const EX_FRAME *leaf = &P->Frame[P->Depth];
    int level;

    if (leaf->At < 0)
        return EX_END;
    if (Block < Key(leaf->Node, (unsigned int)leaf->At))
        return Key(leaf->Node, (unsigned int)leaf->At);     /* before every extent of the leaf */
    for (level = (int)P->Depth; level >= 0; level--) {
        const EX_FRAME *f = &P->Frame[level];
        if ((unsigned int)f->At + 1 < Entries(f->Node))
            return Key(f->Node, (unsigned int)f->At + 1);
    }
    return EX_END;
}

/* where blocks for Block should go: next to the extent before it (or after
   it), in the leaf's own neighbourhood, or the tree's goal */
static unsigned long long GoalOf(const EX_TREE *T, const EX_PATH *P, unsigned int Block)
{
    const EX_FRAME *leaf = &P->Frame[P->Depth];
    EX_EXTENT       x;

    if (leaf->At >= 0) {
        ReadExtent(Entry(leaf->Node, (unsigned int)leaf->At), &x);
        if (Block >= x.Block)
            return x.Start + (Block - x.Block);
        return x.Start > x.Block - Block ? x.Start - (x.Block - Block) : x.Start;
    }
    return P->Depth ? leaf->Block : T->Goal;
}

int ExMap(const EX_TREE *T, unsigned int Block, EX_EXTENT *Found, unsigned long long *Next,
          unsigned long long *Goal)
{
    EX_PATH p;
    int     err = Find(T, Block, &p);

    if (err)
        return err;
    *Next = EX_END;
    *Goal = GoalOf(T, &p, Block);
    if (p.Frame[p.Depth].At >= 0) {
        ReadExtent(Entry(p.Frame[p.Depth].Node, (unsigned int)p.Frame[p.Depth].At), Found);
        if (Block >= Found->Block && Block - Found->Block < Found->Length) {
            Release(T, &p);
            return EX_OK;
        }
    }
    *Next = NextBlock(&p, Block);
    Release(T, &p);
    return EX_ENOENT;
}

/* N3 */
static int Mergeable(const EX_EXTENT *A, const EX_EXTENT *B)
{
    unsigned long long length = (unsigned long long)A->Length + B->Length;

    return A->Unwritten == B->Unwritten && A->Block + (unsigned long long)A->Length == B->Block &&
           A->Start + A->Length == B->Start && length <= (A->Unwritten ? EX_UNWRITTEN_MAX : EX_INIT_MAX);
}

/* the new first key of the node at Level, up the path while it is the
   first entry there too */
static int FixKeys(const EX_TREE *T, EX_PATH *P, unsigned int Level)
{
    unsigned int key = Key(P->Frame[Level].Node, 0);
    int          err = 0;

    while (Level > 0 && !err) {
        EX_FRAME *f = &P->Frame[--Level];

        if (Key(f->Node, (unsigned int)f->At) == key)
            break;
        Put32(Entry(f->Node, (unsigned int)f->At) + EX_I_BLOCK, key);
        err = Dirty(T, f);
        if (f->At != 0)
            break;
    }
    return err;
}

/* the extent at index i of the leaf and the one after it, as one (N3);
   1 when they merged */
static int MergeRight(unsigned char *Leaf, unsigned int i)
{
    EX_EXTENT a, b;
    unsigned int entries = Entries(Leaf);

    if (i + 1 >= entries)
        return 0;
    ReadExtent(Entry(Leaf, i), &a);
    ReadExtent(Entry(Leaf, i + 1), &b);
    if (!Mergeable(&a, &b))
        return 0;
    a.Length += b.Length;
    WriteExtent(Entry(Leaf, i), &a);
    Move(Entry(Leaf, i + 1), Entry(Leaf, i + 2), (entries - i - 2) * EX_ENTRY);
    Zero(Entry(Leaf, entries - 1), EX_ENTRY);
    SetEntries(Leaf, entries - 1);
    return 1;
}

/* N3: the path's extent merged with its neighbours in the leaf, where they
   can be (no first key changes: a merge keeps the left one's) */
static void MergeAround(EX_FRAME *Leaf)
{
    unsigned int at = (unsigned int)Leaf->At;

    MergeRight(Leaf->Node, at);
    if (at > 0 && MergeRight(Leaf->Node, at - 1))
        Leaf->At = (int)at - 1;
}

/* N5: a depth-1 tree whose only leaf fits in the root goes back into it */
static int MergeUp(const EX_TREE *T, EX_PATH *P)
{
    EX_FRAME *leaf = &P->Frame[1];
    int       err;

    if (P->Depth != 1 || Entries(T->Root) != 1 || Entries(leaf->Node) > EX_ROOT_MAX)
        return 0;
    Move(T->Root + EX_HEADER, leaf->Node + EX_HEADER, Entries(leaf->Node) * EX_ENTRY);
    Zero(T->Root + EX_HEADER + Entries(leaf->Node) * EX_ENTRY,
         EX_ROOT_BYTES - EX_HEADER - Entries(leaf->Node) * EX_ENTRY);
    SetEntries(T->Root, Entries(leaf->Node));
    Put16(T->Root + EX_H_MAX, EX_ROOT_MAX);
    Put16(T->Root + EX_H_DEPTH, 0);
    err = T->Io.Dirty(T->Io.Context, 0);
    T->Io.Put(T->Io.Context, leaf->Handle);
    leaf->Handle = 0;
    if (!err)
        err = T->Io.Free(T->Io.Context, leaf->Block, 1, 1);
    P->Depth = 0;
    P->Frame[0].At = leaf->At;
    return err;
}

/* N4: the root's entries to a new node under it, one level more */
static int Deepen(const EX_TREE *T)
{
    unsigned long long block, goal = T->Goal;
    unsigned char     *data;
    void              *handle;
    unsigned int       depth = Depth(T->Root);
    int                err;

    if (depth >= EX_MAX_DEPTH)
        return EX_ENOSPC;
    if (depth > 0 && Child(Entry(T->Root, 0)) > 1)
        goal = Child(Entry(T->Root, 0)) - 1;    /* before the first child, as Linux tries */
    err = T->Io.New(T->Io.Context, goal, &block, &data, &handle);
    if (err)
        return err;
    InitNode(data, T->BlockSize, ExBlockMax(T), depth);
    Move(data + EX_HEADER, T->Root + EX_HEADER, Entries(T->Root) * EX_ENTRY);
    SetEntries(data, Entries(T->Root));
    err = T->Io.Dirty(T->Io.Context, handle);
    T->Io.Put(T->Io.Context, handle);
    if (err)
        return err;
    /* the root: one entry, for the node, under the node's first key */
    WriteIndex(Entry(T->Root, 0), Key(T->Root, 0), block);
    Zero(Entry(T->Root, 1), EX_ROOT_BYTES - EX_HEADER - EX_ENTRY);
    SetEntries(T->Root, 1);
    Put16(T->Root + EX_H_MAX, EX_ROOT_MAX);
    Put16(T->Root + EX_H_DEPTH, depth + 1);
    return T->Io.Dirty(T->Io.Context, 0);
}

/* N4: new nodes for the levels below At, the entries right of the path in
   them, their top one entered in the node at level At (it has room) */
static int Split(const EX_TREE *T, EX_PATH *P, const EX_EXTENT *New, unsigned int At)
{
    unsigned long long blocks[EX_MAX_DEPTH + 1], goal = GoalOf(T, P, New->Block);
    unsigned char     *data[EX_MAX_DEPTH + 1];
    void              *handles[EX_MAX_DEPTH + 1];
    EX_FRAME          *leaf = &P->Frame[P->Depth];
    unsigned int       border, level, moved, i;
    int                err = 0;

    moved = Entries(leaf->Node) - (unsigned int)(leaf->At + 1);
    border = moved ? Key(leaf->Node, (unsigned int)leaf->At + 1) : New->Block;

    for (level = At + 1; level <= P->Depth; level++) {
        err = T->Io.New(T->Io.Context, goal, &blocks[level], &data[level], &handles[level]);
        if (err) {
            /* nothing has changed yet: the blocks taken go back */
            for (i = At + 1; i < level; i++) {
                T->Io.Put(T->Io.Context, handles[i]);
                T->Io.Free(T->Io.Context, blocks[i], 1, 1);
            }
            return err;
        }
    }

    /* the leaf's entries right of the path, to the new leaf */
    InitNode(data[P->Depth], T->BlockSize, ExBlockMax(T), 0);
    Move(data[P->Depth] + EX_HEADER, Entry(leaf->Node, (unsigned int)leaf->At + 1), moved * EX_ENTRY);
    SetEntries(data[P->Depth], moved);
    Zero(Entry(leaf->Node, (unsigned int)leaf->At + 1), moved * EX_ENTRY);
    SetEntries(leaf->Node, (unsigned int)leaf->At + 1);

    /* each index level between: the new child first, then its own entries
       right of the path */
    for (level = P->Depth - 1; level > At; level--) {
        EX_FRAME     *f = &P->Frame[level];
        unsigned int  rest = Entries(f->Node) - (unsigned int)(f->At + 1);

        InitNode(data[level], T->BlockSize, ExBlockMax(T), P->Depth - level);
        WriteIndex(data[level] + EX_HEADER, border, blocks[level + 1]);
        Move(data[level] + EX_HEADER + EX_ENTRY, Entry(f->Node, (unsigned int)f->At + 1), rest * EX_ENTRY);
        SetEntries(data[level], rest + 1);
        Zero(Entry(f->Node, (unsigned int)f->At + 1), rest * EX_ENTRY);
        SetEntries(f->Node, (unsigned int)f->At + 1);
    }

    /* the top new node, entered after the path's entry at level At */
    {
        EX_FRAME     *f = &P->Frame[At];
        unsigned int  entries = Entries(f->Node), pos = (unsigned int)f->At + 1;

        Move(Entry(f->Node, pos + 1), Entry(f->Node, pos), (entries - pos) * EX_ENTRY);
        WriteIndex(Entry(f->Node, pos), border, blocks[At + 1]);
        SetEntries(f->Node, entries + 1);
    }

    for (level = At; level <= P->Depth && !err; level++)
        err = Dirty(T, &P->Frame[level]);
    for (level = At + 1; level <= P->Depth; level++) {
        if (!err)
            err = T->Io.Dirty(T->Io.Context, handles[level]);
        T->Io.Put(T->Io.Context, handles[level]);
    }
    return err;
}

/* N4: room in the path's leaf, made one way or another; the path released */
static int Grow(const EX_TREE *T, EX_PATH *P, const EX_EXTENT *New)
{
    int level, err;

    for (level = (int)P->Depth - 1; level >= 0; level--)
        if (Entries(P->Frame[level].Node) < Max(P->Frame[level].Node))
            break;
    err = level < 0 ? Deepen(T) : Split(T, P, New, (unsigned int)level);
    Release(T, P);
    return err;
}

/* N4: New into the leaf of the path, which has room, at its place */
static int Put(const EX_TREE *T, EX_PATH *P, const EX_EXTENT *New)
{
    EX_FRAME     *leaf = &P->Frame[P->Depth];
    unsigned int  entries = Entries(leaf->Node), pos;
    int           err;

    pos = leaf->At < 0 ? 0 : Key(leaf->Node, (unsigned int)leaf->At) < New->Block ? (unsigned int)leaf->At + 1
                                                                                  : (unsigned int)leaf->At;
    Move(Entry(leaf->Node, pos + 1), Entry(leaf->Node, pos), (entries - pos) * EX_ENTRY);
    WriteExtent(Entry(leaf->Node, pos), New);
    SetEntries(leaf->Node, entries + 1);
    leaf->At = (int)pos;
    err = Dirty(T, leaf);
    if (!err && pos == 0)
        err = FixKeys(T, P, P->Depth);
    return err;
}

int ExInsert(const EX_TREE *T, const EX_EXTENT *New, int Merge)
{
    EX_PATH p;
    int     err;

    if (New->Length == 0 || New->Length > (New->Unwritten ? EX_UNWRITTEN_MAX : EX_INIT_MAX) ||
        New->Block + (unsigned long long)New->Length > EX_END)
        return EX_EIO;

    for (;;) {
        EX_FRAME     *leaf;
        EX_EXTENT     x = { 0, 0, 0, 0 };
        unsigned int  entries;

        err = Find(T, New->Block, &p);
        if (err)
            return err;
        leaf = &p.Frame[p.Depth];
        entries = Entries(leaf->Node);

        /* a hole for the whole of it, or the caller is wrong */
        if (leaf->At >= 0) {
            ReadExtent(Entry(leaf->Node, (unsigned int)leaf->At), &x);
            if ((x.Block <= New->Block && x.Block + (unsigned long long)x.Length > New->Block) ||
                NextBlock(&p, New->Block) < New->Block + (unsigned long long)New->Length) {
                Release(T, &p);
                return EX_EIO;
            }
        }

        if (Merge && leaf->At >= 0) {
            unsigned int at = (unsigned int)leaf->At;

            if (x.Block < New->Block && Mergeable(&x, New)) {
                /* onto the end of the extent before it, and on into the next */
                x.Length += New->Length;
                WriteExtent(Entry(leaf->Node, at), &x);
                MergeRight(leaf->Node, at);
                err = Dirty(T, leaf);
                if (!err)
                    err = MergeUp(T, &p);
                Release(T, &p);
                return err;
            }
            if (x.Block < New->Block)
                at++;
            if (at < entries) {
                EX_EXTENT y;

                ReadExtent(Entry(leaf->Node, at), &y);
                if (Mergeable(New, &y)) {
                    /* in front of the extent after it */
                    y.Block = New->Block;
                    y.Start = New->Start;
                    y.Length += New->Length;
                    WriteExtent(Entry(leaf->Node, at), &y);
                    leaf->At = (int)at;
                    err = Dirty(T, leaf);
                    if (!err && at == 0)
                        err = FixKeys(T, &p, p.Depth);
                    if (!err)
                        err = MergeUp(T, &p);
                    Release(T, &p);
                    return err;
                }
            }
        }

        if (entries < Max(leaf->Node)) {
            err = Put(T, &p, New);
            if (!err && Merge)
                err = MergeUp(T, &p);
            Release(T, &p);
            return err;
        }

        /* the leaf is full: the next one, when the extent lies past this
           one's last and the next has room */
        if (p.Depth > 0 && New->Block > Key(leaf->Node, entries - 1)) {
            unsigned long long next = NextBlock(&p, New->Block);

            if (next != EX_END) {
                EX_PATH q;

                Release(T, &p);
                err = Find(T, (unsigned int)next, &q);
                if (err)
                    return err;
                if (Entries(q.Frame[q.Depth].Node) < Max(q.Frame[q.Depth].Node) &&
                    Key(q.Frame[q.Depth].Node, 0) == next) {
                    q.Frame[q.Depth].At = 0;
                    err = Put(T, &q, New);
                    if (!err && Merge) {
                        /* into the next leaf's first extent, if they touch */
                        MergeAround(&q.Frame[q.Depth]);
                        err = Dirty(T, &q.Frame[q.Depth]);
                    }
                    Release(T, &q);
                    return err;
                }
                Release(T, &q);
                err = Find(T, New->Block, &p);
                if (err)
                    return err;
            }
        }

        err = Grow(T, &p, New);
        if (err)
            return err;
    }
}

/* an extent boundary at Block, the mapping unchanged: the extent across it
   in two (of its kind, not merged back) */
static int SplitAt(const EX_TREE *T, unsigned int Block)
{
    EX_PATH    p;
    EX_EXTENT  x, right;
    int        err = Find(T, Block, &p);

    if (err)
        return err;
    if (p.Frame[p.Depth].At < 0) {
        Release(T, &p);
        return EX_EIO;
    }
    ReadExtent(Entry(p.Frame[p.Depth].Node, (unsigned int)p.Frame[p.Depth].At), &x);
    if (Block <= x.Block || Block - x.Block >= x.Length) {
        Release(T, &p);
        return Block == x.Block ? EX_OK : EX_EIO;
    }
    right = x;
    right.Block = Block;
    right.Start = x.Start + (Block - x.Block);
    right.Length = x.Length - (Block - x.Block);
    x.Length = Block - x.Block;
    WriteExtent(Entry(p.Frame[p.Depth].Node, (unsigned int)p.Frame[p.Depth].At), &x);
    err = Dirty(T, &p.Frame[p.Depth]);
    Release(T, &p);
    if (!err)
        err = ExInsert(T, &right, 0);
    if (err && err != EX_ECORRUPT) {
        /* the right part did not go in: the extent whole again */
        if (Find(T, x.Block, &p) == EX_OK) {
            x.Length += right.Length;
            WriteExtent(Entry(p.Frame[p.Depth].Node, (unsigned int)p.Frame[p.Depth].At), &x);
            Dirty(T, &p.Frame[p.Depth]);
            Release(T, &p);
        }
    }
    return err;
}

int ExConvert(const EX_TREE *T, unsigned int Block, unsigned int Count)
{
    EX_PATH    p;
    EX_EXTENT  x;
    int        err;
    unsigned long long end = Block + (unsigned long long)Count;

    err = Find(T, Block, &p);
    if (err)
        return err;
    if (p.Frame[p.Depth].At < 0) {
        Release(T, &p);
        return EX_EIO;
    }
    ReadExtent(Entry(p.Frame[p.Depth].Node, (unsigned int)p.Frame[p.Depth].At), &x);
    Release(T, &p);
    if (Count == 0 || !x.Unwritten || Block < x.Block || end > x.Block + (unsigned long long)x.Length)
        return EX_EIO;

    if (end < x.Block + (unsigned long long)x.Length) {
        err = SplitAt(T, (unsigned int)end);
        if (err)
            return err;
    }
    if (Block > x.Block) {
        err = SplitAt(T, Block);
        if (err)
            return err;
    }

    /* the extent of exactly those blocks, written now, merged with written
       neighbours */
    err = Find(T, Block, &p);
    if (err)
        return err;
    {
        EX_FRAME     *leaf = &p.Frame[p.Depth];

        ReadExtent(Entry(leaf->Node, (unsigned int)leaf->At), &x);
        x.Unwritten = 0;
        WriteExtent(Entry(leaf->Node, (unsigned int)leaf->At), &x);
        MergeAround(leaf);
        err = Dirty(T, leaf);
        if (!err)
            err = MergeUp(T, &p);
    }
    Release(T, &p);
    return err;
}

int ExTruncate(const EX_TREE *T, unsigned int From)
{
    for (;;) {
        EX_PATH       p;
        EX_FRAME     *leaf;
        EX_EXTENT     x;
        unsigned int  level, at;
        int           err = Find(T, 0xFFFFFFFFu, &p);

        if (err)
            return err;
        leaf = &p.Frame[p.Depth];
        if (leaf->At < 0 && p.Depth == 0) {
            Release(T, &p);
            return EX_OK;                   /* nothing is mapped */
        }
        if (leaf->At >= 0) {
            at = (unsigned int)leaf->At;
            ReadExtent(Entry(leaf->Node, at), &x);
            if (x.Block + (unsigned long long)x.Length <= From) {
                Release(T, &p);
                return EX_OK;               /* nothing is mapped from there on */
            }

            if (x.Block < From) {
                /* across it: the part past it goes */
                unsigned int keep = From - x.Block;

                err = T->Io.Free(T->Io.Context, x.Start + keep, x.Length - keep, 0);
                if (!err) {
                    x.Length = keep;
                    WriteExtent(Entry(leaf->Node, at), &x);
                    err = Dirty(T, leaf);
                }
                Release(T, &p);
                return err;
            }

            /* the whole of it */
            err = T->Io.Free(T->Io.Context, x.Start, x.Length, 0);
            if (err) {
                Release(T, &p);
                return err;
            }
            Zero(Entry(leaf->Node, at), EX_ENTRY);
            SetEntries(leaf->Node, at);
            err = Dirty(T, leaf);
        }

        /* the nodes left empty (an empty leaf found so too), each out of its
           parent first, then freed (a block that cannot go back is lost to
           the volume, the tree stays sound) */
        for (level = p.Depth; level > 0 && !err && Entries(p.Frame[level].Node) == 0; level--) {
            EX_FRAME *f = &p.Frame[level], *up = &p.Frame[level - 1];

            Zero(Entry(up->Node, (unsigned int)up->At), EX_ENTRY);
            SetEntries(up->Node, (unsigned int)up->At);
            err = Dirty(T, up);
            T->Io.Put(T->Io.Context, f->Handle);
            f->Handle = 0;
            if (!err)
                err = T->Io.Free(T->Io.Context, f->Block, 1, 1);
        }
        if (!err && Entries(T->Root) == 0 && Depth(T->Root) > 0) {
            /* every node gone: the root is a leaf again */
            Put16(T->Root + EX_H_DEPTH, 0);
            Put16(T->Root + EX_H_MAX, EX_ROOT_MAX);
            err = T->Io.Dirty(T->Io.Context, 0);
        }
        Release(T, &p);
        if (err)
            return err;
    }
}
