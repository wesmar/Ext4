/*
 * wsl.c - the production journal replay against e2fsck, on real images.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * usage: wsl <image> <journal block list> <home base> <cases> <seed>
 *
 * Each case is a log from jlog.h written into the journal of a copy of the
 * image (its blocks as the list maps them), the journal superblock and the
 * file system's needs_recovery flag set, the home blocks filled with their
 * initial content. One copy is replayed by e2fsck -E journal_only, another
 * by JournalReplay.c; the home blocks of both must equal each other and the
 * generator's expectation, and e2fsck must report what the replay reports.
 * Built by tests/journal-replay-wsl.sh with gcc.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "../../src/journal/JournalReplay.c"
#include "jlog.h"

static int Fd;
static unsigned int Bs, Nblocks;
static unsigned long long *Phys;
static unsigned long long Checks, Cases, Mismatches, Replayed;
static unsigned long long Seen[D_KINDS], Agree[8];

static void Die(const char *what)
{
    perror(what);
    exit(2);
}

static void PRead(int fd, void *buf, unsigned long long block)
{
    if (pread(fd, buf, Bs, (off_t)(block * Bs)) != (ssize_t)Bs)
        Die("pread");
}

static void PWrite(int fd, const void *buf, unsigned long long block)
{
    if (pwrite(fd, buf, Bs, (off_t)(block * Bs)) != (ssize_t)Bs)
        Die("pwrite");
}

static int ReadLog(void *Context, unsigned int Block, unsigned char *Buffer)
{
    (void)Context;
    if (Block >= Nblocks)
        return JR_EIO;
    PRead(Fd, Buffer, Phys[Block]);
    return 0;
}

static int WriteHome(void *Context, unsigned long long Block, const unsigned char *Buffer)
{
    (void)Context;
    PWrite(Fd, Buffer, Block);
    return 0;
}

static unsigned int Crc(unsigned int c, const unsigned char *p, unsigned int n) { return JlCrc32c(c, p, n); }
static void *Allocate(void *Context, unsigned int Bytes) { (void)Context; return malloc(Bytes); }
static void Release(void *Context, void *Memory) { (void)Context; free(Memory); }

static void CopyFile(const char *from, const char *to)
{
    static char buf[1 << 20];
    int a = open(from, O_RDONLY), b = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ssize_t n;
    if (a < 0 || b < 0)
        Die("open");
    while ((n = read(a, buf, sizeof(buf))) > 0)
        if (write(b, buf, (size_t)n) != n)
            Die("write");
    close(a);
    close(b);
}

/* the fs superblock: needs_recovery on, checksum again (metadata_csum) */
static void MarkNeedsRecovery(int fd)
{
    unsigned char sb[1024];
    unsigned int incompat, sum;
    if (pread(fd, sb, 1024, 1024) != 1024)
        Die("sb");
    memcpy(&incompat, sb + 0x60, 4);
    incompat |= 0x4;
    memcpy(sb + 0x60, &incompat, 4);
    sum = JlCrc32c(0xFFFFFFFFu, sb, 0x3FC);
    memcpy(sb + 0x3FC, &sum, 4);
    if (pwrite(fd, sb, 1024, 1024) != 1024)
        Die("sb write");
}

static void WriteCase(JL_CASE *C, int fd, unsigned char *jsb)
{
    unsigned int i;
    unsigned char *b = malloc(Bs);

    for (i = C->First; i < C->Last; i++)
        PWrite(fd, JlAt(C, i), Phys[i]);
    for (i = 0; i < C->Homes; i++) {
        JlHomeInitial(b, Bs, i);
        PWrite(fd, b, C->Base + i);
    }
    memcpy(b, jsb, Bs);
    JfPut32(b + JF_S_SEQUENCE, C->Seq);
    JfPut32(b + JF_S_START, C->Start);
    JfPut32(b + JF_S_COMPAT, C->Compat);
    JfPut32(b + JF_S_INCOMPAT, C->Incompat);
    b[JF_S_CHECKSUM_TYPE] = JfHasCsumV2V3(C->Incompat) ? JF_CRC32C_CHECKSUM : 0;
    JfPut32(b + JF_S_CHECKSUM, 0);
    if (JfHasCsumV2V3(C->Incompat))
        JfPut32(b + JF_S_CHECKSUM, JlCrc32c(0xFFFFFFFFu, b, JF_SUPERBLOCK_SIZE));
    PWrite(fd, b, Phys[0]);
    MarkNeedsRecovery(fd);
    free(b);
}

