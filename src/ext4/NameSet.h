/**
 * NameSet.h - a multiset of 32-bit name hashes: open addressing, linear
 * probing, removal by backward shift. The table of a directory's caseless
 * names (DirectoryNameCache.c).
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * Plain C, no system header: the driver and the host model compile it. The
 * caller owns the memory: NsBytes(slots) for a table of slots (a power of
 * two), zeroed, then NsInit.
 */

#ifndef _EXT4_NAME_SET_H_
#define _EXT4_NAME_SET_H_

typedef struct _NS_SLOT {
    unsigned int    Hash;       /* 0: empty; a hash always has its low bit set */
    unsigned int    Count;      /* names with it */
} NS_SLOT;

typedef struct _NS_SET {
    unsigned int    Mask;       /* slots - 1 */
    unsigned int    Used;       /* slots holding a hash */
    NS_SLOT         Slot[1];
} NS_SET;

#define NsBytes(Slots)  ((unsigned long long)sizeof(NS_SET) + ((unsigned long long)(Slots) - 1) * sizeof(NS_SLOT))

/* FNV-1a of a name with a-z folded to A-Z (as _strnicmp compares), low bit set */
unsigned int NsHash(const char *Name, int Length);

void NsInit(NS_SET *Set, unsigned int Slots);

/* names with Hash (0: none) */
unsigned int NsCount(const NS_SET *Set, unsigned int Hash);

/* Count more names with Hash; the caller keeps the table at most half full */
void NsPut(NS_SET *Set, unsigned int Hash, unsigned int Count);

/* one name with Hash fewer; 0: the set held none (it is wrong) */
int NsTake(NS_SET *Set, unsigned int Hash);

/* every count of From into To, a zeroed table at least twice as large */
void NsCopy(NS_SET *To, const NS_SET *From);

#endif /* _EXT4_NAME_SET_H_ */
