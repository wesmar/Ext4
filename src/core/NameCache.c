/**
 * NameCache.c - MCB name cache: per-volume hash of names, parent/child links.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "core_internal.h"

/* Target becomes a copy of Name in a buffer of its own, terminated; what
   it held goes. A name is one component: the path is built when asked
   (Ext2GetFullName). */
BOOLEAN
Ext2BuildName(
    IN OUT PUNICODE_STRING  Target,
    IN PUNICODE_STRING      Name
)
{
    if (Target->Buffer) {
        DEC_MEM_COUNT(PS_MCB_NAME, Target->Buffer, Target->MaximumLength);
        Ext2FreePool(Target->Buffer, EXT2_FNAME_MAGIC);
        RtlZeroMemory(Target, sizeof(UNICODE_STRING));
    }

    if (Name->Length > MAXUSHORT - sizeof(WCHAR)) {
        return FALSE;
    }
    Target->Buffer = Ext2AllocatePool(PagedPool, Name->Length + sizeof(WCHAR), EXT2_FNAME_MAGIC);
    if (!Target->Buffer) {
        return FALSE;
    }
    RtlCopyMemory(Target->Buffer, Name->Buffer, Name->Length);
    Target->Buffer[Name->Length / sizeof(WCHAR)] = 0;
    Target->Length = Name->Length;
    Target->MaximumLength = Name->Length + sizeof(WCHAR);
    INC_MEM_COUNT(PS_MCB_NAME, Target->Buffer, Target->MaximumLength);

    return TRUE;
}
PEXT2_MCB
Ext2AllocateMcb (
    IN PEXT2_VCB        Vcb,
    IN PUNICODE_STRING  FileName,
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
    InitializeListHead(&Mcb->Hash);

    /* initialize Mcb names */
    if (FileName) {


        if (( FileName->Length >= 4 && FileName->Buffer[0] == L'.') &&
                ((FileName->Length == 4 && FileName->Buffer[1] != L'.') ||
                 FileName->Length >= 6 )) {
            SetFlag(Mcb->FileAttr, FILE_ATTRIBUTE_HIDDEN);
        }

        if (!Ext2BuildName(&Mcb->ShortName, FileName)) {
            ExFreeToNPagedLookasideList(&(Ext2Global->Ext2McbLookasideList), Mcb);
            return NULL;
        }
    }

    INC_MEM_COUNT(PS_MCB, Mcb, sizeof(EXT2_MCB));
    DEBUG(DL_INF, ( "Ext2AllocateMcb: Mcb %wZ created.\n", &Mcb->ShortName));

    return Mcb;
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

    DEBUG(DL_INF, ( "Ext2FreeMcb: Mcb %wZ will be freed.\n", &Mcb->ShortName));

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
 * Only names in the tree (or the root): the reaper takes a name out under
 * IcbLock once it has found it unreferenced, and frees it after.
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
            if (!IsFileDeleted(Name) &&
                (IsFlagOn(Name->Flags, MCB_ENTRY_TREE) || Ino == EXT2_ROOT_INO)) {
                Found = Name;
                Ext2ReferMcb(Found);
                break;
            }
        }
    }
    KeReleaseSpinLock(&Vcb->IcbLock, Irql);

    return Found;
}

/* ---------------------------------------------------------------- sizing */

/*
 * The name cache is one hash of (parent, name) per volume. Its chains are
 * guarded by stripes - chain i by stripe i & StripeMask - rather than by one
 * volume lock: every open, create and delete walks a chain, and with one
 * lock the threads of a volume took turns on it (the bulk of the waits
 * measured under 8 threads). Both sizes are powers of two, so chain and
 * stripe come from the hash by a mask, and both are derived, not set:
 *
 *  - chains: at least half the names the cache keeps (Ext2McbHighWater),
 *    so a walk passes two names on average when the cache is full, at 8
 *    bytes of table per name kept;
 *  - stripes: as many as the processors call for (Ext2StripeCount, where
 *    the reasoning is), never more than chains.
 */
