/*
 * test.c - the production name multiset (src/ext4/NameSet.c) against a
 * plain count of every hash, its rules checked after every change.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * Tables of 2 to 4096 slots kept up to half full; hashes drawn from a few
 * homes - long runs, runs that wrap past the last slot, runs that merge -
 * or from anywhere. Hashes are added, taken (also ones the set does not
 * hold: refused), and the table copied into one twice the size. After each
 * change:
 *   S1  every hash lies on its own probe: no empty slot between its home
 *       and its slot;
 *   every hash in one slot only; Used the slots taken; no count of 0;
 *   every count, of every hash ever drawn, the plain count's.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/ext4/NameSet.c"

#define DRAWN 8192

static unsigned long long Rng, Checks, Ops;
static unsigned int Hashes[DRAWN], Counts[DRAWN], Drawn;

static unsigned int Rand(void)
{
    Rng ^= Rng >> 12;
    Rng ^= Rng << 25;
    Rng ^= Rng >> 27;
    return (unsigned int)((Rng * 2685821657736338717ull) >> 32);
}

static void Fail(const char *What, const NS_SET *Set)
{
    printf("FAIL: %s (op %llu, slots %u, used %u)\n", What, Ops, Set->Mask + 1, Set->Used);
    exit(1);
}

#define CHECK(c, what) do { Checks++; if (!(c)) Fail(what, Set); } while (0)

static void CheckAll(const NS_SET *Set)
{
    unsigned int i, used = 0, d;

    for (i = 0; i <= Set->Mask; i++) {
        unsigned int h = Set->Slot[i].Hash, at;

        if (!h) {
            CHECK(Set->Slot[i].Count == 0, "an empty slot with a count");
            continue;
        }
        used++;
        CHECK(h & 1, "a hash without its low bit");
        CHECK(Set->Slot[i].Count > 0, "a hash held with no name");
        /* S1: no empty slot from its home up to it, and nowhere else */
        for (at = h & Set->Mask; at != i; at = (at + 1) & Set->Mask) {
            CHECK(Set->Slot[at].Hash != 0, "a hash off its probe (S1)");
            CHECK(Set->Slot[at].Hash != h, "a hash in two slots");
        }
    }
    CHECK(used == Set->Used, "Used unlike the slots held");
    for (d = 0; d < Drawn; d++)
        CHECK(NsCount(Set, Hashes[d]) == Counts[d], "a count unlike the plain count");
}

/* a hash: near one of a few homes (clusters, wrapping), or anywhere */
static unsigned int Draw(const NS_SET *Set, unsigned int *Index)
{
    unsigned int d;

    if (Drawn && Rand() % 3) {
        d = Rand() % Drawn;
    } else if (Drawn < DRAWN) {
        unsigned int h, k;
        do {
            if (Rand() % 4) {
                unsigned int home = Rand() % 3 == 0 ? Set->Mask : (Rand() % 4) * (Set->Mask / 4 + 1);
                h = (Rand() << 12 & ~Set->Mask) | ((home + Rand() % 3) & Set->Mask);
            } else {
                h = Rand();
            }
            h |= 1;
            for (k = 0; k < Drawn && Hashes[k] != h; k++)
                ;
        } while (k < Drawn);
        d = Drawn++;
        Hashes[d] = h;
        Counts[d] = 0;
    } else {
        d = Rand() % Drawn;
    }
    *Index = d;
    return Hashes[d];
}

static NS_SET *NewSet(unsigned int Slots)
{
    NS_SET *Set = (NS_SET *)calloc(1, (size_t)NsBytes(Slots));

    NsInit(Set, Slots);
    return Set;
}

static void Run(unsigned int Slots, unsigned int Changes, unsigned int Every)
{
    NS_SET *Set = NewSet(Slots);
    unsigned int c;

    Drawn = 0;
    for (c = 0; c < Changes; c++) {
        unsigned int d, h, r = Rand() % 100;

        Ops++;
        if (r < 2 && Set->Mask + 1 < 8192) {
            /* twice the size: the same counts */
            NS_SET *Bigger = NewSet((Set->Mask + 1) * 2);
            NsCopy(Bigger, Set);
            free(Set);
            Set = Bigger;
        } else if (r < 52) {
            h = Draw(Set, &d);
            if (Counts[d] == 0 && (Set->Used + 1) * 2 > Set->Mask + 1)
                continue;           /* at most half full, as the driver keeps it */
            {
                unsigned int add = 1 + (Rand() % 8 == 0 ? Rand() % 5 : 0);
                NsPut(Set, h, add);
                Counts[d] += add;
            }
        } else {
            int took;
            h = Draw(Set, &d);
            took = NsTake(Set, h);
            CHECK(took == (Counts[d] > 0), "a take answered unlike the count");
            if (Counts[d])
                Counts[d]--;
        }
        if (c % Every == 0)
            CheckAll(Set);
    }
    CheckAll(Set);
    free(Set);
}

int main(int argc, char **argv)
{
    unsigned long long seed = argc > 1 ? strtoull(argv[1], NULL, 10) : 1;
    unsigned int k;

    Rng = seed * 0x9E3779B97F4A7C15ull + 3;
    for (k = 0; k < 4000; k++)
        Run(2u << (k % 6), 400, 1);         /* 2..64 slots: every run wraps */
    for (k = 0; k < 40; k++)
        Run(256u << (k % 5), 20000, 16);    /* up to 4096, checked every 16 */
    printf("NAME-SET-MODEL: seed %llu: %llu changes, %llu checks\n", seed, Ops, Checks);
    return 0;
}
