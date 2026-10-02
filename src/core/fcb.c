/**
 * fcb.c - FCB and CCB life cycle, in-memory inode allocation.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "core_internal.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2AllocateInode)
#pragma alloc_text(PAGE, Ext2DestroyInode)
#endif

PEXT2_FCB
Ext2AllocateFcb (
    IN PEXT2_VCB   Vcb,
    IN PEXT2_MCB   Mcb
)
{
    PEXT2_FCB Fcb;

    ASSERT(ExIsResourceAcquiredExclusiveLite(&Vcb->FcbLock));

    Fcb = (PEXT2_FCB) ExAllocateFromNPagedLookasideList(
              &(Ext2Global->Ext2FcbLookasideList));

    if (!Fcb) {
        return NULL;
    }

    RtlZeroMemory(Fcb, sizeof(EXT2_FCB));
    Fcb->Identifier.Type = EXT2FCB;
    Fcb->Identifier.Size = sizeof(EXT2_FCB);

    FsRtlInitializeOplock(&Fcb->Oplock);
    FsRtlInitializeFileLock (
        &Fcb->FileLockAnchor,
        NULL,
        NULL );

    Fcb->OpenHandleCount = 0;
    Fcb->ReferenceCount = 0;
    InitializeListHead(&Fcb->CcbList);
    Fcb->Vcb = Vcb;
    Fcb->Inode = Mcb->Inode;

    ASSERT(Mcb->Icb->Fcb == NULL);
    Ext2ReferMcb(Mcb);
    Fcb->Mcb = Mcb;
    Mcb->Icb->Fcb = Fcb;

    DEBUG(DL_RES, ("Ext2AllocateFcb: Fcb %p created: %wZ.\n",
                   Fcb, &Fcb->Mcb->FullName));

    Fcb->Header.NodeTypeCode = (CSHORT)(EXT2FCB & 0xFFFF);
    Fcb->Header.NodeByteSize = sizeof(EXT2_FCB);
    Fcb->Header.IsFastIoPossible = FastIoIsNotPossible;
    Fcb->Header.Resource = &(Fcb->MainResource);
    Fcb->Header.PagingIoResource = &(Fcb->PagingIoResource);

    Fcb->Header.FileSize.QuadPart = Mcb->Inode->i_size;
    Fcb->Header.ValidDataLength.QuadPart = Mcb->Inode->i_size;
    Fcb->Header.AllocationSize.QuadPart = CEILING_ALIGNED(ULONGLONG,
                                          Fcb->Header.FileSize.QuadPart, (ULONGLONG)Vcb->BlockSize);

	ExInitializeFastMutex(&Fcb->Mutex);
	FsRtlSetupAdvancedHeader(&Fcb->Header, &Fcb->Mutex);

    Fcb->SectionObject.DataSectionObject = NULL;
    Fcb->SectionObject.SharedCacheMap = NULL;
    Fcb->SectionObject.ImageSectionObject = NULL;

    ExInitializeResourceLite(&(Fcb->MainResource));
    ExInitializeResourceLite(&(Fcb->PagingIoResource));

    Ext2InsertFcb(Vcb, Fcb);

    INC_MEM_COUNT(PS_FCB, Fcb, sizeof(EXT2_FCB));

    return Fcb;
}

/*
 * The Fcb of the directory Mcb, referenced for the caller (Ext2ReleaseFcb),
 * made if there is none yet. It is there nearly always - the directory of
 * the previous create or delete - and then FcbLock is only taken shared: a
 * reference taken under it cannot race the last release, which drops the
 * count to zero with the lock held exclusively (Ext2ReleaseFcb). Every
 * create and delete took it exclusively, and they queued on it.
 */
PEXT2_FCB
Ext2ReferDcb(IN PEXT2_VCB Vcb, IN PEXT2_MCB Mcb)
{
    PEXT2_FCB   Dcb;

    ExAcquireResourceSharedLite(&Vcb->FcbLock, TRUE);
    Dcb = Mcb->Icb->Fcb;
    if (Dcb) {
        Ext2ReferXcb(&Dcb->ReferenceCount);
    }
    ExReleaseResourceLite(&Vcb->FcbLock);

    if (Dcb == NULL) {
        ExAcquireResourceExclusiveLite(&Vcb->FcbLock, TRUE);
        Dcb = Mcb->Icb->Fcb;
        if (Dcb == NULL) {
            Dcb = Ext2AllocateFcb(Vcb, Mcb);
        }
        if (Dcb) {
            Ext2ReferXcb(&Dcb->ReferenceCount);
        }
        ExReleaseResourceLite(&Vcb->FcbLock);
    }

    return Dcb;
}

