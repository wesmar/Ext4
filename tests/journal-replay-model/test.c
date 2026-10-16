/*
 * test.c - the production journal replay (src/journal/JournalReplay.c)
 * against generated logs whose outcome is known (jlog.h).
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * Every feature set, every kind of damage at every transaction of small
 * logs, starts at the ring's wrap and IDs at the 2^32 wrap; then random
 * cases from a seed. Each case checks the result code, the end of the log,
 * the failed commit, every home block, the scan alone, writes outside the
 * home range, allocation failures at every allocation and leaks.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/journal/JournalReplay.c"
#include "jlog.h"

typedef struct {
    JL_CASE        *C;
    unsigned char  *Home;
    unsigned int    Writes;
    int             Outside;
    long            Live;           /* allocations not yet released */
    long            Allocations;
    long            FailAt;         /* fail this allocation (1-based), 0: none */
} CTX;

static unsigned long long Checks, Cases, Replayed, Revokes, RevokeHits, AllocFailures;
static unsigned long long DamageSeen[D_KINDS];

static int ReadLog(void *Context, unsigned int Block, unsigned char *Buffer)
{
    CTX *x = (CTX *)Context;
    if (Block >= x->C->Last)
        return JR_EIO;
    memcpy(Buffer, JlAt(x->C, Block), x->C->BlockSize);
    return 0;
}

static int WriteHome(void *Context, unsigned long long Block, const unsigned char *Buffer)
{
    CTX *x = (CTX *)Context;
    if (Block < x->C->Base || Block - x->C->Base >= x->C->Homes) {
        x->Outside = 1;
        return JR_EIO;
    }
    memcpy(x->Home + (Block - x->C->Base) * x->C->BlockSize, Buffer, x->C->BlockSize);
    x->Writes++;
    return 0;
}

static unsigned int Crc32c(unsigned int Crc, const unsigned char *Data, unsigned int Length)
{
    return JlCrc32c(Crc, Data, Length);
}

static void *Allocate(void *Context, unsigned int Bytes)
{
    CTX *x = (CTX *)Context;
    if (x->FailAt && ++x->Allocations == x->FailAt)
        return NULL;
    if (!x->FailAt)
        x->Allocations++;
    x->Live++;
    return malloc(Bytes);
}

static void Release(void *Context, void *Memory)
{
    CTX *x = (CTX *)Context;
    x->Live--;
    free(Memory);
}

static void Fail(const JL_CASE *C, const char *What, unsigned long long Seed)
{
    printf("FAIL: %s (seed %llu, %s, damage %s at %u of %u, ring %u..%u start %u seq %u, bs %u)\n",
           What, Seed, "case", JlDamageNames[C->Damage], C->DamageTxn, C->Txns,
           C->First, C->Last, C->Start, C->Seq, C->BlockSize);
    exit(1);
}

#define CHECK(cond, what) do { Checks++; if (!(cond)) Fail(C, what, seed); } while (0)

static void Journal(JR_JOURNAL *J, JL_CASE *C, CTX *x)
{
    memset(J, 0, sizeof(*J));
    J->BlockSize = C->BlockSize;
    J->First = C->First;
    J->Last = C->Last;
    J->TotalLen = C->Last;
    J->Compat = C->Compat;
    J->Incompat = C->Incompat;
    J->CsumSeed = C->CsumSeed;
    J->Start = C->Start;
    J->Sequence = C->Seq;
    J->Context = x;
    J->ReadLog = ReadLog;
    J->WriteHome = WriteHome;
    J->Crc32c = Crc32c;
    J->Allocate = Allocate;
    J->Release = Release;
}

