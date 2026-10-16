/**
 * JournalReplay.c - replaying a jbd2 log: what is committed goes home.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * The log is a ring of blocks [First, Last); recovery starts at block Start
 * with transaction Sequence. A transaction is a run of revoke blocks and
 * descriptor blocks - each descriptor followed by the data blocks its tags
 * name - closed by a commit block; every header carries the transaction ID.
 *
 * The rules, in the order the walks apply them:
 *
 *  R1  A walk reads one header after another. A block that is no header (no
 *      magic), a header of another transaction than the one expected, or a
 *      block of unknown type ends the log.
 *  R2  The scan finds End, the first transaction not to replay: the one
 *      after the last commit block reached - unless a commit fails its
 *      checksum, which makes that transaction End. With async_commit the
 *      scan goes on past such a failure (later corruption still counts);
 *      without it the failure is reported (FailedCommit).
 *  R3  A descriptor or revoke block that fails its v2/v3 checksum may be
 *      left over from an earlier round of the ring. The next commit decides:
 *      a commit time older than the last good commit's means stale blocks -
 *      the log ends there; otherwise the log is corrupt (EBADCRC). A commit
 *      block that fails its own checksum with an older time is stale too.
 *  R4  Version 1 checksums: the commit block carries the crc32 of the
 *      transaction's descriptor and data blocks, or says it has none.
 *  R5  A revoke record of block B in transaction T keeps the logged copies
 *      of B from transactions up to and including T from going home; for
 *      each block the newest revoking transaction counts.
 *  R6  The replay sends each tag's data block home unless it is revoked.
 *      With v2/v3 its tag checksum - over the transaction ID and the block
 *      as logged - must match, or the block stays out and the replay ends
 *      with EBADCRC once the rest is done. An escaped block gets its first
 *      word back.
 *  R7  Transactions [Sequence, End) are replayed in order, so what is home
 *      at the end is, for every block, its newest unrevoked copy (IDs
 *      compared on the circle of 2^32).
 *
 * These are the rules of Linux fs/jbd2/recovery.c, which e2fsck shares,
 * and the walks are its three: scan, revokes, replay; the second and third
 * stop at End. Two departures: a failure at transaction 0 is reported as
 * one (there a zero reads as "no end found yet"), and a walk longer than the
 * ring is a corrupt log instead of an endless loop.
 * Tested by tests/journal-replay-model.ps1 (generated logs against these
 * rules, x86 and x64) and tests/journal-replay-wsl.sh (the same logs and
 * damaged ones, replayed by this code and by e2fsck, compared block by
 * block).
 */

#include "JournalFormat.h"
#include "JournalReplay.h"

enum { WALK_SCAN, WALK_REVOKES, WALK_REPLAY };

typedef struct _JR_REVOKE {
    unsigned long long  Block;
    unsigned int        Tid;
    unsigned int        Used;
} JR_REVOKE;

typedef struct _JR_STATE {
    const JR_JOURNAL   *J;
    JR_RESULT          *Result;
    unsigned char      *Header;         /* the header block read last */
    unsigned char      *Data;           /* a data block */
    unsigned int        TagBytes;
    unsigned int        TailBytes;      /* the checksum tail: 0 or 4 */
    int                 CsumV2V3;
    unsigned int        Ring;           /* Last - First */
    unsigned long long  Consumed;       /* log blocks this walk has passed */

    int                 EndKnown;
    unsigned int        End;
    int                 HeadKnown;

    JR_REVOKE          *Revokes;        /* open addressing, power of two */
    unsigned int        RevokeSlots;
    unsigned int        RevokeCount;
} JR_STATE;

unsigned int JrCrc32Be(unsigned int Crc, const unsigned char *Data, unsigned int Length)
{
    unsigned int i;

    while (Length--) {
        Crc ^= (unsigned int)*Data++ << 24;
        for (i = 0; i < 8; i++)
            Crc = (Crc << 1) ^ ((Crc & 0x80000000u) ? 0x04C11DB7u : 0);
    }
    return Crc;
}

/* one step around the ring (a position past Last comes back by one ring) */
static unsigned int Advance(const JR_STATE *S, unsigned int Pos, unsigned int Count)
{
    unsigned long long next = (unsigned long long)Pos + Count;

    if (next >= S->J->Last)
        next -= S->Ring;
    return next > 0xFFFFFFFFull ? 0xFFFFFFFFu : (unsigned int)next;
}

