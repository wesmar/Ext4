/**
 * mcb.c - MCB name cache: per-volume hash of names, parent/child links.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "core_internal.h"

BOOLEAN
Ext2BuildName(
    IN OUT PUNICODE_STRING  Target,
    IN PUNICODE_STRING      File,
    IN PUNICODE_STRING      Parent
)
{
    USHORT  Length = 0;
    USHORT  ParentLen = 0;
    BOOLEAN bBackslash = TRUE;

    /* free the original buffer */
    if (Target->Buffer) {
        DEC_MEM_COUNT(PS_MCB_NAME, Target->Buffer, Target->MaximumLength);
        Ext2FreePool(Target->Buffer, EXT2_FNAME_MAGIC);
        Target->Length = Target->MaximumLength = 0;
    }

    /* check the parent directory's name and backslash */
    if (Parent && Parent->Buffer && Parent->Length > 0) {
        ParentLen = Parent->Length / sizeof(WCHAR);
        if (Parent->Buffer[ParentLen - 1] == L'\\') {
            bBackslash = FALSE;
        }
    }

    if (Parent == NULL || File->Buffer[0] == L'\\') {
        /* must be root inode */
        ASSERT(ParentLen == 0);
        bBackslash = FALSE;
    }

    /* allocate and initialize new name buffer */
    Length  = File->Length;
    Length += (ParentLen + (bBackslash ? 1 : 0)) * sizeof(WCHAR);

    Target->Buffer = Ext2AllocatePool(
                         PagedPool,
                         Length + 2,
                         EXT2_FNAME_MAGIC
                     );

    if (!Target->Buffer) {
        DEBUG(DL_ERR, ( "Ex2BuildName: failed to allocate name bufer.\n"));
        return FALSE;
    }
    RtlZeroMemory(Target->Buffer, Length + 2);

    if (ParentLen) {
        RtlCopyMemory(&Target->Buffer[0],
                      Parent->Buffer,
                      ParentLen * sizeof(WCHAR));
    }

    if (bBackslash) {
        Target->Buffer[ParentLen++] = L'\\';
    }

    RtlCopyMemory( &Target->Buffer[ParentLen],
                   File->Buffer,
                   File->Length);

    INC_MEM_COUNT(PS_MCB_NAME, Target->Buffer, Length + 2);
    Target->Length = Length;
    Target->MaximumLength = Length + 2;

    return TRUE;
}

PEXT2_MCB
Ext2AllocateMcb (
    IN PEXT2_VCB        Vcb,
    IN PUNICODE_STRING  FileName,
    IN PUNICODE_STRING  Parent,
    IN ULONG            FileAttr
)
{
    UNREFERENCED_PARAMETER(Vcb);
    PEXT2_MCB   Mcb = NULL;

    /* the name cache is over its high water mark: trim it */
    Ext2McbReaperWake();

    /* allocate Mcb from LookasideList */
    Mcb = (PEXT2_MCB) (ExAllocateFromNPagedLookasideList(
                           &(Ext2Global->Ext2McbLookasideList)));

    if (Mcb == NULL) {
        return NULL;
    }

    /* initialize Mcb header; the inode side (Icb) is attached by the
       caller with Ext2AttachIcb once the inode number is known */
    RtlZeroMemory(Mcb, sizeof(EXT2_MCB));
    Mcb->Identifier.Type = EXT2MCB;
    Mcb->Identifier.Size = sizeof(EXT2_MCB);
    Mcb->FileAttr = FileAttr;
    InitializeListHead(&Mcb->IcbLink);
    InitializeListHead(&Mcb->Children);
    InitializeListHead(&Mcb->Sibling);
    InitializeListHead(&Mcb->Hash);

    /* initialize Mcb names */
    if (FileName) {

#if EXT2_DEBUG
        if ( FileName->Length == 2 &&
                FileName->Buffer[0] == L'\\') {
            DEBUG(DL_RES, ( "Ext2AllocateMcb: Root Mcb is to be created !\n"));
        }

        if ( FileName->Length == 2 &&
                FileName->Buffer[0] == L'.') {
            DbgBreak();
        }

        if ( FileName->Length == 4 &&
                FileName->Buffer[0] == L'.' &&
                FileName->Buffer[1] == L'.' ) {
            DbgBreak();
        }
#endif

        if (( FileName->Length >= 4 && FileName->Buffer[0] == L'.') &&
                ((FileName->Length == 4 && FileName->Buffer[1] != L'.') ||
                 FileName->Length >= 6 )) {
            SetFlag(Mcb->FileAttr, FILE_ATTRIBUTE_HIDDEN);
        }

        if (!Ext2BuildName(&Mcb->ShortName, FileName, NULL)) {
            goto errorout;
        }
        if (!Ext2BuildName(&Mcb->FullName, FileName, Parent)) {
            goto errorout;
        }
    }

    INC_MEM_COUNT(PS_MCB, Mcb, sizeof(EXT2_MCB));
    DEBUG(DL_INF, ( "Ext2AllocateMcb: Mcb %wZ created.\n", &Mcb->FullName));

    return Mcb;

errorout:

    if (Mcb) {

        if (Mcb->ShortName.Buffer) {
            DEC_MEM_COUNT(PS_MCB_NAME, Mcb->ShortName.Buffer,
                          Mcb->ShortName.MaximumLength);
            Ext2FreePool(Mcb->ShortName.Buffer, EXT2_FNAME_MAGIC);
        }

        if (Mcb->FullName.Buffer) {
            DEC_MEM_COUNT(PS_MCB_NAME, Mcb->FullName.Buffer,
                          Mcb->FullName.MaximumLength);
            Ext2FreePool(Mcb->FullName.Buffer, EXT2_FNAME_MAGIC);
        }

        ExFreeToNPagedLookasideList(&(Ext2Global->Ext2McbLookasideList), Mcb);
    }

    return NULL;
}

