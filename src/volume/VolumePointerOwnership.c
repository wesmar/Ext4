/**
 * VolumePointerOwnership.c - ownership of VPBs published by forced dismounts.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"

/*
 * A forced dismount of a volume with open handles gives its device a fresh
 * VPB of ours (Swap), so that the device can be mounted again while those
 * handles drain; the volume keeps the old one (Old). Once nothing uses Old
 * the two are exchanged back - Old onto the device, Swap freed - so that
 * no allocation of ours outlives the driver. Until then anyone may mount
 * Swap: a later volume of ours, which a forced dismount may take off the
 * device in turn (a chain of pairs, settled from its far end), or another
 * file system. A VPB another file system mounted, or took off the device,
 * is that file system's to keep and to free: Windows swaps VPBs this way
 * and never asks for one back (fastfat). An sc stop that has waited
 * EXT2_SWAP_VPB_GRACE for it leaves it there and frees Old, which nothing
 * references any more: the driver unloads either way. Driver Verifier
 * counts the VPB left behind as pool not freed at unload (0xC4/0x62). The
 * stop itself gives no file system the chance: it goes on mounting a VPB
 * of ours (VolumeMount.c), so only one taken before - NTFS on a volume
 * formatted over ours, say - is ever left.
 *
 * Lock order: the device's DeviceLock (never waited for), the VPB spin
 * lock, SwapVpbLock. The dismount adds pairs under the last two; only
 * Ext2ReclaimVpbs, one pass at a time, changes or removes them.
 */

/* how long an sc stop lets another file system hold a VPB of ours: a
   service's raw open of a dismounted device lets go well within it */
#define EXT2_SWAP_VPB_GRACE     EXT2_MILLISECONDS(2000)

typedef struct _EXT2_SWAP_VPB {
    LIST_ENTRY      Link;
    PVPB            Old;
    PVPB            Swap;           /* NULL once freed as a later pair's Old */
    PDEVICE_OBJECT  Device;
    USHORT          OldPersistent;
} EXT2_SWAP_VPB, *PEXT2_SWAP_VPB;

typedef enum _EXT2_SWAP_FATE {
    Ext2SwapWait,                   /* in use, or another pair goes first */
    Ext2SwapReturn,                 /* Old back onto the device, Swap freed */
    Ext2SwapLeave                   /* Swap is another file system's now */
} EXT2_SWAP_FATE;

/* IoAcquireVpbSpinLock is held by the caller. Reserve ownership before
   publishing Swap: failure leaves the original device/VPB pair intact.
   Keeping the real device referenced also covers surprise removal. */
BOOLEAN
Ext2TrackVpbSwap(IN PVPB Old, IN PVPB Swap)
{
    PEXT2_SWAP_VPB S;

    if (Old->RealDevice == NULL) {
        return FALSE;
    }
    S = Ext2AllocatePool(NonPagedPool, sizeof(*S), TAG_VPB);
    if (S == NULL) {
        return FALSE;
    }
    S->Old = Old;
    S->Swap = Swap;
    S->Device = Old->RealDevice;
    S->OldPersistent = Old->Flags & VPB_PERSISTENT;
    ObReferenceObject(S->Device);
    KeAcquireSpinLockAtDpcLevel(&Ext2Global->SwapVpbLock);
    InsertHeadList(&Ext2Global->SwapVpbList, &S->Link);
    KeReleaseSpinLockFromDpcLevel(&Ext2Global->SwapVpbLock);
    return TRUE;
}

/* SwapVpbLock is held: the pair that published Vpb as its Swap, or the
   one that keeps it as its Old */
static PEXT2_SWAP_VPB
Ext2PairOf(IN PVPB Vpb, IN BOOLEAN AsOld)
{
    PLIST_ENTRY Link;

    for (Link = Ext2Global->SwapVpbList.Flink;
         Link != &Ext2Global->SwapVpbList; Link = Link->Flink) {
        PEXT2_SWAP_VPB S = CONTAINING_RECORD(Link, EXT2_SWAP_VPB, Link);
        if ((AsOld ? S->Old : S->Swap) == Vpb) {
            return S;
        }
    }
    return NULL;
}

