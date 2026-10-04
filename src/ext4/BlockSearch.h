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
 */

#ifndef _EXT4_BLOCK_SEARCH_H_
#define _EXT4_BLOCK_SEARCH_H_

#ifndef EXT2_FREED_SLOTS
#error EXT2_FREED_SLOTS (ext4fs_ext4.h) must be defined first
#endif

#define EXT2_BITS_PER_WORD      64

typedef struct _EXT2_BLOCK_VIEW {
    const ULONG64  *Disk;                       /* the group's bitmap block */
    const ULONG64  *Freed[EXT2_FREED_SLOTS];    /* freed, not yet reusable; NULL: none */
    ULONG           Bits;                       /* the searchable prefix of the group */
} EXT2_BLOCK_VIEW, *PEXT2_BLOCK_VIEW;

/* word Word of the view, a set bit for every block taken; past the end taken */
static __forceinline ULONG64
Ext2ViewTaken(const EXT2_BLOCK_VIEW *View, ULONG Word)
{
    ULONG64 Taken = View->Disk[Word];
    ULONG   Valid = View->Bits - Word * EXT2_BITS_PER_WORD;
    ULONG   s;

    for (s = 0; s < EXT2_FREED_SLOTS; s++) {
        if (View->Freed[s] != NULL) {
            Taken |= View->Freed[s][Word];
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

/*
 * Free runs starting in [From, To): the first of at least Want blocks sets
 * *Index; the first run of any length is kept in *AnyIndex / *AnyLength
 * unless one was kept already.
 */
static __inline BOOLEAN
Ext2ViewFindRun(const EXT2_BLOCK_VIEW *View, ULONG From, ULONG To, ULONG Want,
                PULONG Index, PULONG AnyIndex, PULONG AnyLength)
{
    ULONG Start = Ext2ViewNext(View, From, TRUE);

    while (Start < To) {
        ULONG End = Ext2ViewNext(View, Start + 1, FALSE);

        if (*AnyLength == 0) {
            *AnyIndex = Start;
            *AnyLength = End - Start;
        }
        if (End - Start >= Want) {
            *Index = Start;
            return TRUE;
        }
        Start = Ext2ViewNext(View, End, TRUE);
    }
    return FALSE;
}

/*
 * Where to allocate: a run of *Number from Hint on, then from the start of
 * the group; failing that the first shorter run from Hint on (else from
 * the start), *Number cut to its length. FALSE: nothing free at all.
 */
static __inline BOOLEAN
Ext2FindClearRun(const EXT2_BLOCK_VIEW *View, ULONG Hint, PULONG Number, PULONG Index)
{
    ULONG AnyIndex = 0, AnyLength = 0;

    if (Hint >= View->Bits) {
        Hint = 0;
    }
    if (Ext2ViewFindRun(View, Hint, View->Bits, *Number, Index, &AnyIndex, &AnyLength) ||
        Ext2ViewFindRun(View, 0, Hint, *Number, Index, &AnyIndex, &AnyLength)) {
        return TRUE;
    }
    if (AnyLength == 0) {
        return FALSE;
    }
    *Index = AnyIndex;
    *Number = AnyLength < *Number ? AnyLength : *Number;
    return TRUE;
}

#endif /* _EXT4_BLOCK_SEARCH_H_ */
