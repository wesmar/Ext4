/**
 * dir_ops.c - directory entry operations used by create, rename, link and delete.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "linux\ext4.h"
#include "../core/core_internal.h"

struct dentry * Ext2AllocateEntry()
{
    struct dentry *de;

    de = (struct dentry *)ExAllocateFromNPagedLookasideList(
             &(Ext2Global->Ext2DentryLookasideList));
    if (!de) {
        return NULL;
    }

    RtlZeroMemory(de, sizeof(struct dentry));
    INC_MEM_COUNT(PS_DENTRY, de, sizeof(struct dentry));

    return de;
}

VOID Ext2FreeEntry (IN struct dentry *de)
{
    ASSERT(de != NULL);

    if (de->d_name.name)
        Ext2FreePool(de->d_name.name, 'EB2E');

    ExFreeToNPagedLookasideList(&(Ext2Global->Ext2DentryLookasideList), de);
    DEC_MEM_COUNT(PS_DENTRY, de, sizeof(struct dentry));
}

struct dentry *Ext2BuildEntry(PEXT2_VCB Vcb, PEXT2_MCB Dcb, PUNICODE_STRING FileName)
{
    OEM_STRING      Oem = { 0 };
    struct dentry  *de = NULL;
    NTSTATUS        Status = STATUS_INSUFFICIENT_RESOURCES;

    __try {

        de = Ext2AllocateEntry();
        if (!de) {
            DEBUG(DL_ERR, ("Ext2BuildEntry: failed to allocate dentry.\n"));
            __leave;
        }
        de->d_sb = &Vcb->sb;
        if (Dcb)
            de->d_parent = Dcb->de;

        Oem.MaximumLength = (USHORT)Ext2UnicodeToOEMSize(Vcb, FileName) + 1;
        Oem.Buffer = Ext2AllocatePool(PagedPool, Oem.MaximumLength, 'EB2E');
        if (!Oem.Buffer) {
            DEBUG(DL_ERR, ( "Ex2BuildEntry: failed to allocate OEM name.\n"));
            __leave;
        }
        de->d_name.name = Oem.Buffer;
        RtlZeroMemory(Oem.Buffer, Oem.MaximumLength);
        Status = Ext2UnicodeToOEM(Vcb, &Oem, FileName);
        if (!NT_SUCCESS(Status)) {
            DEBUG(DL_CP, ("Ext2BuildEntry: failed to convert %S to OEM.\n", FileName->Buffer));
            __leave;
        }
        de->d_name.len  = Oem.Length;

    } __finally {

        if (!NT_SUCCESS(Status)) {
            if (de)
                Ext2FreeEntry(de);
        }
    }

    return de;
}

NTSTATUS
Ext2AddEntry (
    IN PEXT2_IRP_CONTEXT   IrpContext,
    IN PEXT2_VCB           Vcb,
    IN PEXT2_FCB           Dcb,
    IN struct inode       *Inode,
    IN PUNICODE_STRING     FileName,
    struct dentry        **Dentry
)
{
    struct dentry          *de = NULL;

    NTSTATUS                status = STATUS_UNSUCCESSFUL;
    int                     rc;

    BOOLEAN                 MainResourceAcquired = FALSE;

    if (!IsDirectory(Dcb)) {
        DbgBreak();
        return STATUS_NOT_A_DIRECTORY;
    }

    /* an immutable directory takes no entries (chattr +i) */
    if (Ext4IsImmutable(Dcb->Inode)) {
        return STATUS_ACCESS_DENIED;
    }

    ExAcquireResourceExclusiveLite(&Dcb->MainResource, TRUE);
    MainResourceAcquired = TRUE;

    __try {

        Ext2ReferXcb(&Dcb->ReferenceCount);

        /* an inline directory becomes a block one before it changes */
        status = Ext4UninlineDir(IrpContext, Vcb, Dcb->Mcb);
        if (!NT_SUCCESS(status)) {
            __leave;
        }

        de = Ext2BuildEntry(Vcb, Dcb->Mcb, FileName);
        if (!de) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }
        de->d_inode = Inode;

        rc = ext3_add_entry(IrpContext, de, Inode);
        status = Ext2WinntError(rc);
        if (NT_SUCCESS(status)) {

            Ext4DirNameAdded(Dcb->Inode, de->d_name.name, de->d_name.len);

            /* increase dir inode's nlink for .. */
            if (S_ISDIR(Inode->i_mode)) {
                ext3_inc_count(Dcb->Inode);
                ext3_mark_inode_dirty(IrpContext, Dcb->Inode);
            }

            /* increase inode nlink reference */
            ext3_inc_count(Inode);
            ext3_mark_inode_dirty(IrpContext, Inode);

            if (Dentry) {
                *Dentry = de;
                de = NULL;
            }
        }

    } __finally {

        Ext2DerefXcb(&Dcb->ReferenceCount);

        if (MainResourceAcquired)    {
            ExReleaseResourceLite(&Dcb->MainResource);
        }

        if (de)
            Ext2FreeEntry(de);
    }

    return status;
}

