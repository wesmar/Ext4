/**
 * BlockSearch.h - free runs in a block group: the bitmap on disk OR the blocks
 * whose release has not committed, a 64-bit word at a time.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Pure functions on memory: no locks, no I/O, nothing of the kernel but a
 * bit scan - so that tests\block-search-model.ps1 compiles this very file
 * and checks it against a bit-by-bit reference on random bitmaps.
 *
 * Bit i of a group is bit i % 64 of little-endian word i / 64, as in the
 * ext4 bitmap block and in RTL_BITMAP. A run is found by two scans: the
 * next free bit (a set bit of ~taken), then the next taken one; whole
 * words of taken or free blocks are skipped by one test each, so a search
 * costs O(words + runs), not O(bits).
 *
 * A run is always maximal: free from its first block to its last, taken
 * (or past the group) on both sides.
 *
 * Other files' windows (BlockWindows.h) may be laid over the view as Cuts:
 * their blocks then count taken too.
 */

#ifndef _EXT4_BLOCK_SEARCH_H_
#define _EXT4_BLOCK_SEARCH_H_

#ifndef EXT2_FREED_SLOTS
#error EXT2_FREED_SLOTS (ext4fs_ext4.h) must be defined first
#endif

#include "../include/BlockWindows.h"

#define EXT2_BITS_PER_WORD      64

typedef struct _EXT2_BLOCK_VIEW {
    const ULONG64  *Disk;                       /* the group's bitmap block */
    const ULONG64  *Freed[EXT2_FREED_SLOTS];    /* freed, not yet reusable; NULL: none */
    const EXT2_BLOCK_CUT *Cuts;                 /* windows of other files, CutCount of them */
    ULONG           CutCount;
    ULONG           Bits;                       /* the searchable prefix of the group */
} EXT2_BLOCK_VIEW, *PEXT2_BLOCK_VIEW;

/* word Word of the view, a set bit for every block taken; past the end taken */
static __forceinline ULONG64
Ext2ViewTaken(const EXT2_BLOCK_VIEW *View, ULONG Word)
{
    ULONG64 Taken = View->Disk[Word];
    ULONG   Valid = View->Bits - Word * EXT2_BITS_PER_WORD;
    ULONG   First = Word * EXT2_BITS_PER_WORD;
    ULONG   s;

    for (s = 0; s < EXT2_FREED_SLOTS; s++) {
        if (View->Freed[s] != NULL) {
            Taken |= View->Freed[s][Word];
        }
    }
    for (s = 0; s < View->CutCount; s++) {
        ULONG From = View->Cuts[s].Start, To = From + View->Cuts[s].Count;
        if (From < First + EXT2_BITS_PER_WORD && To > First) {
            From = From > First ? From - First : 0;
            To = To < First + EXT2_BITS_PER_WORD ? To - First : EXT2_BITS_PER_WORD;
            Taken |= (To - From == EXT2_BITS_PER_WORD ? ~0ULL : ((1ULL << (To - From)) - 1)) << From;
        }
    }
    if (Valid < EXT2_BITS_PER_WORD) {
        Taken |= ~0ULL << Valid;
    }
    return Taken;
}

/* the first bit at or after From that is free (Free) or taken; Bits if none */
static __inline ULONG
Ext2ViewNext(const EXT2_BLOCK_VIEW *View, ULONG From, BOOLEAN Free)
{
    ULONG           Words = (View->Bits + EXT2_BITS_PER_WORD - 1) / EXT2_BITS_PER_WORD;
    ULONG           Word = From / EXT2_BITS_PER_WORD;
    ULONG64         Mask = ~0ULL << (From % EXT2_BITS_PER_WORD);
    unsigned long   Bit;

    if (From >= View->Bits) {
        return View->Bits;
    }
    for (; Word < Words; Word++, Mask = ~0ULL) {
        ULONG64 Taken = Ext2ViewTaken(View, Word);
        if (_BitScanForward64(&Bit, (Free ? ~Taken : Taken) & Mask)) {
            ULONG Found = Word * EXT2_BITS_PER_WORD + Bit;
            return Found < View->Bits ? Found : View->Bits;
        }
    }
    return View->Bits;
}

/* the free run that starts at From: its length, 0 when From is taken */
static __inline ULONG
Ext2ViewRunAt(const EXT2_BLOCK_VIEW *View, ULONG From)
{
    if (From >= View->Bits || Ext2ViewNext(View, From, TRUE) != From) {
        return 0;
    }
    return Ext2ViewNext(View, From, FALSE) - From;
}

/*
 * Best fit: the shortest run of at least Want blocks (Want > 0), and of
 * runs as short the first from Hint on, wrapping - *Index, *Length. The run
 * around Hint counts whole, where it starts. FALSE: no run is that long.
 */
static __inline BOOLEAN
Ext2ViewBestRun(const EXT2_BLOCK_VIEW *View, ULONG Hint, ULONG Want, PULONG Index, PULONG Length)
{
    ULONG Best = 0, Pass, From, To, Start, End;

    if (Hint >= View->Bits) {
        Hint = 0;
    }
    for (Pass = 0; Pass < 2; Pass++) {
        /* runs starting in [Hint, Bits), then in [0, Hint); one that holds
           Hint without starting there belongs to the second pass */
        From = Pass ? 0 : Hint;
        To = Pass ? Hint : View->Bits;
        if (Pass == 0 && Hint > 0 && Ext2ViewNext(View, Hint - 1, TRUE) == Hint - 1) {
            From = Ext2ViewNext(View, Hint, FALSE);
        }
        for (Start = Ext2ViewNext(View, From, TRUE); Start < To; Start = Ext2ViewNext(View, End, TRUE)) {
            End = Ext2ViewNext(View, Start + 1, FALSE);
            if (End - Start >= Want && (Best == 0 || End - Start < Best)) {
                Best = End - Start;
                *Index = Start;
                if (Best == Want) {
                    *Length = Best;     /* none can fit better */
                    return TRUE;
                }
            }
        }
    }
    *Length = Best;
    return Best != 0;
}

/* the length of the longest run, 0 when nothing is free */
static __inline ULONG
Ext2ViewLongestRun(const EXT2_BLOCK_VIEW *View)
{
    ULONG Longest = 0, Start, End;

    for (Start = Ext2ViewNext(View, 0, TRUE); Start < View->Bits; Start = Ext2ViewNext(View, End, TRUE)) {
        End = Ext2ViewNext(View, Start + 1, FALSE);
        if (End - Start > Longest) {
            Longest = End - Start;
        }
    }
    return Longest;
}

#endif /* _EXT4_BLOCK_SEARCH_H_ */
