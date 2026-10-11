/**
 * ExtendedAttributes.c - IRP_MJ_QUERY_EA / IRP_MJ_SET_EA on top of ext4 extended attributes.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include <linux/ext4_xattr.h>

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2QueryEa)
#pragma alloc_text(PAGE, Ext2SetEa)
#pragma alloc_text(PAGE, Ext2IsEaNameValid)
#endif

#define EXT2_EA_NAME_MAX    MAXUCHAR    /* FILE_FULL_EA_INFORMATION.EaNameLength */
#define EXT2_EA_VALUE_MAX   MAXUSHORT   /* FILE_FULL_EA_INFORMATION.EaValueLength */

/*
 * Windows EAs are the user.* xattrs whose value an EA can hold (its length
 * is a USHORT; a value of 64 KiB, which ea_inode allows, is not one -
 * listed, its length used to wrap to 0 over 64 KiB of data). security.*,
 * trusted.*, system.* (ACLs, inline data) stay Linux's: neither listed nor
 * counted, so names never collide once the prefix is gone.
 */
static BOOLEAN
Ext2IsWindowsEa(IN const struct ext4_xattr_item *item)
{
    return item->name_index == EXT4_XATTR_INDEX_USER && item->data_size <= EXT2_EA_VALUE_MAX;
}

/* the bytes of one FILE_FULL_EA_INFORMATION entry, unpadded */
static ULONG
Ext2EaEntrySize(IN ULONG NameLength, IN ULONG ValueLength)
{
    return (ULONG)FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName) + NameLength + 1 + ValueLength;
}

/*
 * EA names are case-insensitive on Windows, xattr names case-sensitive in
 * ext4. A Windows name therefore matches a user.* xattr in any case: a query
 * for "COLOR" finds the "color" Linux wrote, and setting "COLOR" replaces it
 * instead of adding a second attribute Windows could never tell apart.
 * Inodes carry a handful of xattrs, so a scan of the list is the whole cost.
 */
