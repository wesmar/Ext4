/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RUN_MAP_MODEL_STUB_H
#define RUN_MAP_MODEL_STUB_H
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
typedef int64_t LONGLONG, *PLONGLONG;
typedef uint32_t ULONG, *PULONG;
typedef uintptr_t ULONG_PTR;
typedef unsigned char BOOLEAN;
typedef void VOID;
typedef struct { int Held; } KGUARDED_MUTEX;
#define TRUE 1
#define FALSE 0
#define MAXLONGLONG INT64_MAX
#define NonPagedPool 0
#define __attribute__(x)
#define container_of(p,t,m) ((t *)((char *)(p) - offsetof(t,m)))
#ifndef min
#define min(a,b) ((a)<(b)?(a):(b))
#endif
#ifndef max
#define max(a,b) ((a)>(b)?(a):(b))
#endif
static void KeInitializeGuardedMutex(KGUARDED_MUTEX *m) { m->Held = 0; }
static void KeAcquireGuardedMutex(KGUARDED_MUTEX *m) { assert(!m->Held); m->Held = 1; }
static void KeReleaseGuardedMutex(KGUARDED_MUTEX *m) { assert(m->Held); m->Held = 0; }
static unsigned ModelPoolCount;
static int ModelFailAllocation;
static void *Ext2AllocatePool(int pool, size_t size, unsigned tag)
{
    void *p;
    (void)pool; (void)tag;
    if (ModelFailAllocation) return NULL;
    p = malloc(size);
    if (p) ModelPoolCount++;
    return p;
}
static void Ext2FreePool(void *p, unsigned tag)
{
    (void)tag;
    assert(p && ModelPoolCount);
    ModelPoolCount--;
    free(p);
}
#include "../../src/include/RunMap.h"
#endif
