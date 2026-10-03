/* SPDX-License-Identifier: GPL-2.0-only
 * RunMap.h - physical run-map ownership and lookup contract.
 * 64-bit run maps. Ranges are half-open, disjoint and merged when contiguous.
 * The map owns its nodes; callers own the map's lifetime. Each operation holds
 * Mutex, but a multi-call enumeration also needs the caller's mutation guard.
 * Lookup returns implicit holes as Physical == -1 up to the next stored run.
 */
#ifndef EXT4_RUN_MAP_H
#define EXT4_RUN_MAP_H
#include <linux/rbtree.h>

typedef struct _EXT4_RUN_MAP {
    KGUARDED_MUTEX Mutex;
    struct rb_root Root;
    struct rb_node *Cursor;
    ULONG CursorIndex;
    ULONG Count;
    BOOLEAN Initialized;
} EXT4_RUN_MAP, *PEXT4_RUN_MAP;

VOID Ext4RunMapInitialize(PEXT4_RUN_MAP Map);
VOID Ext4RunMapClear(PEXT4_RUN_MAP Map);
VOID Ext4RunMapDestroy(PEXT4_RUN_MAP Map);
ULONG Ext4RunMapCount(PEXT4_RUN_MAP Map);
BOOLEAN Ext4RunMapAdd(PEXT4_RUN_MAP Map, LONGLONG Start, LONGLONG Physical, LONGLONG Length);
BOOLEAN Ext4RunMapRemove(PEXT4_RUN_MAP Map, LONGLONG Start, LONGLONG Length);
/* Mapping lookup is O(log n). Requesting the optional ordinal Index adds O(n). */
BOOLEAN Ext4RunMapLookup(PEXT4_RUN_MAP Map, LONGLONG Start, PLONGLONG Physical,
    PLONGLONG Remaining, PLONGLONG RunPhysical, PLONGLONG RunLength, PULONG Index);
/* Consecutive indexes use a cursor; every mutation invalidates it under Mutex. */
BOOLEAN Ext4RunMapNext(PEXT4_RUN_MAP Map, ULONG Index, PLONGLONG Start,
    PLONGLONG Physical, PLONGLONG Length);
#endif
