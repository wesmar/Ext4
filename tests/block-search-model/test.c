/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The production block search (src\ext4\BlockSearch.h) against a bit-by-bit
 * reference of the same contract, on deterministic random groups: sizes
 * that are not whole words, junk past the end of the last word, densities
 * from empty to full, zero to two overlays of freed blocks, any hint and
 * any wanted length. Every answer must be the reference's, and every run
 * returned must be free in the view and inside the group.
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

/* the contract, one bit at a time */
static BOOLEAN RefPass(const EXT2_BLOCK_VIEW *View, ULONG From, ULONG To, ULONG Want,
                       ULONG *Index, ULONG *AnyIndex, ULONG *AnyLength)
{
    ULONG p = From;

    for (;;) {
        ULONG End;
        while (p < View->Bits && RefTaken(View, p)) p++;
        if (p >= To || p >= View->Bits) {
            return FALSE;
        }
        End = p;
        while (End < View->Bits && !RefTaken(View, End)) End++;
        if (*AnyLength == 0) {
            *AnyIndex = p;
            *AnyLength = End - p;
        }
        if (End - p >= Want) {
            *Index = p;
            return TRUE;
        }
        p = End;
    }
}

static BOOLEAN RefFind(const EXT2_BLOCK_VIEW *View, ULONG Hint, ULONG *Number, ULONG *Index)
{
    ULONG AnyIndex = 0, AnyLength = 0;

    if (Hint >= View->Bits) Hint = 0;
    if (RefPass(View, Hint, View->Bits, *Number, Index, &AnyIndex, &AnyLength) ||
        RefPass(View, 0, Hint, *Number, Index, &AnyIndex, &AnyLength)) {
        return TRUE;
    }
    if (AnyLength == 0) return FALSE;
    *Index = AnyIndex;
    if (AnyLength < *Number) *Number = AnyLength;
    return TRUE;
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

int main(void)
{
    ULONG Case, Failed = 0, Found = 0, Partial = 0;

    for (Case = 0; Case < MODEL_CASES; Case++) {
        EXT2_BLOCK_VIEW View;
        ULONG Bits = (Case % MODEL_FULL_EVERY == 0) ? MODEL_MAX_BITS : 1 + Below(4096);
        ULONG Hint = Below(Bits + 64);            /* past the end too */
        ULONG Want = 1 + Below(Below(8) == 0 ? 2048 : 64);
        ULONG Slots = Below(3);
        ULONG GotIndex = 0, RefIndex = 0, GotNumber = Want, RefNumber = Want;
        BOOLEAN Got, Ref;

        Fill(Disk, Bits, Below(257));
        Fill(FreedA, Bits, Below(64));
        Fill(FreedB, Bits, Below(64));
        View.Disk = Disk;
        View.Freed[0] = Slots >= 1 ? FreedA : NULL;
        View.Freed[1] = Slots >= 2 ? FreedB : NULL;
        View.Bits = Bits;

        Got = Ext2FindClearRun(&View, Hint, &GotNumber, &GotIndex);
        Ref = RefFind(&View, Hint, &RefNumber, &RefIndex);

        if (Got != Ref || (Got && (GotIndex != RefIndex || GotNumber != RefNumber))) {
            if (Failed++ < 10) {
                printf("FAIL case %lu: bits %lu hint %lu want %lu slots %lu: got %d %lu+%lu, want %d %lu+%lu\n",
                       Case, Bits, Hint, Want, Slots, Got, GotIndex, GotNumber, Ref, RefIndex, RefNumber);
            }
            continue;
        }
        if (Got) {
            ULONG i;
            Found++;
            Partial += GotNumber < Want;
            if (GotNumber == 0 || GotNumber > Want || GotIndex + GotNumber > Bits) {
                Failed++;
                printf("FAIL case %lu: run %lu+%lu outside the group of %lu\n", Case, GotIndex, GotNumber, Bits);
                continue;
            }
            for (i = GotIndex; i < GotIndex + GotNumber; i++) {
                if (RefTaken(&View, i)) {
                    Failed++;
                    printf("FAIL case %lu: block %lu of the run is taken\n", Case, i);
                    break;
                }
            }
        }
    }

    printf("block search: %lu cases, %lu found (%lu shorter than wanted), %lu failed\n",
           MODEL_CASES, Found, Partial, Failed);
    printf(Failed ? "BLOCK-SEARCH-MODEL: FAILED\n" : "BLOCK-SEARCH-MODEL: ALL PASSED\n");
    return Failed ? 1 : 0;
}
