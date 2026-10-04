/**
 * PathLookup.c - path lookup, name validation and symbolic link resolution for create.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include <linux/ext4_xattr.h>
#include "create_internal.h"

static NTSTATUS Ext2LookupComponent(PEXT2_IRP_CONTEXT, PEXT2_VCB, PEXT2_MCB, PUNICODE_STRING,
                                    ULONG, BOOLEAN, PEXT2_MCB *);

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2IsNameValid)
#pragma alloc_text(PAGE, Ext2FollowLink)
#pragma alloc_text(PAGE, Ext2IsSpecialSystemFile)
#pragma alloc_text(PAGE, Ext2LookupFile)
#pragma alloc_text(PAGE, Ext2LookupComponent)
#pragma alloc_text(PAGE, Ext2ScanDir)
#pragma alloc_text(PAGE, Ext2InsertName)
#endif

BOOLEAN
Ext2IsNameValid(PUNICODE_STRING FileName)
{
    USHORT  i = 0;
    PUSHORT pName = (PUSHORT) FileName->Buffer;

    if (FileName == NULL) {
        return FALSE;
    }

    while (i < (FileName->Length / sizeof(WCHAR))) {

        if (pName[i] == 0) {
            break;
        }

        if (pName[i] == L'|'  || pName[i] == L':'  ||
                pName[i] == L'/'  || pName[i] == L'*'  ||
                pName[i] == L'?'  || pName[i] == L'\"' ||
                pName[i] == L'<'  || pName[i] == L'>'   ) {

            return FALSE;
        }

        i++;
    }

    return TRUE;
}

NTSTATUS
Ext2FollowLink (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_MCB            Parent,
    IN PEXT2_MCB            Mcb,
    IN ULONG                Linkdep
)
{
    NTSTATUS        Status = STATUS_LINK_FAILED;

    UNICODE_STRING UniName = { 0 };
    OEM_STRING OemName = { 0 };
    BOOLEAN         bOemBuffer = FALSE;

    PEXT2_MCB       Target = NULL;

    USHORT          i;

    __try {

        RtlZeroMemory(&UniName, sizeof(UNICODE_STRING));
        RtlZeroMemory(&OemName, sizeof(OEM_STRING));

        /* exit if we jump into a possible symlink forever loop */
        if ((Linkdep + 1) > EXT2_MAX_NESTED_LINKS ||
            IoGetRemainingStackSize() < 1024) {
            __leave;
        }

        /* read the symlink target path: kept in i_block, or in a block */
        if (ext4_inode_is_fast_symlink(Mcb->Inode)) {

            OemName.Buffer = (PCHAR)&Mcb->Inode->i_block[0];
            OemName.Length = (USHORT)Mcb->Inode->i_size;
            OemName.MaximumLength = OemName.Length + 1;

        } else {

            OemName.Length = (USHORT)Mcb->Inode->i_size;
            OemName.MaximumLength = OemName.Length + 1;
            OemName.Buffer = Ext2AllocatePool(PagedPool,
                                              OemName.MaximumLength,
                                              'NL2E');
            if (OemName.Buffer == NULL) {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                __leave;
            }
            bOemBuffer = TRUE;
            RtlZeroMemory(OemName.Buffer, OemName.MaximumLength);

            Status = Ext2ReadSymlink(
                         IrpContext,
                         Vcb,
                         Mcb,
                         OemName.Buffer,
                         (ULONG)(Mcb->Inode->i_size),
                         NULL);
            if (!NT_SUCCESS(Status)) {
                __leave;
            }
        }

        /* convert Linux slash to Windows backslash */
        for (i=0; i < OemName.Length; i++) {
            if (OemName.Buffer[i] == '/') {
                OemName.Buffer[i] = '\\';
            }
        }

        /* convert oem string to unicode string */
        UniName.MaximumLength = (USHORT)Ext2OEMToUnicodeSize(Vcb, &OemName);
        if (UniName.MaximumLength <= 0) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        UniName.MaximumLength += 2;
        UniName.Buffer = Ext2AllocatePool(PagedPool,
                                          UniName.MaximumLength,
                                          'NL2E');
        if (UniName.Buffer == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }
        RtlZeroMemory(UniName.Buffer, UniName.MaximumLength);
        Status = Ext2OEMToUnicode(Vcb, &UniName, &OemName);
        if (!NT_SUCCESS(Status)) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        /* search the real target */
        Status = Ext2LookupFile(
                     IrpContext,
                     Vcb,
                     &UniName,
                     Parent,
                     &Target,
                     Linkdep
                 );
        if (Target == NULL) {
            Status = STATUS_LINK_FAILED;
        }

        if (Target == NULL /* link target doesn't exist */      ||
            Target == Mcb  /* symlink points to itself */       ||
            IsMcbSpecialFile(Target) /* target not resolved*/   ||
            IsFileDeleted(Target)  /* target deleted */         ) {

            if (Target) {
                ASSERT(Target->Refercount > 0);
                Ext2DerefMcb(Target);
            }
            ClearLongFlag(Mcb->Flags, MCB_TYPE_SYMLINK);
            SetLongFlag(Mcb->Flags, MCB_TYPE_SPECIAL);
            Mcb->Target = NULL;

        } else if (IsMcbSymLink(Target)) {

            ASSERT(Target->Refercount > 0);
            ASSERT(Target->Target != NULL);
            Ext2ReferMcb(Target->Target);
            Mcb->Target = Target->Target;
            Ext2DerefMcb(Target);
            ASSERT(!IsMcbSymLink(Target->Target));
            SetLongFlag(Mcb->Flags, MCB_TYPE_SYMLINK);
            ClearLongFlag(Mcb->Flags, MCB_TYPE_SPECIAL);
            ASSERT(Mcb->Target->Refercount > 0);
            
        } else {

            Mcb->Target = Target;
            SetLongFlag(Mcb->Flags, MCB_TYPE_SYMLINK);
            ClearLongFlag(Mcb->Flags, MCB_TYPE_SPECIAL);
            ASSERT(Mcb->Target->Refercount > 0);
        }

        /* add directory flag to file attribute */
        if (Mcb->Target && IsMcbDirectory(Mcb->Target)) {
            Mcb->FileAttr |= FILE_ATTRIBUTE_DIRECTORY;
        }

    } __finally {

        if (bOemBuffer) {
            Ext2FreePool(OemName.Buffer, 'NL2E');
        }

        if (UniName.Buffer) {
            Ext2FreePool(UniName.Buffer, 'NL2E');
        }
    }

    return Status;
}