static struct ext4_xattr_item *
Ext2FindEa(struct ext4_xattr_ref *ref, const char *name, size_t name_len)
{
    struct list_head *pos;

    for (pos = ref->ordered_list.next; pos != &ref->ordered_list; pos = pos->next) {
        struct ext4_xattr_item *item = list_entry(pos, struct ext4_xattr_item, list_node);
        if (item->name_index == EXT4_XATTR_INDEX_USER && item->name_len == name_len &&
            !_strnicmp(item->name, name, name_len)) {
            return item;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- the list */

static int
Ext2CountEa(struct ext4_xattr_ref *ref, struct ext4_xattr_item *item, BOOL is_last)
{
    PULONG EaSize = ref->iter_arg;

    UNREFERENCED_PARAMETER(is_last);
    if (Ext2IsWindowsEa(item)) {
        /* the packed (FEA) entry: no link to the next one, no padding */
        *EaSize += Ext2EaEntrySize((ULONG)item->name_len, (ULONG)item->data_size) - sizeof(ULONG);
    }
    return EXT4_XATTR_ITERATE_CONT;
}

/*
 * The size of the file's EAs as NTFS reports it (FileEaInformation): the
 * packed list - its length field and the entries without links or padding;
 * 0: none.
 */
NTSTATUS
Ext2EaListSize(IN PEXT2_IRP_CONTEXT IrpContext, IN PEXT2_VCB Vcb, IN PEXT2_MCB Mcb, OUT PULONG EaSize)
{
    struct ext4_xattr_ref   xattr_ref;
    NTSTATUS                Status;

    *EaSize = 0;
    Status = Ext2WinntError(ext4_fs_get_xattr_ref(IrpContext, Vcb, Mcb, &xattr_ref));
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    xattr_ref.iter_arg = EaSize;
    ext4_fs_xattr_iterate(&xattr_ref, Ext2CountEa);
    (void)ext4_fs_put_xattr_ref(&xattr_ref);    /* read only: nothing written */

    if (*EaSize != 0) {
        *EaSize += sizeof(ULONG);
    }
    return STATUS_SUCCESS;
}

/* entries filled into a caller's buffer, one after the other */
typedef struct _EXT2_EA_FILL {
    PFILE_FULL_EA_INFORMATION   Next;       /* where the next entry goes */
    PFILE_FULL_EA_INFORMATION   Last;       /* the entry filled last */
    ULONG                       Left;       /* bytes left */
    ULONG                       Count;      /* entries filled */
    ULONG                       Skip;       /* the index (from 1) to start at */
    ULONG                       Index;      /* the index of the next EA met */
    ULONG                       Seen;       /* Windows EAs met */
    BOOLEAN                     Single;
    BOOLEAN                     Overflow;   /* an entry did not fit */
} EXT2_EA_FILL, *PEXT2_EA_FILL;

/* one entry; FALSE when it does not fit (an entry but the last is padded) */
static BOOLEAN
Ext2FillEa(IN OUT PEXT2_EA_FILL F, IN const char *Name, IN ULONG NameLength,
           IN const void *Value, IN ULONG ValueLength, IN BOOLEAN IsLast)
{
    PFILE_FULL_EA_INFORMATION   Ea = F->Next;
    ULONG                       Size = Ext2EaEntrySize(NameLength, ValueLength);

    if (!IsLast) {
        Size = ALIGN_UP(Size, ULONG);
    }
    if (Size > F->Left) {
        F->Overflow = TRUE;
        return FALSE;
    }

    Ea->NextEntryOffset = 0;
    Ea->Flags = 0;
    Ea->EaNameLength = (UCHAR)NameLength;
    Ea->EaValueLength = (USHORT)ValueLength;
    RtlCopyMemory(Ea->EaName, Name, NameLength);
    Ea->EaName[NameLength] = 0;
    if (ValueLength != 0) {
        RtlCopyMemory(&Ea->EaName[NameLength + 1], Value, ValueLength);
    }

    if (F->Last != NULL) {
        F->Last->NextEntryOffset = (ULONG)((PCHAR)Ea - (PCHAR)F->Last);
    }
    F->Last = Ea;
    F->Next = (PFILE_FULL_EA_INFORMATION)((PCHAR)Ea + Size);
    F->Left -= Size;
    F->Count++;
    return TRUE;
}

/* the file's EAs in their order, from F->Skip on; an entry that does not
   fit stops the fill and is the next one to return (it used to be passed
   over by the next query) */
static int
Ext2FillEaItem(struct ext4_xattr_ref *ref, struct ext4_xattr_item *item, BOOL is_last)
{
    PEXT2_EA_FILL F = ref->iter_arg;

    if (!Ext2IsWindowsEa(item)) {
        return EXT4_XATTR_ITERATE_CONT;
    }
    F->Seen++;
    if (F->Index < F->Skip) {
        F->Index++;
        return EXT4_XATTR_ITERATE_CONT;
    }
    if (!Ext2FillEa(F, item->name, (ULONG)item->name_len, item->data, (ULONG)item->data_size,
                    is_last || F->Single)) {
        return EXT4_XATTR_ITERATE_STOP;
    }
    F->Index++;
    return F->Single ? EXT4_XATTR_ITERATE_STOP : EXT4_XATTR_ITERATE_CONT;
}

/*
 * The FILE_GET_EA_INFORMATION list of a query: every entry, its name
 * included, must lie inside the list, and every link must move forward to a
 * 4-byte boundary. Checked once up front so the answering pass needs no
 * bounds of its own.
 */
static NTSTATUS
Ext2CheckGetEaList(const UCHAR *List, ULONG Length, PULONG ErrorOffset)
{
    ULONG Offset = 0;

    for (;;) {
        const FILE_GET_EA_INFORMATION *GetEa = (const FILE_GET_EA_INFORMATION *)(List + Offset);
        const ULONG Header = (ULONG)FIELD_OFFSET(FILE_GET_EA_INFORMATION, EaName);
        ULONG Left = Length - Offset;

        if (Left < Header || Left - Header < GetEa->EaNameLength || GetEa->EaNameLength == 0) {
            break;
        }
        if (GetEa->NextEntryOffset == 0) {
            return STATUS_SUCCESS;
        }
        if ((GetEa->NextEntryOffset & (sizeof(ULONG) - 1)) || GetEa->NextEntryOffset >= Left) {
            break;
        }
        Offset += GetEa->NextEntryOffset;
    }
    *ErrorOffset = Offset;
    return STATUS_EA_LIST_INCONSISTENT;
}

/* one entry per name asked for, in the caller's order; a name the file does
   not have comes back with an empty value, as NTFS and FAT answer it */
static NTSTATUS
Ext2QueryEaByName(IN struct ext4_xattr_ref *ref, IN PUCHAR List, IN ULONG ListLength,
                  IN OUT PEXT2_EA_FILL F, OUT PULONG_PTR ErrorOffset)
{
    ULONG       Offset = 0, Bad = 0;
    NTSTATUS    Status;

    Status = Ext2CheckGetEaList(List, ListLength, &Bad);
    if (!NT_SUCCESS(Status)) {
        *ErrorOffset = Bad;
        return Status;
    }

    for (;;) {
        PFILE_GET_EA_INFORMATION GetEa = (PFILE_GET_EA_INFORMATION)(List + Offset);
        struct ext4_xattr_item *Item = Ext2FindEa(ref, GetEa->EaName, GetEa->EaNameLength);
        BOOLEAN IsLast = GetEa->NextEntryOffset == 0 || F->Single;

        if (Item != NULL && !Ext2IsWindowsEa(Item)) {
            Item = NULL;
        }
        if (!Ext2FillEa(F, GetEa->EaName, GetEa->EaNameLength,
                        Item ? Item->data : NULL, Item ? (ULONG)Item->data_size : 0, IsLast)) {
            return F->Count ? STATUS_BUFFER_OVERFLOW : STATUS_BUFFER_TOO_SMALL;
        }
        if (IsLast) {
            return STATUS_SUCCESS;
        }
        Offset += GetEa->NextEntryOffset;
    }
}

/* What a walk of the list filled, as FAT and NTFS report it: nothing to
   find on the file, the end of the list passed, a buffer too small for the
   first entry or only for a later one. An index past the end names nothing
   (it used to be answered STATUS_OBJECTID_NOT_FOUND). */
static NTSTATUS
Ext2EaFillStatus(IN PEXT2_EA_FILL F, IN BOOLEAN ByIndex)
{
    if (F->Overflow) {
        return F->Count ? STATUS_BUFFER_OVERFLOW : STATUS_BUFFER_TOO_SMALL;
    }
    if (F->Count != 0) {
        return STATUS_SUCCESS;
    }
    if (F->Seen == 0) {
        return STATUS_NO_EAS_ON_FILE;
    }
    return ByIndex ? STATUS_NONEXISTENT_EA_ENTRY : STATUS_NO_MORE_EAS;
}

/* EAs belong to files and directories: the control device and a volume
   handle have none - their FsContext is no Fcb, and was used as one */
static NTSTATUS
Ext2EaHandle(IN PEXT2_IRP_CONTEXT IrpContext)
{
    PEXT2_FCB   Fcb = IrpContext->Fcb;

    if (IsExt2FsDevice(IrpContext->DeviceObject)) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    if (Fcb == NULL || Fcb->Identifier.Type != EXT2FCB || IrpContext->Ccb == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
Ext2QueryEa (IN PEXT2_IRP_CONTEXT IrpContext)
{
    PIRP                Irp = IrpContext->Irp;
    PIO_STACK_LOCATION  IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PEXT2_VCB           Vcb = (PEXT2_VCB)IrpContext->DeviceObject->DeviceExtension;
    PEXT2_FCB           Fcb = IrpContext->Fcb;
    PEXT2_CCB           Ccb = IrpContext->Ccb;
    ULONG               Length = IrpSp->Parameters.QueryEa.Length;
    PUCHAR              Buffer;
    EXT2_EA_FILL        F = { 0 };
    struct ext4_xattr_ref xattr_ref;
    BOOLEAN             MainResourceAcquired = FALSE;
    BOOLEAN             XattrRefAcquired = FALSE;
    NTSTATUS            Status = STATUS_UNSUCCESSFUL;

    __try {

        Irp->IoStatus.Information = 0;

        Status = Ext2EaHandle(IrpContext);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }
        ASSERT((Ccb->Identifier.Type == EXT2CCB) && (Ccb->Identifier.Size == sizeof(EXT2_CCB)));

        Buffer = Ext2GetUserBuffer(Irp);
        if (Buffer == NULL) {
            Status = STATUS_INVALID_USER_BUFFER;
            __leave;
        }

        /* readers share the inode; Ext2SetEa takes it exclusive */
        if (!ExAcquireResourceSharedLite(&Fcb->MainResource,
                                         IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT))) {
            Status = STATUS_PENDING;
            __leave;
        }
        MainResourceAcquired = TRUE;

        Status = Ext2WinntError(ext4_fs_get_xattr_ref(IrpContext, Vcb, Fcb->Mcb, &xattr_ref));
        if (!NT_SUCCESS(Status)) {
            __leave;
        }
        XattrRefAcquired = TRUE;

        RtlZeroMemory(Buffer, Length);
        F.Next = (PFILE_FULL_EA_INFORMATION)Buffer;
        F.Left = Length;
        F.Index = 1;
        F.Single = BooleanFlagOn(IrpSp->Flags, SL_RETURN_SINGLE_ENTRY);

        if (IrpSp->Parameters.QueryEa.EaList != NULL) {

            Status = Ext2QueryEaByName(&xattr_ref, IrpSp->Parameters.QueryEa.EaList,
                                       IrpSp->Parameters.QueryEa.EaListLength, &F,
                                       &Irp->IoStatus.Information);

        } else if (BooleanFlagOn(IrpSp->Flags, SL_INDEX_SPECIFIED)) {

            /* the one entry at the caller's index (from 1) */
            F.Skip = IrpSp->Parameters.QueryEa.EaIndex;
            F.Single = TRUE;
            xattr_ref.iter_arg = &F;
            ext4_fs_xattr_iterate(&xattr_ref, Ext2FillEaItem);
            Status = F.Skip == 0 ? STATUS_NONEXISTENT_EA_ENTRY : Ext2EaFillStatus(&F, TRUE);

        } else {

            /* on from where the handle's last scan stopped */
            if (BooleanFlagOn(IrpSp->Flags, SL_RESTART_SCAN) || Ccb->EaIndex == 0) {
                Ccb->EaIndex = 1;
            }
            F.Skip = Ccb->EaIndex;
            xattr_ref.iter_arg = &F;
            ext4_fs_xattr_iterate(&xattr_ref, Ext2FillEaItem);
            Ccb->EaIndex = F.Index;
            Status = Ext2EaFillStatus(&F, FALSE);
        }

        if (NT_SUCCESS(Status) || Status == STATUS_BUFFER_OVERFLOW) {
            Irp->IoStatus.Information = Length - F.Left;
        }

    } __finally {

        /* a query writes nothing, and changes nothing to notify about */
        if (XattrRefAcquired) {
            xattr_ref.dirty = FALSE;
            (void)ext4_fs_put_xattr_ref(&xattr_ref);
        }
        if (MainResourceAcquired) {
            ExReleaseResourceLite(&Fcb->MainResource);
        }

        if (!AbnormalTermination()) {
            if (Status == STATUS_PENDING || Status == STATUS_CANT_WAIT) {
                Status = Ext2QueueRequest(IrpContext);
            } else {
                Ext2CompleteIrpContext(IrpContext, Status);
            }
        }
    }

    return Status;
}

/* a name Windows may give an EA: not empty, not longer than an EA holds,
   characters FAT allows */
BOOLEAN
Ext2IsEaNameValid(IN OEM_STRING Name)
{
    ULONG Index;

    if (Name.Length == 0 || Name.Length > EXT2_EA_NAME_MAX) {
        return FALSE;
    }

    for (Index = 0; Index < (ULONG)Name.Length; Index++) {
        UCHAR Char = Name.Buffer[Index];

        /* a lead byte of a double-byte character takes its trail along */
        if (FsRtlIsLeadDbcsCharacter(Char)) {
            ASSERT(Index != (ULONG)(Name.Length - 1));
            Index++;
            continue;
        }
        if (!FsRtlIsAnsiCharacterLegalFat(Char, FALSE)) {
            return FALSE;
        }
    }
    return TRUE;
}

/* the entries of a FILE_FULL_EA_INFORMATION list (IoCheckEaBufferValidity
   has checked its links) */
#define FOR_EACH_FULL_EA(Ea, Buffer, Length)                                         \
    for ((Ea) = (PFILE_FULL_EA_INFORMATION)(Buffer);                                  \
         (PCHAR)(Ea) < (PCHAR)(Buffer) + (Length);                                    \
         (Ea) = (PFILE_FULL_EA_INFORMATION)((Ea)->NextEntryOffset == 0 ?              \
                (PCHAR)(Buffer) + (Length) : (PCHAR)(Ea) + (Ea)->NextEntryOffset))

/*
 * NtSetEaFile semantics, as NTFS implements them: each entry adds or
 * replaces the EA of that name, an entry with an empty value deletes it,
 * and every EA not named is left alone. Windows EAs live in the "user."
 * namespace; the other namespaces of the inode (POSIX ACLs, security
 * labels, capabilities) are never touched from here. The old code purged
 * everything first, so a single EA written by Windows - Smart App Control
 * tags every copied executable - wiped the Linux ACLs of the file.
 */
NTSTATUS
Ext2SetEaList(IN struct ext4_xattr_ref *ref, IN PCHAR Buffer, IN ULONG Length,
              OUT PULONG_PTR ErrorOffset)
{
    PFILE_FULL_EA_INFORMATION   FullEa;
    OEM_STRING                  EaName;
    NTSTATUS                    Status;

    /* every name valid before anything changes */
    FOR_EACH_FULL_EA(FullEa, Buffer, Length) {
        EaName.MaximumLength = EaName.Length = FullEa->EaNameLength;
        EaName.Buffer = FullEa->EaName;
        if (!Ext2IsEaNameValid(EaName)) {
            *ErrorOffset = (PCHAR)FullEa - Buffer;
            return STATUS_INVALID_EA_NAME;
        }
    }

    ref->dirty = TRUE;
    FOR_EACH_FULL_EA(FullEa, Buffer, Length) {

        /* the old value goes, in whatever case it was stored (absent is
           fine); the new one comes unless the caller asked for deletion */
        for (;;) {
            struct ext4_xattr_item *Old = Ext2FindEa(ref, FullEa->EaName, FullEa->EaNameLength);
            if (Old == NULL ||
                ext4_fs_remove_xattr(ref, EXT4_XATTR_INDEX_USER, Old->name, Old->name_len)) {
                break;
            }
        }
        if (FullEa->EaValueLength == 0) {
            continue;
        }

        Status = Ext2WinntError(ext4_fs_set_xattr_ordered(ref, EXT4_XATTR_INDEX_USER,
                                                          FullEa->EaName, FullEa->EaNameLength,
                                                          &FullEa->EaName[FullEa->EaNameLength + 1],
                                                          FullEa->EaValueLength));
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS
Ext2SetEa (IN PEXT2_IRP_CONTEXT IrpContext)
{
    PIRP                Irp = IrpContext->Irp;
    PIO_STACK_LOCATION  IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PEXT2_VCB           Vcb = (PEXT2_VCB)IrpContext->DeviceObject->DeviceExtension;
    PEXT2_FCB           Fcb = IrpContext->Fcb;
    PCHAR               Buffer = Irp->UserBuffer;
    ULONG               Length = IrpSp->Parameters.SetEa.Length;
    struct ext4_xattr_ref xattr_ref;
    BOOLEAN             MainResourceAcquired = FALSE;
    BOOLEAN             XattrRefAcquired = FALSE;
    NTSTATUS            Status = STATUS_UNSUCCESSFUL;

    __try {

        Irp->IoStatus.Information = 0;

        Status = Ext2EaHandle(IrpContext);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }
        ASSERT((IrpContext->Ccb->Identifier.Type == EXT2CCB) &&
               (IrpContext->Ccb->Identifier.Size == sizeof(EXT2_CCB)));

        if (IsVcbReadOnly(Vcb)) {
            Status = STATUS_MEDIA_WRITE_PROTECTED;
            __leave;
        }

        Status = IoCheckEaBufferValidity((PFILE_FULL_EA_INFORMATION)Buffer, Length,
                                         (PULONG)&Irp->IoStatus.Information);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        /* the inode's: one writer at a time, readers wait. The volume's
           FcbLock was held as well - every open of the volume waited for
           the attribute block to be written. */
        if (!ExAcquireResourceExclusiveLite(&Fcb->MainResource,
                                            IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT))) {
            Status = STATUS_PENDING;
            __leave;
        }
        MainResourceAcquired = TRUE;

        /* an immutable or append-only inode keeps its attributes, as Linux
           refuses setxattr on it */
        if (Ext4IsSealed(Fcb->Inode)) {
            Status = STATUS_ACCESS_DENIED;
            __leave;
        }

        Status = Ext2WinntError(ext4_fs_get_xattr_ref(IrpContext, Vcb, Fcb->Mcb, &xattr_ref));
        if (!NT_SUCCESS(Status)) {
            __leave;
        }
        XattrRefAcquired = TRUE;

        Status = Ext2SetEaList(&xattr_ref, Buffer, Length, &Irp->IoStatus.Information);

    } __finally {

        if (XattrRefAcquired) {
            if (!NT_SUCCESS(Status)) {
                xattr_ref.dirty = FALSE;
                (void)ext4_fs_put_xattr_ref(&xattr_ref);    /* a release: nothing written */
            } else {
                Status = Ext2WinntError(ext4_fs_put_xattr_ref(&xattr_ref));
            }
        }

        if (MainResourceAcquired) {
            ExReleaseResourceLite(&Fcb->MainResource);
        }

        if (!AbnormalTermination()) {
            if (NT_SUCCESS(Status)) {
                Ext2NotifyReportChange(IrpContext, Vcb, Fcb->Mcb,
                                       FILE_NOTIFY_CHANGE_EA, FILE_ACTION_MODIFIED);
            }
            if (Status == STATUS_PENDING || Status == STATUS_CANT_WAIT) {
                Status = Ext2QueueRequest(IrpContext);
            } else {
                Ext2CompleteIrpContext(IrpContext, Status);
            }
        }
    }
    return Status;
}
