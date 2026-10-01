/**
 * icb.c - ICB table: one in-memory inode per inode number, shared by all hard links.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "core_internal.h"

/*
 * Icb: the in-memory inode, shared by every name of a hard-linked file
 *
 * Vcb->IcbTable hashes the Icbs of a volume by inode number. The table and
 * the reference counts are protected by Vcb->IcbLock (a spin lock, because
 * the Mcb reaper detaches names without holding Vcb->McbLock). Creation is
 * serialized by Vcb->McbLock, which every caller of Ext2AttachIcb holds
 * exclusively, so a lookup made there sees either no Icb or one whose inode
 * has already been loaded.
 */

VOID
Ext2InitializeIcbTable(IN PEXT2_VCB Vcb)
{
    ULONG i;

    KeInitializeSpinLock(&Vcb->IcbLock);
    for (i = 0; i < EXT2_ICB_BUCKETS; i++) {
        InitializeListHead(&Vcb->IcbTable[i]);
    }
    Vcb->NumOfIcb = 0;
}

PEXT2_ICB
Ext2FindIcbLocked(IN PEXT2_VCB Vcb, IN ULONG Ino)
{
    PLIST_ENTRY Head = &Vcb->IcbTable[Ino % EXT2_ICB_BUCKETS];
    PLIST_ENTRY List;

    for (List = Head->Flink; List != Head; List = List->Flink) {
        PEXT2_ICB Icb = CONTAINING_RECORD(List, EXT2_ICB, Link);
        if (Icb->Inode.i_ino == Ino) {
            return Icb;
        }
    }

    return NULL;
}

static VOID
Ext2FreeIcb(IN PEXT2_VCB Vcb, IN PEXT2_ICB Icb)
{
    UNREFERENCED_PARAMETER(Vcb);
    ASSERT(Icb->Refercount == 0);
    ASSERT(Icb->Fcb == NULL);
    ASSERT(IsListEmpty(&Icb->Names));

    if (FsRtlNumberOfRunsInLargeMcb(&Icb->Extents)) {
        DEBUG(DL_EXT, ("List data extents for inode %xh\n", Icb->Inode.i_ino));
        Ext2ListExtents(&Icb->Extents);
    }
    FsRtlUninitializeLargeMcb(&Icb->Extents);
    if (FsRtlNumberOfRunsInLargeMcb(&Icb->MetaExts)) {
        DEBUG(DL_EXT, ("List meta extents for inode %xh\n", Icb->Inode.i_ino));
        Ext2ListExtents(&Icb->MetaExts);
    }
    FsRtlUninitializeLargeMcb(&Icb->MetaExts);
    Ext4DirNamesFree(Icb);

    Icb->Identifier.Type = 0;
    Icb->Identifier.Size = 0;

    DEC_MEM_COUNT(PS_ICB, Icb, sizeof(EXT2_ICB));
    Ext2FreePool(Icb, EXT2_ICB_MAGIC);
}

/*
 * Attach Mcb to the Icb of inode Ino, creating the Icb when the inode is
 * not cached yet. On return Mcb->Icb / Mcb->Inode are set; the caller must
 * load the inode from disk (and set ICB_INODE_LOADED) unless the flag is
 * already on. Returns FALSE when out of memory.
 */
