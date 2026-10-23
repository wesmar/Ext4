/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The production block search (src\ext4\BlockSearch.h) against a bit-by-bit
 * reference of the same contract, on deterministic random groups: sizes
 * that are not whole words, junk past the end of the last word, densities
 * from empty to full, zero to two overlays of freed blocks, any hint and
 * any wanted length. Every answer must be the reference's: the run at a
 * block, the best fit (shortest run of at least Want, the first from the
 * hint among equals, a run counted whole where it starts) and the longest
 * run; and every run returned must be free in the view and inside the group.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXT2_FREED_SLOTS 2
#include "../../src/ext4/BlockSearch.h"

#define MODEL_MAX_BITS      32768u          /* a 4 KiB bitmap block */
#define MODEL_WORDS         (MODEL_MAX_BITS / EXT2_BITS_PER_WORD)
#define MODEL_CASES         200000u
#define MODEL_FULL_EVERY    64u             /* every 64th case a whole 4 KiB group */

static ULONG64 Disk[MODEL_WORDS];
static ULONG64 FreedA[MODEL_WORDS];
static ULONG64 FreedB[MODEL_WORDS];

/* xorshift64*: deterministic, seedable */
static ULONG64 Seed = 0x9E3779B97F4A7C15ULL;
static ULONG64 Next(void)
{
    Seed ^= Seed >> 12;
    Seed ^= Seed << 25;
    Seed ^= Seed >> 27;
    return Seed * 0x2545F4914F6CDD1DULL;
}
static ULONG Below(ULONG n) { return (ULONG)(Next() % n); }

static int Bit(const ULONG64 *Map, ULONG i) { return (int)((Map[i / 64] >> (i % 64)) & 1); }

static int RefTaken(const EXT2_BLOCK_VIEW *View, ULONG i)
{
    ULONG s;

    if (i >= View->Bits) {
        return 1;
    }
    if (Bit(View->Disk, i)) {
        return 1;
    }
    for (s = 0; s < EXT2_FREED_SLOTS; s++) {
        if (View->Freed[s] != NULL && Bit(View->Freed[s], i)) {
            return 1;
        }
    }
    return 0;
}

static ULONG RefRunAt(const EXT2_BLOCK_VIEW *View, ULONG From)
{
    ULONG End = From;

    while (End < View->Bits && !RefTaken(View, End)) End++;
    return End - From;
}

/* every maximal run, by its start: the shortest of at least Want, ties to
   the first start from Hint on, wrapping */
static BOOLEAN RefBestRun(const EXT2_BLOCK_VIEW *View, ULONG Hint, ULONG Want, ULONG *Index, ULONG *Length)
{
    ULONG Best = 0, BestKey = 0, p = 0;

    if (Hint >= View->Bits) Hint = 0;
    while (p < View->Bits) {
        ULONG Start, End, Key;
        while (p < View->Bits && RefTaken(View, p)) p++;
        if (p >= View->Bits) break;
        Start = p;
        End = Start + RefRunAt(View, Start);
        Key = Start >= Hint ? Start - Hint : Start + View->Bits;
        if (End - Start >= Want &&
            (Best == 0 || End - Start < Best || (End - Start == Best && Key < BestKey))) {
            Best = End - Start;
            BestKey = Key;
            *Index = Start;
        }
        p = End;
    }
    *Length = Best;
    return Best != 0;
}

static ULONG RefLongest(const EXT2_BLOCK_VIEW *View)
{
    ULONG Longest = 0, p = 0;

    while (p < View->Bits) {
        ULONG Length = RefTaken(View, p) ? 0 : RefRunAt(View, p);
        if (Length > Longest) Longest = Length;
        p += Length ? Length : 1;
    }
    return Longest;
}

/* a map with about Density/256 of its bits set, in runs, junk past Bits */
static void Fill(ULONG64 *Map, ULONG Bits, ULONG Density)
{
    ULONG i = 0;

    memset(Map, 0, sizeof(Disk));
    while (i < Bits) {
        ULONG Run = 1 + Below(Below(4) == 0 ? 512 : 16);
        int   Set = Below(256) < Density;
        ULONG j;
        for (j = 0; j < Run && i < Bits; j++, i++) {
            if (Set) Map[i / 64] |= 1ULL << (i % 64);
        }
    }
    /* whatever lies past the group in its last word must not matter */
    if (Bits % 64) {
        Map[Bits / 64] |= Next() & (~0ULL << (Bits % 64));
    }
}

