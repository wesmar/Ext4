/* SPDX-License-Identifier: GPL-2.0-only
 * Compile the actual production vpb.c against a deterministic ownership model.
 * This checks ownership transitions, not real kernel scheduling or IRQL rules.
 */
#include <stdio.h>
#include "ext4fs.h"
#include "../../src/volume/vpb.c"

EXT2_GLOBAL Global;
EXT2_GLOBAL *Ext2Global = &Global;
int ModelPoolCount, ModelFailAllocation, ModelVpbLock;
static int Checks;
#define CHECK(x) do { Checks++; if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

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
    CHECK(Ext2ReclaimVpbs());
    CHECK(!Ext2VpbsPending());
    CHECK(d->Vpb == original);
    CHECK(d->References == 0);
    CHECK(ModelPoolCount == 0);
}
int main(void) {
    DEVICE_OBJECT device = {0};
    VPB original = {0};
    PVPB a, b, c;
    KIRQL irql;
    InitializeListHead(&Global.SwapVpbList);
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
    CHECK(!Ext2ReclaimVpbs()); CHECK(Ext2VpbsPending());
    CHECK(device.Vpb == a); CHECK(ModelPoolCount == 2);
    original.ReferenceCount = 0; Clean(&device, &original);

    /* Raw-volume users of the replacement delay reclaim. */
    a = Swap(&device); Track(&original, a); a->ReferenceCount = 1;
    CHECK(!Ext2ReclaimVpbs()); CHECK(ModelPoolCount == 2);
    a->ReferenceCount = 0; Clean(&device, &original);

    /* Each mount/lock condition independently prevents unsafe replacement. */
    a = Swap(&device); Track(&original, a); a->Flags = VPB_MOUNTED;
    CHECK(!Ext2ReclaimVpbs()); a->Flags = VPB_LOCKED;
    CHECK(!Ext2ReclaimVpbs()); a->Flags = 0;
    a->DeviceObject = &device; CHECK(!Ext2ReclaimVpbs());
    a->DeviceObject = NULL; Clean(&device, &original);

    /* Nested swaps unwind even when list order is adversarial. */
    a = Swap(&device); Track(&original, a);
    b = Swap(&device); Track(a, b);
    c = Swap(&device); Track(b, c);
    InsertTailList(&Global.SwapVpbList, RemoveHeadList(&Global.SwapVpbList));
    Clean(&device, &original);
    CHECK(original.Flags == 0);

    /* One busy chain does not prevent reclaim of unrelated eligible pairs. */
    {
        DEVICE_OBJECT other = {0}; VPB otherOriginal = {0};
        otherOriginal.RealDevice = &other; other.Vpb = &otherOriginal;
        a = Swap(&device); Track(&original, a); a->ReferenceCount = 1;
        b = Swap(&other); Track(&otherOriginal, b);
        CHECK(!Ext2ReclaimVpbs()); CHECK(other.Vpb == &otherOriginal);
        CHECK(other.References == 0); CHECK(ModelPoolCount == 2);
        a->ReferenceCount = 0; Clean(&device, &original);
    }

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
