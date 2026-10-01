/**
 * argon2.c - Argon2i and Argon2id, version 1.3 (RFC 9106), for LUKS2 keyslots.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The memory is Lanes x LaneLength blocks of 1 KiB, filled in Passes
 * passes of 4 slices each. Within a slice every lane depends only on
 * blocks of earlier slices and on its own current slice, so the lanes of a
 * slice are computed by one thread each and joined before the next slice -
 * cryptsetup sets Lanes to the CPU count for exactly that reason.
 */

#include "ext4ctl.h"

#define ARGON2_BLOCK_BYTES      1024
#define ARGON2_QWORDS           (ARGON2_BLOCK_BYTES / 8)
#define ARGON2_SYNC_POINTS      4
#define ARGON2_ADDRESSES        ARGON2_QWORDS       /* per address block */
#define ARGON2_VERSION          0x13
#define ARGON2_PREHASH          64
#define ARGON2_PREHASH_SEED     (ARGON2_PREHASH + 8)
#define ARGON2_MAX_LANES        64

typedef struct _ARGON2_BLOCK { UINT64 v[ARGON2_QWORDS]; } ARGON2_BLOCK;

typedef struct _ARGON2_STATE {
    ARGON2_BLOCK   *Memory;
    UINT32          Passes;
    UINT32          Lanes;
    UINT32          LaneLength;
    UINT32          SegmentLength;
    UINT32          MemoryBlocks;
    UINT32          Type;
} ARGON2_STATE;

typedef struct _ARGON2_POSITION {
    ARGON2_STATE   *State;
    UINT32          Pass;
    UINT32          Lane;
    UINT32          Slice;
} ARGON2_POSITION;

static void Store32(UINT8 *p, UINT32 v) { memcpy(p, &v, 4); }       /* little-endian host */

static UINT64 Rotr(UINT64 x, int n) { return (x >> n) | (x << (64 - n)); }

/* the multiply-hardened BLAKE2b round function */
static UINT64 BlaMka(UINT64 x, UINT64 y)
{
    const UINT64 m = 0xFFFFFFFFULL;
    return x + y + 2 * ((x & m) * (y & m));
}

#define GB(a, b, c, d)                                          \
    do {                                                        \
        a = BlaMka(a, b); d = Rotr(d ^ a, 32);                  \
        c = BlaMka(c, d); b = Rotr(b ^ c, 24);                  \
        a = BlaMka(a, b); d = Rotr(d ^ a, 16);                  \
        c = BlaMka(c, d); b = Rotr(b ^ c, 63);                  \
    } while (0)

#define ROUND(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15) \
    do {                                                        \
        GB(v0, v4, v8, v12); GB(v1, v5, v9, v13);               \
        GB(v2, v6, v10, v14); GB(v3, v7, v11, v15);             \
        GB(v0, v5, v10, v15); GB(v1, v6, v11, v12);             \
        GB(v2, v7, v8, v13); GB(v3, v4, v9, v14);               \
    } while (0)

