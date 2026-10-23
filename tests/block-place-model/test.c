/*
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * The production placement (src\ext4\BlockPlacement.c over the group search
 * of src\ext4\BlockSearch.h) on simulated volumes.
 *
 * Part 1, the rules: random volumes - groups of any density, some not seen
 * yet (bound = free count), blocks freed but not committed, a short last
 * group, other files' windows - and random requests. Every answer must be
 * the reference's (P1 around other windows, else the first size at which
 * any group has a run outside them - at the sizes up to Need, failing
 * that, any run, taken from its end - the first such group from the
 * goal's, its best fit), the run free and no longer than Need, every
 * group's longest-run bound exact after a take, the windows after it the
 * reference's, T2: with no run of Need anywhere, the run taken is longer
 * than half the longest, and T4: blocks in another file's window only
 * when no run as long lies outside the windows.
 *
 * Part 2, aging: a 2 GiB volume of 4 KiB blocks aged by the workload of
 * tests\aging-vm.ps1, then streams, copies, parallel streams, small files
 * and two files written a block at a time in turn onto it - with the
 * placement before this one (the first run of Need from the goal on, else
 * the first run of any length), with the production placement keeping no
 * windows, and with it as it is: the files' fragments, and how many small
 * files lie right after the one written before them. Then T1: a file grown
 * at its end has at most 2 + log2(final / first) fragments, alone and with
 * another file written between its writes.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXT2_FREED_SLOTS 2
#include "../../src/ext4/BlockSearch.h"
#include "../../src/ext4/BlockPlacement.h"

#define MAX_GROUPS      16
#define MAX_BITS        32768u
#define WORDS           (MAX_BITS / 64)
#define RULE_CASES      100000u

/* xorshift64*: deterministic, seedable */
static ULONG64 Seed = 0x9E3779B97F4A7C15ULL;
static ULONG64 Next(void)
{
    Seed ^= Seed >> 12;
    Seed ^= Seed << 25;
    Seed ^= Seed >> 27;
    return Seed * 0x2545F4914F6CDD1DULL;
}
static ULONG Below(ULONG n) { return (ULONG)(Next() % n); }

/* ---------------------------------------------------------------- the volume */

typedef struct VOLUME {
    ULONG   Groups, GroupBits, LastBits;
    ULONG64 Disk[MAX_GROUPS][WORDS];
    ULONG64 Freed[MAX_GROUPS][WORDS];
    int     HasFreed[MAX_GROUPS];
    LONG    Longest[MAX_GROUPS];        /* -1: not seen */
    ULONG   FreeCount[MAX_GROUPS];
    int     Journaled;                  /* the request may take freed blocks */
    EXT2_WINDOWS Windows;
    ULONG   Owner;                      /* the file the request is for */
    int     NoWindows;                  /* the placement before windows: none kept */
    ULONG   Takes;
} VOLUME;

static VOLUME Vol, Before;

static ULONG Bits(const VOLUME *V, ULONG g) { return g + 1 == V->Groups ? V->LastBits : V->GroupBits; }

static void DiskView(const VOLUME *V, ULONG g, EXT2_BLOCK_VIEW *View)
{
    memset(View, 0, sizeof(*View));
    View->Disk = V->Disk[g];
    View->Bits = Bits(V, g);
}

static void TakeView(const VOLUME *V, ULONG g, EXT2_BLOCK_VIEW *View)
{
    DiskView(V, g, View);
    if (V->HasFreed[g] && !V->Journaled)
        View->Freed[0] = V->Freed[g];
}

static int Taken(const EXT2_BLOCK_VIEW *View, ULONG i)
{
    ULONG c;
    if (i >= View->Bits) return 1;
    if ((View->Disk[i / 64] >> (i % 64)) & 1) return 1;
    if (View->Freed[0] && ((View->Freed[0][i / 64] >> (i % 64)) & 1)) return 1;
    for (c = 0; c < View->CutCount; c++)
        if (i >= View->Cuts[c].Start && i - View->Cuts[c].Start < View->Cuts[c].Count) return 1;
    return 0;
}

static ULONG RefRunAt(const EXT2_BLOCK_VIEW *View, ULONG From)
{
    ULONG End = From;
    while (!Taken(View, End)) End++;
    return End - From;
}

static ULONG RefLongest(const EXT2_BLOCK_VIEW *View)
{
    ULONG Longest = 0, p = 0;
    while (p < View->Bits) {
        ULONG n = Taken(View, p) ? 0 : RefRunAt(View, p);
        if (n > Longest) Longest = n;
        p += n ? n : 1;
    }
    return Longest;
}