VOID
Ext2FreeMcb (IN PEXT2_VCB Vcb, IN PEXT2_MCB Mcb)
{

    ASSERT(Mcb != NULL);

    ASSERT((Mcb->Identifier.Type == EXT2MCB) &&
           (Mcb->Identifier.Size == sizeof(EXT2_MCB)));

    if ((Mcb->Identifier.Type != EXT2MCB) ||
            (Mcb->Identifier.Size != sizeof(EXT2_MCB))) {
        return;
    }

    DEBUG(DL_INF, ( "Ext2FreeMcb: Mcb %wZ will be freed.\n", &Mcb->FullName));

    if (IsMcbSymLink(Mcb) && Mcb->Target) {
        Ext2DerefMcb(Mcb->Target);
    }

    if (Mcb->Icb) {
        Ext2DetachIcb(Vcb, Mcb);
    }

    if (Mcb->ShortName.Buffer) {
        DEC_MEM_COUNT(PS_MCB_NAME, Mcb->ShortName.Buffer,
                      Mcb->ShortName.MaximumLength);
        Ext2FreePool(Mcb->ShortName.Buffer, EXT2_FNAME_MAGIC);
    }

    if (Mcb->FullName.Buffer) {
        DEC_MEM_COUNT(PS_MCB_NAME, Mcb->FullName.Buffer,
                      Mcb->FullName.MaximumLength);
        Ext2FreePool(Mcb->FullName.Buffer, EXT2_FNAME_MAGIC);
    }

    /* free dentry */
    if (Mcb->de) {
        Ext2FreeEntry(Mcb->de);
    }

    Mcb->Identifier.Type = 0;
    Mcb->Identifier.Size = 0;

    ExFreeToNPagedLookasideList(&(Ext2Global->Ext2McbLookasideList), Mcb);
    DEC_MEM_COUNT(PS_MCB, Mcb, sizeof(EXT2_MCB));
}

/*
 * A (referenced) name of inode Ino, if the inode is cached. ext4 has no
 * way back from an inode to its names, so an open by file id works for
 * what the driver has seen: every directory on a path that was looked up,
 * every file with a handle or a recent lookup - which is what
 * FindFirstFileName / fsutil hardlink need after FileHardLinkInformation.
 */