static int ReadAt(JR_STATE *S, unsigned int Pos, unsigned char *Buffer)
{
    int err;

    /* every block of the ring once, and the one that ends it */
    if (++S->Consumed > (unsigned long long)S->Ring + 1)
        return JR_ECORRUPT;
    if (Pos >= S->J->TotalLen)
        return JR_ECORRUPT;
    err = S->J->ReadLog(S->J->Context, Pos, Buffer);
    return err < 0 ? err : (err ? JR_EIO : JR_OK);
}

/* crc32c of the block with the 4 bytes at Offset taken as zero */
static unsigned int SumWithout(const JR_STATE *S, unsigned char *Block, unsigned int Offset)
{
    unsigned char saved[4];
    unsigned int  sum, i;

    for (i = 0; i < 4; i++) {
        saved[i] = Block[Offset + i];
        Block[Offset + i] = 0;
    }
    sum = S->J->Crc32c(S->J->CsumSeed, Block, S->J->BlockSize);
    for (i = 0; i < 4; i++)
        Block[Offset + i] = saved[i];
    return sum;
}

/* the tail of a descriptor or revoke block (v2/v3) */
static int TailOk(const JR_STATE *S, unsigned char *Block)
{
    unsigned int at = S->J->BlockSize - JF_TAIL_SIZE;
    return SumWithout(S, Block, at) == JfGet32(Block + at);
}

static int CommitSumOk(const JR_STATE *S, unsigned char *Block)
{
    return SumWithout(S, Block, JF_C_CHECKSUM) == JfGet32(Block + JF_C_CHECKSUM);
}

/* R4: the version 1 commit checksum matches, or the commit has none */
static int V1CommitOk(const unsigned char *Commit, unsigned int Crc)
{
    unsigned int found = JfGet32(Commit + JF_C_CHECKSUM);
    unsigned int type = Commit[JF_C_CHECKSUM_TYPE], size = Commit[JF_C_CHECKSUM_SIZE];

    return (Crc == found && type == JF_CRC32_CHECKSUM && size == JF_CRC32_CHECKSUM_SIZE) ||
           (type == 0 && size == 0 && found == 0);
}

static int TagSumOk(const JR_STATE *S, const unsigned char *Tag, const unsigned char *Data, unsigned int Tid)
{
    unsigned char id[4];
    unsigned int  sum;

    if (!S->CsumV2V3)
        return 1;
    JfPut32(id, Tid);
    sum = S->J->Crc32c(S->J->CsumSeed, id, 4);
    sum = S->J->Crc32c(sum, Data, S->J->BlockSize);
    if (S->J->Incompat & JF_INCOMPAT_CSUM_V3)
        return JfGet32(Tag + JF_T_CHECKSUM32) == sum;
    return JfGet16(Tag + JF_T_CHECKSUM16) == (sum & 0xFFFFu);
}

static unsigned long long TagBlock(const JR_STATE *S, const unsigned char *Tag)
{
    unsigned long long block = JfGet32(Tag + JF_T_BLOCK);

    if (S->J->Incompat & JF_INCOMPAT_64BIT)
        block |= (unsigned long long)JfGet32(Tag + JF_T_BLOCK_HIGH) << 32;
    return block;
}

/*
 * The tags of a descriptor: each takes TagBytes, and 16 more for the uuid
 * unless it says SAME_UUID; the one marked LAST_TAG is the last, and a tag
 * must fit before the checksum tail. Calls Each for every tag, in order,
 * and returns how many there are (the data blocks that follow).
 */
typedef int (*JR_TAG_FN)(JR_STATE *S, const unsigned char *Tag, void *Context);

static unsigned int ForEachTag(JR_STATE *S, const unsigned char *Descriptor, JR_TAG_FN Each, void *Context)
{
    unsigned int limit = S->J->BlockSize - S->TailBytes;
    unsigned int at = JF_HEADER_SIZE, count = 0;

    while (at + S->TagBytes <= limit) {
        const unsigned char *tag = Descriptor + at;
        unsigned int flags = JfGet16(tag + JF_T_FLAGS);

        count++;
        if (Each && Each(S, tag, Context))
            break;
        at += S->TagBytes;
        if (!(flags & JF_FLAG_SAME_UUID))
            at += JF_UUID_SIZE;
        if (flags & JF_FLAG_LAST_TAG)
            break;
    }
    return count;
}

