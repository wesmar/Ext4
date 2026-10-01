/**
 * lookup.c - path lookup, name validation and symbolic link resolution for create.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include <linux/ext4_xattr.h>
#include "create_internal.h"

static NTSTATUS Ext2LookupFileLocked(PEXT2_IRP_CONTEXT, PEXT2_VCB, PUNICODE_STRING,
                                     PEXT2_MCB, PEXT2_MCB *, ULONG, BOOLEAN, PBOOLEAN);

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2IsNameValid)
#pragma alloc_text(PAGE, Ext2FollowLink)
#pragma alloc_text(PAGE, Ext2IsSpecialSystemFile)
#pragma alloc_text(PAGE, Ext2LookupFile)
#pragma alloc_text(PAGE, Ext2LookupFileLocked)
#pragma alloc_text(PAGE, Ext2ScanDir)
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

        /* read the symlink target path */
        if (!Mcb->Inode->i_blocks) {

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
 * One pass of the path walk. Shared: McbLock is held shared and only the
 * name cache is consulted; the first component that would change the tree
 * (a name to read from disk, a dangling link to demote) ends the pass with
 * *Retry set and every reference it took dropped. Exclusive: the full walk,
 * reading directories and inserting what it finds.
 */
static NTSTATUS
Ext2LookupFileLocked (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PUNICODE_STRING      FullName,
    IN PEXT2_MCB            Parent,
    OUT PEXT2_MCB *         Ext2Mcb,
    IN ULONG                Linkdep,
    IN BOOLEAN              Shared,
    OUT PBOOLEAN            Retry
)
{
    NTSTATUS        Status = STATUS_OBJECT_NAME_NOT_FOUND;
    UNICODE_STRING  FileName;
    PEXT2_MCB       Mcb = NULL;
    struct dentry  *de = NULL;

    USHORT          i = 0, End;
    ULONG           Inode;

    BOOLEAN         bParent = FALSE;
    BOOLEAN         bDirectory = FALSE;
    BOOLEAN         LockAcquired = FALSE;
    BOOLEAN         bNotFollow = FALSE;

    *Retry = FALSE;

    __try {

        if (Shared) {
            ExAcquireResourceSharedLite(&Vcb->McbLock, TRUE);
        } else {
            ExAcquireResourceExclusiveLite(&Vcb->McbLock, TRUE);
        }
        LockAcquired = TRUE;

        bNotFollow = IsFlagOn(Linkdep, EXT2_LOOKUP_NOT_FOLLOW);
        Linkdep = ClearFlag(Linkdep, EXT2_LOOKUP_FLAG_MASK);

        *Ext2Mcb = NULL;

        DEBUG(DL_RES, ("Ext2LookupFile: %wZ\n", FullName));

        /* check names and parameters */
        if (FullName->Buffer[0] == L'\\') {
            Parent = Vcb->McbTree;
        } else if (Parent) {
            bParent = TRUE;
        } else {
            Parent = Vcb->McbTree;
        }

        /* make sure the parent is NULL */
        if (!IsMcbDirectory(Parent)) {
            Status =  STATUS_NOT_A_DIRECTORY;
            __leave;
        }

        /* use symlink's target as parent directory */
        if (IsMcbSymLink(Parent)) {
            Parent = Parent->Target;
            ASSERT(!IsMcbSymLink(Parent));
            if (IsFileDeleted(Parent)) {
                Status =  STATUS_NOT_A_DIRECTORY;
                __leave;
            }
        }

        if (NULL == Parent) {
            Status =  STATUS_NOT_A_DIRECTORY;
            __leave;
        }

        /* default is the parent Mcb*/
        Ext2ReferMcb(Parent);
        Mcb = Parent;

        /* is empty file name or root node */
        End = FullName->Length/sizeof(WCHAR);
        if ( (End == 0) || (End == 1 &&
                            FullName->Buffer[0] == L'\\')) {
            Status = STATUS_SUCCESS;
            __leave;
        }

        /* is a directory expected ? */
        while (FullName->Buffer[End - 1] == L'\\') {
            bDirectory = TRUE;
            End -= 1;
        }

        /* loop with every sub name */
        while (i < End) {

            USHORT Start = 0;

            /* zero the prefix '\' */
            while (i < End && FullName->Buffer[i] == L'\\') i++;
            Start = i;

            /* zero the suffix '\' */
            while (i < End && (FullName->Buffer[i] != L'\\')) i++;

            if (i > Start) {

                FileName = *FullName;
                FileName.Buffer += Start;
                FileName.Length = (USHORT)((i - Start) * 2);

                /* make sure the parent is NULL */
                if (!IsMcbDirectory(Parent)) {
                    Status =  STATUS_NOT_A_DIRECTORY;
                    Ext2DerefMcb(Parent);
                    break;
                }

                if (IsMcbSymLink(Parent)) {
                    if (IsFileDeleted(Parent->Target)) {
                        Status =  STATUS_NOT_A_DIRECTORY;
                        Ext2DerefMcb(Parent);
                        break;
                    } else {
                        Ext2ReferMcb(Parent->Target);
                        Ext2DerefMcb(Parent);
                        Parent = Parent->Target;
                    }
                }

                /* search cached Mcb nodes */
                Mcb = Ext2SearchMcbWithoutLock(Parent, &FileName);

                if (Mcb) {

                    /* derefer the parent Mcb */
                    Ext2DerefMcb(Parent);
                    Status = STATUS_SUCCESS;
                    Parent = Mcb;

                    if (IsMcbSymLink(Mcb) && IsFileDeleted(Mcb->Target) &&
                        Mcb->Refercount == 1) {

                        if (Shared) {
                            /* demoting the link changes the node */
                            Ext2DerefMcb(Mcb);
                            Status = STATUS_RETRY;
                            *Retry = TRUE;
                            break;
                        }

                        ASSERT(Mcb->Target);
                        ASSERT(Mcb->Target->Refercount > 0);
                        Ext2DerefMcb(Mcb->Target);
                        Mcb->Target = NULL;
                        ClearLongFlag(Mcb->Flags, MCB_TYPE_SYMLINK);
                        SetLongFlag(Mcb->Flags, MCB_TYPE_SPECIAL);
                        /* still a link on disk, shown as one (NTFS lists a dangling
                           symlink as a reparse point too); no longer a directory */
                        ClearFlag(Mcb->FileAttr, FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_NORMAL);
                        SetFlag(Mcb->FileAttr, FILE_ATTRIBUTE_REPARSE_POINT);
                    }

                    /* A symlink demoted to a special file while its target
                       was missing resolves by path again, as on Linux: the
                       target may exist by now. Only the exclusive pass may
                       rewrite the node; a target still missing leaves it
                       special, at the cost of one link read per open. */
                    if (!bNotFollow && IsFlagOn(Mcb->Flags, MCB_TYPE_SPECIAL) &&
                        S_ISLNK(Mcb->Inode->i_mode) && Mcb->Parent != NULL) {

                        if (Shared) {
                            Ext2DerefMcb(Mcb);
                            Status = STATUS_RETRY;
                            *Retry = TRUE;
                            break;
                        }

                        /* on disk it is a link either way; the directory
                           bit, when the target is one, comes from
                           Ext2FollowLink */
                        ClearFlag(Mcb->FileAttr, FILE_ATTRIBUTE_NORMAL);
                        SetFlag(Mcb->FileAttr, FILE_ATTRIBUTE_REPARSE_POINT);
                        Ext2FollowLink(IrpContext, Vcb, Mcb->Parent, Mcb, Linkdep + 1);
                    }

                } else {

                    /* need create new Mcb node */

                    if (Shared) {
                        /* not cached: the directory must be read and the
                           name added to the tree, which the shared pass
                           may not do */
                        Ext2DerefMcb(Parent);
                        Status = STATUS_RETRY;
                        *Retry = TRUE;
                        break;
                    }

                    /* is a valid ext2 name */
                    if (!Ext2IsNameValid(&FileName)) {
                        Status = STATUS_OBJECT_NAME_INVALID;
                        Ext2DerefMcb(Parent);
                        break;
                    }

                    /* seach the disk */
                    de = NULL;
                    Status = Ext2ScanDir (
                                 IrpContext,
                                 Vcb,
                                 Parent,
                                 &FileName,
                                 &Inode,
                                 &de);

                    if (NT_SUCCESS(Status)) {

                        /* check it's real parent */
                        ASSERT (!IsMcbSymLink(Parent));

                        /* allocate Mcb ... */
                        Mcb = Ext2AllocateMcb(Vcb, &FileName, &Parent->FullName, 0);
                        if (!Mcb) {
                            Status = STATUS_INSUFFICIENT_RESOURCES;
                            Ext2DerefMcb(Parent);
                            break;
                        }
                        Mcb->de = de;
                        de = NULL;

                        /* join the inode node: another name of the same
                           inode (hard link) may have loaded it already */
                        if (!Ext2AttachIcb(Vcb, Mcb, Inode)) {
                            Status = STATUS_INSUFFICIENT_RESOURCES;
                            Ext2DerefMcb(Parent);
                            Ext2FreeMcb(Vcb, Mcb);
                            break;
                        }
                        Mcb->de->d_inode = Mcb->Inode;

                        /* load inode information */
                        if (!IsFlagOn(Mcb->Icb->Flags, ICB_INODE_LOADED)) {
                            if (!Ext2LoadInode(Vcb, Mcb->Inode)) {
                                Status = STATUS_CANT_WAIT;
                                Ext2DerefMcb(Parent);
                                Ext2FreeMcb(Vcb, Mcb);
                                break;
                            }
                            SetLongFlag(Mcb->Icb->Flags, ICB_INODE_LOADED);

                            Mcb->Icb->LastAccessTime = Ext2GetInodeTime(Mcb->Inode->i_atime, Mcb->Inode->i_atime_extra);
                            Mcb->Icb->LastWriteTime = Ext2GetInodeTime(Mcb->Inode->i_mtime, Mcb->Inode->i_mtime_extra);
                            Mcb->Icb->ChangeTime = Ext2GetInodeTime(Mcb->Inode->i_ctime, Mcb->Inode->i_ctime_extra);
                            if (Mcb->Inode->i_crtime)
                                Mcb->Icb->CreationTime = Ext2GetInodeTime(Mcb->Inode->i_crtime, Mcb->Inode->i_crtime_extra);
                            else
                                Mcb->Icb->CreationTime = Ext2GetInodeTime(Mcb->Inode->i_ctime, Mcb->Inode->i_ctime_extra);
                        }

                        /* a directory has one name: a second one (an entry
                           naming its own directory, the root, an ancestor -
                           a corrupt file system) would make the name tree a
                           loop that never ends nor frees. Linux refuses the
                           alias the same way (EIO, "found a directory alias") */
                        if (S_ISDIR(Mcb->Inode->i_mode) &&
                            Mcb->Icb->Names.Flink != Mcb->Icb->Names.Blink) {
                            DbgPrint("ext4: directory inode %u has a second name, refused\n",
                                     Mcb->Inode->i_ino);
                            Status = STATUS_FILE_CORRUPT_ERROR;
                            Ext2DerefMcb(Parent);
                            Ext2FreeMcb(Vcb, Mcb);
                            break;
                        }

                        /* set inode attribute: read-only when the owner
                           write bit is clear (that is what the attribute
                           maps to, both ways) or we cannot write it */
                        if (!Ext2IsOwnerWritable(Mcb->Inode->i_mode) ||
                            Ext4IsImmutable(Mcb->Inode) ||
                            !Ext2CheckFileAccess(Vcb, Mcb, Ext2FileCanWrite)) {
                            SetFlag(Mcb->FileAttr, FILE_ATTRIBUTE_READONLY);
                        }

                        if (S_ISDIR(Mcb->Inode->i_mode)) {
                            SetFlag(Mcb->FileAttr, FILE_ATTRIBUTE_DIRECTORY);
                        } else {
                            if (S_ISREG(Mcb->Inode->i_mode)) {
                                SetFlag(Mcb->FileAttr, FILE_ATTRIBUTE_NORMAL);
                            } else if (S_ISLNK(Mcb->Inode->i_mode)) {
                                SetFlag(Mcb->FileAttr, FILE_ATTRIBUTE_REPARSE_POINT);
                            } else {
                                SetLongFlag(Mcb->Flags, MCB_TYPE_SPECIAL);
                            }
                        }

                        /* process special files under root directory */
                        if (IsMcbRoot(Parent)) {
                            /* set hidden and system attributes for
                               Recycled / RECYCLER / pagefile.sys */
                            BOOLEAN IsDirectory = IsMcbDirectory(Mcb);
                            if (Ext2IsSpecialSystemFile(&Mcb->ShortName, IsDirectory)) {
                                SetFlag(Mcb->FileAttr, FILE_ATTRIBUTE_HIDDEN);
                                SetFlag(Mcb->FileAttr, FILE_ATTRIBUTE_SYSTEM);
                            }
                        }

                        /* process symlink */
                        if (S_ISLNK(Mcb->Inode->i_mode) && !bNotFollow) {
                            Ext2FollowLink( IrpContext,
                                            Vcb,
                                            Parent,
                                            Mcb,
                                            Linkdep+1
                                          );
                        }

                        /* add reference ... */
                        Ext2ReferMcb(Mcb);

                        /* add Mcb to it's parent tree*/
                        Ext2InsertMcb(Vcb, Parent, Mcb);

                        /* it's safe to deref Parent Mcb */
                        Ext2DerefMcb(Parent);

                        /* linking this Mcb*/
                        Ext2LinkTailMcb(Vcb, Mcb);

                        /* set parent to preare re-scan */
                        Parent = Mcb;

                    } else {

                        /* derefernce it's parent */
                        Ext2DerefMcb(Parent);
                        break;
                    }
                }

            } else {

                /* there seems too many \ or / */
                /* Mcb should be already set to Parent */
                ASSERT(Mcb == Parent);
                Status = STATUS_SUCCESS;
                break;
            }
        }

    } __finally {

        if (de) {
            Ext2FreeEntry(de);
        }

        if (NT_SUCCESS(Status)) {
            if (bDirectory) {
                if (IsMcbDirectory(Mcb)) {
                    *Ext2Mcb = Mcb;
                } else {
                    Ext2DerefMcb(Mcb);
                    Status = STATUS_NOT_A_DIRECTORY;
                }
            } else {
                *Ext2Mcb = Mcb;
            }
        }

        if (LockAcquired) {
            ExReleaseResourceLite(&Vcb->McbLock);
        }
    }

    return Status;
}

/*
 * Path lookup. Opens of cached names are the common case and run side by
 * side under the shared McbLock; only a pass that has to read a directory
 * or change a node is repeated with the lock held exclusively. Measured on
 * 8 threads opening existing files: an exclusive walk per open made them
 * queue on McbLock once the volume resource stopped serialising opens.
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
    BOOLEAN     Retry;
    NTSTATUS    Status;

    Status = Ext2LookupFileLocked(IrpContext, Vcb, FullName, Parent, Ext2Mcb,
                                  Linkdep, TRUE, &Retry);
    if (Retry) {
        Status = Ext2LookupFileLocked(IrpContext, Vcb, FullName, Parent, Ext2Mcb,
                                      Linkdep, FALSE, &Retry);
    }
    return Status;
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

        /* parent is a symlink ? */
        if IsMcbSymLink(Parent) {
            if (Parent->Target) {
                Ext2ReferMcb(Parent->Target);
                Ext2DerefMcb(Parent);
                Parent = Parent->Target;
                ASSERT(!IsMcbSymLink(Parent));
            } else {
                DbgBreak();
                Status = STATUS_NOT_A_DIRECTORY;
                __leave;
            }
        }

        de = Ext2BuildEntry(Vcb, Parent, FileName);
        if (!de) {
            DEBUG(DL_ERR, ( "Ex2ScanDir: failed to allocate dentry.\n"));
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        bh = ext3_find_entry(IrpContext, de, &dir_entry);
        if (dir_entry) {
            Status = STATUS_SUCCESS;
            *Inode = dir_entry->inode;
            *dentry = de;
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
