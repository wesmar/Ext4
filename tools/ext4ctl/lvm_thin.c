/**
 * lvm_thin.c - a thin volume's chunks, found through the pool's dm-thin metadata.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * A thin volume lives in a pool. Its chunks are found through dm-thin's
 * metadata (the pool's _tmeta LV, 4 KiB blocks): the superblock gives the
 * root of a two-level btree, device id -> a btree of virtual block ->
 * (data block << 24 | time); the data block is a chunk of the pool's
 * _tdata LV. Chunks never written are holes (zeros).
 */

#include "lvm_internal.h"

typedef struct _THIN {
    LVM_VG         *G;
    const LVM_NODE *Meta;                   /* the pool's _tmeta LV (linear) */
    const LVM_NODE *Data;                   /* the pool's _tdata LV (linear) */
    UINT64          ChunkBytes;
    UINT8           Block[THIN_BLOCK];
    RUNS           *R;
    UINT64          VolumeChunks;
    UINT64          NextChunk;              /* the first chunk not yet in R */
    ULONG           Mapped;
} THIN;

/* a btree node: its header, then MaxEntries keys, then MaxEntries values */
typedef struct _THIN_NODE {
    UINT32  Flags;
    UINT32  Entries;
    UINT32  MaxEntries;
} THIN_NODE;

static BOOL
ThinRead(THIN *T, UINT64 Block)
{
    UINT64 Pv, Left;

    if (!LvmLinearToPv(T->G, T->Meta, Block * THIN_BLOCK, &Pv, &Left) || Left < THIN_BLOCK) {
        return FALSE;
    }
    return LuksReadAt(&T->G->Dev, Pv, T->Block, THIN_BLOCK);
}

static UINT64
ThinKey(const THIN *T, UINT32 i)
{
    return Le64(T->Block + sizeof(THIN_NODE_HEADER) + sizeof(UINT64) * i);
}

static UINT64
ThinValue(const THIN *T, const THIN_NODE *N, UINT32 i)
{
    return Le64(T->Block + sizeof(THIN_NODE_HEADER) + sizeof(UINT64) * ((SIZE_T)N->MaxEntries + i));
}

/* a btree node read into T->Block, sanity-checked */
static BOOL
ThinNode(THIN *T, UINT64 Block, THIN_NODE *N)
{
    const THIN_NODE_HEADER *H = (const THIN_NODE_HEADER *)T->Block;

    if (!ThinRead(T, Block) || Le64(H->Blocknr) != Block) {
        return FALSE;
    }
    N->Flags = Le32(H->Flags);
    N->Entries = Le32(H->NrEntries);
    N->MaxEntries = Le32(H->MaxEntries);
    return (N->Flags == THIN_INTERNAL_NODE || N->Flags == THIN_LEAF_NODE) &&
           N->Entries <= N->MaxEntries && N->MaxEntries > 0 &&
           Le32(H->ValueSize) == THIN_VALUE_BYTES &&
           sizeof(THIN_NODE_HEADER) + (UINT64)N->MaxEntries * 2 * sizeof(UINT64) <= THIN_BLOCK;
}

/* value of Key in the btree at Root */
static BOOL
ThinLookup(THIN *T, UINT64 Root, UINT64 Key, UINT64 *Value)
{
    int Depth;

    for (Depth = 0; Depth < THIN_MAX_DEPTH; Depth++) {
        THIN_NODE   N;
        UINT32      i, Pick = MAXUINT32;

        if (!ThinNode(T, Root, &N) || N.Entries == 0) {
            return FALSE;
        }
        for (i = 0; i < N.Entries; i++) {
            if (ThinKey(T, i) <= Key) {
                Pick = i;
            } else {
                break;
            }
        }
        if (Pick == MAXUINT32) {
            return FALSE;
        }
        if (N.Flags == THIN_LEAF_NODE) {
            if (ThinKey(T, Pick) != Key) {
                return FALSE;
            }
            *Value = ThinValue(T, &N, Pick);
            return TRUE;
        }
        Root = ThinValue(T, &N, Pick);
    }
    return FALSE;
}

/* one mapped chunk, in virtual order: the holes before it, then the chunk */
static BOOL
ThinChunk(THIN *T, UINT64 Virtual, UINT64 DataBlock)
{
    UINT64 Pv, Left;

    if (Virtual < T->NextChunk || Virtual >= T->VolumeChunks) {
        return Virtual >= T->VolumeChunks;
    }
    if (Virtual > T->NextChunk) {
        RunsAdd(T->R, T->NextChunk * T->ChunkBytes, (Virtual - T->NextChunk) * T->ChunkBytes, EXT4_LV_HOLE);
    }
    if (!LvmLinearToPv(T->G, T->Data, DataBlock * T->ChunkBytes, &Pv, &Left) || Left < T->ChunkBytes) {
        return FALSE;
    }
    RunsAdd(T->R, Virtual * T->ChunkBytes, T->ChunkBytes, Pv);
    T->NextChunk = Virtual + 1;
    T->Mapped++;
    return TRUE;
}