static void RunCase(JL_CASE *C, unsigned char *Home, unsigned char *Want, unsigned long long seed, JL_RNG *r)
{
    JR_JOURNAL  J;
    JR_RESULT   R;
    CTX         x;
    unsigned int i;
    int         rc;
    size_t      bytes = (size_t)C->Homes * C->BlockSize;
    long        allocations;

    JlExpectHome(C, Want);
    for (i = 0; i < C->Homes; i++)
        JlHomeInitial(Home + (size_t)i * C->BlockSize, C->BlockSize, i);

    memset(&x, 0, sizeof(x));
    x.C = C;
    x.Home = Home;
    Journal(&J, C, &x);
    rc = JrRecover(&J, &R);
    allocations = x.Allocations;
    Cases++;
    DamageSeen[C->Damage]++;
    CHECK(rc == C->ExpectError, "result code");
    CHECK(!x.Outside, "a write outside the home blocks");
    CHECK(x.Live == 0, "allocations leaked");
    if (C->ExpectScanError == JR_OK) {
        CHECK(R.EndTid == C->ExpectEnd, "end of the log");
        CHECK(R.ScanEndTid == C->ExpectScanEnd, "where the scan stopped");
        CHECK(R.HasFailedCommit == C->ExpectFailed, "failed commit reported");
        if (C->ExpectFailed)
            CHECK(R.FailedCommit == C->ExpectFailedTid, "failed commit ID");
    }
    CHECK(memcmp(Home, Want, bytes) == 0, "home blocks");
    Replayed += R.Replayed;
    Revokes += R.Revokes;
    RevokeHits += R.RevokeHits;

    /* the scan alone: the same end, nothing written */
    memset(&x, 0, sizeof(x));
    x.C = C;
    x.Home = Want;      /* must stay untouched */
    Journal(&J, C, &x);
    rc = JrScan(&J, &R);
    CHECK(x.Writes == 0 && !x.Outside, "the scan wrote");
    CHECK(x.Live == 0, "the scan leaked");
    CHECK(rc == C->ExpectScanError, "scan result code");
    if (C->ExpectScanError == JR_OK)
        CHECK(R.EndTid == C->ExpectEnd, "scan: end of the log");

    /* every allocation failing in turn: ENOMEM, nothing leaked; a failure
       before the replay walk leaves every home block as it was */
    if (allocations && JlBelow(r, 4) == 0) {
        long k;
        for (k = 1; k <= allocations; k++) {
            for (i = 0; i < C->Homes; i++)
                JlHomeInitial(Home + (size_t)i * C->BlockSize, C->BlockSize, i);
            memset(&x, 0, sizeof(x));
            x.C = C;
            x.Home = Home;
            x.FailAt = k;
            Journal(&J, C, &x);
            rc = JrRecover(&J, &R);
            AllocFailures++;
            CHECK(rc == JR_ENOMEM, "allocation failure reported");
            CHECK(x.Writes == 0, "a write after an allocation failure");
            CHECK(x.Live == 0, "allocation failure leaked");
        }
    }
}

static void MakeCase(JL_CASE *C, unsigned int Features, unsigned int BlockSize, unsigned int Last,
                  unsigned int Start, unsigned int Seq, JL_RNG *r)
{
    C->BlockSize = BlockSize;
    C->First = 1;
    C->Last = Last;
    C->Start = Start;
    C->Seq = Seq;
    C->Compat = JlFeatureSets[Features].Compat;
    C->Incompat = JlFeatureSets[Features].Incompat;
    C->CsumSeed = JlRand(r);
    C->Homes = 4 + JlBelow(r, JL_MAX_HOME - 4);
    C->Base = (C->Incompat & JF_INCOMPAT_64BIT) ? 0x100000000ull + JlRand(r) : 1000 + JlBelow(r, 100000);
}

/* logs no writer makes: the walk must still end, with nothing written */
static void EdgeCases(JL_CASE *C, unsigned char *Home, unsigned long long seed, JL_RNG *r)
{
    JR_JOURNAL  J;
    JR_RESULT   R;
    CTX         x;
    unsigned int i;
    int         rc;

    MakeCase(C, 6, 1024, 300, 1, 77, r);
    C->Damage = D_NONE;
    C->DamageTxn = 0;
    C->Txns = 0;

    /* a start outside the journal */
    memset(&x, 0, sizeof(x));
    x.C = C;
    x.Home = Home;
    C->Start = C->Last;
    Journal(&J, C, &x);
    rc = JrRecover(&J, &R);
    CHECK(rc == JR_ECORRUPT && x.Writes == 0 && x.Live == 0, "start outside the journal");

    /* nothing to replay */
    C->Start = 0;
    Journal(&J, C, &x);
    rc = JrRecover(&J, &R);
    CHECK(rc == JR_OK && x.Writes == 0 && R.EndTid == C->Seq && x.Live == 0, "an empty log");

    /* a ring of revoke blocks of one transaction: no commit ever comes */
    for (i = C->First; i < C->Last; i++) {
        unsigned char *b = JlAt(C, i);
        JlHeader(b, C->BlockSize, JF_REVOKE, C->Seq);
        JfPut32(b + JF_R_COUNT, JF_REVOKE_HEADER_SIZE);
        JlSealTail(C, b, C->CsumSeed);
    }
    C->Start = 1 + JlBelow(r, C->Last - 1);
    Journal(&J, C, &x);
    rc = JrRecover(&J, &R);
    CHECK(rc == JR_ECORRUPT && x.Writes == 0 && x.Live == 0, "a ring with no end");
    rc = JrScan(&J, &R);
    CHECK(rc == JR_ECORRUPT && x.Live == 0, "a ring with no end, scanned");
}