/* ---- R5: the revoke table, block -> newest revoking transaction ---- */

static unsigned int RevokeSlot(const JR_STATE *S, unsigned long long Block)
{
    unsigned long long h = Block * 0x9E3779B97F4A7C15ull;
    return (unsigned int)(h >> 32) & (S->RevokeSlots - 1);
}

static JR_REVOKE *RevokeFind(const JR_STATE *S, unsigned long long Block)
{
    unsigned int i;

    if (!S->RevokeSlots)
        return 0;
    for (i = RevokeSlot(S, Block); S->Revokes[i].Used; i = (i + 1) & (S->RevokeSlots - 1)) {
        if (S->Revokes[i].Block == Block)
            return &S->Revokes[i];
    }
    return 0;
}

static int RevokeGrow(JR_STATE *S)
{
    JR_REVOKE   *old = S->Revokes;
    unsigned int oldSlots = S->RevokeSlots, slots, i, k;

    slots = oldSlots ? oldSlots * 2 : 256;
    if (slots > 0x04000000u)
        return JR_ENOMEM;
    S->Revokes = (JR_REVOKE *)S->J->Allocate(S->J->Context, slots * (unsigned int)sizeof(JR_REVOKE));
    if (!S->Revokes) {
        S->Revokes = old;
        return JR_ENOMEM;
    }
    for (i = 0; i < slots; i++)
        S->Revokes[i].Used = 0;
    S->RevokeSlots = slots;
    for (i = 0; i < oldSlots; i++) {
        if (!old[i].Used)
            continue;
        for (k = RevokeSlot(S, old[i].Block); S->Revokes[k].Used; k = (k + 1) & (slots - 1))
            ;
        S->Revokes[k] = old[i];
    }
    if (old)
        S->J->Release(S->J->Context, old);
    return JR_OK;
}

static int RevokeSet(JR_STATE *S, unsigned long long Block, unsigned int Tid)
{
    JR_REVOKE   *r = RevokeFind(S, Block);
    unsigned int i;
    int          err;

    if (r) {
        if (JfTidAfter(Tid, r->Tid))
            r->Tid = Tid;
        return JR_OK;
    }
    if ((S->RevokeCount + 1) * 2 > S->RevokeSlots) {
        err = RevokeGrow(S);
        if (err)
            return err;
    }
    for (i = RevokeSlot(S, Block); S->Revokes[i].Used; i = (i + 1) & (S->RevokeSlots - 1))
        ;
    S->Revokes[i].Block = Block;
    S->Revokes[i].Tid = Tid;
    S->Revokes[i].Used = 1;
    S->RevokeCount++;
    return JR_OK;
}

/* revoked: a revoke of the block in this transaction or a later one */
static int RevokeHolds(const JR_STATE *S, unsigned long long Block, unsigned int Tid)
{
    const JR_REVOKE *r = RevokeFind(S, Block);
    return r && !JfTidAfter(Tid, r->Tid);
}

static int ReadRevokes(JR_STATE *S, const unsigned char *Block, unsigned int Tid)
{
    unsigned int used = JfGet32(Block + JF_R_COUNT);
    unsigned int size = (S->J->Incompat & JF_INCOMPAT_64BIT) ? 8 : 4;
    unsigned int at = JF_REVOKE_HEADER_SIZE;
    int          err;

    if (used > S->J->BlockSize - S->TailBytes)
        return JR_EINVAL;
    for (; at + size <= used; at += size) {
        err = RevokeSet(S, size == 8 ? JfGet64(Block + at) : JfGet32(Block + at), Tid);
        if (err)
            return err;
        S->Result->Revokes++;
    }
    return JR_OK;
}

/* ---- the replay of one descriptor's data blocks (R6) ---- */

typedef struct _JR_REPLAY {
    unsigned int    Tid;
    unsigned int    Pos;        /* the log block of the next data block */
    int             Status;     /* the last error, the walk goes on */
} JR_REPLAY;