VOID
Ext2UnlinkFcb(IN PEXT2_FCB Fcb)
{
    PEXT2_VCB  Vcb = Fcb->Vcb;
    PEXT2_MCB  Mcb;

    /* the caller holds FcbLock exclusively, which guards Icb->Fcb; the
       name's own state has its own locks (Ext2RemoveMcb, the list lock) */
    Mcb = Fcb->Mcb;

    DEBUG(DL_INF, ("Ext2FreeFcb: Fcb (%p) to be unlinked: %wZ.\n",
                    Fcb, Mcb ? &Mcb->FullName : NULL));

    if ((Mcb != NULL) && 
        (Mcb->Identifier.Type == EXT2MCB) &&
        (Mcb->Identifier.Size == sizeof(EXT2_MCB))) {

        ASSERT (Mcb->Icb->Fcb == Fcb);
        if (IsMcbSpecialFile(Mcb) ||
            IsFileDeleted(Mcb)) {

            ASSERT(!IsRoot(Fcb));
            Ext2RemoveMcb(Vcb, Mcb);
            Mcb->Icb->Fcb = NULL;

            /* due for the reaper: oldest end first, then let go */
            Ext2MoveMcbToHead(Vcb, Mcb);
            Ext2DerefMcb(Mcb);

        } else {
            Mcb->Icb->Fcb = NULL;
            Ext2DerefMcb(Mcb);
        }
        Fcb->Mcb = NULL;
    }
}

VOID
Ext2FreeFcb (IN PEXT2_FCB Fcb)
{
    PEXT2_VCB   Vcb = Fcb->Vcb;

    __try {

        ASSERT((Fcb != NULL) && (Fcb->Identifier.Type == EXT2FCB) &&
               (Fcb->Identifier.Size == sizeof(EXT2_FCB)));
        ASSERT(0 == Fcb->ReferenceCount);

        FsRtlTeardownPerStreamContexts(&Fcb->Header);

        FsRtlUninitializeFileLock(&Fcb->FileLockAnchor);
        FsRtlUninitializeOplock(&Fcb->Oplock);
        ExDeleteResourceLite(&Fcb->MainResource);
        ExDeleteResourceLite(&Fcb->PagingIoResource);

        Fcb->Identifier.Type = 0;
        Fcb->Identifier.Size = 0;

        ExFreeToNPagedLookasideList(&(Ext2Global->Ext2FcbLookasideList), Fcb);
        DEC_MEM_COUNT(PS_FCB, Fcb, sizeof(EXT2_FCB));

        if (0 == Ext2DerefXcb(&Vcb->ReferenceCount)) {
            if (!IsMounted(Vcb) || IsDispending(Vcb)) {
                Ext2CheckDismount(NULL, Vcb, FALSE);
            }
        }

    } __finally {
    }
}

VOID
Ext2ReleaseFcb (IN PEXT2_FCB Fcb)
{
    PEXT2_VCB   Vcb = Fcb->Vcb;
    PEXT2_MCB   Mcb;
    BOOLEAN     Gone;
    LONG        Count;

    /* Any reference but the last goes without a lock. The last one goes
       under FcbLock: the reaper frees an Fcb whose count it sees at 0
       (under that lock), and an Fcb dropped to 0 before the lock was
       taken here could be freed - or moved to the reaper's own list -
       before it is moved below (a list entry removed twice, 0x139). */
    for (Count = Fcb->ReferenceCount; Count > 1; Count = Fcb->ReferenceCount) {
        if (InterlockedCompareExchange(&Fcb->ReferenceCount, Count - 1, Count) == Count) {
            return;
        }
    }

    ExAcquireResourceExclusiveLite(&Vcb->FcbLock, TRUE);
    if (0 != Ext2DerefXcb(&Fcb->ReferenceCount)) {
        /* an open took a new reference meanwhile */
        ExReleaseResourceLite(&Vcb->FcbLock);
        return;
    }
    ExAcquireResourceExclusiveLite(&Fcb->MainResource, TRUE);

    Mcb = Fcb->Mcb;
    RemoveEntryList(&Fcb->Next);

    Gone = IsFlagOn(Fcb->Flags, FCB_DELETE_PENDING) ||
           NULL == Mcb || IsFileDeleted(Mcb);
    if (Gone) {
        InsertHeadList(&Vcb->FcbList, &Fcb->Next);
        Fcb->TsDrop.QuadPart = 0;
    } else {
        InsertTailList(&Vcb->FcbList, &Fcb->Next);
        KeQuerySystemTime(&Fcb->TsDrop);
    }
    ExReleaseResourceLite(&Fcb->MainResource);
    ExReleaseResourceLite(&Vcb->FcbLock);

    /* Fcb may be freed from here on */
    if (Gone) {
        Ext2FcbGone(Vcb);
    }

    /* at once when the volume or the driver is going, or the cache has
       outgrown its limit; otherwise only a reaper without a deadline */
    Ext2ReaperKick(&Ext2Global->FcbReaper,
                   !IsMounted(Vcb) || IsDispending(Vcb) ||
                   Ext2Global->UnloadState != EXT2_UNLOAD_IDLE ||
                   (ULONG)Vcb->FcbCount > Ext2FcbHighWater());
}

/* Insert Fcb to Vcb->FcbList queue, with Vcb->FcbLock Acquired. */