static ULONG CountFree(const VOLUME *V, ULONG g)
{
    EXT2_BLOCK_VIEW View;
    ULONG i, n = 0;
    DiskView(V, g, &View);
    for (i = 0; i < View.Bits; i++) n += !Taken(&View, i);
    return n;
}

static void SetRun(VOLUME *V, ULONG g, ULONG Index, ULONG Count, int Set)
{
    ULONG i;
    EXT2_BLOCK_VIEW View;
    for (i = Index; i < Index + Count; i++) {
        if (Set) V->Disk[g][i / 64] |= 1ULL << (i % 64);
        else     V->Disk[g][i / 64] &= ~(1ULL << (i % 64));
    }
    V->FreeCount[g] = Set ? V->FreeCount[g] - Count : V->FreeCount[g] + Count;
    DiskView(V, g, &View);
    V->Longest[g] = (LONG)Ext2ViewLongestRun(&View);
}


/* the driver's Bound and Take (BlockAllocator.c), over the simulation */
static unsigned int ModelBound(void *Context, unsigned int g)
{
    VOLUME *V = Context;
    return V->Longest[g] < 0 ? V->FreeCount[g] : (unsigned int)V->Longest[g];
}

static int ModelTake(void *Context, const EXT2_PLACE_TAKE *Ask, unsigned int *Index, unsigned int *Count)
{
    VOLUME         *V = Context;
    EXT2_BLOCK_VIEW View;
    EXT2_BLOCK_CUT  Cuts[EXT2_WINDOW_SLOTS], After;
    ULONG           g = Ask->Group, Length = 0, At = 0, n;

    V->Takes++;
    if (V->FreeCount[g] == 0) {
        V->Longest[g] = 0;
        return EXT2_PLACE_NONE;
    }
    if (V->Longest[g] < 0) {
        DiskView(V, g, &View);
        V->Longest[g] = (LONG)Ext2ViewLongestRun(&View);
    }
    n = Ext2WindowCuts(&V->Windows, g, V->Owner, Cuts);
    if (Ask->Any && n == 0)
        return EXT2_PLACE_NONE;
    TakeView(V, g, &View);
    if (!Ask->Any) {
        View.Cuts = Cuts;
        View.CutCount = n;
    }
    if (Ask->Want == 0) {
        At = Ask->Hint;
        Length = Ext2ViewRunAt(&View, At);
    } else if (!Ext2ViewBestRun(&View, Ask->Hint, Ask->Want, &At, &Length)) {
        Length = 0;
    }
    if (Length == 0)
        return EXT2_PLACE_NONE;

    *Count = Ask->Need < Length ? Ask->Need : Length;
    *Index = Ask->Any ? At + Length - *Count : At;
    SetRun(V, g, *Index, *Count, 1);
    Ext2WindowTaken(&V->Windows, g, *Index, *Count);
    if (Ask->Room != 0 && !V->NoWindows) {
        After.Start = *Index + *Count;
        After.Count = Ext2ViewRunAt(&View, After.Start);
        if (After.Count > Ask->Room) After.Count = Ask->Room;
        Ext2WindowSet(&V->Windows, V->Owner, g, After);
    }
    return 0;
}

static int Place(VOLUME *V, ULONG Owner, ULONG Group, ULONG Hint, ULONG Need, ULONG Room, int AtGoal,
                 unsigned int *g, unsigned int *Index, unsigned int *Count)
{
    EXT2_PLACE_IO   Io = { V, V->Groups, V->GroupBits, ModelBound, ModelTake };
    EXT2_PLACE      Request = { Group, Hint, Need, Room, AtGoal };

    V->Owner = Owner;
    return Ext2PlaceBlocks(&Io, &Request, g, Index, Count);
}

static void CloseFile(VOLUME *V, ULONG Owner)
{
    EXT2_BLOCK_CUT None = { 0, 0 };
    Ext2WindowSet(&V->Windows, Owner, 0, None);
}

/* ---------------------------------------------------------------- part 1 */

static ULONG Failed;

static void Fail(ULONG Case, const char *What, ULONG a, ULONG b, ULONG c, ULONG d)
{
    if (Failed++ < 10)
        printf("FAIL case %lu %s: %lu %lu / %lu %lu\n", Case, What, a, b, c, d);
}

/* the reference's windows: a plain list, changed by its own rules */
typedef struct REFWIN { ULONG Owner, Group, Start, Count; } REFWIN;
#define REF_WINDOWS 10