PEXT2_MCB
Ext2LookupMcbByInode(IN PEXT2_VCB Vcb, IN ULONG Ino)
{
    PEXT2_ICB   Icb;
    PEXT2_MCB   Found = NULL;
    PLIST_ENTRY List;
    KIRQL       Irql;

    KeAcquireSpinLock(&Vcb->IcbLock, &Irql);
    Icb = Ext2FindIcbLocked(Vcb, Ino);
    if (Icb) {
        for (List = Icb->Names.Flink; List != &Icb->Names; List = List->Flink) {
            PEXT2_MCB Name = CONTAINING_RECORD(List, EXT2_MCB, IcbLink);
            if (!IsFileDeleted(Name)) {
                Found = Name;
                Ext2ReferMcb(Found);
                break;
            }
        }
    }
    KeReleaseSpinLock(&Vcb->IcbLock, Irql);

    return Found;
}

PEXT2_MCB
Ext2SearchMcb(
    PEXT2_VCB           Vcb,
    PEXT2_MCB           Parent,
    PUNICODE_STRING     FileName
)
{
    BOOLEAN   LockAcquired = FALSE;
    PEXT2_MCB Mcb = NULL;

    __try {
        ExAcquireResourceSharedLite(&Vcb->McbLock, TRUE);
        LockAcquired = TRUE;
        Mcb = Ext2SearchMcbWithoutLock(Parent, FileName);
    } __finally {
        if (LockAcquired) {
            ExReleaseResourceLite(&Vcb->McbLock);
        }
    }

    return Mcb;
}

PEXT2_MCB
Ext2SearchMcbWithoutLock(
    PEXT2_MCB       Parent,
    PUNICODE_STRING FileName
)
{
    PEXT2_MCB TmpMcb = NULL;
    PEXT2_MCB Held = Parent;    /* the node referenced here and released on
                                   exit; Parent moves on to a link's target */

    DEBUG(DL_RES, ("Ext2SearchMcb: %wZ\n", FileName));

    __try {

        Ext2ReferMcb(Held);

        if (Ext2IsDot(FileName)) {
            TmpMcb = Parent;
            Ext2ReferMcb(Parent);
            __leave;
        }

        if (Ext2IsDotDot(FileName)) {
            if (IsMcbRoot(Parent)) {
                TmpMcb = Parent;
            } else {
                TmpMcb = Parent->Parent;
            }
            if (TmpMcb) {
                Ext2ReferMcb(TmpMcb);
            }
            __leave;
        }

        if (IsMcbSymLink(Parent)) {
            if (Parent->Target) {
                Parent = Parent->Target;
                ASSERT(!IsMcbSymLink(Parent));
            } else {
                TmpMcb = NULL;
                __leave;
            }
        }

        /* the volume's name hash: names of one directory that hash alike
           share a bucket, so a hit costs one short chain walk however big
           the directory (children used to be a linear list) */
        {
            PEXT2_VCB   Vcb = (PEXT2_VCB)Parent->Inode->i_sb->s_priv;
            ULONG       Hash = Ext2HashMcbName(FileName);
            PLIST_ENTRY Head = &Vcb->McbHash[Hash % EXT2_MCB_HASH_BUCKETS];
            PLIST_ENTRY List;

            for (List = Head->Flink; List != Head; List = List->Flink) {
                PEXT2_MCB Mcb = CONTAINING_RECORD(List, EXT2_MCB, Hash);
                if (Mcb->Parent == Parent && Mcb->NameHash == Hash &&
                    RtlEqualUnicodeString(&Mcb->ShortName, FileName, TRUE)) {
                    Ext2ReferMcb(Mcb);
                    /* second chance for the reaper (no list surgery here:
                       Ext2SearchMcb holds the lock shared) */
                    SetLongFlag(Mcb->Flags, MCB_ACCESSED);
                    TmpMcb = Mcb;
                    break;
                }
            }
        }

    } __finally {

        Ext2DerefMcb(Held);
    }

    return TmpMcb;
}

/* case-insensitive hash of a name, the key of Vcb->McbHash */
ULONG
Ext2HashMcbName(IN PUNICODE_STRING Name)
{
    ULONG Hash = 0;

    if (!NT_SUCCESS(RtlHashUnicodeString(Name, TRUE,
                                         HASH_STRING_ALGORITHM_X65599, &Hash))) {
        Hash = Name->Length;
    }
    return Hash;
}