VOID
Ext2InsertFcb(PEXT2_VCB Vcb, PEXT2_FCB Fcb)
{
    ASSERT(ExIsResourceAcquiredExclusiveLite(&Vcb->FcbLock));

    KeQuerySystemTime(&Fcb->TsDrop);
    Ext2ReferXcb(&Vcb->FcbCount);
    Ext2ReferXcb(&Vcb->ReferenceCount);
    InsertTailList(&Vcb->FcbList, &Fcb->Next);
}

PEXT2_CCB
Ext2AllocateCcb (ULONG Flags, PEXT2_MCB Mcb, PEXT2_MCB SymLink)
{
    PEXT2_CCB Ccb;

    Ccb = (PEXT2_CCB) (ExAllocateFromNPagedLookasideList(
                           &(Ext2Global->Ext2CcbLookasideList)));
    if (!Ccb) {
        return NULL;
    }

    DEBUG(DL_RES, ( "ExtAllocateCcb: Ccb created: %ph.\n", Ccb));

    RtlZeroMemory(Ccb, sizeof(EXT2_CCB));

    Ccb->Identifier.Type = EXT2CCB;
    Ccb->Identifier.Size = sizeof(EXT2_CCB);
    Ccb->Flags = Flags;

    /* the name the handle was opened through; it keeps the Mcb alive
       (and with it the Icb) for as long as the handle exists */
    Ccb->Mcb = Mcb;
    if (Mcb) {
        ASSERT(Mcb->Refercount > 0);
        Ext2ReferMcb(Mcb);
    }

    Ccb->SymLink = SymLink;
    if (SymLink) {
        ASSERT(SymLink->Refercount > 0);
        Ext2ReferMcb(SymLink);
        DEBUG(DL_INF, ( "ExtAllocateCcb: Ccb SymLink: %wZ.\n",
                        &Ccb->SymLink->FullName));
    }

    Ccb->DirectorySearchPattern.Length = 0;
    Ccb->DirectorySearchPattern.MaximumLength = 0;
    Ccb->DirectorySearchPattern.Buffer = 0;

    INC_MEM_COUNT(PS_CCB, Ccb, sizeof(EXT2_CCB));

    return Ccb;
}

VOID
Ext2FreeCcb (IN PEXT2_VCB Vcb, IN PEXT2_CCB Ccb)
{
    ASSERT(Ccb != NULL);
    ASSERT((Ccb->Identifier.Type == EXT2CCB) &&
           (Ccb->Identifier.Size == sizeof(EXT2_CCB)));

    DEBUG(DL_RES, ( "Ext2FreeCcb: Ccb = %ph.\n", Ccb));

    if (Ccb->SymLink) {
        DEBUG(DL_INF, ( "Ext2FreeCcb: Ccb SymLink: %wZ.\n",
                        &Ccb->SymLink->FullName));
        /* the link may have been demoted meanwhile (its target went, an
           open cleared Target): no target is as gone as a deleted one.
           Target is read under LinkLock: a demotion drops its reference. */
        BOOLEAN Gone;

        KeEnterCriticalRegion();
        ExAcquireResourceSharedLite(&Vcb->LinkLock, TRUE);
        Gone = Ccb->SymLink->Target == NULL || IsFileDeleted(Ccb->SymLink->Target);
        ExReleaseResourceLite(&Vcb->LinkLock);
        KeLeaveCriticalRegion();

        if (Gone) {
            Ext2MoveMcbToHead(Vcb, Ccb->SymLink);
        }
        Ext2DerefMcb(Ccb->SymLink);
    }

    if (Ccb->Mcb) {
        Ext2DerefMcb(Ccb->Mcb);
        Ccb->Mcb = NULL;
    }

    if (Ccb->DirectorySearchPattern.Buffer != NULL) {
        DEC_MEM_COUNT(PS_DIR_PATTERN, Ccb->DirectorySearchPattern.Buffer,
                      Ccb->DirectorySearchPattern.MaximumLength );
        Ext2FreePool(Ccb->DirectorySearchPattern.Buffer, EXT2_DIRSP_MAGIC);
    }

    ExFreeToNPagedLookasideList(&(Ext2Global->Ext2CcbLookasideList), Ccb);
    DEC_MEM_COUNT(PS_CCB, Ccb, sizeof(EXT2_CCB));
}

PEXT2_INODE
Ext2AllocateInode (PEXT2_VCB  Vcb)
{
    PVOID inode = NULL;

    inode = ExAllocateFromNPagedLookasideList(
                &(Vcb->InodeLookasideList));
    if (!inode) {
        return NULL;
    }

    RtlZeroMemory(inode, INODE_SIZE);

    DEBUG(DL_INF, ("ExtAllocateInode: Inode created: %ph.\n", inode));
    INC_MEM_COUNT(PS_EXT2_INODE, inode, INODE_SIZE);

    return inode;
}

VOID
Ext2DestroyInode (IN PEXT2_VCB Vcb, IN PEXT2_INODE inode)
{
    ASSERT(inode != NULL);

    DEBUG(DL_INF, ("Ext2FreeInode: Inode = %ph.\n", inode));

    ExFreeToNPagedLookasideList(&(Vcb->InodeLookasideList), inode);
    DEC_MEM_COUNT(PS_EXT2_INODE, inode, INODE_SIZE);
}