static int ReplayTag(JR_STATE *S, const unsigned char *Tag, void *Context)
{
    JR_REPLAY          *R = (JR_REPLAY *)Context;
    unsigned int        at = R->Pos;
    unsigned long long  block;
    int                 err;

    R->Pos = Advance(S, R->Pos, 1);
    err = ReadAt(S, at, S->Data);
    if (err) {
        R->Status = err;    /* recover what can be, report it at the end */
        return err == JR_ECORRUPT && S->Consumed > (unsigned long long)S->Ring + 1;
    }
    block = TagBlock(S, Tag);
    if (RevokeHolds(S, block, R->Tid)) {
        S->Result->RevokeHits++;
        return 0;
    }
    if (!TagSumOk(S, Tag, S->Data, R->Tid)) {
        R->Status = JR_EBADCRC;
        return 0;
    }
    if (JfGet16(Tag + JF_T_FLAGS) & JF_FLAG_ESCAPE)
        JfPut32(S->Data, JF_MAGIC);
    err = S->J->WriteHome(S->J->Context, block, S->Data);
    if (err) {
        if (!S->Result->WriteError)
            S->Result->WriteError = err < 0 ? err : JR_EIO;
        R->Status = S->Result->WriteError;
        return 0;
    }
    S->Result->Replayed++;
    return 0;
}

/* R4: the version 1 checksum over a descriptor and its data blocks */
typedef struct _JR_SUM {
    unsigned int    Pos;
    unsigned int    Crc;
    int             Failed;
} JR_SUM;

static int SumTag(JR_STATE *S, const unsigned char *Tag, void *Context)
{
    JR_SUM      *C = (JR_SUM *)Context;
    unsigned int at = C->Pos;

    (void)Tag;
    C->Pos = Advance(S, C->Pos, 1);
    if (ReadAt(S, at, S->Data)) {
        C->Failed = 1;
        return 1;
    }
    C->Crc = JrCrc32Be(C->Crc, S->Data, S->J->BlockSize);
    return 0;
}

/* ---- the walk ---- */

static int Walk(JR_STATE *S, int Kind)
{
    const JR_JOURNAL   *J = S->J;
    unsigned int        tid = J->Sequence;
    unsigned int        pos = J->Start;
    unsigned int        head = pos;         /* after the last good commit */
    unsigned long long  lastTime = 0;       /* of the last good commit */
    int                 stale = 0;          /* R3: a checksum failed, see the commit */
    unsigned int        crc = 0xFFFFFFFFu;  /* R4 */
    int                 status = JR_OK;     /* replay errors, the walk goes on */
    int                 err;

    S->Consumed = 0;

    for (;;) {
        unsigned char *h = S->Header;
        unsigned int   type;

        if (Kind != WALK_SCAN && JfTidAtLeast(tid, S->End))
            break;
        err = ReadAt(S, pos, h);
        if (err)
            return err;
        pos = Advance(S, pos, 1);

        /* R1 */
        if (JfGet32(h + JF_H_MAGIC) != JF_MAGIC || JfGet32(h + JF_H_SEQUENCE) != tid)
            break;
        type = JfGet32(h + JF_H_TYPE);

        if (type == JF_DESCRIPTOR) {
            if (S->CsumV2V3 && !TailOk(S, h)) {
                if (Kind != WALK_SCAN)
                    return JR_EBADCRC;
                stale = 1;
            }
            if (Kind == WALK_REPLAY) {
                JR_REPLAY r;

                r.Tid = tid;
                r.Pos = pos;
                r.Status = JR_OK;
                ForEachTag(S, h, ReplayTag, &r);
                pos = r.Pos;
                if (r.Status)
                    status = r.Status;
                if (S->Consumed > (unsigned long long)S->Ring + 1)
                    return JR_ECORRUPT;
                continue;
            }
            if (Kind == WALK_SCAN && (J->Compat & JF_COMPAT_CHECKSUM) && !stale && !S->EndKnown) {
                JR_SUM sum;

                sum.Pos = pos;
                sum.Crc = JrCrc32Be(crc, h, J->BlockSize);
                sum.Failed = 0;
                ForEachTag(S, h, SumTag, &sum);
                pos = sum.Pos;
                crc = sum.Crc;
                if (S->Consumed > (unsigned long long)S->Ring + 1)
                    return JR_ECORRUPT;
                continue;   /* a block that cannot be read: on with the next */
            }
            {
                unsigned int n = ForEachTag(S, h, 0, 0);
                S->Consumed += n;
                pos = Advance(S, pos, n);
            }
            continue;
        }

        if (type == JF_COMMIT) {
            unsigned long long time = JfGet64(h + JF_C_COMMIT_SEC);
            int bad = 0;

            if (stale) {
                /* R3 (only the scan ever sets it) */
                if (time >= lastTime)
                    return JR_EBADCRC;
                break;
            }
            if (Kind == WALK_SCAN) {
                if (J->Compat & JF_COMPAT_CHECKSUM) {
                    if (S->EndKnown) {
                        /* a commit after a failed one: the failure stands */
                        S->Result->FailedCommit = S->End;
                        S->Result->HasFailedCommit = 1;
                        continue;
                    }
                    if (V1CommitOk(h, crc))
                        crc = 0xFFFFFFFFu;
                    else
                        bad = 1;
                }
                if (!bad && S->CsumV2V3 && !CommitSumOk(S, h))
                    bad = 1;
                if (bad) {
                    if (time < lastTime)
                        break;      /* R3: stale */
                    /* R2 */
                    S->End = tid;
                    S->EndKnown = 1;
                    S->Result->HeadBlock = head;
                    S->HeadKnown = 1;
                    if (!(J->Incompat & JF_INCOMPAT_ASYNC_COMMIT)) {
                        S->Result->FailedCommit = tid;
                        S->Result->HasFailedCommit = 1;
                        continue;   /* the next header ends the log */
                    }
                }
                lastTime = time;
                head = pos;
            }
            tid++;
            continue;
        }

        if (type == JF_REVOKE) {
            if (Kind == WALK_SCAN && S->CsumV2V3 && !TailOk(S, h))
                stale = 1;
            if (Kind == WALK_REVOKES) {
                err = ReadRevokes(S, h, tid);
                if (err)
                    return err;
            }
            continue;
        }

        break;      /* R1: unknown type */
    }

    if (Kind == WALK_SCAN) {
        S->Result->ScanEndTid = tid;
        if (!S->EndKnown) {
            S->End = tid;
            S->EndKnown = 1;
        }
        if (!S->HeadKnown) {
            S->Result->HeadBlock = head;
            S->HeadKnown = 1;
        }
    } else if (tid != S->End && status == JR_OK) {
        status = JR_EIO;    /* this walk ended elsewhere than the scan */
    }
    return status;
}