/* the windows of Group not Owner's, as cuts */
static ULONG RefCuts(const REFWIN *W, ULONG nW, ULONG Group, ULONG Owner, EXT2_BLOCK_CUT *Cuts)
{
    ULONG i, n = 0;
    for (i = 0; i < nW; i++)
        if (W[i].Group == Group && W[i].Owner != Owner) {
            Cuts[n].Start = W[i].Start;
            Cuts[n].Count = W[i].Count;
            n++;
        }
    return n;
}

/* the reference answer on the volume before: the shortest run of at least
   Want (ties: first start from Hint, wrapping), by bits, Cuts taken */
static int RefBest(const VOLUME *V, ULONG g, ULONG Hint, ULONG Want, const EXT2_BLOCK_CUT *Cuts, ULONG nCuts,
                   ULONG *Index, ULONG *Length)
{
    EXT2_BLOCK_VIEW View;
    ULONG           Best = 0, BestKey = 0, p = 0;

    TakeView(V, g, &View);
    View.Cuts = Cuts;
    View.CutCount = nCuts;
    if (Hint >= View.Bits) Hint = 0;
    while (p < View.Bits) {
        ULONG n, Key;
        if (Taken(&View, p)) { p++; continue; }
        n = RefRunAt(&View, p);
        Key = p >= Hint ? p - Hint : p + View.Bits;
        if (n >= Want && (Best == 0 || n < Best || (n == Best && Key < BestKey))) {
            Best = n; BestKey = Key; *Index = p;
        }
        p += n;
    }
    *Length = Best;
    return Best != 0;
}

/* the windows after Owner took [Index, Index + Count) of g with Room, the
   free run after the blocks being Rest long */
static void RefWindowsAfter(REFWIN *W, ULONG *nW, ULONG Owner, ULONG g, ULONG Index, ULONG Count,
                            ULONG Room, ULONG Rest)
{
    ULONG i, End = Index + Count;

    for (i = 0; i < *nW; ) {
        ULONG Last = W[i].Start + W[i].Count;
        if (W[i].Group == g && W[i].Start < End && Last > Index) {
            if (W[i].Start < Index)  W[i].Count = Index - W[i].Start;
            else if (Last > End)     { W[i].Start = End; W[i].Count = Last - End; }
            else                     { W[i] = W[--*nW]; continue; }
        }
        i++;
    }
    if (Room == 0)
        return;
    for (i = 0; i < *nW; i++)
        if (W[i].Owner == Owner) { W[i] = W[--*nW]; break; }
    if (Rest > Room) Rest = Room;
    if (Rest) {
        W[*nW].Owner = Owner; W[*nW].Group = g; W[*nW].Start = End; W[*nW].Count = Rest;
        ++*nW;
    }
}

static int RefPlace(const VOLUME *V, REFWIN *W, ULONG *nW, ULONG Owner, ULONG Group, ULONG Hint,
                    ULONG Need, ULONG Room, int AtGoal, ULONG *g, ULONG *Index, ULONG *Count)
{
    EXT2_BLOCK_VIEW View;
    EXT2_BLOCK_CUT  Cuts[REF_WINDOWS];
    ULONG           Want, i, Length, n, Any;
    ULONG64         Total = (ULONG64)Need + Room;

    if (AtGoal) {
        TakeView(V, Group, &View);
        View.Cuts = Cuts;
        View.CutCount = RefCuts(W, *nW, Group, Owner, Cuts);
        Length = Hint < View.Bits ? RefRunAt(&View, Hint) : 0;
        if (Length) {
            *g = Group; *Index = Hint; *Count = Need < Length ? Need : Length;
            RefWindowsAfter(W, nW, Owner, *g, *Index, *Count, Room,
                            *Index + *Count < View.Bits ? RefRunAt(&View, *Index + *Count) : 0);
            return 0;
        }
    }
    Want = Total < V->GroupBits ? (ULONG)Total : V->GroupBits;
    for (;;) {
        for (Any = 0; Any < (Want <= Need ? 2u : 1u); Any++) {
            for (i = 0; i < V->Groups; i++) {
                ULONG h = (Group + i) % V->Groups;
                n = RefCuts(W, *nW, h, Owner, Cuts);
                if (Any && n == 0) continue;
                if (RefBest(V, h, i == 0 ? Hint : 0, Want, Cuts, Any ? 0 : n, Index, &Length)) {
                    *g = h; *Count = Need < Length ? Need : Length;
                    if (Any) *Index += Length - *Count;
                    TakeView(V, h, &View);
                    View.Cuts = Cuts;
                    View.CutCount = Any ? 0 : n;
                    RefWindowsAfter(W, nW, Owner, h, *Index, *Count, Room,
                                    *Index + *Count < View.Bits ? RefRunAt(&View, *Index + *Count) : 0);
                    return 0;
                }
            }
        }
        if (Want == 1) return EXT2_PLACE_NONE;
        Want = Want > Need ? Need : Want - Want / 2;
    }
}