NTSTATUS
Ext2InitializeNameCache(IN PEXT2_VCB Vcb)
{
    ULONG   Chains, Stripes, i;

    Chains = Ext2RoundUpPow2(max(Ext2McbHighWater() / EXT4_NAMES_PER_CHAIN, EXT4_MIN_CHAINS));
    Stripes = Ext2StripeCount(Chains);

    InitializeListHead(&Vcb->McbList);
    KeInitializeSpinLock(&Vcb->McbLruLock);
    ExInitializeFastMutex(&Vcb->McbReapLock);
    ExInitializePushLock(&Vcb->NameLock);

    Vcb->McbHash = Ext2AllocatePool(NonPagedPool, sizeof(LIST_ENTRY) * Chains, TAG_VPB);
    if (Vcb->McbHash == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    for (i = 0; i < Chains; i++) {
        InitializeListHead(&Vcb->McbHash[i]);
    }
    Vcb->McbHashMask = Chains - 1;

    Vcb->McbStripes = Ext2AllocateStripes(Stripes, &Vcb->McbStripePool);
    if (Vcb->McbStripes == NULL) {
        Ext2FreePool(Vcb->McbHash, TAG_VPB);
        Vcb->McbHash = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Vcb->McbStripeMask = Stripes - 1;

    ExInitializeResourceLite(&Vcb->LinkLock);
    return STATUS_SUCCESS;
}

VOID
Ext2DestroyNameCache(IN PEXT2_VCB Vcb)
{
    if (Vcb->McbStripes) {
        Ext2FreeStripes(Vcb->McbStripes, Vcb->McbStripeMask + 1, Vcb->McbStripePool);
        Vcb->McbStripes = NULL;
        Vcb->McbStripePool = NULL;
        ExDeleteResourceLite(&Vcb->LinkLock);
    }
    if (Vcb->McbHash) {
        Ext2FreePool(Vcb->McbHash, TAG_VPB);
        Vcb->McbHash = NULL;
    }
}

/* ---------------------------------------------------------------- stripes */

/* case-insensitive hash of a name, the key of Vcb->McbHash with its parent */
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

/*
 * The chain of (Parent, NameHash). The parent's address is spread by a
 * Fibonacci multiply and mixed in: directories full of the same names - one
 * per thread of a benchmark or a build - would otherwise share every chain,
 * and so every stripe.
 */
static __inline ULONG
Ext2McbChain(IN PEXT2_VCB Vcb, IN PEXT2_MCB Parent, IN ULONG NameHash)
{
    ULONG Spread = (ULONG)(((ULONG64)(ULONG_PTR)Parent * EXT4_FIBONACCI_64) >> 32);

    return (NameHash ^ Spread) & Vcb->McbHashMask;
}

/*
 * Stripes are push locks: a lookup's shared acquire and release are an
 * interlocked operation each, where a resource took a queued spin lock
 * twice - an eighth of a directory listing's time. A push lock is not
 * recursive, and no thread takes a stripe twice: a name goes into the tree
 * under the stripe its caller holds (Ext2InsertMcbLocked), and a rename
 * whose two places share a stripe takes it once (Ext2MoveMcb).
 */
PEX_PUSH_LOCK
Ext2AcquireNameStripe(IN PEXT2_VCB Vcb, IN PEXT2_MCB Parent, IN ULONG NameHash,
                      IN BOOLEAN Exclusive)
{
    PEX_PUSH_LOCK Stripe = &Vcb->McbStripes[Ext2McbChain(Vcb, Parent, NameHash) &
                                            Vcb->McbStripeMask].Lock;

    KeEnterCriticalRegion();
    if (Exclusive) {
        ExAcquirePushLockExclusive(Stripe);
    } else {
        ExAcquirePushLockShared(Stripe);
    }
    return Stripe;
}

VOID
Ext2ReleaseNameStripe(IN PEX_PUSH_LOCK Stripe, IN BOOLEAN Exclusive)
{
    if (Exclusive) {
        ExReleasePushLockExclusive(Stripe);
    } else {
        ExReleasePushLockShared(Stripe);
    }
    KeLeaveCriticalRegion();
}

/* ---------------------------------------------------------------- search */

/*
 * The cached name FileName (hash Hash) of directory Parent, referenced, or
 * NULL. The caller holds the name's stripe and Parent is no symlink.
 */
PEXT2_MCB
Ext2FindMcbLocked(IN PEXT2_VCB Vcb, IN PEXT2_MCB Parent,
                  IN PUNICODE_STRING FileName, IN ULONG Hash)
{
    PLIST_ENTRY Head = &Vcb->McbHash[Ext2McbChain(Vcb, Parent, Hash)];
    PLIST_ENTRY List;

    for (List = Head->Flink; List != Head; List = List->Flink) {
        PEXT2_MCB Mcb = CONTAINING_RECORD(List, EXT2_MCB, Hash);
        /* a deleted name may stay in the tree a moment after its entry is
           gone (Ext2DeleteFile takes it out once the directory is
           released): the disk answers for it */
        if (Mcb->Parent == Parent && Mcb->NameHash == Hash &&
            !IsFlagOn(Mcb->Flags, MCB_FILE_DELETED) &&
            RtlEqualUnicodeString(&Mcb->ShortName, FileName, TRUE)) {
            Ext2ReferMcb(Mcb);
            /* second chance for the reaper; no list surgery here */
            SetLongFlag(Mcb->Flags, MCB_ACCESSED);
            return Mcb;
        }
    }
    return NULL;
}

/*
 * The directory Mcb lives in, referenced, or NULL (the root, or a name out
 * of the tree). Mcb->Parent changes under Mcb's stripe, which depends on the
 * parent itself: the pointer is read, its stripe taken and the pointer read
 * again until the two agree. The pointer is only compared before that.
 */
PEXT2_MCB
Ext2ReferParent(IN PEXT2_VCB Vcb, IN PEXT2_MCB Mcb)
{
    PEXT2_MCB       Parent;
    PEX_PUSH_LOCK   Stripe;

    for (;;) {
        Parent = (PEXT2_MCB)ReadPointerAcquire((PVOID *)&Mcb->Parent);
        if (Parent == NULL) {
            return NULL;
        }
        Stripe = Ext2AcquireNameStripe(Vcb, Parent, Mcb->NameHash, FALSE);
        if (Mcb->Parent == Parent) {
            Ext2ReferMcb(Parent);
            Ext2ReleaseNameStripe(Stripe, FALSE);
            return Parent;
        }
        Ext2ReleaseNameStripe(Stripe, FALSE);
    }
}

/*
 * The target of symlink Mcb, referenced, or NULL (no symlink, or one whose
 * target has been dropped). Read under LinkLock: a demotion of the link
 * drops the reference that keeps its target alive.
 */
PEXT2_MCB
Ext2ReferLinkTarget(IN PEXT2_VCB Vcb, IN PEXT2_MCB Mcb)
{
    PEXT2_MCB Target = NULL;

    if (!IsMcbSymLink(Mcb)) {
        return NULL;
    }

    KeEnterCriticalRegion();
    ExAcquireResourceSharedLite(&Vcb->LinkLock, TRUE);
    if (IsMcbSymLink(Mcb) && Mcb->Target != NULL) {
        Target = Mcb->Target;
        ASSERT(!IsMcbSymLink(Target));
        Ext2ReferMcb(Target);
    }
    ExReleaseResourceLite(&Vcb->LinkLock);
    KeLeaveCriticalRegion();

    return Target;
}

/* symlink Mcb leads nowhere: its target was deleted, or dropped */
BOOLEAN
Ext2IsLinkDangling(IN PEXT2_VCB Vcb, IN PEXT2_MCB Mcb)
{
    PEXT2_MCB   Target = Ext2ReferLinkTarget(Vcb, Mcb);
    BOOLEAN     Dangling = (Target == NULL) || IsFileDeleted(Target);

    if (Target) {
        Ext2DerefMcb(Target);
    }
    return Dangling;
}

/*
 * The directory a lookup continues in: Dir itself, or the target of a
 * symlink to a directory. Referenced, or NULL when there is none (a
 * dangling link, a deleted target).
 */
PEXT2_MCB
Ext2ReferDirectory(IN PEXT2_VCB Vcb, IN PEXT2_MCB Dir)
{
    PEXT2_MCB Target;

    if (!IsMcbSymLink(Dir)) {
        Ext2ReferMcb(Dir);
        return Dir;
    }

    Target = Ext2ReferLinkTarget(Vcb, Dir);
    if (Target != NULL && IsFileDeleted(Target)) {
        Ext2DerefMcb(Target);
        Target = NULL;
    }
    return Target;
}

/*
 * The cached name FileName in directory Parent (a symlink stands for its
 * target), referenced, or NULL; "." and ".." are answered from the tree.
 */
PEXT2_MCB
Ext2SearchMcb(
    PEXT2_VCB           Vcb,
    PEXT2_MCB           Parent,
    PUNICODE_STRING     FileName
)
{
    PEXT2_MCB       Dir, Found;
    PEX_PUSH_LOCK   Stripe;
    ULONG           Hash;

    if (Ext2IsDot(FileName)) {
        Ext2ReferMcb(Parent);
        return Parent;
    }
    if (Ext2IsDotDot(FileName)) {
        if (IsMcbRoot(Parent)) {
            Ext2ReferMcb(Parent);
            return Parent;
        }
        return Ext2ReferParent(Vcb, Parent);
    }

    Dir = Ext2ReferDirectory(Vcb, Parent);
    if (Dir == NULL) {
        return NULL;
    }
    Hash = Ext2HashMcbName(FileName);
    Stripe = Ext2AcquireNameStripe(Vcb, Dir, Hash, FALSE);
    Found = Ext2FindMcbLocked(Vcb, Dir, FileName, Hash);
    Ext2ReleaseNameStripe(Stripe, FALSE);
    Ext2DerefMcb(Dir);

    return Found;
}

/* ---------------------------------------------------------------- tree */

/*
 * Child becomes the name Child->ShortName of directory Parent, which is no
 * symlink. The caller (Ext2InsertName) holds the name's stripe exclusively.
 */
VOID
Ext2InsertMcbLocked (
    PEXT2_VCB Vcb,
    PEXT2_MCB Parent,
    PEXT2_MCB Child
)
{
    ASSERT(!IsMcbSymLink(Parent));
    Child->NameHash = Ext2HashMcbName(&Child->ShortName);

    if (IsFlagOn(Child->Flags, MCB_ENTRY_TREE)) {
        /* already attached in the tree */
    } else {
        InsertHeadList(&Vcb->McbHash[Ext2McbChain(Vcb, Parent, Child->NameHash)],
                       &Child->Hash);
        Ext2ReferMcb(Parent);
        Child->Parent = Parent;
        Child->de->d_parent = Parent->de;
        SetLongFlag(Child->Flags, MCB_ENTRY_TREE);
    }
}

/* Take Mcb out of the tree; the caller holds its stripe and, for a name a
   path may be built through, NameLock exclusively (the reaper takes
   neither: a name nobody references is on no path - each name on one is
   referenced by the next, the last by the builder). Returns the parent,
   whose reference the caller drops, never under NameLock: once no path
   holds it, the reaper may free it. */
static PEXT2_MCB
Ext2UnhashMcbLocked(IN PEXT2_MCB Mcb)
{
    PEXT2_MCB Parent = Mcb->Parent;

    RemoveEntryList(&Mcb->Hash);
    InitializeListHead(&Mcb->Hash);
    ClearLongFlag(Mcb->Flags, MCB_ENTRY_TREE);
    Mcb->Parent = NULL;
    Mcb->de->d_parent = NULL;
    return Parent;
}

BOOLEAN
Ext2RemoveMcb (
    PEXT2_VCB Vcb,
    PEXT2_MCB Mcb
)
{
    PEXT2_MCB       Parent;
    PEX_PUSH_LOCK   Stripe;

    /* the stripe depends on the parent: read, lock, check (Ext2ReferParent) */
    for (;;) {
        Parent = (PEXT2_MCB)ReadPointerAcquire((PVOID *)&Mcb->Parent);
        if (Parent == NULL) {
            return TRUE;
        }
        Stripe = Ext2AcquireNameStripe(Vcb, Parent, Mcb->NameHash, TRUE);
        if (Mcb->Parent == Parent) {
            break;
        }
        Ext2ReleaseNameStripe(Stripe, TRUE);
    }

    /* a path may be built through this name right now (Ext2GetFullName) */
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&Vcb->NameLock);
    if (IsFlagOn(Mcb->Flags, MCB_ENTRY_TREE)) {
        Ext2UnhashMcbLocked(Mcb);
    } else {
        Mcb->Parent = NULL;
        Mcb->de->d_parent = NULL;
        Parent = NULL;
    }
    ExReleasePushLockExclusive(&Vcb->NameLock);
    KeLeaveCriticalRegion();
    Ext2ReleaseNameStripe(Stripe, TRUE);

    if (Parent) {
        Ext2DerefMcb(Parent);
    }
    return TRUE;
}

/*
 * A rename in the name cache: Mcb becomes NewName in directory NewParent in
 * one step. The stripes of both places are taken, then NameLock, so a path
 * built meanwhile is the old one or the new one; out of the tree and back,
 * it was a path without the directories above for a moment. NewName takes
 * the old name's place - the caller frees what comes back. A rename into a
 * symlink to a directory lands in the directory; with the target gone, the
 * name is only taken out of the tree.
 */
VOID
Ext2MoveMcb(IN PEXT2_VCB Vcb, IN PEXT2_MCB Mcb, IN PEXT2_MCB NewParent, IN OUT PUNICODE_STRING NewName)
{
    PEXT2_MCB       OldParent, Dir = NULL;
    PEX_PUSH_LOCK   From, To = NULL;
    ULONG           Hash = Ext2HashMcbName(NewName);
    UNICODE_STRING  Old;

    if (IsMcbSymLink(NewParent)) {
        Dir = Ext2ReferDirectory(Vcb, NewParent);
        NewParent = Dir;
    }

    /* the stripe of the old place depends on the parent: read, lock, check */
    for (;;) {
        OldParent = (PEXT2_MCB)ReadPointerAcquire((PVOID *)&Mcb->Parent);
        From = OldParent ? Ext2AcquireNameStripe(Vcb, OldParent, Mcb->NameHash, TRUE) : NULL;
        if (Mcb->Parent == OldParent) {
            break;
        }
        if (From) {
            Ext2ReleaseNameStripe(From, TRUE);
        }
    }
    if (NewParent) {
        /* both places on one stripe: it is held already */
        To = &Vcb->McbStripes[Ext2McbChain(Vcb, NewParent, Hash) & Vcb->McbStripeMask].Lock;
        To = To == From ? NULL : Ext2AcquireNameStripe(Vcb, NewParent, Hash, TRUE);
        Ext2ReferMcb(NewParent);
    }

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&Vcb->NameLock);
    if (IsFlagOn(Mcb->Flags, MCB_ENTRY_TREE)) {
        Ext2UnhashMcbLocked(Mcb);
    } else {
        /* out of the tree, the parent holds no reference of it */
        Mcb->Parent = NULL;
        Mcb->de->d_parent = NULL;
        OldParent = NULL;
    }
    Old = Mcb->ShortName;
    Mcb->ShortName = *NewName;
    *NewName = Old;
    Mcb->NameHash = Hash;
    if (NewParent) {
        InsertHeadList(&Vcb->McbHash[Ext2McbChain(Vcb, NewParent, Hash)], &Mcb->Hash);
        Mcb->Parent = NewParent;
        Mcb->de->d_parent = NewParent->de;
        SetLongFlag(Mcb->Flags, MCB_ENTRY_TREE);
    }
    ExReleasePushLockExclusive(&Vcb->NameLock);
    KeLeaveCriticalRegion();

    if (To) {
        Ext2ReleaseNameStripe(To, TRUE);
    }
    if (From) {
        Ext2ReleaseNameStripe(From, TRUE);
    }
    if (OldParent) {
        Ext2DerefMcb(OldParent);
    }
    if (Dir) {
        Ext2DerefMcb(Dir);
    }
}