BOOLEAN
Ext2IsSpecialSystemFile(
    IN PUNICODE_STRING FileName,
    IN BOOLEAN         bDirectory
)
{
    PWSTR SpecialFileList[] = {
        L"pagefile.sys",
        L"swapfile.sys",
        L"hiberfil.sys",
        NULL
    };

    PWSTR SpecialDirList[] = {
        L"Recycled",
        L"RECYCLER",
        L"$RECYCLE.BIN",
        NULL
    };

    PWSTR   entryName;
    ULONG   length;
    int     i;

    for (i = 0; TRUE; i++) {

        if (bDirectory) {
            entryName = SpecialDirList[i];
        } else {
            entryName = SpecialFileList[i];
        }

        if (NULL == entryName) {
            break;
        }

        length = (ULONG)(wcslen(entryName) * sizeof(WCHAR));
        if (FileName->Length == length) {
            if ( 0 == _wcsnicmp( entryName,
                                 FileName->Buffer,
                                 length / sizeof(WCHAR) )) {
                return TRUE;
            }
        }
    }

    return FALSE;
}

/*
 * Put the name FileName of directory Parent - entry de, inode Ino - into
 * the name tree: a new Mcb joined to the inode's Icb (loaded from disk
 * unless another name has it), with its attributes. The caller holds the
 * name's stripe exclusively, so nobody else inserts it meanwhile. A symlink
 * goes in unresolved (MCB_TYPE_SPECIAL): following it looks up other names,
 * which must not happen under a stripe; Ext2ResolveLink does it after. On
 * success *Result is the new name, referenced for the caller. de becomes
 * the name's, or goes with it on failure.
 *
 * Fresh: the inode was made a moment ago by the caller. A new directory
 * then comes back with its DirResource held, taken before the name can be
 * found: nobody makes a name in it before the caller has written "." and
 * ".." (Ext2CreateNewName), and nobody can be waiting for it yet.
 */
