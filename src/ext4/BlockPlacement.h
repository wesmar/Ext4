/**
 * BlockPlacement.h - where new blocks go, over the groups of a volume.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * Portable: no kernel, no locks, no I/O of its own. The groups are reached
 * through EXT2_PLACE_IO, so that tests\block-place-model.ps1 runs this very
 * code over simulated volumes; the driver's side is BlockAllocator.c.
 */

#ifndef _EXT4_BLOCK_PLACEMENT_H_
#define _EXT4_BLOCK_PLACEMENT_H_

/* what Take answers besides 0 (taken) and an error of its own */
#define EXT2_PLACE_NONE     1               /* nothing in this group: go on */

typedef struct _EXT2_PLACE_IO {
    void           *Context;
    unsigned int    Groups;                 /* searched: 0 .. Groups - 1 */
    unsigned int    GroupBlocks;            /* the longest run a group can have */

    /* at least the longest free run of Group: exact once its bitmap was seen */
    unsigned int  (*Bound)(void *Context, unsigned int Group);

    /* Up to Need blocks of Group taken, *Index the first, *Count how many:
       Want 0 - from the run starting at Hint; Want > 0 - from the start of
       the shortest run of at least Want (Ext2ViewBestRun from Hint).
       0, EXT2_PLACE_NONE, or an error that ends the search. */
    int           (*Take)(void *Context, unsigned int Group, unsigned int Hint, unsigned int Want,
                          unsigned int Need, unsigned int *Index, unsigned int *Count);
} EXT2_PLACE_IO;

typedef struct _EXT2_PLACE {
    unsigned int    Group;                  /* the goal's group */
    unsigned int    Hint;                   /* the goal's index in it */
    unsigned int    Need;                   /* blocks wanted, at most */
    unsigned int    Room;                   /* blocks the owner may grow by after them */
    int             AtGoal;                 /* the goal is the blocks' place in the owner's layout */
} EXT2_PLACE;

/* 0 with *Group, *Index, *Count; EXT2_PLACE_NONE: no free block anywhere;
   or Take's error */
int Ext2PlaceBlocks(const EXT2_PLACE_IO *Io, const EXT2_PLACE *Request,
                    unsigned int *Group, unsigned int *Index, unsigned int *Count);

#endif /* _EXT4_BLOCK_PLACEMENT_H_ */
