/* SPDX-License-Identifier: GPL-2.0-only
 * Compile the actual production VolumePointerOwnership.c against a deterministic ownership model.
 * This checks ownership transitions, not real kernel scheduling or IRQL rules.
 */
#include <stdio.h>
#include "ext4fs.h"
#include "../../src/volume/VolumePointerOwnership.c"

EXT2_GLOBAL Global;
EXT2_GLOBAL *Ext2Global = &Global;
int ModelPoolCount, ModelFailAllocation, ModelVpbLock;
ULONGLONG ModelInterruptTime;
static int Checks;
#define CHECK(x) do { Checks++; if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static DRIVER_OBJECT Ours, Theirs;
static DEVICE_OBJECT Control = { NULL, 0, { 1 }, &Ours };

static PVPB Swap(PDEVICE_OBJECT d) {
    PVPB v = Ext2AllocatePool(NonPagedPool, sizeof(*v), TAG_VPB);
    CHECK(v != NULL); v->RealDevice = d; return v;
}
static void Track(PVPB old, PVPB swap) {
    KIRQL irql;
    IoAcquireVpbSpinLock(&irql);
    CHECK(Ext2TrackVpbSwap(old, swap));
    old->RealDevice->Vpb = swap;
    IoReleaseVpbSpinLock(irql);
}
static void Clean(PDEVICE_OBJECT d, PVPB original) {
    CHECK(Ext2ReclaimVpbs(FALSE));
    CHECK(!Ext2VpbsPending());
    CHECK(d->Vpb == original);
    CHECK(d->References == 0);
    CHECK(d->DeviceLock.Signaled);
    CHECK(ModelPoolCount == 0);
}
/* an sc stop: draining, and the clock past the grace period or not */
static void Stopping(BOOLEAN PastGrace) {
    Global.UnloadState = EXT2_UNLOAD_DRAINING;
    Global.SwapVpbsHeldSince = 0;
    ModelInterruptTime = 1000;
    CHECK(!Ext2ReclaimVpbs(TRUE));          /* starts the clock */
    if (PastGrace) ModelInterruptTime += EXT2_MILLISECONDS(2000);
}
static void Running(void) {
    Global.UnloadState = EXT2_UNLOAD_IDLE;
    Global.SwapVpbsHeldSince = 0;
}
int main(void) {
    DEVICE_OBJECT device = { NULL, 0, { 1 }, NULL };
    DEVICE_OBJECT foreign = { NULL, 0, { 1 }, &Theirs };
    DEVICE_OBJECT volume = { NULL, 0, { 1 }, &Ours };
    VPB original = {0};
    PVPB a, b, c, o;
    KIRQL irql;
    InitializeListHead(&Global.SwapVpbList);
    Global.DiskdevObject = &Control;
    original.RealDevice = &device; device.Vpb = &original;

    /* No swap. */
    Clean(&device, &original);
    /* An original device-owned persistence flag survives restoration. */
    original.Flags = VPB_PERSISTENT;
    a = Swap(&device); Track(&original, a); Clean(&device, &original);
    CHECK(original.Flags == VPB_PERSISTENT);
    original.Flags = 0;
    /* A referenced original must never be freed or forgotten. */
    a = Swap(&device); Track(&original, a); original.ReferenceCount = 1;
    CHECK(!Ext2ReclaimVpbs(FALSE)); CHECK(Ext2VpbsPending());
    CHECK(device.Vpb == a); CHECK(ModelPoolCount == 2);
    original.ReferenceCount = 0; Clean(&device, &original);
    /* Nor one the dismounted volume has not let go of yet. */
    a = Swap(&device); Track(&original, a); original.DeviceObject = &volume;
    CHECK(!Ext2ReclaimVpbs(FALSE)); CHECK(device.Vpb == a);
    original.DeviceObject = NULL; Clean(&device, &original);
    /* A mount in flight holds the device lock: nothing is exchanged under it. */
    a = Swap(&device); Track(&original, a); device.DeviceLock.Signaled = 0;
    CHECK(!Ext2ReclaimVpbs(FALSE)); CHECK(device.Vpb == a); CHECK(ModelPoolCount == 2);
    device.DeviceLock.Signaled = 1; Clean(&device, &original);

    /* Raw-volume users of the replacement delay reclaim. */
    a = Swap(&device); Track(&original, a); a->ReferenceCount = 1;
    CHECK(!Ext2ReclaimVpbs(FALSE)); CHECK(ModelPoolCount == 2);
    a->ReferenceCount = 0; Clean(&device, &original);

    /* Each mount/lock condition independently prevents unsafe replacement. */
    a = Swap(&device); Track(&original, a); a->Flags = VPB_MOUNTED;
    CHECK(!Ext2ReclaimVpbs(FALSE)); a->Flags = VPB_LOCKED;
    CHECK(!Ext2ReclaimVpbs(FALSE)); a->Flags = 0;
    a->DeviceObject = &foreign; CHECK(!Ext2ReclaimVpbs(FALSE));
    a->DeviceObject = NULL; Clean(&device, &original);
    /* A removal pending on the device stays with the original. */
    a = Swap(&device); Track(&original, a); a->Flags = VPB_REMOVE_PENDING;
    Clean(&device, &original); CHECK(original.Flags == VPB_REMOVE_PENDING);
    original.Flags = 0;

    /* Nested swaps unwind even when list order is adversarial. */
    a = Swap(&device); Track(&original, a);
    b = Swap(&device); Track(a, b);
    c = Swap(&device); Track(b, c);
    InsertTailList(&Global.SwapVpbList, RemoveHeadList(&Global.SwapVpbList));
    CHECK(Ext2VpbIsOurs(a)); CHECK(Ext2VpbIsOurs(c)); CHECK(!Ext2VpbIsOurs(&original));
    Clean(&device, &original);
    CHECK(original.Flags == 0);

    /* One busy chain does not prevent reclaim of unrelated eligible pairs. */
    {
        DEVICE_OBJECT other = { NULL, 0, { 1 }, NULL }; VPB otherOriginal = {0};
        otherOriginal.RealDevice = &other; other.Vpb = &otherOriginal;
        a = Swap(&device); Track(&original, a); a->ReferenceCount = 1;
        b = Swap(&other); Track(&otherOriginal, b);
        CHECK(!Ext2ReclaimVpbs(FALSE)); CHECK(other.Vpb == &otherOriginal);
        CHECK(other.References == 0); CHECK(ModelPoolCount == 2);
        a->ReferenceCount = 0; Clean(&device, &original);
    }

    /* A later volume of ours on the replacement: never left, even by a stop
       past its grace - it gives the VPB back when it goes. */
    a = Swap(&device); Track(&original, a);
    a->Flags = VPB_MOUNTED; a->DeviceObject = &volume; a->ReferenceCount = 1;
    Stopping(TRUE);
    CHECK(!Ext2ReclaimVpbs(TRUE)); CHECK(device.Vpb == a); CHECK(ModelPoolCount == 2);
    a->Flags = 0; a->DeviceObject = NULL; a->ReferenceCount = 0;
    Running(); Clean(&device, &original);

    /* Another file system mounted the replacement: kept while running and
       through the grace period; then left to it, and the I/O manager's
       original - nothing references it - freed. */
    o = Swap(&device); device.Vpb = o;
    a = Swap(&device); Track(o, a);
    a->Flags = VPB_MOUNTED; a->DeviceObject = &foreign; a->ReferenceCount = 1;
    CHECK(!Ext2ReclaimVpbs(FALSE)); CHECK(!Ext2ReclaimVpbs(TRUE));
    Stopping(FALSE); CHECK(!Ext2ReclaimVpbs(TRUE)); CHECK(ModelPoolCount == 3);
    Stopping(TRUE);
    CHECK(Ext2ReclaimVpbs(TRUE)); CHECK(!Ext2VpbsPending());
    CHECK(device.Vpb == a); CHECK(device.References == 0);
    CHECK(ModelPoolCount == 1);              /* the replacement, the other's now */
    Ext2FreePool(a, TAG_VPB); Running();
    device.Vpb = &original;

    /* Another file system took the replacement off the device: left alike,
       without the replacement being read again. */
    o = Swap(&device); device.Vpb = o;
    a = Swap(&device); Track(o, a);
    b = Swap(&device); b->Flags = VPB_MOUNTED; device.Vpb = b;  /* its own */
    Stopping(TRUE);
    CHECK(Ext2ReclaimVpbs(TRUE)); CHECK(device.Vpb == b); CHECK(ModelPoolCount == 2);
    Ext2FreePool(a, TAG_VPB); Ext2FreePool(b, TAG_VPB); Running();
    device.Vpb = &original;

    /* A chain whose far end another file system took: the far pair frees
       our middle VPB and makes the near one forget it; the near pair frees
       the original. Only the taken VPB remains. */
    o = Swap(&device); device.Vpb = o;
    a = Swap(&device); Track(o, a);
    b = Swap(&device); Track(a, b);
    c = Swap(&device); c->Flags = VPB_MOUNTED; device.Vpb = c;   /* took b off */
    Stopping(TRUE);
    CHECK(Ext2ReclaimVpbs(TRUE)); CHECK(!Ext2VpbsPending());
    CHECK(device.Vpb == c); CHECK(device.References == 0);
    CHECK(ModelPoolCount == 2);              /* b and c: the other's */
    Ext2FreePool(b, TAG_VPB); Ext2FreePool(c, TAG_VPB); Running();
    device.Vpb = &original;

    /* Leaving never forgets a busy original. */
    o = Swap(&device); device.Vpb = o;
    a = Swap(&device); Track(o, a); a->Flags = VPB_MOUNTED; a->DeviceObject = &foreign;
    o->ReferenceCount = 1;
    Stopping(TRUE); CHECK(!Ext2ReclaimVpbs(TRUE)); CHECK(ModelPoolCount == 3);
    o->ReferenceCount = 0; CHECK(Ext2ReclaimVpbs(TRUE)); CHECK(ModelPoolCount == 1);
    Ext2FreePool(a, TAG_VPB); Running();
    device.Vpb = &original;

    /* Failed bookkeeping allocation must precede publication. */
    a = Swap(&device); ModelFailAllocation = 1;
    IoAcquireVpbSpinLock(&irql);
    CHECK(!Ext2TrackVpbSwap(&original, a));
    IoReleaseVpbSpinLock(irql);
    CHECK(device.Vpb == &original); CHECK(!Ext2VpbsPending());
    CHECK(device.References == 0);
    ModelFailAllocation = 0; Ext2FreePool(a, TAG_VPB);
    Clean(&device, &original);
    printf("VPB-MODEL: %d checks passed\n", Checks);
    return 0;
}
