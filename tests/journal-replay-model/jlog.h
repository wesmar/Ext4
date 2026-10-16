/*
 * jlog.h - generated jbd2 logs, and what replaying each must give.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * A case is a ring filled with an older round of transactions, then a new
 * log written from Start: transactions with revoke blocks, descriptors and
 * data (escaped where the first word is the magic), commit blocks with the
 * checksums the features call for, and one kind of damage. The generator
 * knows what it wrote, so the expectation is computed from that, not from
 * the bytes: the end of the log, the error, the failed commit, and every
 * home block (the newest unrevoked copy among the replayed transactions).
 * Used by test.c (MSVC, x86/x64, the log in memory) and wsl.c (gcc, the log
 * in a real ext4 image that e2fsck replays too).
 */

#ifndef JLOG_H
#define JLOG_H

#include <string.h>
#include "../../src/journal/JournalFormat.h"
#include "../../src/journal/JournalReplay.h"

typedef struct { unsigned long long s; } JL_RNG;

static unsigned int JlRand(JL_RNG *r)
{
    r->s ^= r->s >> 12;
    r->s ^= r->s << 25;
    r->s ^= r->s >> 27;
    return (unsigned int)((r->s * 2685821657736338717ull) >> 32);
}

static unsigned int JlBelow(JL_RNG *r, unsigned int n)
{
    return n ? JlRand(r) % n : 0;
}

static unsigned int JlCrcTable[256];

static void JlCrcInit(void)
{
    unsigned int i, k, c;
    for (i = 0; i < 256; i++) {
        c = i;
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ ((c & 1) ? 0x82F63B78u : 0);
        JlCrcTable[i] = c;
    }
}