/* the production table's windows as the reference's, by owner */
static int SameWindows(const EXT2_WINDOWS *T, const REFWIN *W, ULONG nW)
{
    ULONG i, j, Live = 0;
    for (i = 0; i < EXT2_WINDOW_SLOTS; i++) {
        const EXT2_WINDOW *s = &T->Slot[i];
        if (s->Blocks.Count == 0) continue;
        Live++;
        for (j = 0; j < nW; j++)
            if (W[j].Owner == s->Owner) break;
        if (j == nW || W[j].Group != s->Group || W[j].Start != s->Blocks.Start || W[j].Count != s->Blocks.Count)
            return 0;
    }
    return Live == nW && T->Live == nW;
}

static void Fill(ULONG64 *Map, ULONG n, ULONG Density)
{
    ULONG i = 0;
    memset(Map, 0, WORDS * sizeof(ULONG64));
    while (i < n) {
        ULONG Run = 1 + Below(Below(4) == 0 ? 512 : 16);
        int   Set = Below(256) < Density;
        ULONG j;
        for (j = 0; j < Run && i < n; j++, i++)
            if (Set) Map[i / 64] |= 1ULL << (i % 64);
    }
    if (n % 64)
        Map[n / 64] |= Next() & (~0ULL << (n % 64));
}