NTSTATUS
Ext2SetFileType (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_FCB            Dcb,
    IN PEXT2_MCB            Mcb,
    IN umode_t              mode
    )
{
    UNREFERENCED_PARAMETER(Vcb);
    struct inode *dir = Dcb->Inode;
    struct buffer_head *bh = NULL;
    struct ext3_dir_entry_2 *de;
    struct inode *inode;
    NTSTATUS Status = STATUS_UNSUCCESSFUL;
    BOOLEAN  MainResourceAcquired = FALSE;

    if (!EXT4_HAS_INCOMPAT_FEATURE(dir->i_sb, EXT3_FEATURE_INCOMPAT_FILETYPE)) {
        return STATUS_SUCCESS;
    }

    if (!IsDirectory(Dcb)) {
        return STATUS_NOT_A_DIRECTORY;
    }

    ExAcquireResourceExclusiveLite(&Dcb->MainResource, TRUE);
    MainResourceAcquired = TRUE;

    __try {

        Ext2ReferXcb(&Dcb->ReferenceCount);

        /* an inline directory becomes a block one before it changes */
        Status = Ext4UninlineDir(IrpContext, Vcb, Dcb->Mcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }
        Status = STATUS_UNSUCCESSFUL;

        bh = ext3_find_entry(IrpContext, Mcb->de, &de);
        if (!bh)
            __leave;

        inode = Mcb->Inode;
        if (le32_to_cpu(de->inode) != inode->i_ino)
            __leave;

        ext3_set_de_type(inode->i_sb, de, mode);
        /* the entry lives in a leaf block: its tail checksum covers it */
        ext4_dirent_csum_set(dir, (struct ext4_dir_entry *)bh->b_data);
        mark_buffer_dirty(bh);

        if (S_ISDIR(inode->i_mode) == S_ISDIR(mode)) {
        } else if (S_ISDIR(inode->i_mode)) {
            ext3_dec_count(dir);
        } else if (S_ISDIR(mode)) {
            ext3_inc_count(dir);
        }
        dir->i_ctime = dir->i_mtime = ext3_current_time(dir);
        ext3_mark_inode_dirty(IrpContext, dir);

        inode->i_mode = mode;
        ext3_mark_inode_dirty(IrpContext, inode);

        Status = STATUS_SUCCESS;

    } __finally {

        Ext2DerefXcb(&Dcb->ReferenceCount);

        if (MainResourceAcquired)
            ExReleaseResourceLite(&Dcb->MainResource);

        if (bh)
            brelse(bh);
    }

    return Status;
}

NTSTATUS
Ext2RemoveEntry (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_FCB            Dcb,
    IN PEXT2_MCB            Mcb
)
{
    UNREFERENCED_PARAMETER(Vcb);
    struct inode *dir = Dcb->Inode;
    struct buffer_head *bh = NULL;
    struct ext3_dir_entry_2 *de;
    struct inode *inode;
    int rc = -ENOENT;
    NTSTATUS Status = STATUS_UNSUCCESSFUL;
    BOOLEAN  MainResourceAcquired = FALSE;

    if (!IsDirectory(Dcb)) {
        return STATUS_NOT_A_DIRECTORY;
    }

    /* entries leave neither an immutable nor an append-only directory, nor
       does a name of an immutable or append-only inode go */
    if (Ext4IsSealed(dir) || Ext4IsSealed(Mcb->Inode)) {
        return STATUS_ACCESS_DENIED;
    }

    ExAcquireResourceExclusiveLite(&Dcb->MainResource, TRUE);
    MainResourceAcquired = TRUE;

    __try {

        Ext2ReferXcb(&Dcb->ReferenceCount);

        /* an inline directory becomes a block one before it changes */
        Status = Ext4UninlineDir(IrpContext, Vcb, Dcb->Mcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }
        Status = STATUS_UNSUCCESSFUL;

        bh = ext3_find_entry(IrpContext, Mcb->de, &de);
        if (!bh)
            __leave;

        inode = Mcb->Inode;
        if (le32_to_cpu(de->inode) != inode->i_ino)
            __leave;

        if (!inode->i_nlink) {
            ext3_warning (inode->i_sb, "ext3_unlink",
                          "Deleting nonexistent file (%lu), %d",
                          inode->i_ino, inode->i_nlink);
            inode->i_nlink = 1;
        }
        rc = ext3_delete_entry(IrpContext, dir, de, bh);
        if (rc) {
            Status = Ext2WinntError(rc);
            __leave;
        }
        Ext4DirNameRemoved(dir);
        /*
        	    if (!inode->i_nlink)
        		    ext3_orphan_add(handle, inode);
        */
        dir->i_ctime = dir->i_mtime = ext3_current_time(dir);
        inode->i_ctime = inode->i_mtime = ext3_current_time(inode);
        ext3_dec_count(inode);
        ext3_mark_inode_dirty(IrpContext, inode);

        /* decrease dir inode's nlink for .. */
        if (S_ISDIR(inode->i_mode)) {
            ext3_update_dx_flag(dir);
            ext3_dec_count(dir);
            ext3_mark_inode_dirty(IrpContext, dir);
        }

        Status = STATUS_SUCCESS;

    } __finally {

        Ext2DerefXcb(&Dcb->ReferenceCount);

        if (MainResourceAcquired)
            ExReleaseResourceLite(&Dcb->MainResource);

        if (bh)
            brelse(bh);
    }

    return Status;
}

