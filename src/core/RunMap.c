/* SPDX-License-Identifier: GPL-2.0-only
 * RunMap.c - checked physical run mappings and dirty-range tracking.
 * Copyright (c) 2026 Marek Wesolowski
 * 64-bit run cache and dirty-range tracking over the shared red-black tree.
 */
#include "ext4fs.h"

#define RUN_MAP_TAG '64RE'
typedef struct _EXT4_RUN {
    struct rb_node Node;
    LONGLONG Start, Physical, Length;
} EXT4_RUN;

static EXT4_RUN *Run(struct rb_node *Node)
{
    return Node ? rb_entry(Node, EXT4_RUN, Node) : NULL;
}

static EXT4_RUN *Floor(PEXT4_RUN_MAP Map, LONGLONG Start)
{
    struct rb_node *Node = Map->Root.rb_node;
    EXT4_RUN *Found = NULL;
    while (Node) {
        EXT4_RUN *Each = Run(Node);
        if (Each->Start > Start) {
            Node = Node->rb_left;
        } else {
            Found = Each;
            Node = Node->rb_right;
        }
    }
    return Found;
}

static EXT4_RUN *First(PEXT4_RUN_MAP Map, LONGLONG Start)
{
    EXT4_RUN *Each = Floor(Map, Start);
    if (!Each)
        return Run(rb_first(&Map->Root));
    return Each->Start + Each->Length >= Start ? Each : Run(rb_next(&Each->Node));
}

static VOID Insert(PEXT4_RUN_MAP Map, EXT4_RUN *Each)
{
    struct rb_node **Link = &Map->Root.rb_node;
    struct rb_node *Parent = NULL;
    while (*Link) {
        Parent = *Link;
        Link = Each->Start < Run(Parent)->Start ? &Parent->rb_left : &Parent->rb_right;
    }
    rb_link_node(&Each->Node, Parent, Link);
    rb_insert_color(&Each->Node, &Map->Root);
    Map->Count++;
}

VOID Ext4RunMapInitialize(PEXT4_RUN_MAP Map)
{
    KeInitializeGuardedMutex(&Map->Mutex);
    Map->Root.rb_node = NULL;
    Map->Cursor = NULL;
    Map->Count = 0;
    Map->Initialized = TRUE;
}

VOID Ext4RunMapClear(PEXT4_RUN_MAP Map)
{
    struct rb_node *Node;
    KeAcquireGuardedMutex(&Map->Mutex);
    Map->Cursor = NULL;
    while ((Node = rb_first(&Map->Root)) != NULL) {
        rb_erase(Node, &Map->Root);
        Ext2FreePool(Run(Node), RUN_MAP_TAG);
    }
    Map->Count = 0;
    KeReleaseGuardedMutex(&Map->Mutex);
}

VOID Ext4RunMapDestroy(PEXT4_RUN_MAP Map)
{
    if (Map->Initialized) {
        Ext4RunMapClear(Map);
        Map->Initialized = FALSE;
    }
}

ULONG Ext4RunMapCount(PEXT4_RUN_MAP Map)
{
    ULONG Count;
    KeAcquireGuardedMutex(&Map->Mutex);
    Count = Map->Count;
    KeReleaseGuardedMutex(&Map->Mutex);
    return Count;
}

BOOLEAN Ext4RunMapAdd(PEXT4_RUN_MAP Map, LONGLONG Start, LONGLONG Physical, LONGLONG Length)
{
    EXT4_RUN *Each, *Keep = NULL, *Next;
    LONGLONG End, Begin = Start, Bias;
    BOOLEAN Result = FALSE;
    if (Start < 0 || Physical <= 0 || Length <= 0 ||
        Start > MAXLONGLONG - Length || Physical > MAXLONGLONG - Length)
        return FALSE;
    End = Start + Length;
    Bias = Physical - Start;
    KeAcquireGuardedMutex(&Map->Mutex);
    Each = Floor(Map, Start);
    if (Each && Each->Start + Each->Length >= End &&
        Each->Physical - Each->Start == Bias) {
        Result = TRUE;
        goto out;
    }
    /* Validate every overlap before changing the tree or its accounting. */
    for (Each = First(Map, Start); Each && Each->Start <= End;
         Each = Run(rb_next(&Each->Node))) {
        LONGLONG EachEnd = Each->Start + Each->Length;
        if (Each->Physical - Each->Start != Bias) {
            if (Each->Start < End && EachEnd > Begin)
                goto out;
            continue;
        }
        if (!Keep)
            Keep = Each;
        Begin = min(Begin, Each->Start);
        End = max(End, EachEnd);
    }
    if (!Keep) {
        Keep = Ext2AllocatePool(NonPagedPool, sizeof(*Keep), RUN_MAP_TAG);
        if (!Keep)
            goto out;
    }
    /* A merged run reuses its first node. A new run reserves its node first. */
    Map->Cursor = NULL;
    Each = First(Map, Begin);
    while (Each && Each->Start <= End) {
        Next = Run(rb_next(&Each->Node));
        if (Each->Physical - Each->Start == Bias) {
            rb_erase(&Each->Node, &Map->Root);
            Map->Count--;
            if (Each != Keep)
                Ext2FreePool(Each, RUN_MAP_TAG);
        }
        Each = Next;
    }
    Keep->Start = Begin;
    Keep->Physical = Begin + Bias;
    Keep->Length = End - Begin;
    Insert(Map, Keep);
    Result = TRUE;
out:
    KeReleaseGuardedMutex(&Map->Mutex);
    return Result;
}