static void Rules(void)
{
    ULONG Case, Found = 0, Adjacent = 0, Relaxed = 0, Windowed = 0, Into = 0;

    for (Case = 0; Case < RULE_CASES; Case++) {
        ULONG g, Need, Room, Group, Hint, Longest = 0, LongestAll = 0, Owner, nW = 0, k;
        ULONG rg = 0, ri = 0, rc = 0;
        unsigned int pg = 0, pi = 0, pc = 0;
        REFWIN W[REF_WINDOWS], Wb[REF_WINDOWS];
        ULONG nWb;
        int adj, Prod, Ref;

        Vol.Groups = 1 + Below(Case % 32 == 0 ? 4 : 12);
        Vol.GroupBits = Case % 32 == 0 ? MAX_BITS : 64 * (1 + Below(64));
        Vol.LastBits = 1 + Below(Vol.GroupBits);
        for (g = 0; g < Vol.Groups; g++) {
            EXT2_BLOCK_VIEW View;
            Fill(Vol.Disk[g], Bits(&Vol, g), Below(4) == 0 ? 256 : Below(257));
            Vol.HasFreed[g] = Below(3) == 0;
            Fill(Vol.Freed[g], Bits(&Vol, g), Below(64));
            Vol.FreeCount[g] = CountFree(&Vol, g);
            DiskView(&Vol, g, &View);
            Vol.Longest[g] = Below(3) == 0 ? -1 : (LONG)RefLongest(&View);
        }
        Vol.Journaled = Below(4) == 0;
        Group = Below(Vol.Groups);
        Hint = Below(Bits(&Vol, Group));
        Need = 1 + Below(Below(4) == 0 ? 4096 : 64);
        Room = Below(3) == 0 ? 0 : Below(8192);
        adj = (int)Below(2);
        Owner = 1 + Below(REF_WINDOWS);

        /* other files' windows - and maybe the owner's - anywhere, some at
           the goal; the table and the reference's list alike */
        memset(&Vol.Windows, 0, sizeof(Vol.Windows));
        if (Below(3) != 0) {
            ULONG Many = 1 + Below(REF_WINDOWS);
            for (k = 0; k < Many; k++) {
                EXT2_BLOCK_CUT w;
                ULONG o = 1 + Below(REF_WINDOWS), j, gw = Below(Vol.Groups), b = Bits(&Vol, gw);
                w.Start = gw == Group && Below(4) == 0 ? Hint : Below(b);
                w.Count = 1 + Below(Below(2) ? 64 : b - w.Start);
                if (w.Count > b - w.Start) w.Count = b - w.Start;
                Ext2WindowSet(&Vol.Windows, o, gw, w);
                for (j = 0; j < nW && W[j].Owner != o; j++) {}
                W[j].Owner = o; W[j].Group = gw; W[j].Start = w.Start; W[j].Count = w.Count;
                if (j == nW) nW++;
            }
        }
        memcpy(&Before, &Vol, sizeof(Vol));
        memcpy(Wb, W, sizeof(W));
        nWb = nW;

        Prod = Place(&Vol, Owner, Group, Hint, Need, Room, adj, &pg, &pi, &pc);
        Ref = RefPlace(&Before, W, &nW, Owner, Group, Hint, Need, Room, adj, &rg, &ri, &rc);
        if ((Prod == 0) != (Ref == 0) || (Prod == 0 && (pg != rg || pi != ri || pc != rc))) {
            Fail(Case, "placement differs (group index count)", pg, pi, rg, ri);
            continue;
        }
        if (!SameWindows(&Vol.Windows, W, nW))
            Fail(Case, "windows differ", Vol.Windows.Live, nW, Owner, Room);
        if (Prod != 0)
            continue;
        Found++;
        Windowed += nWb != 0;

        /* T4: in another file's window only when no run as long lies outside them */
        {
            EXT2_BLOCK_CUT Cuts[REF_WINDOWS];
            ULONG Outside = 0, n;
            int Overlap = 0;
            for (k = 0; k < nWb; k++)
                if (Wb[k].Owner != Owner && Wb[k].Group == pg && Wb[k].Start < pi + pc && Wb[k].Start + Wb[k].Count > pi)
                    Overlap = 1;
            if (Overlap) {
                Into++;
                for (g = 0; g < Before.Groups; g++) {
                    EXT2_BLOCK_VIEW View;
                    TakeView(&Before, g, &View);
                    n = RefCuts(Wb, nWb, g, Owner, Cuts);
                    View.Cuts = Cuts;
                    View.CutCount = n;
                    Longest = RefLongest(&View);
                    if (Longest > Outside) Outside = Longest;
                }
                if (Outside >= pc) Fail(Case, "T4: into a window with a run outside", pc, Outside, pg, pi);
            }
        }

        /* free before, inside the group, no more than Need */
        {
            EXT2_BLOCK_VIEW View;
            ULONG i;
            TakeView(&Before, pg, &View);
            if (pc == 0 || pc > Need || pi + pc > View.Bits) Fail(Case, "run outside", pi, pc, Need, View.Bits);
            for (i = pi; i < pi + pc; i++)
                if (Taken(&View, i)) { Fail(Case, "took a taken block", pg, i, 0, 0); break; }
        }

        /* the bounds stay exact */
        for (g = 0; g < Vol.Groups; g++) {
            EXT2_BLOCK_VIEW View;
            DiskView(&Vol, g, &View);
            if (Vol.Longest[g] >= 0 && (ULONG)Vol.Longest[g] != RefLongest(&View))
                Fail(Case, "bound not exact", g, Vol.Longest[g], RefLongest(&View), 0);
            if (Vol.FreeCount[g] != CountFree(&Vol, g))
                Fail(Case, "free count", g, Vol.FreeCount[g], CountFree(&Vol, g), 0);
        }

        /* T2 */
        for (g = 0; g < Before.Groups; g++) {
            EXT2_BLOCK_VIEW View;
            TakeView(&Before, g, &View);
            Longest = RefLongest(&View);
            if (Longest > LongestAll) LongestAll = Longest;
        }
        if (adj && pg == Group && pi == Hint) {
            Adjacent++;
        } else if (LongestAll < Need) {
            Relaxed++;
            if (2 * pc <= LongestAll) Fail(Case, "T2: run not over half the longest", pc, LongestAll, 0, 0);
        }
    }
    printf("rules: %lu requests, %lu placed (%lu after the owner, %lu below Need, %lu among windows,"
           " %lu into one), %lu failed\n", RULE_CASES, Found, Adjacent, Relaxed, Windowed, Into, Failed);
}

/* ---------------------------------------------------------------- part 2 */

typedef struct RUN { ULONG64 Start; ULONG Length; } RUN;
typedef struct FILE_ { RUN *Runs; ULONG Count, Capacity; ULONG64 Blocks; ULONG Home, Owner; int Live; } FILE_;

#define AGE_GROUPS      16
#define AGE_BITS        32768u          /* 128 MiB groups of 4 KiB blocks */
#define AGE_FILES       20000
#define CHUNK           64              /* 256 KiB writes */
#define EXTENT_MAX      32768u

static FILE_ Files[AGE_FILES];
static int   OldPolicy;