NTSTATUS
Ext2InsertName (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_MCB            Parent,
    IN PUNICODE_STRING      FileName,
    IN ULONG                Ino,
    IN struct dentry       *de,
    IN BOOLEAN              Fresh,
    OUT PEXT2_MCB          *Result
)
{
    PEXT2_MCB   Mcb;
    PEXT2_ICB   Icb;

    UNREFERENCED_PARAMETER(IrpContext);
    ASSERT(!IsMcbSymLink(Parent));
    *Result = NULL;

    Mcb = Ext2AllocateMcb(Vcb, FileName, &Parent->FullName, 0);
    if (!Mcb) {
        Ext2FreeEntry(de);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Mcb->de = de;

    /* join the inode node: another name of the same inode (hard link) may
       have loaded it already */
    if (!Ext2AttachIcb(Vcb, Mcb, Ino)) {
        Ext2FreeMcb(Vcb, Mcb);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Mcb->de->d_inode = Mcb->Inode;
    Icb = Mcb->Icb;

    /* Load the inode once: a second name of it, in another directory under
       another stripe, may be at this very point - and loading it again
       over an inode already in use would throw away its changes */
    if (!IsFlagOn(Icb->Flags, ICB_INODE_LOADED)) {
        BOOLEAN Loaded = TRUE;

        KeEnterCriticalRegion();
        ExAcquireResourceExclusiveLite(&Icb->EntryResource, TRUE);
        if (!IsFlagOn(Icb->Flags, ICB_INODE_LOADED)) {
            Loaded = Ext2LoadInode(Vcb, Mcb->Inode);
            if (Loaded) {
                struct inode *Inode = Mcb->Inode;

                Icb->LastAccessTime = Ext2GetInodeTime(Inode->i_atime, Inode->i_atime_extra);
                Icb->LastWriteTime = Ext2GetInodeTime(Inode->i_mtime, Inode->i_mtime_extra);
                Icb->ChangeTime = Ext2GetInodeTime(Inode->i_ctime, Inode->i_ctime_extra);
                if (Inode->i_crtime)
                    Icb->CreationTime = Ext2GetInodeTime(Inode->i_crtime, Inode->i_crtime_extra);
                else
                    Icb->CreationTime = Ext2GetInodeTime(Inode->i_ctime, Inode->i_ctime_extra);
                SetLongFlag(Icb->Flags, ICB_INODE_LOADED);
            }
        }
        ExReleaseResourceLite(&Icb->EntryResource);
        KeLeaveCriticalRegion();

        if (!Loaded) {
            Ext2FreeMcb(Vcb, Mcb);
            /* unreadable or failing its checksum: CANT_WAIT would post the
               request to be retried, again and again */
            return STATUS_UNEXPECTED_IO_ERROR;
        }
    }

    /* a directory has one name: a second one (an entry naming its own
       directory, the root, an ancestor - a corrupt file system) would make
       the name tree a loop that never ends nor frees. Linux refuses the
       alias the same way (EIO, "found a directory alias") */
    if (S_ISDIR(Mcb->Inode->i_mode) &&
        Icb->Names.Flink != Icb->Names.Blink) {
        DbgPrint("ext4: directory inode %u has a second name, refused\n",
                 Mcb->Inode->i_ino);
        Ext2FreeMcb(Vcb, Mcb);
        return STATUS_FILE_CORRUPT_ERROR;
    }

    /* set inode attribute: read-only when the owner write bit is clear
       (that is what the attribute maps to, both ways) or we cannot write it */
    if (!Ext2IsOwnerWritable(Mcb->Inode->i_mode) ||
        Ext4IsImmutable(Mcb->Inode) ||
        !Ext2CheckFileAccess(Vcb, Mcb, Ext2FileCanWrite)) {
        SetFlag(Mcb->FileAttr, FILE_ATTRIBUTE_READONLY);
    }

    if (S_ISDIR(Mcb->Inode->i_mode)) {
        SetFlag(Mcb->FileAttr, FILE_ATTRIBUTE_DIRECTORY);
    } else if (S_ISREG(Mcb->Inode->i_mode)) {
        SetFlag(Mcb->FileAttr, FILE_ATTRIBUTE_NORMAL);
    } else if (S_ISLNK(Mcb->Inode->i_mode)) {
        /* unresolved until Ext2ResolveLink follows it */
        SetFlag(Mcb->FileAttr, FILE_ATTRIBUTE_REPARSE_POINT);
        SetLongFlag(Mcb->Flags, MCB_TYPE_SPECIAL);
    } else {
        SetLongFlag(Mcb->Flags, MCB_TYPE_SPECIAL);
    }

    /* process special files under root directory */
    if (IsMcbRoot(Parent)) {
        /* set hidden and system attributes for Recycled / RECYCLER /
           pagefile.sys */
        BOOLEAN IsDirectory = IsMcbDirectory(Mcb);
        if (Ext2IsSpecialSystemFile(&Mcb->ShortName, IsDirectory)) {
            SetFlag(Mcb->FileAttr, FILE_ATTRIBUTE_HIDDEN);
            SetFlag(Mcb->FileAttr, FILE_ATTRIBUTE_SYSTEM);
        }
    }

    if (Fresh && S_ISDIR(Mcb->Inode->i_mode)) {
        KeEnterCriticalRegion();
        ExAcquireResourceExclusiveLite(&Icb->DirResource, TRUE);
    }

    /* the caller's reference; into the parent's tree and the reaper's list */
    Ext2ReferMcb(Mcb);
    Ext2InsertMcb(Vcb, Parent, Mcb);
    Ext2LinkTailMcb(Vcb, Mcb);

    *Result = Mcb;
    return STATUS_SUCCESS;
}

/*
 * The symlink state of a name just found, brought up to date under
 * LinkLock (never called with a stripe held: following a link looks up
 * its target). Two cases, as on Linux, where a link resolves by path at
 * every use:
 *  - a link whose target has gone, and which nobody else holds, becomes a
 *    special file again - still a link on disk, shown as one (NTFS lists
 *    a dangling symlink as a reparse point too), no longer a directory;
 *  - an unresolved link (new, or demoted while its target was missing) is
 *    followed, unless the caller asked not to: the target may exist by
 *    now. One still missing leaves it special, at the cost of one link
 *    read per open.
 */
VOID
Ext2ResolveLink(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb,
                IN PEXT2_MCB Mcb, IN ULONG Linkdep, IN BOOLEAN NotFollow)
{
    BOOLEAN Dangling = FALSE, Unresolved;

    /* flags read unlocked only decide whether to look closer; a resolved
       link - the common case - is checked under LinkLock shared, and only
       a change takes it exclusively (the target is read only under it: a
       demotion drops the reference that keeps it alive) */
    Unresolved = !NotFollow && IsFlagOn(Mcb->Flags, MCB_TYPE_SPECIAL) &&
                 S_ISLNK(Mcb->Inode->i_mode);
    if (IsMcbSymLink(Mcb)) {
        KeEnterCriticalRegion();
        ExAcquireResourceSharedLite(&Vcb->LinkLock, TRUE);
        Dangling = IsMcbSymLink(Mcb) && Mcb->Target != NULL &&
                   IsFileDeleted(Mcb->Target) && Mcb->Refercount == 1;
        ExReleaseResourceLite(&Vcb->LinkLock);
        KeLeaveCriticalRegion();
    }
    if (!Dangling && !Unresolved) {
        return;
    }

    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&Vcb->LinkLock, TRUE);

    if (IsMcbSymLink(Mcb) && Mcb->Target != NULL &&
        IsFileDeleted(Mcb->Target) && Mcb->Refercount == 1) {
        Ext2DerefMcb(Mcb->Target);
        Mcb->Target = NULL;
        ClearLongFlag(Mcb->Flags, MCB_TYPE_SYMLINK);
        SetLongFlag(Mcb->Flags, MCB_TYPE_SPECIAL);
        ClearFlag(Mcb->FileAttr, FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_NORMAL);
        SetFlag(Mcb->FileAttr, FILE_ATTRIBUTE_REPARSE_POINT);
    }

    if (!NotFollow && IsFlagOn(Mcb->Flags, MCB_TYPE_SPECIAL) &&
        S_ISLNK(Mcb->Inode->i_mode)) {
        PEXT2_MCB Parent = Ext2ReferParent(Vcb, Mcb);
        if (Parent) {
            /* on disk it is a link either way; the directory bit, when the
               target is one, comes from Ext2FollowLink */
            ClearFlag(Mcb->FileAttr, FILE_ATTRIBUTE_NORMAL);
            SetFlag(Mcb->FileAttr, FILE_ATTRIBUTE_REPARSE_POINT);
            Ext2FollowLink(IrpContext, Vcb, Parent, Mcb, Linkdep + 1);
            Ext2DerefMcb(Parent);
        }
    }

    ExReleaseResourceLite(&Vcb->LinkLock);
    KeLeaveCriticalRegion();
}

/*
 * One component of a path: the name FileName of directory Dir (referenced
 * by the caller, no symlink), referenced into *Result.
 *
 * A name in the cache costs one chain walk under its stripe, shared. One
 * that is not cached is looked for on disk, still shared - every create of
 * a new file asks for a name that is not there, and those answers need no
 * change to the tree. A name found on disk goes into the tree under the
 * stripe held exclusively, from a second check of the cache through the
 * directory read to the insertion: a delete removes the disk entry of a
 * name it holds in the tree, so in that window the name is either still
 * in the tree or gone from the disk - never inserted stale.
 */
static NTSTATUS
Ext2LookupComponent (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_MCB            Dir,
    IN PUNICODE_STRING      FileName,
    IN ULONG                Linkdep,
    IN BOOLEAN              NotFollow,
    OUT PEXT2_MCB          *Result
)
{
    PEXT2_MCB       Mcb;
    PERESOURCE      Stripe;
    struct dentry  *de = NULL;
    ULONG           Hash, Ino;
    NTSTATUS        Status;

    *Result = NULL;

    if (Ext2IsDot(FileName) || Ext2IsDotDot(FileName)) {
        Mcb = Ext2SearchMcb(Vcb, Dir, FileName);
        if (Mcb == NULL) {
            return STATUS_OBJECT_PATH_NOT_FOUND;
        }
        *Result = Mcb;
        return STATUS_SUCCESS;
    }

    Hash = Ext2HashMcbName(FileName);
    Stripe = Ext2AcquireNameStripe(Vcb, Dir, Hash, FALSE);
    Mcb = Ext2FindMcbLocked(Vcb, Dir, FileName, Hash);
    if (Mcb == NULL) {
        if (!Ext2IsNameValid(FileName)) {
            Ext2ReleaseNameStripe(Stripe);
            return STATUS_OBJECT_NAME_INVALID;
        }
        Status = Ext2ScanDir(IrpContext, Vcb, Dir, FileName, &Ino, &de);
        Ext2ReleaseNameStripe(Stripe);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
        Ext2FreeEntry(de);
        de = NULL;

        Stripe = Ext2AcquireNameStripe(Vcb, Dir, Hash, TRUE);
        Mcb = Ext2FindMcbLocked(Vcb, Dir, FileName, Hash);
        if (Mcb == NULL) {
            Status = Ext2ScanDir(IrpContext, Vcb, Dir, FileName, &Ino, &de);
            if (NT_SUCCESS(Status)) {
                Status = Ext2InsertName(IrpContext, Vcb, Dir, FileName, Ino, de, FALSE, &Mcb);
            }
        }
        Ext2ReleaseNameStripe(Stripe);
        if (Mcb == NULL) {
            return Status;
        }
    } else {
        Ext2ReleaseNameStripe(Stripe);
    }

    Ext2ResolveLink(IrpContext, Vcb, Mcb, Linkdep, NotFollow);

    *Result = Mcb;
    return STATUS_SUCCESS;
}

/*
 * Path lookup: FullName from directory Parent (the root for an absolute
 * name or none), one component at a time. No lock is held from one
 * component to the next - the directory reached is held by its reference
 * - so walks of different paths, or of one path, run side by side.
 */
NTSTATUS
Ext2LookupFile (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PUNICODE_STRING      FullName,
    IN PEXT2_MCB            Parent,
    OUT PEXT2_MCB *         Ext2Mcb,
    IN ULONG                Linkdep
)
{
    NTSTATUS        Status = STATUS_SUCCESS;
    UNICODE_STRING  FileName;
    PEXT2_MCB       Mcb, Dir, Next;
    USHORT          i = 0, End, Start;
    BOOLEAN         bDirectory = FALSE;
    BOOLEAN         NotFollow;

    NotFollow = IsFlagOn(Linkdep, EXT2_LOOKUP_NOT_FOLLOW);
    Linkdep = ClearFlag(Linkdep, EXT2_LOOKUP_FLAG_MASK);

    *Ext2Mcb = NULL;

    DEBUG(DL_RES, ("Ext2LookupFile: %wZ\n", FullName));

    if (Parent == NULL || (FullName->Length > 0 && FullName->Buffer[0] == L'\\')) {
        Parent = Vcb->McbTree;
    }
    if (!IsMcbDirectory(Parent)) {
        return STATUS_NOT_A_DIRECTORY;
    }
    /* a symlink parent stands for its target */
    Mcb = Ext2ReferDirectory(Vcb, Parent);
    if (Mcb == NULL) {
        return STATUS_NOT_A_DIRECTORY;
    }

    /* the root or the parent itself */
    End = FullName->Length / sizeof(WCHAR);
    if (End == 0 || (End == 1 && FullName->Buffer[0] == L'\\')) {
        *Ext2Mcb = Mcb;
        return STATUS_SUCCESS;
    }

    /* is a directory expected ? */
    while (End > 0 && FullName->Buffer[End - 1] == L'\\') {
        bDirectory = TRUE;
        End -= 1;
    }

    while (i < End) {

        /* skip separators, then take one name */
        while (i < End && FullName->Buffer[i] == L'\\') i++;
        Start = i;
        while (i < End && FullName->Buffer[i] != L'\\') i++;
        if (i == Start) {
            break;
        }

        FileName = *FullName;
        FileName.Buffer += Start;
        FileName.Length = (USHORT)((i - Start) * sizeof(WCHAR));

        /* go on in the directory Mcb is (or a symlink to one leads to) */
        Dir = IsMcbDirectory(Mcb) ? Ext2ReferDirectory(Vcb, Mcb) : NULL;
        Ext2DerefMcb(Mcb);
        Mcb = NULL;
        if (Dir == NULL) {
            Status = STATUS_NOT_A_DIRECTORY;
            break;
        }

        Status = Ext2LookupComponent(IrpContext, Vcb, Dir, &FileName,
                                     Linkdep, NotFollow, &Next);
        Ext2DerefMcb(Dir);
        if (!NT_SUCCESS(Status)) {
            break;
        }
        Mcb = Next;
    }

    if (!NT_SUCCESS(Status)) {
        if (Mcb) {
            Ext2DerefMcb(Mcb);
        }
        return Status;
    }

    if (bDirectory && !IsMcbDirectory(Mcb)) {
        Ext2DerefMcb(Mcb);
        return STATUS_NOT_A_DIRECTORY;
    }

    *Ext2Mcb = Mcb;
    return STATUS_SUCCESS;
}

NTSTATUS
Ext2ScanDir (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_MCB            Parent,
    IN PUNICODE_STRING      FileName,
    OUT PULONG              Inode,
    OUT struct dentry     **dentry
)
{
    struct ext3_dir_entry_2 *dir_entry = NULL;
    struct buffer_head     *bh = NULL;
    struct dentry          *de = NULL;

    NTSTATUS                Status = STATUS_NO_SUCH_FILE;

    DEBUG(DL_RES, ("Ext2ScanDir: %wZ\\%wZ\n", &Parent->FullName, FileName));

    __try {

        /* grab parent's reference first */
        Ext2ReferMcb(Parent);

        /* bad request ! */
        if (!IsMcbDirectory(Parent)) {
            Status = STATUS_NOT_A_DIRECTORY;
            __leave;
        }

        /* a symlink stands for its target (read under LinkLock) */
        if (IsMcbSymLink(Parent)) {
            PEXT2_MCB Target = Ext2ReferDirectory(Vcb, Parent);

            if (Target == NULL) {
                Status = STATUS_NOT_A_DIRECTORY;
                __leave;
            }
            Ext2DerefMcb(Parent);
            Parent = Target;
        }

        de = Ext2BuildEntry(Vcb, Parent, FileName);
        if (!de) {
            DEBUG(DL_ERR, ( "Ext2ScanDir: failed to allocate dentry.\n"));
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        /* the blocks hold still while an entry change waits (DirectoryOperations.c);
           the entry is read before they may move again */
        ExAcquireResourceSharedLite(&Parent->Icb->EntryResource, TRUE);
        __try {
            bh = ext3_find_entry(IrpContext, de, &dir_entry);
            if (dir_entry) {
                Status = STATUS_SUCCESS;
                *Inode = dir_entry->inode;
                *dentry = de;
            }
        } __finally {
            ExReleaseResourceLite(&Parent->Icb->EntryResource);
        }

    } __finally {

        Ext2DerefMcb(Parent);

        if (bh)
            __brelse(bh);

        if (!NT_SUCCESS(Status)) {
            if (de)
                Ext2FreeEntry(de);
        }
    }

    return Status;
}
