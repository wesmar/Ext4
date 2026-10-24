/* SPDX-License-Identifier: GPL-2.0-only
 * Minimal kernel model for the production VPB ownership implementation.
 * No concurrency or I/O-manager behaviour is simulated by these stubs: the
 * device lock is a flag a test sets to stand for a mount in flight, and the
 * interrupt clock moves only when a test moves it.
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
typedef unsigned long long ULONGLONG;
typedef long NTSTATUS;
typedef int KIRQL;
typedef int KSPIN_LOCK;
typedef int FAST_MUTEX;
typedef struct { int Signaled; } KEVENT;
typedef union { long long QuadPart; } LARGE_INTEGER;
typedef struct _LIST_ENTRY { struct _LIST_ENTRY *Flink, *Blink; } LIST_ENTRY, *PLIST_ENTRY;
typedef struct _VPB VPB, *PVPB;
typedef struct _DRIVER_OBJECT { int Unused; } DRIVER_OBJECT, *PDRIVER_OBJECT;
typedef struct _DEVICE_OBJECT {
    PVPB Vpb; int References; KEVENT DeviceLock; PDRIVER_OBJECT DriverObject;
} DEVICE_OBJECT, *PDEVICE_OBJECT;
struct _VPB {
    unsigned short Flags, VolumeLabelLength;
    ULONG ReferenceCount;
    PDEVICE_OBJECT RealDevice, DeviceObject;
};
typedef struct {
    LIST_ENTRY SwapVpbList; KSPIN_LOCK SwapVpbLock; FAST_MUTEX SwapVpbMutex;
    ULONGLONG SwapVpbsHeldSince; long UnloadState; PDEVICE_OBJECT DiskdevObject;
} EXT2_GLOBAL;
extern EXT2_GLOBAL *Ext2Global;
extern int ModelPoolCount, ModelFailAllocation, ModelVpbLock;
extern ULONGLONG ModelInterruptTime;
#define NonPagedPool 0
#define TAG_VPB 0
#define PS_VPB 0
#define VPB_MOUNTED 1
#define VPB_LOCKED 2
#define VPB_PERSISTENT 4
#define VPB_REMOVE_PENDING 8
#define STATUS_SUCCESS 0L
#define STATUS_TIMEOUT 0x102L
#define Executive 0
#define KernelMode 0
#define IO_NO_INCREMENT 0
#define EXT2_UNLOAD_IDLE 0
#define EXT2_UNLOAD_DRAINING 2
#define EXT2_MILLISECONDS(ms) ((long long)(ms) * 10000)
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
/* only under the VPB spin lock, as at DISPATCH_LEVEL */
static inline void KeAcquireSpinLockAtDpcLevel(KSPIN_LOCK *l) { assert(ModelVpbLock); assert(!*l); *l = 1; }
static inline void KeReleaseSpinLockFromDpcLevel(KSPIN_LOCK *l) { assert(*l); *l = 0; }
static inline void IoAcquireVpbSpinLock(KIRQL *i) { assert(!ModelVpbLock); ModelVpbLock = 1; *i = 0; }
static inline void IoReleaseVpbSpinLock(KIRQL i) { (void)i; assert(ModelVpbLock); ModelVpbLock = 0; }
static inline void ExAcquireFastMutex(FAST_MUTEX *m) { assert(!*m); *m = 1; }
static inline void ExReleaseFastMutex(FAST_MUTEX *m) { assert(*m); *m = 0; }
/* a zero timeout only: the production code never waits for a device lock */
static inline NTSTATUS KeWaitForSingleObject(KEVENT *e, int r, int m, int a, LARGE_INTEGER *t) {
    (void)r; (void)m; (void)a; assert(t && t->QuadPart == 0); assert(!ModelVpbLock);
    if (!e->Signaled) return STATUS_TIMEOUT;
    e->Signaled = 0; return STATUS_SUCCESS;
}
static inline long KeSetEvent(KEVENT *e, int p, int w) { (void)p; (void)w; assert(!e->Signaled); e->Signaled = 1; return 0; }
static inline ULONGLONG KeQueryInterruptTime(void) { return ModelInterruptTime; }
static inline void *Ext2AllocatePool(int p, size_t n, int t) {
    void *v;
    (void)p; (void)t;
    if (ModelFailAllocation) return NULL;
    v = calloc(1, n); if (v) ModelPoolCount++;
    return v;
}
static inline void Ext2FreePool(void *v, int t) { (void)t; assert(v); ModelPoolCount--; free(v); }
static inline void ExFreePoolWithTag(void *v, int t) { Ext2FreePool(v, t); }
static inline void ObReferenceObject(PDEVICE_OBJECT d) { d->References++; }
static inline void ObDereferenceObject(PDEVICE_OBJECT d) { assert(d->References > 0); d->References--; }
#endif