static void AddRun(FILE_ *f, ULONG64 Start, ULONG Length)
{
    if (f->Count && f->Runs[f->Count - 1].Start + f->Runs[f->Count - 1].Length == Start) {
        f->Runs[f->Count - 1].Length += Length;
    } else {
        if (f->Count == f->Capacity) {
            f->Capacity = f->Capacity ? 2 * f->Capacity : 4;
            f->Runs = realloc(f->Runs, f->Capacity * sizeof(RUN));
        }
        f->Runs[f->Count].Start = Start;
        f->Runs[f->Count].Length = Length;
        f->Count++;
    }
    f->Blocks += Length;
}

/* the placement before this one: in each group from the goal's, the first
   run of Need from the hint on, then from the start; failing that the first
   run of any length from the hint on */
static int OldPlace(VOLUME *V, ULONG Group, ULONG Hint, ULONG Need, unsigned int *g, unsigned int *Index, unsigned int *Count)
{
    ULONG i;
    for (i = 0; i < V->Groups; i++) {
        ULONG h = (Group + i) % V->Groups, Pass, AnyIndex = 0, AnyLength = 0, Start, End;
        EXT2_BLOCK_VIEW View;
        if (i) Hint = 0;
        if (V->FreeCount[h] == 0) continue;
        TakeView(V, h, &View);
        if (Hint >= View.Bits) Hint = 0;
        for (Pass = 0; Pass < 2; Pass++) {
            ULONG From = Pass ? 0 : Hint, To = Pass ? Hint : View.Bits;
            for (Start = Ext2ViewNext(&View, From, TRUE); Start < To; Start = Ext2ViewNext(&View, End, TRUE)) {
                End = Ext2ViewNext(&View, Start + 1, FALSE);
                if (AnyLength == 0) { AnyIndex = Start; AnyLength = End - Start; }
                if (End - Start >= Need) {
                    *g = h; *Index = Start; *Count = Need;
                    SetRun(V, h, Start, Need, 1);
                    return 0;
                }
            }
        }
        if (AnyLength) {
            *g = h; *Index = AnyIndex; *Count = Need < AnyLength ? Need : AnyLength;
            SetRun(V, h, AnyIndex, *Count, 1);
            return 0;
        }
    }
    return EXT2_PLACE_NONE;
}

/* Need blocks more for f at its end, Room its size so far when it streams */
static int Grow(FILE_ *f, ULONG64 Need, int Stream)
{
    while (Need) {
        ULONG64 Goal = f->Count ? f->Runs[f->Count - 1].Start + f->Runs[f->Count - 1].Length : 0;
        ULONG   Group = f->Home, Hint = 0, Ask = (ULONG)(Need < EXTENT_MAX ? Need : EXTENT_MAX);
        unsigned int g, Index, Count;
        int     Adjacent = 0, err;

        if (f->Count && Goal < (ULONG64)Vol.Groups * AGE_BITS) {
            Group = (ULONG)(Goal / AGE_BITS);
            Hint = (ULONG)(Goal % AGE_BITS);
            Adjacent = 1;
        }
        err = OldPolicy ? OldPlace(&Vol, Group, Hint, Ask, &g, &Index, &Count)
                        : Place(&Vol, f->Owner, Group, Hint, Ask, Stream ? (ULONG)min(f->Blocks, 0xFFFFFFFFull) : 0,
                                Adjacent, &g, &Index, &Count);
        if (err) return err;
        AddRun(f, (ULONG64)g * AGE_BITS + Index, Count);
        Need -= Count;
    }
    return 0;
}

/* the last handle closed: its window goes */
static void Close(FILE_ *f)
{
    CloseFile(&Vol, f->Owner);
}

static void Delete(FILE_ *f)
{
    ULONG i;
    Close(f);
    for (i = 0; i < f->Count; i++)
        SetRun(&Vol, (ULONG)(f->Runs[i].Start / AGE_BITS), (ULONG)(f->Runs[i].Start % AGE_BITS), f->Runs[i].Length, 0);
    free(f->Runs);
    memset(f, 0, sizeof(*f));
}

/* file i of the aging run, new */
static FILE_ *New(int i, ULONG Home)
{
    Files[i].Home = Home;
    Files[i].Owner = (ULONG)i + 1;
    return &Files[i];
}

/* the aging workload of aging-vm/guest.ps1, in blocks */
static ULONG64 AgeSize(void)
{
    double u = (double)Below(1u << 30) / (1u << 30), lo, hi, x;
    if (u < 0.60)      { lo = 4096;          hi = 256.0 * 1024; }
    else if (u < 0.95) { lo = 256.0 * 1024;  hi = 8.0 * 1048576; }
    else               { lo = 8.0 * 1048576; hi = 64.0 * 1048576; }
    x = (double)Below(1u << 30) / (1u << 30);
    return (ULONG64)((exp(log(lo) + x * (log(hi) - log(lo))) + 4095) / 4096);
}

