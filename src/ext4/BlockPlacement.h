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

/* one Take: what to look for in a group, and what to leave behind */
typedef struct _EXT2_PLACE_TAKE {
    unsigned int    Group;
    unsigned int    Hint;                   /* where to look from */
    unsigned int    Want;                   /* 0: the run starting at Hint; else a run of at least Want */
    unsigned int    Need;                   /* blocks wanted, at most */
    unsigned int    Room;                   /* > 0: the owner's window after them, at most this */
    int             Any;                    /* other files' windows count free (BlockWindows.h) */
} EXT2_PLACE_TAKE;

typedef struct _EXT2_PLACE_IO {
    void           *Context;
    unsigned int    Groups;                 /* searched: 0 .. Groups - 1 */
    unsigned int    GroupBlocks;            /* the longest run a group can have */

    /* at least the longest free run of Group, windows counted free: exact
       once its bitmap was seen */
    unsigned int  (*Bound)(void *Context, unsigned int Group);

    /* Up to Need blocks of Group taken, *Index the first, *Count how many.
       Want 0: from the run starting at Hint. Want > 0: from the shortest run
       of at least Want (Ext2ViewBestRun from Hint) - at its start, or with
       Any, where it holds another file's window, at its end. Other files'
       windows are taken unless Any; with Any a group that has none answers
       EXT2_PLACE_NONE at once. Windows the blocks fell in shrink
       (Ext2WindowTaken); with Room > 0 the owner's window becomes the free
       run after the blocks, up to Room blocks long.
       0, EXT2_PLACE_NONE, or an error that ends the search. */
    int           (*Take)(void *Context, const EXT2_PLACE_TAKE *Ask, unsigned int *Index, unsigned int *Count);
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
