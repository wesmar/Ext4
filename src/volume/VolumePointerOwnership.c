/**
 * VolumePointerOwnership.c - ownership of VPBs published by forced dismounts.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"

typedef struct _EXT2_SWAP_VPB {
    LIST_ENTRY      Link;
    PVPB            Old;
    PVPB            Swap;
    PDEVICE_OBJECT  Device;
    USHORT          OldPersistent;
} EXT2_SWAP_VPB, *PEXT2_SWAP_VPB;

/* IoAcquireVpbSpinLock is held by the caller. Reserve ownership before
   publishing Swap: failure leaves the original device/VPB pair intact.
   Keeping the real device referenced also covers surprise removal. */
BOOLEAN
Ext2TrackVpbSwap(IN PVPB Old, IN PVPB Swap)
{
    PEXT2_SWAP_VPB S;
    KIRQL Irql;

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
    KeAcquireSpinLock(&Ext2Global->SwapVpbLock, &Irql);
    InsertHeadList(&Ext2Global->SwapVpbList, &S->Link);
    KeReleaseSpinLock(&Ext2Global->SwapVpbLock, Irql);
    return TRUE;
}

static BOOLEAN
Ext2GiveVpbBack(IN PEXT2_SWAP_VPB S, IN BOOLEAN KeepPersistent)
{
    PVPB Old = S->Old, Swap = S->Swap;
    BOOLEAN Done = FALSE;
    KIRQL Irql;

    IoAcquireVpbSpinLock(&Irql);
    if (Old->RealDevice && Old->RealDevice->Vpb == Swap &&
        Old->ReferenceCount == 0 && Swap->ReferenceCount == 0 &&
        Swap->DeviceObject == NULL &&
        !FlagOn(Swap->Flags, VPB_MOUNTED | VPB_LOCKED)) {
        /* An intermediate VPB is still owned by another exchange. Do not
           let the I/O manager delete it between two reclaim attempts.
           Temporary persistence must not propagate to the final original. */
        Old->Flags = (USHORT)((Swap->Flags & ~VPB_PERSISTENT) | S->OldPersistent |
                             (KeepPersistent ? VPB_PERSISTENT : 0));
        Old->DeviceObject = NULL;
        Old->VolumeLabelLength = 0;
        Old->RealDevice->Vpb = Old;
        Done = TRUE;
    }
    IoReleaseVpbSpinLock(Irql);
    return Done;
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

/* Prepare-to-unload calls this with the global resource held, both VCB
   lists empty and no mount or VCB teardown in flight. No new swap can be
   published in that state. A busy pair stays owned, never discarded.
   Repeated passes also unwind nested swaps independently of list order. */
BOOLEAN
Ext2ReclaimVpbs(VOID)
{
    PLIST_ENTRY Link;
    ULONG Count = 0;
    BOOLEAN Progress;
    KIRQL Irql;

    KeAcquireSpinLock(&Ext2Global->SwapVpbLock, &Irql);
    for (Link = Ext2Global->SwapVpbList.Flink;
         Link != &Ext2Global->SwapVpbList; Link = Link->Flink) {
        Count++;
    }
    KeReleaseSpinLock(&Ext2Global->SwapVpbLock, Irql);

    do {
        ULONG Remaining = Count;
        Progress = FALSE;
        while (Remaining-- != 0) {
            PEXT2_SWAP_VPB S;
            BOOLEAN KeepPersistent = FALSE;
            KeAcquireSpinLock(&Ext2Global->SwapVpbLock, &Irql);
            S = CONTAINING_RECORD(RemoveHeadList(&Ext2Global->SwapVpbList),
                                 EXT2_SWAP_VPB, Link);
            for (Link = Ext2Global->SwapVpbList.Flink;
                 Link != &Ext2Global->SwapVpbList; Link = Link->Flink) {
                PEXT2_SWAP_VPB Other = CONTAINING_RECORD(Link, EXT2_SWAP_VPB, Link);
                if (Other->Swap == S->Old) {
                    KeepPersistent = TRUE;
                    break;
                }
            }
            KeReleaseSpinLock(&Ext2Global->SwapVpbLock, Irql);
            if (Ext2GiveVpbBack(S, KeepPersistent)) {
                Ext2FreePool(S->Swap, TAG_VPB);
                DEC_MEM_COUNT(PS_VPB, S->Swap, sizeof(VPB));
                ObDereferenceObject(S->Device);
                Ext2FreePool(S, TAG_VPB);
                Count--;
                Progress = TRUE;
            } else {
                KeAcquireSpinLock(&Ext2Global->SwapVpbLock, &Irql);
                InsertTailList(&Ext2Global->SwapVpbList, &S->Link);
                KeReleaseSpinLock(&Ext2Global->SwapVpbLock, Irql);
            }
        }
    } while (Count != 0 && Progress);

    return Count == 0;
}