static void Report(const char *Name, const char *Phase, int First, int Count)
{
    ULONG n = 0, Worst = 0;
    int   i;

    for (i = First; i < First + Count; i++) {
        n += Files[i].Count;
        if (Files[i].Count > Worst) Worst = Files[i].Count;
    }
    printf("  %-10s %-9s %5lu fragments, %.2f a file (most %lu)\n", Name, Phase, n, (double)n / Count, Worst);
}

/* Policy 0: the placement before; 1: the production one keeping no windows; 2: as it is */
static void Age(const char *Name, int Policy)
{
    ULONG64 Capacity = (ULONG64)AGE_GROUPS * AGE_BITS, Used = 0, Written = 0;
    int     Live[AGE_FILES], LiveCount = 0, Next_ = 0, i, m;
    ULONG   g;

    memset(&Vol, 0, sizeof(Vol));
    OldPolicy = Policy == 0;
    Vol.NoWindows = Policy == 1;
    Vol.Groups = AGE_GROUPS;
    Vol.GroupBits = Vol.LastBits = AGE_BITS;
    for (g = 0; g < AGE_GROUPS; g++) {
        /* the group's own metadata at its start: bitmaps and inode table
           in group 0 with flex_bg, as mkfs puts them */
        Vol.FreeCount[g] = AGE_BITS;
        Vol.Longest[g] = AGE_BITS;
        if (g == 0) SetRun(&Vol, 0, 0, 1024, 1);
    }
    Seed = 0x2545F4914F6CDD1DULL;
    while (Written < 3 * Capacity) {
        int     create = LiveCount == 0 || Used < Capacity * 55 / 100 || (Used <= Capacity * 80 / 100 && Below(2) == 0);
        ULONG64 Size = AgeSize();
        int     SizeFirst = Below(3) == 0;
        if (create && Used + Size > Capacity * 80 / 100 + 16384) create = 0;
        if (create) {
            FILE_ *f = New(Next_, Below(16) % AGE_GROUPS);
            f->Live = 1;
            if (SizeFirst) Grow(f, Size, 0);
            else for (ULONG64 left = Size; left; ) { ULONG64 n = left < CHUNK ? left : CHUNK; Grow(f, n, 1); left -= n; }
            Close(f);
            Live[LiveCount++] = Next_++;
            Used += Size;
            Written += Size;
        } else {
            int v = (int)Below(LiveCount);
            Used -= Files[Live[v]].Blocks;
            Delete(&Files[Live[v]]);
            Live[v] = Live[--LiveCount];
        }
    }
    while (Used > Capacity * 55 / 100) {
        int v = (int)Below(LiveCount);
        Used -= Files[Live[v]].Blocks;
        Delete(&Files[Live[v]]);
        Live[v] = Live[--LiveCount];
    }
    {
        ULONG n = 0, big = 0, bigRuns = 0;
        for (i = 0; i < LiveCount; i++) {
            n += Files[Live[i]].Count;
            if (Files[Live[i]].Blocks >= 256) { big++; bigRuns += Files[Live[i]].Count; }
        }
        printf("  %-10s aged: %d files, %lu fragments, files of 1 MiB or more %.2f each\n",
               Name, LiveCount, n, big ? (double)bigRuns / big : 0.0);
    }

    /* the measured phases, files from Next_ on */
    m = Next_;
    for (i = 0; i < 4; i++) {
        FILE_ *f = New(Next_++, 0);
        for (ULONG k = 0; k < 8192; k += 256) Grow(f, 256, 1);
        Close(f);
    }
    Report(Name, "stream", m, 4);
    m = Next_;
    for (i = 0; i < 4; i++) { FILE_ *f = New(Next_++, 0); Grow(f, 8192, 0); Close(f); }
    Report(Name, "copy", m, 4);
    m = Next_;
    for (i = 0; i < 4; i++) New(m + i, 0);
    for (ULONG k = 0; k < 8192; k += 256)
        for (i = 0; i < 4; i++) Grow(&Files[m + i], 256, 1);
    for (i = 0; i < 4; i++) Close(&Files[m + i]);
    Next_ += 4;
    Report(Name, "parallel", m, 4);
    m = Next_;
    for (i = 0; i < 2000; i++) { FILE_ *f = New(Next_++, 0); Grow(f, 4, 1); Close(f); }
    Report(Name, "small", m, 2000);
    {
        ULONG Next = 0;
        for (i = m; i + 1 < m + 2000; i++)
            Next += Files[i + 1].Runs[0].Start == Files[i].Runs[Files[i].Count - 1].Start + Files[i].Runs[Files[i].Count - 1].Length;
        printf("  %-10s small     %5lu of 1999 right after the file before\n", Name, Next);
    }
    m = Next_;
    for (i = 0; i < 2; i++) New(m + i, 0);
    for (ULONG k = 0; k < 6000; k++)
        for (i = 0; i < 2; i++) Grow(&Files[m + i], 1, 1);
    for (i = 0; i < 2; i++) Close(&Files[m + i]);
    Next_ += 2;
    Report(Name, "lockstep", m, 2);

    for (i = 0; i < Next_; i++) if (Files[i].Runs) Delete(&Files[i]);
}

