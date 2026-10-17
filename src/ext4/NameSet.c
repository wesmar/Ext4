/**
 * NameSet.c - a multiset of 32-bit name hashes.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 *  S1  A hash lives in the first slot of its probe - from its home slot
 *      (hash & mask) onwards, cyclically - that holds it or is empty; every
 *      slot from its home up to it holds a hash. So a lookup stops at the
 *      first empty slot.
 *  S2  A hash whose count drops to 0 leaves its slot; each later hash of the
 *      run whose home is not cyclically within (gap, its slot] moves back
 *      into the gap, which moves on to its slot - until an empty slot ends
 *      the run (Knuth, The Art of Computer Programming 6.4, algorithm R).
 *      S1 holds after it, and no slot is ever a tombstone.
 *
 * Tested by tests/name-set-model.ps1 against a plain count of every hash.
 */

#include "NameSet.h"

unsigned int NsHash(const char *Name, int Length)
{
    unsigned int h = 2166136261u;
    int          i;

    for (i = 0; i < Length; i++) {
        unsigned char c = (unsigned char)Name[i];
        if (c >= 'a' && c <= 'z')
            c = (unsigned char)(c - 'a' + 'A');
        h = (h ^ c) * 16777619u;
    }
    return h | 1;
}

void NsInit(NS_SET *Set, unsigned int Slots)
{
    Set->Mask = Slots - 1;
    Set->Used = 0;
}

/* S1: the slot of Hash, or the empty one that ends its probe */
static unsigned int Find(const NS_SET *Set, unsigned int Hash)
{
    unsigned int i = Hash & Set->Mask;

    while (Set->Slot[i].Hash && Set->Slot[i].Hash != Hash)
        i = (i + 1) & Set->Mask;
    return i;
}

unsigned int NsCount(const NS_SET *Set, unsigned int Hash)
{
    return Set->Slot[Find(Set, Hash)].Count;
}

void NsPut(NS_SET *Set, unsigned int Hash, unsigned int Count)
{
    unsigned int i = Find(Set, Hash);

    if (Set->Slot[i].Hash == 0) {
        Set->Slot[i].Hash = Hash;
        Set->Used++;
    }
    Set->Slot[i].Count += Count;
}

int NsTake(NS_SET *Set, unsigned int Hash)
{
    unsigned int i = Find(Set, Hash), j, home;

    if (Set->Slot[i].Hash == 0 || Set->Slot[i].Count == 0)
        return 0;
    if (--Set->Slot[i].Count)
        return 1;

    /* S2 */
    Set->Used--;
    for (j = i; ; ) {
        Set->Slot[i].Hash = 0;
        Set->Slot[i].Count = 0;
        for (;;) {
            j = (j + 1) & Set->Mask;
            if (Set->Slot[j].Hash == 0)
                return 1;
            home = Set->Slot[j].Hash & Set->Mask;
            if (i <= j ? (i < home && home <= j) : (i < home || home <= j))
                continue;       /* it may stay: the gap is not on its probe */
            break;
        }
        Set->Slot[i] = Set->Slot[j];
        i = j;
    }
}

void NsCopy(NS_SET *To, const NS_SET *From)
{
    unsigned int i;

    for (i = 0; i <= From->Mask; i++)
        if (From->Slot[i].Hash)
            NsPut(To, From->Slot[i].Hash, From->Slot[i].Count);
}