static ULONG Failed;

static void Fail(ULONG Case, const char *What, ULONG Bits, ULONG Hint, ULONG Want,
                 ULONG Got, ULONG GotLength, ULONG Ref, ULONG RefLength)
{
    if (Failed++ < 10) {
        printf("FAIL case %lu %s: bits %lu hint %lu want %lu: got %lu+%lu, want %lu+%lu\n",
               Case, What, Bits, Hint, Want, Got, GotLength, Ref, RefLength);
    }
}

/* the run [Index, Index + Length) lies in the group, free in the view, maximal at its end */
static void CheckRun(ULONG Case, const EXT2_BLOCK_VIEW *View, ULONG Index, ULONG Length)
{
    ULONG i;

    if (Length == 0 || Index + Length > View->Bits) {
        Fail(Case, "run outside the group", View->Bits, 0, 0, Index, Length, 0, 0);
        return;
    }
    for (i = Index; i < Index + Length; i++) {
        if (RefTaken(View, i)) {
            Fail(Case, "run holds a taken block", View->Bits, 0, 0, Index, Length, i, 0);
            return;
        }
    }
    if (!RefTaken(View, Index + Length)) {
        Fail(Case, "run not maximal", View->Bits, 0, 0, Index, Length, 0, 0);
    }
}

int main(void)
{
    ULONG Case, Fits = 0, Exact = 0, Ats = 0;

    for (Case = 0; Case < MODEL_CASES; Case++) {
        EXT2_BLOCK_VIEW View;
        ULONG Bits = (Case % MODEL_FULL_EVERY == 0) ? MODEL_MAX_BITS : 1 + Below(4096);
        ULONG Hint = Below(Bits + 64);            /* past the end too */
        ULONG Want = 1 + Below(Below(8) == 0 ? 2048 : 64);
        ULONG Slots = Below(3);
        ULONG GotIndex = 0, GotLength = 0, RefIndex = 0, RefLength = 0;
        ULONG At = Below(Bits + 8), GotAt, RefAt;
        BOOLEAN Got, Ref;

        Fill(Disk, Bits, Below(257));
        Fill(FreedA, Bits, Below(64));
        Fill(FreedB, Bits, Below(64));
        View.Disk = Disk;
        View.Freed[0] = Slots >= 1 ? FreedA : NULL;
        View.Freed[1] = Slots >= 2 ? FreedB : NULL;
        View.Bits = Bits;

        /* the run at a block */
        GotAt = Ext2ViewRunAt(&View, At);
        RefAt = (At < Bits && !RefTaken(&View, At)) ? RefRunAt(&View, At) : 0;
        if (GotAt != RefAt) {
            Fail(Case, "run at", Bits, At, 0, At, GotAt, At, RefAt);
        } else if (GotAt) {
            Ats++;
            CheckRun(Case, &View, At, GotAt);
        }

        /* best fit */
        Got = Ext2ViewBestRun(&View, Hint, Want, &GotIndex, &GotLength);
        Ref = RefBestRun(&View, Hint, Want, &RefIndex, &RefLength);
        if (Got != Ref || (Got && (GotIndex != RefIndex || GotLength != RefLength))) {
            Fail(Case, "best fit", Bits, Hint, Want, GotIndex, Got ? GotLength : 0, RefIndex, Ref ? RefLength : 0);
        } else if (Got) {
            Fits++;
            Exact += GotLength == Want;
            CheckRun(Case, &View, GotIndex, GotLength);
            if (GotIndex > 0 && !RefTaken(&View, GotIndex - 1)) {
                Fail(Case, "best fit not a whole run", Bits, Hint, Want, GotIndex, GotLength, 0, 0);
            }
        }

        /* the longest run */
        if (Ext2ViewLongestRun(&View) != RefLongest(&View)) {
            Fail(Case, "longest", Bits, 0, 0, 0, Ext2ViewLongestRun(&View), 0, RefLongest(&View));
        }
    }

    printf("block search: %lu cases - %lu runs at a block, %lu best fits (%lu exact), %lu failed\n",
           MODEL_CASES, Ats, Fits, Exact, Failed);
    printf(Failed ? "BLOCK-SEARCH-MODEL: FAILED\n" : "BLOCK-SEARCH-MODEL: ALL PASSED\n");
    return Failed ? 1 : 0;
}