/* T1: a file grown by Chunk at its end, on a volume whose first half is
   cut into short runs and whose second half is free, at most
   2 + log2(blocks / Chunk) fragments (a first run of at least Chunk, then
   at least a doubling between two jumps) - alone (Mode 0), and with another
   file grown the same way between its writes (Mode 1): both files are
   bounded, each keeping its window. Mode 2: blocks taken right after its
   end behind the placement's back, as by a full volume's last resort, every
   eighth write: each costs at most one jump more - reported, not bounded. */
static void Doubling(void)
{
    static const char *Modes[] = { "alone", "+ another file written in turn", "+ the next block taken" };
    ULONG   g, Chunk, Mode;

    for (Mode = 0; Mode < 3; Mode++) {
        for (Chunk = 1; Chunk <= 256; Chunk *= 4) {
            FILE_   f[2];
            ULONG   Takes = 0, i;
            double  Bound;

            memset(&Vol, 0, sizeof(Vol));
            memset(f, 0, sizeof(f));
            f[0].Owner = 1;
            f[1].Owner = 2;
            Vol.Groups = AGE_GROUPS;
            Vol.GroupBits = Vol.LastBits = AGE_BITS;
            for (g = 0; g < AGE_GROUPS; g++) {
                if (g < AGE_GROUPS / 2) Fill(Vol.Disk[g], AGE_BITS, 128);
                Vol.FreeCount[g] = CountFree(&Vol, g);
                Vol.Longest[g] = -1;
            }
            while (f[0].Blocks < 16384) {
                if (Mode == 2 && f[0].Count && Below(8) == 0) {
                    ULONG64 End = f[0].Runs[f[0].Count - 1].Start + f[0].Runs[f[0].Count - 1].Length;
                    EXT2_BLOCK_VIEW View;
                    g = (ULONG)(End / AGE_BITS);
                    if (g < AGE_GROUPS) {
                        TakeView(&Vol, g, &View);
                        if (!Taken(&View, (ULONG)(End % AGE_BITS))) {
                            SetRun(&Vol, g, (ULONG)(End % AGE_BITS), 1, 1);
                            Ext2WindowTaken(&Vol.Windows, g, (ULONG)(End % AGE_BITS), 1);
                            Takes++;
                        }
                    }
                }
                Grow(&f[0], Chunk, 1);
                if (Mode == 1) Grow(&f[1], Chunk, 1);
            }
            Bound = 2 + log((double)f[0].Blocks / Chunk) / log(2.0);
            printf("  T1 %-30s %3lu-block writes to %llu blocks: %4lu", Modes[Mode], Chunk, f[0].Blocks, f[0].Count);
            if (Mode == 1) printf(" and %lu", f[1].Count);
            if (Mode == 2) printf(" fragments, %lu taken\n", Takes);
            else printf(" fragments (bound %.1f)\n", Bound);
            for (i = 0; i < (Mode == 1 ? 2u : 1u); i++)
                if (f[i].Count > Bound + Takes) Fail(Chunk, "T1: more fragments than the bound", f[i].Count, (ULONG)Bound, Takes, Mode);
            free(f[0].Runs);
            free(f[1].Runs);
        }
    }
}

int main(void)
{
    Rules();
    printf("aging (2 GiB, 4 KiB blocks):\n");
    Age("before", 0);
    Age("no window", 1);
    Age("now", 2);
    OldPolicy = 0;
    Doubling();
    printf(Failed ? "BLOCK-PLACE-MODEL: FAILED\n" : "BLOCK-PLACE-MODEL: ALL PASSED\n");
    return Failed ? 1 : 0;
}