/*
 * The path of Mcb from the volume root ("\dir\file", "\" for the root),
 * built from the names up the tree into a buffer the caller frees with
 * Ext2FreeFullName. A name out of the tree - a deleted file still open -
 * is "\name". *LastOffset, when asked, is where the last component starts
 * (bytes), as change notification wants it. Built under NameLock shared:
 * every name on the way is referenced by the next, the first by the
 * caller, and none changes its parent or its name meanwhile.
 */
NTSTATUS
Ext2GetFullName(IN PEXT2_VCB Vcb, IN PEXT2_MCB Mcb, OUT PUNICODE_STRING Name,
                OUT PUSHORT LastOffset OPTIONAL)
{
    PEXT2_MCB   Node;
    ULONG       Length = 0;
    ULONG       Pos;
    NTSTATUS    Status = STATUS_SUCCESS;

    RtlZeroMemory(Name, sizeof(UNICODE_STRING));

    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&Vcb->NameLock);

    for (Node = Mcb; Node != NULL && Node != Vcb->McbTree; Node = Node->Parent) {
        Length += sizeof(WCHAR) + Node->ShortName.Length;
    }
    if (Length == 0) {
        Length = sizeof(WCHAR);
    }

    if (Length + sizeof(WCHAR) > MAXUSHORT) {
        Status = STATUS_NAME_TOO_LONG;
    } else {
        Name->Buffer = Ext2AllocatePool(PagedPool, Length + sizeof(WCHAR), EXT2_FNAME_MAGIC);
        if (Name->Buffer == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    if (NT_SUCCESS(Status)) {
        Name->Length = (USHORT)Length;
        Name->MaximumLength = (USHORT)(Length + sizeof(WCHAR));
        Name->Buffer[Length / sizeof(WCHAR)] = 0;
        Name->Buffer[0] = L'\\';

        /* from the end: each name, the backslash before it */
        Pos = Length;
        for (Node = Mcb; Node != NULL && Node != Vcb->McbTree; Node = Node->Parent) {
            Pos -= Node->ShortName.Length;
            RtlCopyMemory((PUCHAR)Name->Buffer + Pos, Node->ShortName.Buffer, Node->ShortName.Length);
            Pos -= sizeof(WCHAR);
            Name->Buffer[Pos / sizeof(WCHAR)] = L'\\';
        }
        if (LastOffset) {
            *LastOffset = (Mcb == Vcb->McbTree) ? 0 :
                          (USHORT)(Length - Mcb->ShortName.Length);
        }
    }

    ExReleasePushLockShared(&Vcb->NameLock);
    KeLeaveCriticalRegion();
    return Status;
}

VOID
Ext2FreeFullName(IN OUT PUNICODE_STRING Name)
{
    if (Name->Buffer) {
        Ext2FreePool(Name->Buffer, EXT2_FNAME_MAGIC);
        RtlZeroMemory(Name, sizeof(UNICODE_STRING));
    }
}

/* ---------------------------------------------------------------- reaping order */

/* Link Mcb to the young end of Vcb->McbList */
VOID
Ext2LinkTailMcb(PEXT2_VCB Vcb, PEXT2_MCB Mcb)
{
    KIRQL Irql;

    if (Mcb->Inode->i_ino == EXT2_ROOT_INO) {
        return;
    }

    KeAcquireSpinLock(&Vcb->McbLruLock, &Irql);
    if (!IsFlagOn(Mcb->Flags, MCB_VCB_LINK)) {
        InsertTailList(&Vcb->McbList, &Mcb->Link);
        SetLongFlag(Mcb->Flags, MCB_VCB_LINK);
        Ext2ReferXcb(&Vcb->NumOfMcb);
    }
    KeReleaseSpinLock(&Vcb->McbLruLock, Irql);
}

/* Mcb is due: to the old end of Vcb->McbList, where the reaper starts */
VOID
Ext2MoveMcbToHead(PEXT2_VCB Vcb, PEXT2_MCB Mcb)
{
    KIRQL Irql;

    if (Mcb->Inode->i_ino == EXT2_ROOT_INO) {
        return;
    }

    KeAcquireSpinLock(&Vcb->McbLruLock, &Irql);
    if (IsFlagOn(Mcb->Flags, MCB_VCB_LINK)) {
        RemoveEntryList(&Mcb->Link);
    } else {
        SetLongFlag(Mcb->Flags, MCB_VCB_LINK);
        Ext2ReferXcb(&Vcb->NumOfMcb);
    }
    InsertHeadList(&Vcb->McbList, &Mcb->Link);
    KeReleaseSpinLock(&Vcb->McbLruLock, Irql);
}

/*
 * Take up to Number unused names off Vcb->McbList, oldest first, onto
 * Reaped (linked through Mcb->Link; the caller frees them). Bounded work:
 * at most a few times Number entries are looked at, whatever the size of
 * the cache. A name that was looked up since the last pass (MCB_ACCESSED)
 * gets a second chance and moves to the young end; so does one in use.
 *
 * A name is freed only by the reaper - one at a time per volume, under
 * McbReapLock - so a candidate stays valid while its stripe is taken. Its
 * reference count is final under that stripe (lookups take references
 * under it) and under IcbLock (Ext2LookupMcbByInode takes them there);
 * the name leaves the tree under both, so neither can revive it after.
 */
static ULONG
Ext2ReapUnusedMcbsLocked(PEXT2_VCB Vcb, ULONG Number, PLIST_ENTRY Reaped)
{
    PEXT2_MCB       Mcb, Parent;
    PEX_PUSH_LOCK   Stripe;
    ULONG           Budget, Count = 0, Hash;
    KIRQL       Irql;
    BOOLEAN     Dead;

    Budget = Number * EXT4_REAP_LOOK_FACTOR + EXT4_REAP_LOOK_EXTRA;
    if (Budget > (ULONG)Vcb->NumOfMcb) {
        Budget = Vcb->NumOfMcb;
    }

    while (Budget-- && Count < Number) {

        KeAcquireSpinLock(&Vcb->McbLruLock, &Irql);
        if (IsListEmpty(&Vcb->McbList)) {
            KeReleaseSpinLock(&Vcb->McbLruLock, Irql);
            break;
        }
        Mcb = CONTAINING_RECORD(Vcb->McbList.Flink, EXT2_MCB, Link);
        ASSERT(IsFlagOn(Mcb->Flags, MCB_VCB_LINK));

        /* an Fcb references the name it was reached through, so a count
           of zero also means no Fcb hangs off this name (the inode may
           still be open through another hard link); a directory with
           cached children is referenced by each of them */
        if (IsMcbRoot(Mcb) || Mcb->Refercount != 0 ||
            IsFlagOn(Mcb->Flags, MCB_ACCESSED)) {
            ClearLongFlag(Mcb->Flags, MCB_ACCESSED);
            RemoveEntryList(&Mcb->Link);
            InsertTailList(&Vcb->McbList, &Mcb->Link);
            KeReleaseSpinLock(&Vcb->McbLruLock, Irql);
            continue;
        }
        Parent = Mcb->Parent;
        Hash = Mcb->NameHash;
        KeReleaseSpinLock(&Vcb->McbLruLock, Irql);

        /* stripe, then IcbLock, then the list (spin locks innermost) */
        Stripe = Parent ? Ext2AcquireNameStripe(Vcb, Parent, Hash, TRUE) : NULL;
        KeAcquireSpinLock(&Vcb->IcbLock, &Irql);
        KeAcquireSpinLockAtDpcLevel(&Vcb->McbLruLock);
        Dead = IsFlagOn(Mcb->Flags, MCB_VCB_LINK) && Mcb->Parent == Parent &&
               Mcb->Refercount == 0 && !IsFlagOn(Mcb->Flags, MCB_ACCESSED);
        if (Dead) {
            RemoveEntryList(&Mcb->Link);
            ClearLongFlag(Mcb->Flags, MCB_VCB_LINK);
            if (Parent) {
                Ext2UnhashMcbLocked(Mcb);
            }
        }
        KeReleaseSpinLockFromDpcLevel(&Vcb->McbLruLock);
        KeReleaseSpinLock(&Vcb->IcbLock, Irql);
        if (Stripe) {
            Ext2ReleaseNameStripe(Stripe, TRUE);
        }

        if (Dead) {
            /* a name changed meanwhile is simply looked at again */
            if (Parent) {
                Ext2DerefMcb(Parent);
            }
            Ext2DerefXcb(&Vcb->NumOfMcb);
            InsertTailList(Reaped, &Mcb->Link);
            Count++;
        }
    }

    return Count;
}

ULONG
Ext2FirstUnusedMcb(PEXT2_VCB Vcb, BOOLEAN Wait, ULONG Number, PLIST_ENTRY Reaped)
{
    ULONG Count;

    if (Number == 0) {
        return 0;
    }
    if (Wait) {
        ExAcquireFastMutex(&Vcb->McbReapLock);
    } else if (!ExTryToAcquireFastMutex(&Vcb->McbReapLock)) {
        return 0;
    }
    Count = Ext2ReapUnusedMcbsLocked(Vcb, Number, Reaped);
    ExReleaseFastMutex(&Vcb->McbReapLock);

    return Count;
}

/*
 * Free the whole name cache of a volume going away. Nothing else touches
 * its names any more; McbReapLock keeps the reaper thread out.
 */
VOID
Ext2CleanupAllMcbs(PEXT2_VCB Vcb)
{
    PEXT2_MCB   Mcb = NULL;
    PLIST_ENTRY List;
    KIRQL       Irql;

    ExAcquireFastMutex(&Vcb->McbReapLock);

    /* The reaper gives a recently looked-up name a second chance: it
       clears MCB_ACCESSED and moves on. At dismount there is no later
       pass to take that chance - a pass that found only such names
       reaped none, ended this loop, and every one of them leaked with
       its Icb, dentry and name buffers (~750 bytes of nonpaged and
       paged pool per file deleted since the mount, measured). The flags
       go first, so each pass frees whatever nothing references; the next
       one takes the directories their children were holding. */
    KeAcquireSpinLock(&Vcb->McbLruLock, &Irql);
    for (List = Vcb->McbList.Flink; List != &Vcb->McbList; List = List->Flink) {
        ClearLongFlag(CONTAINING_RECORD(List, EXT2_MCB, Link)->Flags, MCB_ACCESSED);
    }
    KeReleaseSpinLock(&Vcb->McbLruLock, Irql);

    for (;;) {

        LIST_ENTRY  Reaped;

        InitializeListHead(&Reaped);
        if (Ext2ReapUnusedMcbsLocked(Vcb, Vcb->NumOfMcb, &Reaped) == 0) {
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
       counts are right). Names are paged: no spin lock here, nothing
       else runs on this volume any more. */
    if (!IsListEmpty(&Vcb->McbList)) {
        ULONG Left = 0;

        for (List = Vcb->McbList.Flink; List != &Vcb->McbList; List = List->Flink) {
            Mcb = CONTAINING_RECORD(List, EXT2_MCB, Link);
            if (Mcb == Vcb->McbTree) {
                continue;
            }
            if (Left++ < EXT4_LEAK_REPORT_MAX) {
                DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                           "ext4: name left behind: %wZ refs=%d flags=%08x ino=%u\n",
                           &Mcb->ShortName, Mcb->Refercount, Mcb->Flags,
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
    }

    ExReleaseFastMutex(&Vcb->McbReapLock);
}