/* Next = G(Prev, Ref), XORed into Next's old value from the second pass on */
static void
FillBlock(const ARGON2_BLOCK *Prev, const ARGON2_BLOCK *Ref, ARGON2_BLOCK *Next, BOOL WithXor)
{
    ARGON2_BLOCK R, T;
    UINT64 *v = R.v;
    int i;

    for (i = 0; i < ARGON2_QWORDS; i++) {
        R.v[i] = Ref->v[i] ^ Prev->v[i];
        T.v[i] = WithXor ? R.v[i] ^ Next->v[i] : R.v[i];
    }
    for (i = 0; i < 8; i++) {               /* rows of 16 words */
        UINT64 *r = v + 16 * i;
        ROUND(r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
              r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
    }
    for (i = 0; i < 8; i++) {               /* columns of 2-word pairs */
        ROUND(v[2 * i], v[2 * i + 1], v[2 * i + 16], v[2 * i + 17],
              v[2 * i + 32], v[2 * i + 33], v[2 * i + 48], v[2 * i + 49],
              v[2 * i + 64], v[2 * i + 65], v[2 * i + 80], v[2 * i + 81],
              v[2 * i + 96], v[2 * i + 97], v[2 * i + 112], v[2 * i + 113]);
    }
    for (i = 0; i < ARGON2_QWORDS; i++) {
        Next->v[i] = T.v[i] ^ R.v[i];
    }
    SecureZeroMemory(&R, sizeof(R));
    SecureZeroMemory(&T, sizeof(T));
}

/* H': BLAKE2b stretched to any output length */
static void
HashLong(void *Out, UINT32 OutLength, const void *In, size_t InLength)
{
    UINT8   Len[4], V[BLAKE2B_OUT], Tmp[BLAKE2B_OUT];
    UINT8  *o = (UINT8 *)Out;
    BLAKE2B S;

    Store32(Len, OutLength);
    if (OutLength <= BLAKE2B_OUT) {
        Blake2bInit(&S, OutLength);
        Blake2bUpdate(&S, Len, sizeof(Len));
        Blake2bUpdate(&S, In, InLength);
        Blake2bFinal(&S, Out);
        return;
    }

    Blake2bInit(&S, BLAKE2B_OUT);
    Blake2bUpdate(&S, Len, sizeof(Len));
    Blake2bUpdate(&S, In, InLength);
    Blake2bFinal(&S, V);
    memcpy(o, V, BLAKE2B_OUT / 2);
    o += BLAKE2B_OUT / 2;
    OutLength -= BLAKE2B_OUT / 2;

    while (OutLength > BLAKE2B_OUT) {
        memcpy(Tmp, V, BLAKE2B_OUT);
        Blake2b(V, BLAKE2B_OUT, Tmp, BLAKE2B_OUT);
        memcpy(o, V, BLAKE2B_OUT / 2);
        o += BLAKE2B_OUT / 2;
        OutLength -= BLAKE2B_OUT / 2;
    }
    memcpy(Tmp, V, BLAKE2B_OUT);
    Blake2b(V, OutLength, Tmp, BLAKE2B_OUT);
    memcpy(o, V, OutLength);
    SecureZeroMemory(V, sizeof(V));
    SecureZeroMemory(Tmp, sizeof(Tmp));
}

/* the reference block within the lane chosen, from 32 random bits */
static UINT32
IndexAlpha(const ARGON2_POSITION *Pos, UINT32 Index, UINT32 PseudoRand, BOOL SameLane)
{
    const ARGON2_STATE *S = Pos->State;
    UINT32  Area;
    UINT64  Relative;
    UINT32  Start = 0;

    if (Pos->Pass == 0) {
        if (Pos->Slice == 0) {
            Area = Index - 1;
        } else if (SameLane) {
            Area = Pos->Slice * S->SegmentLength + Index - 1;
        } else {
            Area = Pos->Slice * S->SegmentLength - (Index == 0 ? 1 : 0);
        }
    } else {
        if (SameLane) {
            Area = S->LaneLength - S->SegmentLength + Index - 1;
        } else {
            Area = S->LaneLength - S->SegmentLength - (Index == 0 ? 1 : 0);
        }
    }

    Relative = PseudoRand;
    Relative = (Relative * Relative) >> 32;
    Relative = Area - 1 - (((UINT64)Area * Relative) >> 32);

    if (Pos->Pass != 0 && Pos->Slice != ARGON2_SYNC_POINTS - 1) {
        Start = (Pos->Slice + 1) * S->SegmentLength;
    }
    return (UINT32)((Start + Relative) % S->LaneLength);
}

static void
NextAddresses(ARGON2_BLOCK *Address, ARGON2_BLOCK *Input, const ARGON2_BLOCK *Zero)
{
    Input->v[6]++;
    FillBlock(Zero, Input, Address, FALSE);
    FillBlock(Zero, Address, Address, FALSE);
}

static void
FillSegment(const ARGON2_POSITION *Pos)
{
    ARGON2_STATE   *S = Pos->State;
    ARGON2_BLOCK    Address, Input, Zero;
    BOOL            Independent;
    UINT32          Start = 0, i;
    UINT64          Current, Previous;

    Independent = S->Type == ARGON2_I ||
                  (S->Type == ARGON2_ID && Pos->Pass == 0 && Pos->Slice < ARGON2_SYNC_POINTS / 2);

    if (Independent) {
        memset(&Zero, 0, sizeof(Zero));
        memset(&Input, 0, sizeof(Input));
        Input.v[0] = Pos->Pass;
        Input.v[1] = Pos->Lane;
        Input.v[2] = Pos->Slice;
        Input.v[3] = S->MemoryBlocks;
        Input.v[4] = S->Passes;
        Input.v[5] = S->Type;
    }

    if (Pos->Pass == 0 && Pos->Slice == 0) {
        Start = 2;                          /* the first two blocks come from H0 */
        if (Independent) {
            NextAddresses(&Address, &Input, &Zero);
        }
    }

    Current = (UINT64)Pos->Lane * S->LaneLength + (UINT64)Pos->Slice * S->SegmentLength + Start;
    Previous = (Current % S->LaneLength == 0) ? Current + S->LaneLength - 1 : Current - 1;

    for (i = Start; i < S->SegmentLength; i++, Current++, Previous++) {
        UINT64  Rand;
        UINT32  RefLane, RefIndex;

        if (Current % S->LaneLength == 1) {
            Previous = Current - 1;
        }
        if (Independent) {
            if (i % ARGON2_ADDRESSES == 0) {
                NextAddresses(&Address, &Input, &Zero);
            }
            Rand = Address.v[i % ARGON2_ADDRESSES];
        } else {
            Rand = S->Memory[Previous].v[0];
        }

        RefLane = (UINT32)((Rand >> 32) % S->Lanes);
        if (Pos->Pass == 0 && Pos->Slice == 0) {
            RefLane = Pos->Lane;
        }
        RefIndex = IndexAlpha(Pos, i, (UINT32)Rand, RefLane == Pos->Lane);

        FillBlock(&S->Memory[Previous],
                  &S->Memory[(UINT64)S->LaneLength * RefLane + RefIndex],
                  &S->Memory[Current], Pos->Pass != 0);
    }
}

static DWORD WINAPI
FillSegmentThread(LPVOID Context)
{
    FillSegment((const ARGON2_POSITION *)Context);
    return 0;
}

BOOL
Argon2(const ARGON2_INPUT *In, void *Out, UINT32 OutLength)
{
    ARGON2_STATE    S;
    UINT8           H0[ARGON2_PREHASH_SEED];
    UINT8           Word[4];
    UINT8           Bytes[ARGON2_BLOCK_BYTES];
    ARGON2_BLOCK    Final;
    BLAKE2B         B;
    UINT32          Lane, Pass, Slice, i;
    SIZE_T          MemoryBytes;

    if (In->Lanes == 0 || In->Lanes > ARGON2_MAX_LANES || In->Passes == 0 || OutLength < 4 ||
        (In->Type != ARGON2_I && In->Type != ARGON2_ID)) {
        return FALSE;
    }

    memset(&S, 0, sizeof(S));
    S.Passes = In->Passes;
    S.Lanes = In->Lanes;
    S.Type = In->Type;
    S.MemoryBlocks = max(In->MemoryKiB, 2 * ARGON2_SYNC_POINTS * In->Lanes);
    S.SegmentLength = S.MemoryBlocks / (In->Lanes * ARGON2_SYNC_POINTS);
    S.MemoryBlocks = S.SegmentLength * In->Lanes * ARGON2_SYNC_POINTS;
    S.LaneLength = S.SegmentLength * ARGON2_SYNC_POINTS;

    MemoryBytes = (SIZE_T)S.MemoryBlocks * sizeof(ARGON2_BLOCK);
    S.Memory = (ARGON2_BLOCK *)VirtualAlloc(NULL, MemoryBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (S.Memory == NULL) {
        return FALSE;
    }

    /* H0 */
    Blake2bInit(&B, ARGON2_PREHASH);
    Store32(Word, In->Lanes);                   Blake2bUpdate(&B, Word, 4);
    Store32(Word, OutLength);                   Blake2bUpdate(&B, Word, 4);
    Store32(Word, In->MemoryKiB);               Blake2bUpdate(&B, Word, 4);
    Store32(Word, In->Passes);                  Blake2bUpdate(&B, Word, 4);
    Store32(Word, ARGON2_VERSION);              Blake2bUpdate(&B, Word, 4);
    Store32(Word, In->Type);                    Blake2bUpdate(&B, Word, 4);
    Store32(Word, (UINT32)In->PasswordLength);  Blake2bUpdate(&B, Word, 4);
    Blake2bUpdate(&B, In->Password, In->PasswordLength);
    Store32(Word, (UINT32)In->SaltLength);      Blake2bUpdate(&B, Word, 4);
    Blake2bUpdate(&B, In->Salt, In->SaltLength);
    Store32(Word, (UINT32)In->SecretLength);    Blake2bUpdate(&B, Word, 4);
    if (In->SecretLength) Blake2bUpdate(&B, In->Secret, In->SecretLength);
    Store32(Word, (UINT32)In->DataLength);      Blake2bUpdate(&B, Word, 4);
    if (In->DataLength) Blake2bUpdate(&B, In->Data, In->DataLength);
    Blake2bFinal(&B, H0);

    /* the first two blocks of every lane */
    for (Lane = 0; Lane < S.Lanes; Lane++) {
        for (i = 0; i < 2; i++) {
            Store32(H0 + ARGON2_PREHASH, i);
            Store32(H0 + ARGON2_PREHASH + 4, Lane);
            HashLong(Bytes, ARGON2_BLOCK_BYTES, H0, ARGON2_PREHASH_SEED);
            memcpy(&S.Memory[(UINT64)Lane * S.LaneLength + i], Bytes, ARGON2_BLOCK_BYTES);
        }
    }

    /* passes x slices; the lanes of a slice in parallel */
    for (Pass = 0; Pass < S.Passes; Pass++) {
        for (Slice = 0; Slice < ARGON2_SYNC_POINTS; Slice++) {
            ARGON2_POSITION Pos[ARGON2_MAX_LANES];
            HANDLE          Threads[ARGON2_MAX_LANES];

            for (Lane = 0; Lane < S.Lanes; Lane++) {
                Pos[Lane].State = &S;
                Pos[Lane].Pass = Pass;
                Pos[Lane].Lane = Lane;
                Pos[Lane].Slice = Slice;
                Threads[Lane] = S.Lanes > 1 ?
                    CreateThread(NULL, 0, FillSegmentThread, &Pos[Lane], 0, NULL) : NULL;
                if (Threads[Lane] == NULL) {
                    FillSegment(&Pos[Lane]);        /* one lane, or no thread: inline */
                }
            }
            for (Lane = 0; Lane < S.Lanes; Lane++) {
                if (Threads[Lane]) {
                    WaitForSingleObject(Threads[Lane], INFINITE);
                    CloseHandle(Threads[Lane]);
                }
            }
        }
    }

    /* the last blocks of all lanes, XORed, stretched to the tag */
    Final = S.Memory[S.LaneLength - 1];
    for (Lane = 1; Lane < S.Lanes; Lane++) {
        const ARGON2_BLOCK *Last = &S.Memory[(UINT64)Lane * S.LaneLength + S.LaneLength - 1];
        for (i = 0; i < ARGON2_QWORDS; i++) {
            Final.v[i] ^= Last->v[i];
        }
    }
    memcpy(Bytes, &Final, ARGON2_BLOCK_BYTES);
    HashLong(Out, OutLength, Bytes, ARGON2_BLOCK_BYTES);

    SecureZeroMemory(S.Memory, MemoryBytes);
    VirtualFree(S.Memory, 0, MEM_RELEASE);
    SecureZeroMemory(H0, sizeof(H0));
    SecureZeroMemory(Bytes, sizeof(Bytes));
    SecureZeroMemory(&Final, sizeof(Final));
    return TRUE;
}