/* every mapping of the subtree at Block, in key order */
static BOOL
ThinWalk(THIN *T, UINT64 Block, int Depth)
{
    THIN_NODE   N;
    UINT64     *Keys, *Values;
    UINT32      i;
    BOOL        Ok = TRUE;

    if (Depth > THIN_MAX_DEPTH || !ThinNode(T, Block, &N)) {
        return FALSE;
    }
    /* the buffer is reused by the recursion: keep this node's entries */
    Keys = (UINT64 *)HeapAlloc(GetProcessHeap(), 0, ((SIZE_T)N.Entries + 1) * 2 * sizeof(UINT64));
    if (Keys == NULL) {
        return FALSE;
    }
    Values = Keys + N.Entries;
    for (i = 0; i < N.Entries; i++) {
        Keys[i] = ThinKey(T, i);
        Values[i] = ThinValue(T, &N, i);
    }
    for (i = 0; i < N.Entries && Ok; i++) {
        Ok = N.Flags == THIN_INTERNAL_NODE ? ThinWalk(T, Values[i], Depth + 1) :
                                             ThinChunk(T, Keys[i], Values[i] >> THIN_TIME_BITS);
    }
    HeapFree(GetProcessHeap(), 0, Keys);
    return Ok;
}

/* the pool of a thin LV: its metadata and data volumes, its chunk size */
static BOOL
ThinPool(LVM_VG *G, const LVM_NODE *Lv, THIN *T, UINT64 *DeviceId, const char **PoolName)
{
    const LVM_NODE *Seg = LvmOnlySegment(Lv, NULL), *Pool, *PoolSeg;
    const char     *MetaName, *DataName;
    UINT64          Chunk;

    if (Seg == NULL || (*PoolName = LvmStr(Seg, "thin_pool")) == NULL ||
        !LvmU64(Seg, "device_id", DeviceId) || (Pool = LvmFindLv(G, *PoolName)) == NULL ||
        (PoolSeg = LvmOnlySegment(Pool, NULL)) == NULL ||
        (MetaName = LvmStr(PoolSeg, "metadata")) == NULL || (DataName = LvmStr(PoolSeg, "pool")) == NULL ||
        !LvmU64(PoolSeg, "chunk_size", &Chunk) || Chunk == 0) {
        Fail("thin volume %s: pool metadata incomplete", Lv->Name);
        return FALSE;
    }
    T->G = G;
    T->Meta = LvmFindLv(G, MetaName);
    T->Data = LvmFindLv(G, DataName);
    T->ChunkBytes = Chunk * LVM_SECTOR;
    if (T->Meta == NULL || T->Data == NULL) {
        Fail("thin pool %s: metadata or data volume missing", *PoolName);
        return FALSE;
    }
    return TRUE;
}

BOOL
LvmThinRuns(LVM_VG *G, const LVM_NODE *Lv, UINT64 Size, RUNS *R, ULONG *Mapped)
{
    const THIN_SUPERBLOCK  *Super;
    const char             *PoolName;
    UINT64                  DeviceId, Subtree, DataBlock;
    THIN                    T;

    memset(&T, 0, sizeof(T));
    if (!ThinPool(G, Lv, &T, &DeviceId, &PoolName)) {
        return FALSE;
    }
    T.R = R;
    T.VolumeChunks = Size / T.ChunkBytes;
    if (Size % T.ChunkBytes) {
        Fail("thin pool %s: metadata or data volume missing", PoolName);
        return FALSE;
    }

    /* superblock: magic, data mapping root, data block size = chunk size */
    Super = (const THIN_SUPERBLOCK *)T.Block;
    if (!ThinRead(&T, 0) || Le64(Super->Blocknr) != 0 || Le64(Super->Magic) != THIN_MAGIC) {
        Fail("thin pool %s: no dm-thin superblock", PoolName);
        return FALSE;
    }
    DataBlock = Le32(Super->DataBlockSize);
    if (DataBlock * LVM_SECTOR != T.ChunkBytes) {
        Fail("thin pool %s: block size %llu, LVM says %llu", PoolName, DataBlock, T.ChunkBytes / LVM_SECTOR);
        return FALSE;
    }

    if (!ThinLookup(&T, Le64(Super->DataMappingRoot), DeviceId, &Subtree)) {
        /* a thin volume never written to has no mappings at all */
        RunsAdd(R, 0, Size, EXT4_LV_HOLE);
        *Mapped = 0;
        return !R->Failed;
    }
    if (!ThinWalk(&T, Subtree, 0)) {
        Fail("thin volume %s: the mapping btree is damaged", Lv->Name);
        return FALSE;
    }
    if (T.NextChunk < T.VolumeChunks) {
        RunsAdd(R, T.NextChunk * T.ChunkBytes, (T.VolumeChunks - T.NextChunk) * T.ChunkBytes, EXT4_LV_HOLE);
    }
    *Mapped = T.Mapped;
    return !R->Failed;
}