BOOLEAN
Ext2AttachIcb(IN PEXT2_VCB Vcb, IN PEXT2_MCB Mcb, IN ULONG Ino)
{
    PEXT2_ICB   Icb = NULL, Fresh = NULL;
    KIRQL       Irql;

    ASSERT(Mcb->Icb == NULL);
    ASSERT(ExIsResourceAcquiredExclusiveLite(&Vcb->McbLock));

    KeAcquireSpinLock(&Vcb->IcbLock, &Irql);
    Icb = Ext2FindIcbLocked(Vcb, Ino);
    KeReleaseSpinLock(&Vcb->IcbLock, Irql);

    if (Icb == NULL) {

        Fresh = (PEXT2_ICB) Ext2AllocatePool(NonPagedPool, sizeof(EXT2_ICB), EXT2_ICB_MAGIC);
        if (Fresh == NULL) {
            return FALSE;
        }
        RtlZeroMemory(Fresh, sizeof(EXT2_ICB));
        Fresh->Identifier.Type = EXT2ICB;
        Fresh->Identifier.Size = sizeof(EXT2_ICB);
        InitializeListHead(&Fresh->Names);
        Fresh->Inode.i_ino = Ino;
        Fresh->Inode.i_sb = &Vcb->sb;

        /* it will raise an exception if failed */
        __try {
            FsRtlInitializeLargeMcb(&Fresh->Extents, NonPagedPool);
            FsRtlInitializeLargeMcb(&Fresh->MetaExts, NonPagedPool);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            DbgBreak();
            Ext2FreePool(Fresh, EXT2_ICB_MAGIC);
            return FALSE;
        }
        INC_MEM_COUNT(PS_ICB, Fresh, sizeof(EXT2_ICB));

        KeAcquireSpinLock(&Vcb->IcbLock, &Irql);
        Icb = Ext2FindIcbLocked(Vcb, Ino);
        if (Icb == NULL) {
            Icb = Fresh;
            Fresh = NULL;
            InsertHeadList(&Vcb->IcbTable[Ino % EXT2_ICB_BUCKETS], &Icb->Link);
            Vcb->NumOfIcb++;
        }
    } else {
        KeAcquireSpinLock(&Vcb->IcbLock, &Irql);
    }

    Icb->Refercount++;
    InsertTailList(&Icb->Names, &Mcb->IcbLink);
    if (Icb->Inode.i_priv == NULL) {
        Icb->Inode.i_priv = Mcb;
    }
    Mcb->Icb = Icb;
    Mcb->Inode = &Icb->Inode;
    KeReleaseSpinLock(&Vcb->IcbLock, Irql);

    if (Fresh) {
        /* lost the race (cannot happen under McbLock, but stay safe) */
        Fresh->Refercount = 0;
        Ext2FreeIcb(Vcb, Fresh);
    }

    DEBUG(DL_INF, ("Ext2AttachIcb: %wZ -> inode %xh (%s, refs %u)\n",
                   &Mcb->FullName, Ino,
                   IsFlagOn(Icb->Flags, ICB_INODE_LOADED) ? "cached" : "new",
                   Icb->Refercount));

    return TRUE;
}

/* Take Mcb off its Icb; the Icb goes away with its last name. */
VOID
Ext2DetachIcb(IN PEXT2_VCB Vcb, IN PEXT2_MCB Mcb)
{
    PEXT2_ICB   Icb = Mcb->Icb;
    KIRQL       Irql;
    BOOLEAN     Free = FALSE;

    ASSERT(Icb != NULL);
    ASSERT((Icb->Identifier.Type == EXT2ICB) &&
           (Icb->Identifier.Size == sizeof(EXT2_ICB)));

    KeAcquireSpinLock(&Vcb->IcbLock, &Irql);

    RemoveEntryList(&Mcb->IcbLink);
    InitializeListHead(&Mcb->IcbLink);
    if (Icb->Inode.i_priv == Mcb) {
        Icb->Inode.i_priv = IsListEmpty(&Icb->Names) ? NULL :
                            CONTAINING_RECORD(Icb->Names.Flink, EXT2_MCB, IcbLink);
    }

    ASSERT(Icb->Refercount > 0);
    if (--Icb->Refercount == 0) {
        ASSERT(Icb->Fcb == NULL);
        if (!IsFlagOn(Icb->Flags, ICB_UNHASHED)) {
            RemoveEntryList(&Icb->Link);
            SetLongFlag(Icb->Flags, ICB_UNHASHED);
            Vcb->NumOfIcb--;
        }
        Free = TRUE;
    }

    Mcb->Icb = NULL;
    Mcb->Inode = NULL;
    KeReleaseSpinLock(&Vcb->IcbLock, Irql);

    if (Free) {
        Ext2FreeIcb(Vcb, Icb);
    }
}

/*
 * The on-disk inode has been freed: the file is gone for every handle
 * still holding this Icb (IsInodeDeleted), and nothing may find it by
 * number any more (the number can be handed out again while those
 * handles keep the old Icb alive).
 */
