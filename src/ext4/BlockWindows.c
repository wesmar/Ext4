/**
 * BlockWindows.c - room kept free after files that grow at their end.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * A linear table: a few dozen slots, scanned whole, cheaper than any tree at
 * this size and with nothing to allocate.
 */

#include "../include/BlockWindows.h"

unsigned int Ext2WindowCuts(const EXT2_WINDOWS *Table, unsigned int Group, unsigned int Owner,
                            EXT2_BLOCK_CUT *Cuts)
{
    unsigned int i, n = 0;

    if (Table->Live == 0)
        return 0;
    for (i = 0; i < EXT2_WINDOW_SLOTS; i++) {
        const EXT2_WINDOW *w = &Table->Slot[i];
        if (w->Blocks.Count != 0 && w->Group == Group && w->Owner != Owner)
            Cuts[n++] = w->Blocks;
    }
    return n;
}

void Ext2WindowTaken(EXT2_WINDOWS *Table, unsigned int Group, unsigned int Index, unsigned int Count)
{
    unsigned int i, End = Index + Count;

    if (Table->Live == 0)
        return;
    for (i = 0; i < EXT2_WINDOW_SLOTS; i++) {
        EXT2_WINDOW    *w = &Table->Slot[i];
        unsigned int    Start = w->Blocks.Start, Last = Start + w->Blocks.Count;

        if (w->Blocks.Count == 0 || w->Group != Group || Start >= End || Last <= Index)
            continue;
        if (Start < Index) {
            w->Blocks.Count = Index - Start;
        } else if (Last > End) {
            w->Blocks.Start = End;
            w->Blocks.Count = Last - End;
        } else {
            w->Blocks.Count = 0;
            Table->Live--;
        }
    }
}

void Ext2WindowSet(EXT2_WINDOWS *Table, unsigned int Owner, unsigned int Group, EXT2_BLOCK_CUT Window)
{
    EXT2_WINDOW    *Slot = 0;
    unsigned int    i;

    /* the owner's own slot, else a free one, else the one set longest ago */
    for (i = 0; i < EXT2_WINDOW_SLOTS; i++) {
        EXT2_WINDOW *w = &Table->Slot[i];
        if (w->Blocks.Count != 0 && w->Owner == Owner) {
            Slot = w;
            break;
        }
        if (Window.Count != 0 &&
            (Slot == 0 || (Slot->Blocks.Count != 0 &&
                           (w->Blocks.Count == 0 || Table->Clock - w->Set > Table->Clock - Slot->Set))))
            Slot = w;
    }
    if (Slot == 0)
        return;                                     /* nothing to drop */
    if (Slot->Blocks.Count == 0)
        Table->Live++;
    if (Window.Count == 0)
        Table->Live--;
    Slot->Owner = Owner;
    Slot->Group = Group;
    Slot->Blocks = Window;
    Slot->Set = ++Table->Clock;
}