static int ReadHomes(const char *path, const JL_CASE *C, unsigned char *out)
{
    int fd = open(path, O_RDONLY);
    unsigned int i;
    if (fd < 0)
        Die("open homes");
    for (i = 0; i < C->Homes; i++)
        PRead(fd, out + (size_t)i * Bs, C->Base + i);
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    const char *image = argv[1];
    unsigned long long base, seed;
    unsigned int cases, i, n = 0, f, d;
    unsigned char *jsb, *ours, *theirs, *want;
    FILE *list;
    JL_CASE *C = calloc(1, sizeof(JL_CASE));
    JL_RNG r;

    if (argc < 6) {
        fprintf(stderr, "usage: wsl <image> <blocklist> <base> <cases> <seed>\n");
        return 2;
    }
    base = strtoull(argv[3], NULL, 10);
    cases = (unsigned int)strtoul(argv[4], NULL, 10);
    seed = strtoull(argv[5], NULL, 10);
    JlCrcInit();

    list = fopen(argv[2], "r");
    Phys = malloc(sizeof(*Phys) * 300000);
    while (list && fscanf(list, "%llu", &Phys[n]) == 1)
        n++;
    Nblocks = n;

    Fd = open(image, O_RDONLY);
    if (Fd < 0)
        Die("image");
    {
        unsigned char sb[1024];
        if (pread(Fd, sb, 1024, 1024) != 1024)
            Die("sb");
        Bs = 1024u << (sb[0x18] | sb[0x19] << 8);
    }
    jsb = malloc(Bs);
    PRead(Fd, jsb, Phys[0]);
    close(Fd);
    if (JfGet32(jsb) != JF_MAGIC || JfGet32(jsb + JF_S_MAXLEN) != Nblocks) {
        fprintf(stderr, "journal superblock does not match the block list\n");
        return 2;
    }
    C->Log = malloc((size_t)Nblocks * Bs);
    ours = malloc((size_t)JL_MAX_HOME * Bs);
    theirs = malloc((size_t)JL_MAX_HOME * Bs);
    want = malloc((size_t)JL_MAX_HOME * Bs);
    r.s = seed * 0x9E3779B97F4A7C15ull + 3;

    for (i = 0; i < cases; i++) {
        JR_JOURNAL J;
        JR_RESULT R;
        unsigned int txns = JlBelow(&r, JL_MAX_TXN + 1);
        int rc, fsck, mismatch = 0;
        char cmd[512], out[4096];
        FILE *o;
        size_t got;
        int saysChecksum, saysCorrupt, saysAborted;

        f = JlBelow(&r, JL_FEATURE_SETS);
        d = JlBelow(&r, 3) ? JlBelow(&r, D_KINDS) : D_NONE;
        for (;;) {
            C->BlockSize = Bs;
            C->First = JfGet32(jsb + JF_S_FIRST);
            C->Last = Nblocks;
            C->Start = C->First + JlBelow(&r, Nblocks - C->First);
            if (JlBelow(&r, 4) == 0)
                C->Start = Nblocks - 1 - JlBelow(&r, 4);
            C->Seq = JlBelow(&r, 5) ? JlRand(&r) | 1 : 0xFFFFFFFEu - JlBelow(&r, 3);
            C->Compat = JlFeatureSets[f].Compat;
            C->Incompat = JlFeatureSets[f].Incompat;
            C->CsumSeed = JlCrc32c(0xFFFFFFFFu, jsb + JF_S_UUID, 16);
            C->Homes = 4 + JlBelow(&r, JL_MAX_HOME - 4);
            C->Base = base;
            if (JlBuild(C, txns, (int)d, txns ? JlBelow(&r, txns) : 0, &r))
                break;
            if (txns)
                txns--;
        }
        /* jbd2 reads a zero at transaction 0 as "no end yet": not compared */
        if (C->ExpectEnd == 0 && C->Damage != D_NONE)
            continue;

        CopyFile(image, "/dev/shm/jr-a.img");
        CopyFile(image, "/dev/shm/jr-b.img");
        Fd = open("/dev/shm/jr-a.img", O_RDWR);
        WriteCase(C, Fd, jsb);
        close(Fd);
        Fd = open("/dev/shm/jr-b.img", O_RDWR);
        WriteCase(C, Fd, jsb);

        memset(&J, 0, sizeof(J));
        J.BlockSize = Bs;
        J.First = C->First;
        J.Last = C->Last;
        J.TotalLen = Nblocks;
        J.Compat = C->Compat;
        J.Incompat = C->Incompat;
        J.CsumSeed = C->CsumSeed;
        J.Start = C->Start;
        J.Sequence = C->Seq;
        J.ReadLog = ReadLog;
        J.WriteHome = WriteHome;
        J.Crc32c = Crc;
        J.Allocate = Allocate;
        J.Release = Release;
        rc = JrRecover(&J, &R);
        close(Fd);

        snprintf(cmd, sizeof(cmd), "e2fsck -f -y -E journal_only /dev/shm/jr-a.img > /dev/shm/jr-a.out 2>&1");
        fsck = system(cmd);
        o = fopen("/dev/shm/jr-a.out", "r");
        got = o ? fread(out, 1, sizeof(out) - 1, o) : 0;
        out[got] = 0;
        if (o)
            fclose(o);
        saysChecksum = strstr(out, "checksum error") != NULL;
        saysCorrupt = strstr(out, "orrupt") != NULL;
        saysAborted = strstr(out, "replay was aborted") != NULL || strstr(out, "was corrupt") != NULL;

        ReadHomes("/dev/shm/jr-a.img", C, theirs);
        ReadHomes("/dev/shm/jr-b.img", C, ours);
        JlExpectHome(C, want);
        Cases++;
        Seen[C->Damage]++;
        Replayed += R.Replayed;

        Checks += 3;
        if (memcmp(ours, theirs, (size_t)C->Homes * Bs)) {
            mismatch = 1;
            printf("MISMATCH home blocks vs e2fsck\n");
        }
        if (memcmp(ours, want, (size_t)C->Homes * Bs)) {
            mismatch = 1;
            printf("MISMATCH home blocks vs the expectation\n");
        }
        if (rc != C->ExpectError) {
            mismatch = 1;
            printf("MISMATCH result %d, expected %d\n", rc, C->ExpectError);
        }
        /* the report: an error where e2fsck reports one */
        Checks++;
        if ((rc == JR_EBADCRC) != saysChecksum && !(rc == JR_EBADCRC && saysCorrupt)) {
            mismatch = 1;
            printf("MISMATCH checksum report: ours %d, e2fsck says checksum %d corrupt %d\n", rc, saysChecksum, saysCorrupt);
        }
        Checks++;
        if (R.HasFailedCommit != saysAborted) {
            mismatch = 1;
            printf("MISMATCH failed commit: ours %d (tid %u), e2fsck %d\n", R.HasFailedCommit, R.FailedCommit, saysAborted);
        }
        if (rc == JR_OK && !R.HasFailedCommit) Agree[0]++;
        if (rc == JR_EBADCRC) Agree[1]++;
        if (R.HasFailedCommit) Agree[2]++;
        if (rc == JR_EINVAL) Agree[3]++;
        if (mismatch) {
            Mismatches++;
            printf("  case %u: %s, damage %s at %u of %u, start %u seq %u, rc %d end %u, fsck exit %d\n%s\n",
                   i, JlFeatureSets[f].Name, JlDamageNames[C->Damage], C->DamageTxn, C->Txns,
                   C->Start, C->Seq, rc, R.EndTid, fsck, out);
            if (Mismatches > 5)
                break;
        }
    }
    printf("JOURNAL-REPLAY-WSL: %llu cases (bs %u, journal %u blocks), %llu checks, %llu blocks replayed, %llu mismatches\n",
           Cases, Bs, Nblocks, Checks, Replayed, Mismatches);
    printf("  outcomes: clean %llu, checksum error %llu, failed commit %llu, bad revoke count %llu\n",
           Agree[0], Agree[1], Agree[2], Agree[3]);
    printf("  damage:");
    for (d = 0; d < D_KINDS; d++)
        printf(" %s %llu", JlDamageNames[d], Seen[d]);
    printf("\n");
    return Mismatches ? 1 : 0;
}