VOID
Ext2UnhashIcb(IN PEXT2_VCB Vcb, IN PEXT2_ICB Icb)
{
    KIRQL   Irql;

    KeAcquireSpinLock(&Vcb->IcbLock, &Irql);
    SetLongFlag(Icb->Flags, ICB_FILE_DELETED);
    if (!IsFlagOn(Icb->Flags, ICB_UNHASHED)) {
        RemoveEntryList(&Icb->Link);
        InitializeListHead(&Icb->Link);
        SetLongFlag(Icb->Flags, ICB_UNHASHED);
        Vcb->NumOfIcb--;
    }
    KeReleaseSpinLock(&Vcb->IcbLock, Irql);
}

/*
 * Copy the cached inode of Ino, if any. Directory listings use it so that a
 * name whose own Mcb is not cached still shows the live size of an open,
 * hard-linked file instead of the stale on-disk copy.
 */
BOOLEAN
Ext2PeekCachedInode(IN PEXT2_VCB Vcb, IN ULONG Ino, OUT struct inode *Inode)
{
    PEXT2_ICB   Icb;
    KIRQL       Irql;
    BOOLEAN     Found = FALSE;

    KeAcquireSpinLock(&Vcb->IcbLock, &Irql);
    Icb = Ext2FindIcbLocked(Vcb, Ino);
    if (Icb && IsFlagOn(Icb->Flags, ICB_INODE_LOADED)) {
        *Inode = Icb->Inode;
        Found = TRUE;
    }
    KeReleaseSpinLock(&Vcb->IcbLock, Irql);

    return Found;
}

/*
 * The directory entry of Mcb is gone while the inode lives on through
 * other names: move the name to the back of the Icb list and, if the Fcb
 * was reached through it, hand the Fcb to a name that still exists, so
 * that IsFileDeleted(Fcb->Mcb) keeps meaning "the inode is gone".
 * Called by Ext2DeleteFile with the Fcb's resources held exclusively; the
 * old name stays alive (its handle references it) for anyone who read the
 * pointer a moment earlier.
 */
VOID
Ext2NameUnlinked(IN PEXT2_VCB Vcb, IN PEXT2_MCB Mcb)
{
    PEXT2_ICB   Icb = Mcb->Icb;
    PEXT2_FCB   Fcb = Icb->Fcb;
    PEXT2_MCB   Live = NULL;
    PLIST_ENTRY List;
    KIRQL       Irql;

    ASSERT(IsFileDeleted(Mcb));

    KeAcquireSpinLock(&Vcb->IcbLock, &Irql);
    RemoveEntryList(&Mcb->IcbLink);
    InsertTailList(&Icb->Names, &Mcb->IcbLink);
    if (Fcb && Fcb->Mcb == Mcb) {
        for (List = Icb->Names.Flink; List != &Icb->Names; List = List->Flink) {
            PEXT2_MCB Name = CONTAINING_RECORD(List, EXT2_MCB, IcbLink);
            if (!IsFileDeleted(Name)) {
                Live = Name;
                break;
            }
        }
        if (Live) {
            Ext2ReferMcb(Live);
        }
    }
    KeReleaseSpinLock(&Vcb->IcbLock, Irql);

    if (Live) {
        DEBUG(DL_INF, ("Ext2NameUnlinked: Fcb %p moves from %wZ to %wZ\n",
                       Fcb, &Mcb->FullName, &Live->FullName));
        Fcb->Mcb = Live;
        Ext2DerefMcb(Mcb);
    }
}

/* A referenced name of Icb marked MCB_DELETE_PENDING and not yet gone,
   or NULL. Cleanup deletes the marked names one by one. */
PEXT2_MCB
Ext2NextPendingName(IN PEXT2_VCB Vcb, IN PEXT2_ICB Icb)
{
    PEXT2_MCB   Found = NULL;
    PLIST_ENTRY List;
    KIRQL       Irql;

    KeAcquireSpinLock(&Vcb->IcbLock, &Irql);
    for (List = Icb->Names.Flink; List != &Icb->Names; List = List->Flink) {
        PEXT2_MCB Name = CONTAINING_RECORD(List, EXT2_MCB, IcbLink);
        if (IsFlagOn(Name->Flags, MCB_DELETE_PENDING) && !IsFileDeleted(Name)) {
            Found = Name;
            Ext2ReferMcb(Found);
            break;
        }
    }
    KeReleaseSpinLock(&Vcb->IcbLock, Irql);

    return Found;
}