static int Setup(JR_STATE *S, const JR_JOURNAL *J, JR_RESULT *R)
{
    unsigned int i;

    S->J = J;
    S->Result = R;
    S->TagBytes = JfTagBytes(J->Incompat);
    S->CsumV2V3 = JfHasCsumV2V3(J->Incompat);
    S->TailBytes = S->CsumV2V3 ? JF_TAIL_SIZE : 0;
    S->Ring = J->Last > J->First ? J->Last - J->First : 0;
    S->Consumed = 0;
    S->EndKnown = 0;
    S->End = 0;
    S->HeadKnown = 0;
    S->Revokes = 0;
    S->RevokeSlots = 0;
    S->RevokeCount = 0;
    S->Header = (unsigned char *)J->Allocate(J->Context, 2 * J->BlockSize);
    S->Data = S->Header ? S->Header + J->BlockSize : 0;

    for (i = 0; i < sizeof(*R); i++)
        ((unsigned char *)R)[i] = 0;
    R->StartTid = J->Sequence;
    R->EndTid = J->Sequence;
    R->ScanEndTid = J->Sequence;
    R->HeadBlock = J->Start;
    return S->Header ? JR_OK : JR_ENOMEM;
}

static void Cleanup(JR_STATE *S)
{
    if (S->Revokes)
        S->J->Release(S->J->Context, S->Revokes);
    if (S->Header)
        S->J->Release(S->J->Context, S->Header);
}

int JrScan(const JR_JOURNAL *Journal, JR_RESULT *Result)
{
    JR_STATE S;
    int      err;

    err = Setup(&S, Journal, Result);
    if (!err && Journal->Start)
        err = Walk(&S, WALK_SCAN);
    if (S.EndKnown)
        Result->EndTid = S.End;
    Cleanup(&S);
    return err;
}

int JrRecover(const JR_JOURNAL *Journal, JR_RESULT *Result)
{
    JR_STATE S;
    int      err;

    err = Setup(&S, Journal, Result);
    if (!err && Journal->Start) {
        err = Walk(&S, WALK_SCAN);
        if (!err)
            err = Walk(&S, WALK_REVOKES);
        if (!err)
            err = Walk(&S, WALK_REPLAY);
    }
    if (S.EndKnown)
        Result->EndTid = S.End;
    Cleanup(&S);
    return err;
}