BOOLEAN Ext4RunMapRemove(PEXT4_RUN_MAP Map, LONGLONG Start, LONGLONG Length)
{
    EXT4_RUN *Each, *Next, *Right = NULL;
    LONGLONG End;
    BOOLEAN Result = FALSE;
    if (Start < 0 || Length < 0 || Start > MAXLONGLONG - Length)
        return FALSE;
    if (!Length)
        return TRUE;
    End = Start + Length;
    KeAcquireGuardedMutex(&Map->Mutex);
    Each = Floor(Map, Start);
    /* Only one interval can contain both cut points. Reserve its split first. */
    if (Each && Each->Start < Start && Each->Start + Each->Length > End) {
        Right = Ext2AllocatePool(NonPagedPool, sizeof(*Right), RUN_MAP_TAG);
        if (!Right)
            goto out;
        Right->Start = End;
        Right->Physical = Each->Physical + (End - Each->Start);
        Right->Length = Each->Start + Each->Length - End;
        Map->Cursor = NULL;
        Each->Length = Start - Each->Start;
        Insert(Map, Right);
        Result = TRUE;
        goto out;
    }
    Map->Cursor = NULL;
    for (Each = First(Map, Start); Each && Each->Start < End; Each = Next) {
        LONGLONG EachEnd = Each->Start + Each->Length;
        Next = Run(rb_next(&Each->Node));
        if (EachEnd <= Start)
            continue;
        if (Each->Start < Start) {
            Each->Length = Start - Each->Start;
        } else if (EachEnd > End) {
            Each->Physical += End - Each->Start;
            Each->Start = End;
            Each->Length = EachEnd - End;
        } else {
            rb_erase(&Each->Node, &Map->Root);
            Map->Count--;
            Ext2FreePool(Each, RUN_MAP_TAG);
        }
    }
    Result = TRUE;
out:
    KeReleaseGuardedMutex(&Map->Mutex);
    return Result;
}

BOOLEAN Ext4RunMapLookup(PEXT4_RUN_MAP Map, LONGLONG Start, PLONGLONG Physical,
    PLONGLONG Remaining, PLONGLONG RunPhysical, PLONGLONG RunLength, PULONG Index)
{
    EXT4_RUN *Each, *Next;
    LONGLONG Base, End, Value;
    BOOLEAN Result = FALSE;
    if (Start < 0)
        return FALSE;
    KeAcquireGuardedMutex(&Map->Mutex);
    Each = Floor(Map, Start);
    if (Each && Each->Start + Each->Length > Start) {
        Base = Each->Start;
        End = Base + Each->Length;
        Value = Each->Physical;
    } else {
        Next = Each ? Run(rb_next(&Each->Node)) : Run(rb_first(&Map->Root));
        if (!Next)
            goto out;
        Base = Each ? Each->Start + Each->Length : 0;
        End = Next->Start;
        Value = -1;
    }
    if (Physical) *Physical = Value == -1 ? -1 : Value + (Start - Base);
    if (Remaining) *Remaining = End - Start;
    if (RunPhysical) *RunPhysical = Value;
    if (RunLength) *RunLength = End - Base;
    if (Index) {
        struct rb_node *Node;
        *Index = 0;
        for (Node = rb_first(&Map->Root); Node && Run(Node)->Start < Base; Node = rb_next(Node))
            (*Index)++;
    }
    Result = TRUE;
out:
    KeReleaseGuardedMutex(&Map->Mutex);
    return Result;
}

BOOLEAN Ext4RunMapNext(PEXT4_RUN_MAP Map, ULONG Index, PLONGLONG Start,
    PLONGLONG Physical, PLONGLONG Length)
{
    struct rb_node *Node;
    ULONG Position;
    BOOLEAN Result = FALSE;
    KeAcquireGuardedMutex(&Map->Mutex);
    /* Sequential enumeration walks the tree once, including fragmented files. */
    if (Map->Cursor && Index >= Map->CursorIndex) {
        Node = Map->Cursor;
        Position = Map->CursorIndex;
    } else {
        Node = rb_first(&Map->Root);
        Position = 0;
    }
    while (Node && Position < Index) {
        Node = rb_next(Node);
        Position++;
    }
    if (Node) {
        EXT4_RUN *Each = Run(Node);
        Map->Cursor = Node;
        Map->CursorIndex = Index;
        *Start = Each->Start;
        *Physical = Each->Physical;
        *Length = Each->Length;
        Result = TRUE;
    }
    KeReleaseGuardedMutex(&Map->Mutex);
    return Result;
}