/* crc32c without the final inversion (the kernel's crc32c_le) */
static unsigned int JlCrc32c(unsigned int crc, const unsigned char *p, unsigned int n)
{
    while (n--)
        crc = JlCrcTable[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc;
}

/* the feature sets a case is written with */
typedef struct { unsigned int Compat, Incompat; const char *Name; } JL_FEATURES;
static const JL_FEATURES JlFeatureSets[] = {
    { 0, 0, "plain" },
    { 0, JF_INCOMPAT_REVOKE | JF_INCOMPAT_64BIT, "64bit" },
    { JF_COMPAT_CHECKSUM, JF_INCOMPAT_REVOKE, "v1" },
    { JF_COMPAT_CHECKSUM, JF_INCOMPAT_REVOKE | JF_INCOMPAT_ASYNC_COMMIT, "v1-async" },
    { 0, JF_INCOMPAT_REVOKE | JF_INCOMPAT_CSUM_V2, "v2" },
    { 0, JF_INCOMPAT_REVOKE | JF_INCOMPAT_CSUM_V2 | JF_INCOMPAT_64BIT | JF_INCOMPAT_ASYNC_COMMIT, "v2-64bit-async" },
    { 0, JF_INCOMPAT_REVOKE | JF_INCOMPAT_CSUM_V3, "v3" },
    { 0, JF_INCOMPAT_REVOKE | JF_INCOMPAT_CSUM_V3 | JF_INCOMPAT_64BIT, "v3-64bit" },
    { 0, JF_INCOMPAT_REVOKE | JF_INCOMPAT_CSUM_V3 | JF_INCOMPAT_ASYNC_COMMIT, "v3-async" },
};
#define JL_FEATURE_SETS ((unsigned int)(sizeof(JlFeatureSets) / sizeof(JlFeatureSets[0])))

/* one kind of damage per case */
enum {
    D_NONE,             /* the log ends by a block of something else */
    D_TORN,             /* the last transaction has no commit block */
    D_BAD_COMMIT,       /* a commit fails its checksum */
    D_STALE_COMMIT,     /* ... with a time older than the last good commit */
    D_BAD_DESCRIPTOR,   /* a descriptor fails its tail checksum (v2/v3) */
    D_STALE_DESCRIPTOR, /* ... and its commit is older: left from before */
    D_BAD_REVOKE,       /* a revoke block fails its tail checksum (v2/v3) */
    D_STALE_REVOKE,
    D_BAD_DATA,         /* a data block does not match its tag (v1: the commit) */
    D_UNKNOWN_TYPE,     /* a header of an unknown type inside a transaction */
    D_FOREIGN_OLDER,    /* after the log, a transaction of another journal */
    D_FOREIGN_NEWER,    /*   (other uuid, same next ID): older / newer time */
    D_REVOKE_COUNT,     /* a revoke block claims more bytes than it has */
    D_KINDS
};
static const char *const JlDamageNames[D_KINDS] = {
    "none", "torn", "bad-commit", "stale-commit", "bad-descriptor", "stale-descriptor",
    "bad-revoke", "stale-revoke", "bad-data", "unknown-type", "foreign-older", "foreign-newer",
    "revoke-count"
};

#define JL_MAX_TXN      16
#define JL_MAX_COPIES   48
#define JL_MAX_REVOKES  24
#define JL_MAX_HOME     64

typedef struct {
    unsigned int        Copies;
    unsigned int        Home[JL_MAX_COPIES];     /* home index of each copy */
    unsigned int        Seed[JL_MAX_COPIES];     /* its content */
    int                 Escaped[JL_MAX_COPIES];
    int                 Excluded[JL_MAX_COPIES]; /* fails its tag checksum */
    int                 NoSum;                   /* v1: a commit without a checksum */
    unsigned int        Revokes;
    unsigned int        Revoked[JL_MAX_REVOKES]; /* home indexes */
    unsigned long long  Time;
} JL_TXN;

typedef struct {
    /* geometry and features */
    unsigned int        BlockSize, First, Last, Start, Seq;
    unsigned int        Compat, Incompat, CsumSeed;
    unsigned long long  Base;                   /* volume block of home index 0 */
    unsigned int        Homes;                  /* home blocks in use */
    unsigned int        Txns;
    JL_TXN              Txn[JL_MAX_TXN];
    int                 Damage;
    unsigned int        DamageTxn;
    /* the expectation */
    int                 ExpectError;            /* JR_* of the replay */
    int                 ExpectScanError;        /* JR_* of the scan alone */
    int                 NothingHome;            /* an error before the replay walk */
    unsigned int        ExpectEnd;
    unsigned int        ExpectScanEnd;          /* where the scan stops */
    int                 ExpectFailed;
    unsigned int        ExpectFailedTid;
    /* the log: Last blocks */
    unsigned char      *Log;
    unsigned int        Written;                /* blocks of the new log */
} JL_CASE;

static unsigned char *JlAt(JL_CASE *C, unsigned int Pos)
{
    return C->Log + (unsigned long long)Pos * C->BlockSize;
}

static unsigned int JlNext(const JL_CASE *C, unsigned int Pos)
{
    return Pos + 1 >= C->Last ? C->First : Pos + 1;
}

/* the content of a logged copy, and of a home block before the replay */
static void JlFill(unsigned char *Block, unsigned int Size, unsigned int Seed)
{
    JL_RNG r;
    unsigned int i;
    r.s = 0x9E3779B97F4A7C15ull ^ ((unsigned long long)Seed << 17) ^ Seed;
    for (i = 0; i < Size; i += 4) {
        unsigned int v = JlRand(&r);
        memcpy(Block + i, &v, 4);
    }
}

static void JlHomeInitial(unsigned char *Block, unsigned int Size, unsigned int Index)
{
    JlFill(Block, Size, 0xA5A50000u + Index);
}

static void JlCopyContent(const JL_CASE *C, const JL_TXN *T, unsigned int j, unsigned char *Block)
{
    JlFill(Block, C->BlockSize, T->Seed[j]);
    if (T->Escaped[j])
        JfPut32(Block, JF_MAGIC);
}

static void JlHeader(unsigned char *b, unsigned int Size, unsigned int Type, unsigned int Tid)
{
    memset(b, 0, Size);
    JfPut32(b + JF_H_MAGIC, JF_MAGIC);
    JfPut32(b + JF_H_TYPE, Type);
    JfPut32(b + JF_H_SEQUENCE, Tid);
}

static void JlSealTail(const JL_CASE *C, unsigned char *b, unsigned int Seed)
{
    if (!JfHasCsumV2V3(C->Incompat))
        return;
    JfPut32(b + C->BlockSize - JF_TAIL_SIZE, 0);
    JfPut32(b + C->BlockSize - JF_TAIL_SIZE, JlCrc32c(Seed, b, C->BlockSize));
}

static unsigned int JlDescriptorCapacity(const JL_CASE *C)
{
    unsigned int tail = JfHasCsumV2V3(C->Incompat) ? JF_TAIL_SIZE : 0;
    unsigned int at = JF_HEADER_SIZE, n = 0, tb = JfTagBytes(C->Incompat);
    while (at + tb + (n == 0 ? JF_UUID_SIZE : 0) <= C->BlockSize - tail) {
        at += tb + (n == 0 ? JF_UUID_SIZE : 0);
        n++;
    }
    return n;
}

static unsigned int JlRevokeCapacity(const JL_CASE *C)
{
    unsigned int tail = JfHasCsumV2V3(C->Incompat) ? JF_TAIL_SIZE : 0;
    unsigned int size = (C->Incompat & JF_INCOMPAT_64BIT) ? 8 : 4;
    return (C->BlockSize - tail - JF_REVOKE_HEADER_SIZE) / size;
}

/* how many log blocks a transaction takes, with the group sizes chosen */
typedef struct {
    unsigned int RevokeBlocks;
    unsigned int RevokeSplit[JL_MAX_REVOKES];   /* records per revoke block */
    unsigned int Groups;
    unsigned int GroupSize[JL_MAX_COPIES];
    int          OmitLast[JL_MAX_COPIES];       /* a full descriptor, no LAST_TAG */
} JL_LAYOUT;

static unsigned int JlPlan(const JL_CASE *C, const JL_TXN *T, JL_LAYOUT *L, JL_RNG *r)
{
    unsigned int cap = JlDescriptorCapacity(C), rcap = JlRevokeCapacity(C), left, blocks = 1;

    L->RevokeBlocks = 0;
    for (left = T->Revokes; left; ) {
        unsigned int n = 1 + JlBelow(r, left < rcap ? left : rcap);
        if (n > left)
            n = left;
        L->RevokeSplit[L->RevokeBlocks++] = n;
        left -= n;
        blocks++;
    }
    L->Groups = 0;
    for (left = T->Copies; left; ) {
        unsigned int n = 1 + JlBelow(r, left < cap ? left : cap);
        if (n > left)
            n = left;
        if (JlBelow(r, 4) == 0 && left >= cap)
            n = cap;
        L->GroupSize[L->Groups] = n;
        L->OmitLast[L->Groups] = n == cap && JlBelow(r, 2);
        L->Groups++;
        left -= n;
        blocks += 1 + n;
    }
    return blocks;
}

/* write one transaction at *Pos; Seed is the checksum seed it is written with */
static void JlWriteTxn(JL_CASE *C, unsigned int Index, unsigned int Tid, const JL_TXN *T, const JL_LAYOUT *L,
                       unsigned int Seed, int Damage, unsigned int *Pos, int Commit, JL_RNG *r)
{
    unsigned int bs = C->BlockSize, tb = JfTagBytes(C->Incompat), g, k, j = 0, rv = 0;
    unsigned int crc = 0xFFFFFFFFu;
    int csum = JfHasCsumV2V3(C->Incompat), damaged = 0;
    unsigned char *b;

    /* revoke blocks */
    for (g = 0; g < L->RevokeBlocks; g++) {
        unsigned int size = (C->Incompat & JF_INCOMPAT_64BIT) ? 8 : 4, at = JF_REVOKE_HEADER_SIZE;
        b = JlAt(C, *Pos);
        JlHeader(b, bs, JF_REVOKE, Tid);
        for (k = 0; k < L->RevokeSplit[g]; k++, rv++) {
            unsigned long long block = C->Base + T->Revoked[rv];
            if (size == 8)
                JfPut64(b + at, block);
            else
                JfPut32(b + at, (unsigned int)block);
            at += size;
        }
        JfPut32(b + JF_R_COUNT, at);
        if (Damage == D_REVOKE_COUNT && !damaged) {
            JfPut32(b + JF_R_COUNT, bs - (JfHasCsumV2V3(C->Incompat) ? JF_TAIL_SIZE : 0) + 4);
            damaged = 1;
        }
        JlSealTail(C, b, Seed);
        if ((Damage == D_BAD_REVOKE || Damage == D_STALE_REVOKE) && !damaged) {
            b[bs - JF_TAIL_SIZE - 1] ^= 0x40;   /* before the tail: only the sum sees it */
            damaged = 1;
        }
        *Pos = JlNext(C, *Pos);
    }

    /* descriptor groups */
    for (g = 0; g < L->Groups; g++) {
        unsigned int at = JF_HEADER_SIZE, dpos = *Pos;
        unsigned char *d = JlAt(C, dpos);
        unsigned int first = j;

        JlHeader(d, bs, JF_DESCRIPTOR, Tid);
        *Pos = JlNext(C, *Pos);
        for (k = 0; k < L->GroupSize[g]; k++, j++) {
            unsigned char *tag = d + at;
            unsigned long long block = C->Base + T->Home[j];
            unsigned int flags = 0;
            unsigned char *data = JlAt(C, *Pos);

            JlCopyContent(C, T, j, data);
            if (T->Escaped[j]) {
                JfPut32(data, 0);
                flags |= JF_FLAG_ESCAPE;
            }
            if (k > 0)
                flags |= JF_FLAG_SAME_UUID;
            if (k == L->GroupSize[g] - 1 && !L->OmitLast[g])
                flags |= JF_FLAG_LAST_TAG;
            JfPut32(tag + JF_T_BLOCK, (unsigned int)block);
            if (C->Incompat & JF_INCOMPAT_64BIT)
                JfPut32(tag + JF_T_BLOCK_HIGH, (unsigned int)(block >> 32));
            JfPut16(tag + JF_T_FLAGS, flags);
            if (csum) {
                unsigned char id[4];
                unsigned int sum;
                JfPut32(id, Tid);
                sum = JlCrc32c(JlCrc32c(Seed, id, 4), data, bs);
                if (C->Incompat & JF_INCOMPAT_CSUM_V3)
                    JfPut32(tag + JF_T_CHECKSUM32, sum);
                else
                    JfPut16(tag + JF_T_CHECKSUM16, sum & 0xFFFF);
            }
            at += tb;
            if (k == 0) {
                memset(d + at, 0x5C, JF_UUID_SIZE);
                at += JF_UUID_SIZE;
            }
            *Pos = JlNext(C, *Pos);
        }
        JlSealTail(C, d, Seed);
        /* the version 1 sum is over the blocks as written, before damage */
        if (C->Compat & JF_COMPAT_CHECKSUM) {
            unsigned int p = dpos;
            for (k = 0; k <= L->GroupSize[g]; k++) {
                crc = JrCrc32Be(crc, JlAt(C, p), bs);
                p = JlNext(C, p);
            }
        }
        if ((Damage == D_BAD_DESCRIPTOR || Damage == D_STALE_DESCRIPTOR) && !damaged) {
            d[bs - JF_TAIL_SIZE - 1] ^= 0x10;   /* past the tags: only the sum sees it */
            damaged = 1;
        }
        if (Damage == D_BAD_DATA && !damaged) {
            /* the copy chosen to fail, if it is in this group */
            for (k = 0; k < L->GroupSize[g]; k++) {
                if (T->Excluded[first + k]) {
                    unsigned int p = dpos, s;
                    for (s = 0; s <= k; s++)
                        p = JlNext(C, p);
                    JlAt(C, p)[bs / 2] ^= 0x01;
                    damaged = 1;
                    break;
                }
            }
        }
    }

    if (Damage == D_UNKNOWN_TYPE) {
        JlHeader(JlAt(C, *Pos), bs, 7, Tid);
        *Pos = JlNext(C, *Pos);
    }

    if (!Commit)
        return;
    b = JlAt(C, *Pos);
    JlHeader(b, bs, JF_COMMIT, Tid);
    JfPut64(b + JF_C_COMMIT_SEC, T->Time);
    JfPut32(b + JF_C_COMMIT_NSEC, JlBelow(r, 1000000000u));
    if ((C->Compat & JF_COMPAT_CHECKSUM) && !T->NoSum) {
        b[JF_C_CHECKSUM_TYPE] = JF_CRC32_CHECKSUM;
        b[JF_C_CHECKSUM_SIZE] = JF_CRC32_CHECKSUM_SIZE;
        JfPut32(b + JF_C_CHECKSUM, crc);
        if (Damage == D_BAD_COMMIT || Damage == D_STALE_COMMIT)
            JfPut32(b + JF_C_CHECKSUM, crc ^ 0x00010000u);
    }
    if (csum) {
        JfPut32(b + JF_C_CHECKSUM, JlCrc32c(Seed, b, bs));
        if (Damage == D_BAD_COMMIT || Damage == D_STALE_COMMIT)
            b[100] ^= 0x08;
    }
    (void)Index;
    *Pos = JlNext(C, *Pos);
}

/* is this damage meaningful for these features (else the case is plain) */
static int JlDamageApplies(const JL_CASE *C, int Damage, unsigned int k)
{
    int v1 = (C->Compat & JF_COMPAT_CHECKSUM) != 0, v23 = JfHasCsumV2V3(C->Incompat);

    switch (Damage) {
    case D_BAD_COMMIT:          return v1 || v23;
    case D_STALE_COMMIT:        return (v1 || v23) && k > 0;
    case D_BAD_DESCRIPTOR:      return v23;
    case D_STALE_DESCRIPTOR:    return v23 && k > 0;
    case D_BAD_REVOKE:          return v23;
    case D_STALE_REVOKE:        return v23 && k > 0;
    case D_BAD_DATA:            return v1 || v23;
    case D_FOREIGN_OLDER:
    case D_FOREIGN_NEWER:       return v23 && C->Txns > 0;
    default:                    return 1;
    }
}

/*
 * Build a case. Ring sizes, starts and IDs come from the caller (Last,
 * Start, Seq set, Log allocated with Last blocks); the rest from r.
 */
static int JlBuild(JL_CASE *C, unsigned int Txns, int Damage, unsigned int DamageTxn, JL_RNG *r)
{
    unsigned int ring = C->Last - C->First, i, j, pos, total = 0, oldSeq, oldPos;
    unsigned long long time = 1000000 + JlBelow(r, 1000);
    JL_LAYOUT layouts[JL_MAX_TXN];
    int async = (C->Incompat & JF_INCOMPAT_ASYNC_COMMIT) != 0;
    int v1 = (C->Compat & JF_COMPAT_CHECKSUM) != 0;

    C->Txns = Txns;
    if (!JlDamageApplies(C, Damage, DamageTxn) || (Damage != D_NONE && Damage != D_TORN &&
        Damage != D_FOREIGN_OLDER && Damage != D_FOREIGN_NEWER && DamageTxn >= Txns))
        Damage = D_NONE;
    if (Damage == D_TORN && Txns == 0)
        Damage = D_NONE;
    C->Damage = Damage;
    C->DamageTxn = DamageTxn;

    /* the transactions */
    for (i = 0; i < Txns; i++) {
        JL_TXN *T = &C->Txn[i];
        unsigned int used[JL_MAX_HOME];
        memset(used, 0, sizeof(used));
        memset(T, 0, sizeof(*T));
        T->Copies = JlBelow(r, 1 + (JlBelow(r, 3) ? 8 : JL_MAX_COPIES));
        if (T->Copies > C->Homes)
            T->Copies = C->Homes;
        for (j = 0; j < T->Copies; j++) {
            unsigned int h;
            do h = JlBelow(r, C->Homes); while (used[h]);
            used[h] = 1;
            T->Home[j] = h;
            T->Seed[j] = JlRand(r);
            T->Escaped[j] = JlBelow(r, 6) == 0;
        }
        T->Revokes = JlBelow(r, 3) ? 0 : 1 + JlBelow(r, JL_MAX_REVOKES);
        if ((Damage == D_BAD_REVOKE || Damage == D_STALE_REVOKE || Damage == D_REVOKE_COUNT) &&
            i == DamageTxn && !T->Revokes)
            T->Revokes = 1;
        for (j = 0; j < T->Revokes; j++)
            T->Revoked[j] = JlBelow(r, C->Homes);
        if ((Damage == D_BAD_DESCRIPTOR || Damage == D_STALE_DESCRIPTOR || Damage == D_BAD_DATA) &&
            i == DamageTxn && !T->Copies) {
            T->Copies = 1;
            T->Home[0] = JlBelow(r, C->Homes);
            T->Seed[0] = JlRand(r);
            T->Escaped[0] = 0;
        }
        if (Damage == D_BAD_DATA && i == DamageTxn)
            T->Excluded[JlBelow(r, T->Copies)] = 1;
        /* an old kernel's commit: the checksum feature, no checksum */
        T->NoSum = v1 && JlBelow(r, 5) == 0 &&
                   !((Damage == D_BAD_COMMIT || Damage == D_STALE_COMMIT || Damage == D_BAD_DATA) &&
                     i == DamageTxn);
        /* commit times are seconds: equal ones are common */
        time += JlBelow(r, 3);
        T->Time = time;
        if ((Damage == D_STALE_COMMIT || Damage == D_STALE_DESCRIPTOR || Damage == D_STALE_REVOKE) &&
            i == DamageTxn)
            T->Time = C->Txn[i - 1].Time - 1 - JlBelow(r, 5);
        total += JlPlan(C, T, &layouts[i], r);
    }
    total += (Damage == D_UNKNOWN_TYPE) ? 1 : 0;
    if (total + 2 > ring)
        return 0;   /* does not fit: the caller picks again */

    /* an older round all over the ring: same seed, IDs far behind */
    oldSeq = C->Seq - 5000 - JlBelow(r, 100000);
    oldPos = C->First;
    while (oldPos < C->Last) {
        unsigned int n = 1 + JlBelow(r, 6), k;
        if (oldPos + n + 2 > C->Last) {
            for (; oldPos < C->Last; oldPos++)
                memset(JlAt(C, oldPos), 0, C->BlockSize);
            break;
        }
        JlHeader(JlAt(C, oldPos), C->BlockSize, JF_DESCRIPTOR, oldSeq);
        {
            unsigned char *d = JlAt(C, oldPos);
            unsigned int at = JF_HEADER_SIZE, tb = JfTagBytes(C->Incompat);
            for (k = 0; k < n; k++) {
                JfPut32(d + at, C->Homes + k);
                JfPut16(d + at + JF_T_FLAGS, (k ? JF_FLAG_SAME_UUID : 0) | (k == n - 1 ? JF_FLAG_LAST_TAG : 0));
                at += tb + (k ? 0 : JF_UUID_SIZE);
            }
            JlSealTail(C, d, C->CsumSeed);
        }
        oldPos++;
        for (k = 0; k < n; k++, oldPos++)
            JlFill(JlAt(C, oldPos), C->BlockSize, oldSeq * 31 + k);
        JlHeader(JlAt(C, oldPos), C->BlockSize, JF_COMMIT, oldSeq);
        JfPut64(JlAt(C, oldPos) + JF_C_COMMIT_SEC, 1000 + oldSeq % 1000);
        oldPos++;
        oldSeq++;
    }

    /* the new log */
    pos = C->Start;
    for (i = 0; i < Txns; i++) {
        int commit = !(Damage == D_TORN && i == Txns - 1);
        JlWriteTxn(C, i, C->Seq + i, &C->Txn[i], &layouts[i], C->CsumSeed,
                   i == DamageTxn ? Damage : D_NONE, &pos, commit, r);
    }
    C->Written = total;

    /* what follows the log */
    if (Damage == D_FOREIGN_OLDER || Damage == D_FOREIGN_NEWER) {
        JL_TXN foreign;
        JL_LAYOUT fl;
        memset(&foreign, 0, sizeof(foreign));
        foreign.Copies = 1;
        foreign.Home[0] = 0;
        foreign.Seed[0] = 7;
        foreign.Time = Damage == D_FOREIGN_OLDER ? 10 : time + 100;
        if (JlPlan(C, &foreign, &fl, r) + total + 2 > ring)
            return 0;
        JlWriteTxn(C, Txns, C->Seq + Txns, &foreign, &fl, C->CsumSeed ^ 0x5EED5EEDu, D_NONE, &pos, 1, r);
    } else {
        switch (JlBelow(r, 4)) {
        case 0: memset(JlAt(C, pos), 0, C->BlockSize); break;
        case 1: JlFill(JlAt(C, pos), C->BlockSize, JlRand(r)); break;
        case 2: JlHeader(JlAt(C, pos), C->BlockSize, JF_DESCRIPTOR, C->Seq + Txns + 3); break;
        default: break; /* the older round */
        }
    }

    /* the expectation */
    C->ExpectError = JR_OK;
    C->ExpectScanError = JR_OK;
    C->NothingHome = 0;
    C->ExpectFailed = 0;
    C->ExpectEnd = C->Seq + Txns;
    switch (Damage) {
    case D_TORN:
        C->ExpectEnd = C->Seq + Txns - 1;
        break;
    case D_BAD_COMMIT:
    case D_BAD_DATA:
        if (Damage == D_BAD_DATA && !v1) {
            /* v2/v3: the scan does not read data; the replay leaves the
               block out and reports it - unless a revoke skips it first */
            unsigned int e, n;
            int revoked = 0;
            for (n = 0; n < C->Txn[DamageTxn].Copies && !C->Txn[DamageTxn].Excluded[n]; n++)
                ;
            for (e = DamageTxn; e < Txns && !revoked; e++)
                for (j = 0; j < C->Txn[e].Revokes; j++)
                    if (C->Txn[e].Revoked[j] == C->Txn[DamageTxn].Home[n])
                        revoked = 1;
            if (!revoked)
                C->ExpectError = JR_EBADCRC;
            break;
        }
        C->ExpectEnd = C->Seq + DamageTxn;
        if (!async) {
            C->ExpectFailed = 1;
            C->ExpectFailedTid = C->Seq + DamageTxn;
        } else if (v1 && DamageTxn + 1 < Txns) {
            /* v1: a commit after the failed one confirms the failure */
            C->ExpectFailed = 1;
            C->ExpectFailedTid = C->Seq + DamageTxn;
        }
        break;
    case D_STALE_COMMIT:
    case D_STALE_DESCRIPTOR:
    case D_STALE_REVOKE:
        C->ExpectEnd = C->Seq + DamageTxn;
        break;
    case D_BAD_DESCRIPTOR:
    case D_BAD_REVOKE:
    case D_FOREIGN_NEWER:
        C->ExpectError = C->ExpectScanError = JR_EBADCRC;
        C->NothingHome = 1;
        break;
    case D_REVOKE_COUNT:
        C->ExpectError = JR_EINVAL;     /* found by the revoke walk */
        C->NothingHome = 1;
        break;
    case D_UNKNOWN_TYPE:
        C->ExpectEnd = C->Seq + DamageTxn;
        break;
    default:
        break;
    }
    C->ExpectScanEnd = C->ExpectEnd;
    if (async && (Damage == D_BAD_COMMIT || (Damage == D_BAD_DATA && v1)))
        C->ExpectScanEnd = v1 ? C->Seq + DamageTxn + 1 : C->Seq + Txns;  /* the scan goes on */
    return 1;
}

/*
 * The home blocks after the replay: from the initial content, every
 * replayed transaction in order writes its copies that no revoke in it or
 * a later replayed transaction names, except a copy that fails its tag.
 */
static void JlExpectHome(const JL_CASE *C, unsigned char *Home)
{
    unsigned int i, j, k, replayed;
    int newest[JL_MAX_HOME];

    for (i = 0; i < C->Homes; i++)
        JlHomeInitial(Home + (unsigned long long)i * C->BlockSize, C->BlockSize, i);
    if (C->NothingHome)
        return;
    replayed = C->ExpectEnd - C->Seq;
    for (i = 0; i < C->Homes; i++)
        newest[i] = -1;
    for (i = 0; i < replayed; i++)
        for (k = 0; k < C->Txn[i].Revokes; k++)
            newest[C->Txn[i].Revoked[k]] = (int)i;
    for (i = 0; i < replayed; i++) {
        const JL_TXN *T = &C->Txn[i];
        for (j = 0; j < T->Copies; j++) {
            if (newest[T->Home[j]] >= (int)i)
                continue;
            if (T->Excluded[j] && JfHasCsumV2V3(C->Incompat))
                continue;
            JlCopyContent(C, T, j, Home + (unsigned long long)T->Home[j] * C->BlockSize);
        }
    }
}

#endif /* JLOG_H */