/* The VPB spin lock and SwapVpbLock are held. Swap is read only while it
   is the device's VPB: once off the device it may be freed already. */
static EXT2_SWAP_FATE
Ext2SwapFate(IN PEXT2_SWAP_VPB S, IN BOOLEAN Leave)
{
    PVPB Swap = S->Swap;

    /* the I/O manager closing the last file object of the dismounted
       volume still uses Old, or the volume itself has not let go of it
       (Ext2LeaveVpb) - none is left when an sc stop may leave */
    if (S->Old->ReferenceCount != 0 || (S->Old->DeviceObject != NULL && !Leave)) {
        return Ext2SwapWait;
    }

    if (Swap != NULL && S->Device->Vpb == Swap) {
        PDEVICE_OBJECT Mounted = Swap->DeviceObject;

        if (Swap->ReferenceCount == 0 && Mounted == NULL &&
            !FlagOn(Swap->Flags, VPB_MOUNTED | VPB_LOCKED)) {
            return Ext2SwapReturn;
        }
        /* a later volume of ours, which comes back here when it goes */
        if (Mounted != NULL && Ext2Global->DiskdevObject != NULL &&
            Mounted->DriverObject == Ext2Global->DiskdevObject->DriverObject) {
            return Ext2SwapWait;
        }
    } else if (Swap != NULL && Ext2PairOf(Swap, TRUE) != NULL) {
        /* a later volume of ours took Swap off the device: that pair goes
           first, and gives Swap back or frees it */
        return Ext2SwapWait;
    }

    /* mounted by another file system, taken off the device by one, or
       freed with a later pair already */
    return Leave ? Ext2SwapLeave : Ext2SwapWait;
}

/* One pair; TRUE once it is settled and gone. */
static BOOLEAN
Ext2SettleSwap(IN PEXT2_SWAP_VPB S, IN BOOLEAN Leave)
{
    PDEVICE_OBJECT Device = S->Device;
    EXT2_SWAP_FATE Fate;
    PVPB Ours = NULL, Theirs = NULL;
    LARGE_INTEGER Now;
    KIRQL Irql;

    /* Swap sitting on the device unmounted may be in a mount right now -
       by another file system as well as ours. The I/O manager holds no
       reference and no spin lock for that: it waits for the device's
       DeviceLock, reads RealDevice->Vpb and passes it to every file system
       in turn, and sets the lock only when they are done (IopMountVolume;
       IoVerifyVolume alike). A Swap freed in between was read by fastfat's
       mount: bugcheck 0x50 under Verifier, during an sc stop. So a pair is
       settled only while we hold DeviceLock; a mount in flight keeps it
       for the next pass. Never waited for: the mount in flight may be
       ours, waiting for the global resource our caller holds. */
    Now.QuadPart = 0;
    if (KeWaitForSingleObject(&Device->DeviceLock, Executive, KernelMode,
                              FALSE, &Now) != STATUS_SUCCESS) {
        return FALSE;
    }

    IoAcquireVpbSpinLock(&Irql);
    KeAcquireSpinLockAtDpcLevel(&Ext2Global->SwapVpbLock);

    Fate = Ext2SwapFate(S, Leave);
    if (Fate == Ext2SwapReturn) {
        PVPB Old = S->Old, Swap = S->Swap;
        /* Old stays persistent while it is an earlier pair's Swap: the I/O
           manager must not free it if it leaves the device again */
        Old->Flags = (USHORT)((Swap->Flags & ~VPB_PERSISTENT) | S->OldPersistent |
                              (Ext2PairOf(Old, FALSE) != NULL ? VPB_PERSISTENT : 0));
        Old->DeviceObject = NULL;
        Old->VolumeLabelLength = 0;
        Device->Vpb = Old;
        Ours = Swap;
    } else if (Fate == Ext2SwapLeave) {
        /* Old is ours to dispose of: an earlier pair's Swap, which that
           pair forgets now, or the I/O manager's own, kept from it by
           VPB_PERSISTENT */
        PEXT2_SWAP_VPB Earlier = Ext2PairOf(S->Old, FALSE);
        if (Earlier != NULL) {
            Earlier->Swap = NULL;
            Ours = S->Old;
        } else {
            Theirs = S->Old;
        }
    }
    if (Fate != Ext2SwapWait) {
        RemoveEntryList(&S->Link);
    }

    KeReleaseSpinLockFromDpcLevel(&Ext2Global->SwapVpbLock);
    IoReleaseVpbSpinLock(Irql);
    KeSetEvent(&Device->DeviceLock, IO_NO_INCREMENT, FALSE);

    if (Fate == Ext2SwapWait) {
        return FALSE;
    }
    if (Ours != NULL) {
        ExFreePoolWithTag(Ours, TAG_VPB);
        DEC_MEM_COUNT(PS_VPB, Ours, sizeof(VPB));
    }
    if (Theirs != NULL) {
        ExFreePoolWithTag(Theirs, 0);
    }
    ObDereferenceObject(Device);
    Ext2FreePool(S, TAG_VPB);
    return TRUE;
}