int main(int argc, char **argv)
{
    unsigned long long seed = argc > 1 ? strtoull(argv[1], NULL, 10) : 1;
    unsigned int f, d, n, k, s, q, i;
    static const unsigned int starts[] = { 0, 1, 2, 3, 5, 9 };     /* blocks before the wrap */
    static const unsigned int seqs[] = { 7, 0xFFFFFFFEu, 0xFFFFFFFFu, 0x80000000u };
    JL_CASE *C = (JL_CASE *)calloc(1, sizeof(JL_CASE));
    unsigned char *home = (unsigned char *)malloc((size_t)JL_MAX_HOME * 4096);
    unsigned char *want = (unsigned char *)malloc((size_t)JL_MAX_HOME * 4096);
    JL_RNG r;

    JlCrcInit();
    C->Log = (unsigned char *)malloc((size_t)1200 * 4096);
    r.s = seed * 0x9E3779B97F4A7C15ull + 1;

    /* every feature set, damage, transaction count and damaged transaction,
       start at the wrap and ID at the 2^32 wrap */
    for (f = 0; f < JL_FEATURE_SETS; f++)
        for (d = 0; d < D_KINDS; d++)
            for (n = 0; n <= 4; n++)
                for (k = 0; k < (n ? n : 1); k++)
                    for (s = 0; s < sizeof(starts) / sizeof(starts[0]); s++)
                        for (q = 0; q < sizeof(seqs) / sizeof(seqs[0]); q++) {
                            unsigned int last = 220 + JlBelow(&r, 60);
                            unsigned int start = starts[s] ? last - starts[s] : 1;
                            MakeCase(C, f, 1024, last, start, seqs[q], &r);
                            if (!JlBuild(C, n, (int)d, k, &r))
                                continue;
                            RunCase(C, home, want, seed, &r);
                        }

    EdgeCases(C, home, seed, &r);

    /* random cases */
    for (i = 0; i < 40000; i++) {
        unsigned int bs = JlBelow(&r, 8) == 0 ? 4096 : (JlBelow(&r, 4) == 0 ? 2048 : 1024);
        unsigned int last = 40 + JlBelow(&r, bs == 4096 ? 400 : 1100);
        unsigned int txns = JlBelow(&r, JL_MAX_TXN + 1);
        f = JlBelow(&r, JL_FEATURE_SETS);
        d = JlBelow(&r, 3) ? JlBelow(&r, D_KINDS) : D_NONE;
        for (;;) {
            MakeCase(C, f, bs, last, 1 + JlBelow(&r, last - 1), JlBelow(&r, 4) ? JlRand(&r) : seqs[JlBelow(&r, 4)], &r);
            if (JlBuild(C, txns, (int)d, txns ? JlBelow(&r, txns) : 0, &r))
                break;
            if (txns)
                txns--;
        }
        RunCase(C, home, want, seed, &r);
    }

    printf("JOURNAL-REPLAY-MODEL: seed %llu: %llu cases, %llu checks, %llu blocks replayed, "
           "%llu revoke records, %llu revoke hits, %llu allocation failures\n",
           seed, Cases, Checks, Replayed, Revokes, RevokeHits, AllocFailures);
    printf("  damage:");
    for (d = 0; d < D_KINDS; d++)
        printf(" %s %llu", JlDamageNames[d], DamageSeen[d]);
    printf("\n");
    free(C->Log);
    free(C);
    free(home);
    free(want);
    return 0;
}