VOID
Ext2InsertMcb (
    PEXT2_VCB Vcb,
    PEXT2_MCB Parent,
    PEXT2_MCB Child
)
{
    BOOLEAN     LockAcquired = FALSE;

    __try {

        ExAcquireResourceExclusiveLite(
            &Vcb->McbLock,
            TRUE );
        LockAcquired = TRUE;

        /* use it's target if it's a symlink */
        if (IsMcbSymLink(Parent)) {
            Parent = Parent->Target;
            ASSERT(!IsMcbSymLink(Parent));
        }

        if (IsFlagOn(Child->Flags, MCB_ENTRY_TREE)) {

            /* already attached in the tree */
            DEBUG(DL_ERR, ( "Ext2InsertMcb: Child Mcb is alreay attached.\n"));
            DbgBreak();

        } else {

            Child->NameHash = Ext2HashMcbName(&Child->ShortName);
            InsertHeadList(&Parent->Children, &Child->Sibling);
            InsertHeadList(&Vcb->McbHash[Child->NameHash % EXT2_MCB_HASH_BUCKETS],
                           &Child->Hash);
            Child->Parent = Parent;
            Child->de->d_parent = Parent->de;
            Ext2ReferMcb(Parent);
            SetLongFlag(Child->Flags, MCB_ENTRY_TREE);
        }

    } __finally {

        if (LockAcquired) {
            ExReleaseResourceLite(&Vcb->McbLock);
        }
    }
}

BOOLEAN
Ext2RemoveMcb (
    PEXT2_VCB Vcb,
    PEXT2_MCB Mcb
)
{
    BOOLEAN     LockAcquired = FALSE;

    __try {

        ExAcquireResourceExclusiveLite(&Vcb->McbLock, TRUE);
        LockAcquired = TRUE;

        if (Mcb->Parent) {

            if (IsFlagOn(Mcb->Flags, MCB_ENTRY_TREE)) {
                DEBUG(DL_RES, ("Mcb %p %wZ removed from Mcb %p %wZ\n", Mcb,
                               &Mcb->FullName, Mcb->Parent, &Mcb->Parent->FullName));
                RemoveEntryList(&Mcb->Sibling);
                InitializeListHead(&Mcb->Sibling);
                RemoveEntryList(&Mcb->Hash);
                InitializeListHead(&Mcb->Hash);
                Ext2DerefMcb(Mcb->Parent);
                ClearLongFlag(Mcb->Flags, MCB_ENTRY_TREE);
            } else {
                DbgBreak();
            }
            Mcb->Parent = NULL;
            Mcb->de->d_parent = NULL;
        }

    } __finally {

        if (LockAcquired) {
            ExReleaseResourceLite(&Vcb->McbLock);
        }
    }

    return TRUE;
}

VOID
Ext2CleanupAllMcbs(PEXT2_VCB Vcb)
{
    BOOLEAN   LockAcquired = FALSE;
    PEXT2_MCB Mcb = NULL;

    __try {

        ExAcquireResourceExclusiveLite(
            &Vcb->McbLock,
            TRUE );
        LockAcquired = TRUE;

        /* The reaper gives a recently looked-up name a second chance: it
           clears MCB_ACCESSED and moves on. At dismount there is no later
           pass to take that chance - a pass that found only such names
           reaped none, ended this loop, and every one of them leaked with
           its Icb, dentry and name buffers (~750 bytes of nonpaged and
           paged pool per file deleted since the mount, measured). The flags go first, so each pass frees
           whatever nothing references; the next one takes the directories
           their children were holding. */
        {
            PLIST_ENTRY List;
            for (List = Vcb->McbList.Flink; List != &Vcb->McbList; List = List->Flink) {
                ClearLongFlag(CONTAINING_RECORD(List, EXT2_MCB, Link)->Flags, MCB_ACCESSED);
            }
        }

        for (;;) {

            LIST_ENTRY  Reaped;

            InitializeListHead(&Reaped);
            if (Ext2FirstUnusedMcb(Vcb, TRUE, Vcb->NumOfMcb, &Reaped) == 0) {
                break;
            }
            while (!IsListEmpty(&Reaped)) {
                Mcb = CONTAINING_RECORD(RemoveHeadList(&Reaped), EXT2_MCB, Link);
                /* a link's reference keeps its target alive, so the target
                   is still here: Ext2FreeMcb drops that reference and the
                   next pass frees the target (clearing Target instead left
                   every linked-to name referenced for good) */
                Ext2FreeMcb(Vcb, Mcb);
            }
        }
        /* whatever is still listed holds a reference nobody will drop: a
           leak, reported by name so that it can be traced (silent when the
           counts are right) */
        if (!IsListEmpty(&Vcb->McbList)) {
            PLIST_ENTRY List;
            ULONG       Left = 0;

            for (List = Vcb->McbList.Flink; List != &Vcb->McbList; List = List->Flink) {
                Mcb = CONTAINING_RECORD(List, EXT2_MCB, Link);
                if (Mcb == Vcb->McbTree) {
                    continue;
                }
                if (Left++ < EXT4_LEAK_REPORT_MAX) {
                    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                               "ext4: name left behind: %wZ refs=%d flags=%08x ino=%u\n",
                               &Mcb->FullName, Mcb->Refercount, Mcb->Flags,
                               Mcb->Inode ? Mcb->Inode->i_ino : 0);
                }
            }
            if (Left) {
                DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                           "ext4: %u names left behind at dismount (counted %d)\n",
                           Left, Vcb->NumOfMcb);
            }
        }

        Ext2FreeMcb(Vcb, Vcb->McbTree);
        Vcb->McbTree = NULL;

        if (Vcb->NumOfIcb != 0) {
            DEBUG(DL_ERR, ("Ext2CleanupAllMcbs: %u inode nodes left behind\n",
                           Vcb->NumOfIcb));
            DbgBreak();
        }

    } __finally {

        if (LockAcquired) {
            ExReleaseResourceLite(&Vcb->McbLock);
        }
    }
}

