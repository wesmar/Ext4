/**
 * BlockWindows.h - room kept free after files that grow at their end.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * A file written at its end takes its blocks and, when they come from a
 * longer free run, leaves the rest of that run, up to its size so far, as its
 * window: the blocks it will grow into next. Other files' blocks go around
 * the windows while there is room elsewhere (BlockPlacement.c), so that two
 * files written side by side do not cut each other into pieces. A window is
 * in memory only: nothing on disk knows of it, and its blocks count free.
 *
 * Portable: no kernel, no locks. The volume keeps one table under a lock of
 * its own (BlockAllocator.c); tests\block-place-model.ps1 runs this code.
 *
 * The table holds EXT2_WINDOW_SLOTS windows, one per owner at most.
 * Full, it gives up the window set longest ago: a window is a hint, never a
 * promise, so one lost - its file closed without a word, or simply the
 * oldest - costs placement, not correctness.
 */

#ifndef _EXT4_BLOCK_WINDOWS_H_
#define _EXT4_BLOCK_WINDOWS_H_

#define EXT2_WINDOW_SLOTS        32

/* blocks [Start, Start + Count) of a group */
typedef struct _EXT2_BLOCK_CUT {
    unsigned int    Start;
    unsigned int    Count;
} EXT2_BLOCK_CUT;

typedef struct _EXT2_WINDOW {
    unsigned int    Owner;                  /* the file's inode number */
    unsigned int    Group;
    EXT2_BLOCK_CUT  Blocks;                 /* Count 0: the slot is free */
    unsigned int    Set;                    /* the table's Clock when it was set */
} EXT2_WINDOW;

typedef struct _EXT2_WINDOWS {
    EXT2_WINDOW     Slot[EXT2_WINDOW_SLOTS];
    unsigned int    Clock;
    unsigned int    Live;                   /* slots in use */
} EXT2_WINDOWS;

/* the windows in Group of owners other than Owner, into Cuts (room for
   EXT2_WINDOW_SLOTS): how many */
unsigned int Ext2WindowCuts(const EXT2_WINDOWS *Table, unsigned int Group, unsigned int Owner,
                            EXT2_BLOCK_CUT *Cuts);

/* blocks [Index, Index + Count) of Group taken: windows that held any of
   them keep their part before them (where their owner grows on), or, taken
   from their start, their part after */
void Ext2WindowTaken(EXT2_WINDOWS *Table, unsigned int Group, unsigned int Index, unsigned int Count);

/* Owner's window is now Window in Group (Count 0: none) */
void Ext2WindowSet(EXT2_WINDOWS *Table, unsigned int Owner, unsigned int Group, EXT2_BLOCK_CUT Window);

#endif /* _EXT4_BLOCK_WINDOWS_H_ */