/* Vpb is one we published and still own */
BOOLEAN
Ext2VpbIsOurs(IN PVPB Vpb)
{
    BOOLEAN Ours;
    KIRQL Irql;

    KeAcquireSpinLock(&Ext2Global->SwapVpbLock, &Irql);
    Ours = Ext2PairOf(Vpb, FALSE) != NULL;
    KeReleaseSpinLock(&Ext2Global->SwapVpbLock, Irql);
    return Ours;
}

BOOLEAN
Ext2VpbsPending(VOID)
{
    BOOLEAN Pending;
    KIRQL Irql;

    KeAcquireSpinLock(&Ext2Global->SwapVpbLock, &Irql);
    Pending = !IsListEmpty(&Ext2Global->SwapVpbList);
    KeReleaseSpinLock(&Ext2Global->SwapVpbLock, Irql);
    return Pending;
}

/* An sc stop leaves what another file system holds, once it has held it
   for the grace period; a prepare-to-unload from user mode never does. */
static BOOLEAN
Ext2MayLeaveVpbs(VOID)
{
    ULONGLONG Now = KeQueryInterruptTime();

    if (Ext2Global->UnloadState != EXT2_UNLOAD_DRAINING) {
        return FALSE;
    }
    if (Ext2Global->SwapVpbsHeldSince == 0) {
        Ext2Global->SwapVpbsHeldSince = Now;
    }
    return Now - Ext2Global->SwapVpbsHeldSince >= (ULONGLONG)EXT2_SWAP_VPB_GRACE;
}

/* Settles every pair it can, TRUE when none is left. Called when a volume
   has gone (a pair may have become free), and by prepare-to-unload. Passes
   repeat while they make progress: a chain settles from its far end. */
BOOLEAN
Ext2ReclaimVpbs(IN BOOLEAN Unloading)
{
    BOOLEAN Progress, Leave;
    KIRQL Irql;

    if (!Ext2VpbsPending()) {
        return TRUE;
    }

    ExAcquireFastMutex(&Ext2Global->SwapVpbMutex);
    Leave = Unloading && Ext2MayLeaveVpbs();
    do {
        PLIST_ENTRY Link;

        Progress = FALSE;
        KeAcquireSpinLock(&Ext2Global->SwapVpbLock, &Irql);
        Link = Ext2Global->SwapVpbList.Flink;
        while (Link != &Ext2Global->SwapVpbList) {
            /* a pair is removed only here, so the next one stays */
            PEXT2_SWAP_VPB S = CONTAINING_RECORD(Link, EXT2_SWAP_VPB, Link);
            Link = Link->Flink;
            KeReleaseSpinLock(&Ext2Global->SwapVpbLock, Irql);
            if (Ext2SettleSwap(S, Leave)) {
                Progress = TRUE;
            }
            KeAcquireSpinLock(&Ext2Global->SwapVpbLock, &Irql);
        }
        KeReleaseSpinLock(&Ext2Global->SwapVpbLock, Irql);
    } while (Progress);
    ExReleaseFastMutex(&Ext2Global->SwapVpbMutex);

    return !Ext2VpbsPending();
}