NTSTATUS
Ext2SetParentEntry (
    IN PEXT2_IRP_CONTEXT   IrpContext,
    IN PEXT2_VCB           Vcb,
    IN PEXT2_FCB           Dcb,
    IN ULONG               OldParent,
    IN ULONG               NewParent )
{
    UNREFERENCED_PARAMETER(Vcb);
    NTSTATUS                Status = STATUS_UNSUCCESSFUL;
    struct inode           *dir;
    struct buffer_head     *bh = NULL;
    struct ext3_dir_entry_2 *pSelf, *pParent;
    BOOLEAN                 MainResourceAcquired = FALSE;
    int                     err = 0;

    if (!IsDirectory(Dcb)) {
        return STATUS_NOT_A_DIRECTORY;
    }

    if (OldParent == NewParent) {
        return STATUS_SUCCESS;
    }

    MainResourceAcquired =
        ExAcquireResourceExclusiveLite(&Dcb->MainResource, TRUE);

    __try {

        Ext2ReferXcb(&Dcb->ReferenceCount);

        /* ".." is the second entry of the directory's first block. Go
           through the buffer layer like every other directory change:
           journaled, and the block's tail checksum is refreshed (the data
           path used before left a stale checksum behind on every move) */
        dir = Dcb->Inode;
        /* an inline directory becomes a block one before it changes */
        Status = Ext4UninlineDir(IrpContext, Vcb, Dcb->Mcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }
        bh = ext3_bread(IrpContext, dir, 0, &err);
        if (!bh) {
            DEBUG(DL_ERR, ( "Ext2SetParentEntry: failed to read directory (%d).\n", err));
            Status = err ? Ext2WinntError(err) : STATUS_UNEXPECTED_IO_ERROR;
            __leave;
        }

        pSelf = (struct ext3_dir_entry_2 *)bh->b_data;
        pParent = ext3_next_entry(pSelf);

        if (pSelf->name_len == 1 && pSelf->name[0] == '.' &&
                pParent->name_len == 2 && pParent->name[0] == '.' &&
                pParent->name[1] == '.') {

            if (le32_to_cpu(pParent->inode) != OldParent) {
                DbgBreak();
            }
            pParent->inode = cpu_to_le32(NewParent);
            ext4_dirent_csum_set(dir, (struct ext4_dir_entry *)bh->b_data);
            mark_buffer_dirty(bh);
            Status = STATUS_SUCCESS;

        } else {
            DbgBreak();
            Status = STATUS_FILE_CORRUPT_ERROR;
        }

    } __finally {

        if (bh) {
            brelse(bh);
        }

        if (Ext2DerefXcb(&Dcb->ReferenceCount) == 0) {
            DEBUG(DL_ERR, ( "Ext2SetParentEntry: Dcb reference goes to ZERO.\n"));
        }

        if (MainResourceAcquired)    {
            ExReleaseResourceLite(&Dcb->MainResource);
        }
    }

    return Status;
}

int ext3_check_dir_entry (const char * function, struct inode * dir,
                          struct ext3_dir_entry_2 * de,
                          struct buffer_head * bh,
                          unsigned long offset)
{
    const char * error_msg = NULL;
    const int rlen = ext3_rec_len_from_disk(de->rec_len);

    if (rlen < EXT3_DIR_REC_LEN(1))
        error_msg = "rec_len is smaller than minimal";
    else if (rlen % 4 != 0)
        error_msg = "rec_len % 4 != 0";
    else if (rlen < EXT3_DIR_REC_LEN(de->name_len))
        error_msg = "rec_len is too small for name_len";
    else if ((char *) de + rlen > bh->b_data + dir->i_sb->s_blocksize)
        error_msg = "directory entry across blocks";
    else if (le32_to_cpu(de->inode) >
             le32_to_cpu(EXT4_SB(dir->i_sb)->s_es->s_inodes_count))
        error_msg = "inode out of bounds";

    if (error_msg != NULL) {
        DEBUG(DL_ERR, ("%s: bad entry in directory %u: %s - "
                       "offset=%u, inode=%u, rec_len=%d, name_len=%d\n",
                       function, dir->i_ino, error_msg, offset,
                       (unsigned long) le32_to_cpu(de->inode),
                       rlen, de->name_len));
    }
    return error_msg == NULL ? 1 : 0;
}

/*
 * p is at least 6 bytes before the end of page
 */
struct ext3_dir_entry_2 *
            ext3_next_entry(struct ext3_dir_entry_2 *p)
{
    return (struct ext3_dir_entry_2 *)((char *)p +
                                       ext3_rec_len_from_disk(p->rec_len));
}
