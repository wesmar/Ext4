/* SPDX-License-Identifier: GPL-2.0-only
 * Minimal kernel model for the production VPB ownership implementation.
 * No concurrency or I/O-manager behaviour is simulated by these stubs.
 */
#ifndef EXT4_VPB_MODEL_H
#define EXT4_VPB_MODEL_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#define IN
#define VOID void
#define TRUE 1
#define FALSE 0
typedef int BOOLEAN;
typedef unsigned long ULONG;
typedef unsigned short USHORT;
typedef int KIRQL;
typedef int KSPIN_LOCK;
typedef struct _LIST_ENTRY { struct _LIST_ENTRY *Flink, *Blink; } LIST_ENTRY, *PLIST_ENTRY;
typedef struct _VPB VPB, *PVPB;
typedef struct _DEVICE_OBJECT { PVPB Vpb; int References; } DEVICE_OBJECT, *PDEVICE_OBJECT;
struct _VPB {
    unsigned short Flags, VolumeLabelLength;
    ULONG ReferenceCount;
    PDEVICE_OBJECT RealDevice, DeviceObject;
};
typedef struct { LIST_ENTRY SwapVpbList; KSPIN_LOCK SwapVpbLock; } EXT2_GLOBAL;
extern EXT2_GLOBAL *Ext2Global;
extern int ModelPoolCount, ModelFailAllocation, ModelVpbLock;
#define NonPagedPool 0
#define TAG_VPB 0
#define PS_VPB 0
#define VPB_MOUNTED 1
#define VPB_LOCKED 2
#define VPB_PERSISTENT 4
#define FlagOn(f, m) ((f) & (m))
#define DEC_MEM_COUNT(a, b, c) ((void)0)
#define CONTAINING_RECORD(p, t, f) ((t *)((char *)(p) - offsetof(t, f)))
static inline void InitializeListHead(PLIST_ENTRY h) { h->Flink = h->Blink = h; }
static inline int IsListEmpty(PLIST_ENTRY h) { return h->Flink == h; }
static inline void RemoveEntryList(PLIST_ENTRY e) { e->Blink->Flink = e->Flink; e->Flink->Blink = e->Blink; }
static inline void InsertHeadList(PLIST_ENTRY h, PLIST_ENTRY e) { e->Flink = h->Flink; e->Blink = h; h->Flink->Blink = e; h->Flink = e; }
static inline void InsertTailList(PLIST_ENTRY h, PLIST_ENTRY e) { e->Blink = h->Blink; e->Flink = h; h->Blink->Flink = e; h->Blink = e; }
static inline PLIST_ENTRY RemoveHeadList(PLIST_ENTRY h) { PLIST_ENTRY e = h->Flink; assert(e != h); RemoveEntryList(e); return e; }
static inline void KeAcquireSpinLock(KSPIN_LOCK *l, KIRQL *i) { assert(!*l); *l = 1; *i = 0; }
static inline void KeReleaseSpinLock(KSPIN_LOCK *l, KIRQL i) { (void)i; assert(*l); *l = 0; }
static inline void IoAcquireVpbSpinLock(KIRQL *i) { assert(!ModelVpbLock); ModelVpbLock = 1; *i = 0; }
static inline void IoReleaseVpbSpinLock(KIRQL i) { (void)i; assert(ModelVpbLock); ModelVpbLock = 0; }
static inline void *Ext2AllocatePool(int p, size_t n, int t) {
    void *v;
    (void)p; (void)t;
    if (ModelFailAllocation) return NULL;
    v = calloc(1, n); if (v) ModelPoolCount++;
    return v;
}
static inline void Ext2FreePool(void *v, int t) { (void)t; assert(v); ModelPoolCount--; free(v); }
static inline void ObReferenceObject(PDEVICE_OBJECT d) { d->References++; }
static inline void ObDereferenceObject(PDEVICE_OBJECT d) { assert(d->References > 0); d->References--; }
#endif
