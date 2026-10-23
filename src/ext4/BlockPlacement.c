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
 *  P4  A file that grows at its end (Room > 0) keeps the free run after its
 *      new blocks, up to Room long, as its window (BlockWindows.h). P1 and
 *      P2 see other files' windows as taken. Only at a size of Need or less,
 *      when no group has such a run outside them, a second pass sees them
 *      free and takes the blocks from the end of the run that holds one:
 *      its start, where its owner grows next, goes last.
 *
 * What follows, for the view each Take searches (Bound is never below it):
 *
 *  T1  A file grown at its end, with Room = its size so far, at least
 *      doubles between two new extents as long as the volume has runs of
 *      twice its size outside other windows - whatever other files are
 *      written between its writes: at most 2 + log2(final / first)
 *      extents, not one per write. (After a jump at size S the run holds
 *      S + Need; the rest is its window, which P1 then fills.)
 *  T2  A run of the size that succeeds is taken only when no run of the
 *      size before exists: with no run of Need anywhere, windows counted
 *      free, the run taken is longer than half the longest. A window never
 *      makes a take shorter.
 *  T3  Best fit keeps the long runs for the requests that need them: a run
 *      is split only when no shorter one holds the size.
 *  T4  Blocks fall into another file's window only when no run of their
 *      size, at most Need, lies outside all windows.
 *
 * Cost: O(log Need) sizes, each a pass over the groups' bounds; a group is
 * visited (locked, its bitmap searched) only when its bound holds the size,
 * and in a second pass only when it holds a window.
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
    EXT2_PLACE_TAKE     Ask;
    unsigned int        i;
    int                 err;

    if (Request->Need == 0 || Io->Groups == 0 || Request->Group >= Io->Groups)
        return EXT2_PLACE_NONE;

    Ask.Group = Request->Group;
    Ask.Hint = Request->Hint;
    Ask.Want = 0;
    Ask.Need = Request->Need;
    Ask.Room = Request->Room;
    Ask.Any = 0;

    /* P1 */
    if (Request->AtGoal) {
        err = Io->Take(Io->Context, &Ask, Index, Count);
        if (err != EXT2_PLACE_NONE) {
            *Group = Ask.Group;
            return err;
        }
    }

    /* P2, P3, P4 */
    Ask.Want = room < Io->GroupBlocks ? (unsigned int)room : Io->GroupBlocks;
    for (;;) {
        for (Ask.Any = 0; Ask.Any < (Ask.Want <= Request->Need ? 2 : 1); Ask.Any++) {
            for (i = 0, Ask.Group = Request->Group; i < Io->Groups;
                 i++, Ask.Group = (Ask.Group + 1 == Io->Groups) ? 0 : Ask.Group + 1) {
                if (Io->Bound(Io->Context, Ask.Group) < Ask.Want)
                    continue;
                Ask.Hint = i == 0 ? Request->Hint : 0;
                err = Io->Take(Io->Context, &Ask, Index, Count);
                if (err != EXT2_PLACE_NONE) {
                    *Group = Ask.Group;
                    return err;
                }
            }
        }
        if (Ask.Want == 1)
            return EXT2_PLACE_NONE;
        Ask.Want = Smaller(Ask.Want, Request->Need);
    }
}