/* Link Mcb to tail of Vcb->McbList queue */

VOID
Ext2LinkTailMcb(PEXT2_VCB Vcb, PEXT2_MCB Mcb)
{
    if (Mcb->Inode->i_ino == EXT2_ROOT_INO) {
        return;
    }

    ExAcquireResourceExclusiveLite(&Vcb->McbLock, TRUE);

    if (IsFlagOn(Mcb->Flags, MCB_VCB_LINK)) {
        DEBUG(DL_RES, ( "Ext2LinkTailMcb: %wZ already linked.\n",
                        &Mcb->FullName));
    } else {
        InsertTailList(&Vcb->McbList, &Mcb->Link);
        SetLongFlag(Mcb->Flags, MCB_VCB_LINK);
        Ext2ReferXcb(&Vcb->NumOfMcb);
    }

    ExReleaseResourceLite(&Vcb->McbLock);
}

/* Link Mcb to head of Vcb->McbList queue */

VOID
Ext2LinkHeadMcb(PEXT2_VCB Vcb, PEXT2_MCB Mcb)
{
    if (Mcb->Inode->i_ino == EXT2_ROOT_INO) {
        return;
    }

    ExAcquireResourceExclusiveLite(&Vcb->McbLock, TRUE);

    if (!IsFlagOn(Mcb->Flags, MCB_VCB_LINK)) {
        InsertHeadList(&Vcb->McbList, &Mcb->Link);
        SetLongFlag(Mcb->Flags, MCB_VCB_LINK);
        Ext2ReferXcb(&Vcb->NumOfMcb);
    } else {
        DEBUG(DL_RES, ( "Ext2LinkHeadMcb: %wZ already linked.\n",
                        &Mcb->FullName));
    }
    ExReleaseResourceLite(&Vcb->McbLock);
}

/* Unlink Mcb from Vcb->McbList queue */

VOID
Ext2UnlinkMcb(PEXT2_VCB Vcb, PEXT2_MCB Mcb)
{
    if (Mcb->Inode->i_ino == EXT2_ROOT_INO) {
        return;
    }

    ExAcquireResourceExclusiveLite(&Vcb->McbLock, TRUE);

    if (IsFlagOn(Mcb->Flags, MCB_VCB_LINK)) {
        RemoveEntryList(&(Mcb->Link));
        ClearLongFlag(Mcb->Flags, MCB_VCB_LINK);
        Ext2DerefXcb(&Vcb->NumOfMcb);
    } else {
        DEBUG(DL_RES, ( "Ext2UnlinkMcb: %wZ already unlinked.\n",
                        &Mcb->FullName));
    }
    ExReleaseResourceLite(&Vcb->McbLock);
}
