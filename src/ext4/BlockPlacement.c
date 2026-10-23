/**
 * BlockPlacement.c - where new blocks go, over the groups of a volume.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * The rules, in order:
 *
 *  P1  Blocks whose goal is their place in the owner's layout (AtGoal) -
 *      right after the blocks before them, or as far from the nearest
 *      extent as they are in the file - go there, as many as the free run
 *      there holds: an extent grows, or a hole is filled where it lies.
 *  P2  Otherwise a free run is looked for at sizes Want = Need + Room (the
 *      owner's room to grow, at most a group), then Need, then half as much
 *      each time down to one block. At each size the groups are tried from
 *      the goal's on, wrapping, and the first that has such a run gives its
 *      shortest one (best fit). The blocks are taken from the run's start,
 *      so the rest of the run lies right after them.
 *  P3  A group whose Bound is below the size is passed over unvisited.
 *
 * What follows, for the view each Take searches (Bound is never below it):
 *
 *  T1  A file grown at its end alone, with Room = its size so far, at least
 *      doubles between two new extents as long as the volume has runs of
 *      twice its size: at most 1 + log2(final / first) extents, not one per
 *      write. (After a jump at size S the run holds S + Need; P1 then fills
 *      it before the next jump.)
 *  T2  A run of the size that succeeds is taken only when no run of the
 *      size before exists: with no run of Need anywhere, the run taken is
 *      longer than half the longest there is.
 *  T3  Best fit keeps the long runs for the requests that need them: a run
 *      is split only when no shorter one holds the size.
 *
 * Cost: O(log Need) sizes, each a pass over the groups' bounds; a group is
 * visited (locked, its bitmap searched) only when its bound holds the size.
 */

#include "BlockPlacement.h"

/* the size after Want: down to Need first, then halves (rounded up) */
static unsigned int Smaller(unsigned int Want, unsigned int Need)
{
    return Want > Need ? Need : Want - Want / 2;
}

int Ext2PlaceBlocks(const EXT2_PLACE_IO *Io, const EXT2_PLACE *Request,
                    unsigned int *Group, unsigned int *Index, unsigned int *Count)
{
    unsigned long long  room = (unsigned long long)Request->Need + Request->Room;
    unsigned int        want, i, g;
    int                 err;

    if (Request->Need == 0 || Io->Groups == 0 || Request->Group >= Io->Groups)
        return EXT2_PLACE_NONE;

    /* P1 */
    if (Request->AtGoal) {
        err = Io->Take(Io->Context, Request->Group, Request->Hint, 0, Request->Need, Index, Count);
        if (err != EXT2_PLACE_NONE) {
            *Group = Request->Group;
            return err;
        }
    }

    /* P2, P3 */
    want = room < Io->GroupBlocks ? (unsigned int)room : Io->GroupBlocks;
    for (;;) {
        for (i = 0, g = Request->Group; i < Io->Groups; i++, g = (g + 1 == Io->Groups) ? 0 : g + 1) {
            if (Io->Bound(Io->Context, g) < want)
                continue;
            err = Io->Take(Io->Context, g, i == 0 ? Request->Hint : 0, want, Request->Need, Index, Count);
            if (err != EXT2_PLACE_NONE) {
                *Group = g;
                return err;
            }
        }
        if (want == 1)
            return EXT2_PLACE_NONE;
        want = Smaller(want, Request->Need);
    }
}
