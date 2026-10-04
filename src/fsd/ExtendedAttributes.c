/**
 * ExtendedAttributes.c - IRP_MJ_QUERY_EA / IRP_MJ_SET_EA on top of ext4 extended attributes.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include <linux/ext4_xattr.h>

/* Ea iterator */
struct EaIterator {
	/* Return only an entry */
	BOOLEAN ReturnSingleEntry;

	/* Is the buffer overflowing? */
	BOOL OverFlow;

	/* FILE_FULL_EA_INFORMATION output buffer */
	PFILE_FULL_EA_INFORMATION FullEa;
	PFILE_FULL_EA_INFORMATION LastFullEa;

	/* UserBuffer's size */
	ULONG UserBufferLength;

	/* Remaining UserBuffer's size */
	ULONG RemainingUserBufferLength;

	/* Start scanning from this EA */
	ULONG EaIndex;

	/* Next EA index returned by Ext2IterateAllEa */
	ULONG EaIndexCounter;
};

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Ext2QueryEa)
#pragma alloc_text(PAGE, Ext2SetEa)
#pragma alloc_text(PAGE, Ext2IsEaNameValid)
#endif

static int Ext2IterateAllEa(struct ext4_xattr_ref *xattr_ref, struct ext4_xattr_item *item, BOOL is_last)
{
	struct EaIterator *pEaIterator = xattr_ref->iter_arg;
	ULONG EaEntrySize = (ULONG)(4 + 1 + 1 + 2 + item->name_len + 1 + item->data_size);
	ASSERT(pEaIterator);

	/* Windows EAs are the user.* namespace: security.*, trusted.*,
	   system.* (ACLs, inline data) stay Linux's and are neither listed nor
	   counted in the index, so names never collide after the prefix goes */
	if (item->name_index != EXT4_XATTR_INDEX_USER)
		return EXT4_XATTR_ITERATE_CONT;

	if (!is_last && !pEaIterator->ReturnSingleEntry)
		EaEntrySize = ALIGN_UP(EaEntrySize, ULONG);

	/* Start iteration from index specified */
	if (pEaIterator->EaIndexCounter < pEaIterator->EaIndex) {
		pEaIterator->EaIndexCounter++;
		return EXT4_XATTR_ITERATE_CONT;
	}
	pEaIterator->EaIndexCounter++;

	if (EaEntrySize > pEaIterator->RemainingUserBufferLength) {
		pEaIterator->OverFlow = TRUE;
		return EXT4_XATTR_ITERATE_STOP;
	}
	pEaIterator->FullEa->NextEntryOffset = 0;
	pEaIterator->FullEa->Flags = 0;
	pEaIterator->FullEa->EaNameLength = (UCHAR)item->name_len;
	pEaIterator->FullEa->EaValueLength = (USHORT)item->data_size;
	RtlCopyMemory(&pEaIterator->FullEa->EaName[0],
		item->name,
		item->name_len);
	RtlCopyMemory(&pEaIterator->FullEa->EaName[0] + item->name_len + 1,
		item->data,
		item->data_size);

	/* Link FullEa and LastFullEa together */
	if (pEaIterator->LastFullEa) {
		pEaIterator->LastFullEa->NextEntryOffset = (ULONG)
			((PCHAR)pEaIterator->FullEa - (PCHAR)pEaIterator->LastFullEa);
	}

	pEaIterator->LastFullEa = pEaIterator->FullEa;
	pEaIterator->FullEa = (PFILE_FULL_EA_INFORMATION)
			((PCHAR)pEaIterator->FullEa + EaEntrySize);
	pEaIterator->RemainingUserBufferLength -= EaEntrySize;

	if (pEaIterator->ReturnSingleEntry)
		return EXT4_XATTR_ITERATE_STOP;

	return EXT4_XATTR_ITERATE_CONT;
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
		struct ext4_xattr_item *item =
			list_entry(pos, struct ext4_xattr_item, list_node);
		if (item->name_index == EXT4_XATTR_INDEX_USER &&
		    item->name_len == name_len &&
		    !_strnicmp(item->name, name, name_len))
			return item;
	}
	return NULL;
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
		const FILE_GET_EA_INFORMATION *GetEa =
			(const FILE_GET_EA_INFORMATION *)(List + Offset);
		const ULONG Header = (ULONG)FIELD_OFFSET(FILE_GET_EA_INFORMATION, EaName);
		ULONG Left = Length - Offset;

		if (Left < Header || Left - Header < GetEa->EaNameLength ||
		    GetEa->EaNameLength == 0)
			break;
		if (GetEa->NextEntryOffset == 0)
			return STATUS_SUCCESS;
		if ((GetEa->NextEntryOffset & (sizeof(ULONG) - 1)) ||
		    GetEa->NextEntryOffset >= Left)
			break;
		Offset += GetEa->NextEntryOffset;
	}
	*ErrorOffset = Offset;
	return STATUS_EA_LIST_INCONSISTENT;
}

NTSTATUS
Ext2QueryEa (
	IN PEXT2_IRP_CONTEXT    IrpContext
)
{
	PIRP                Irp = NULL;
	PIO_STACK_LOCATION  IrpSp;

	PDEVICE_OBJECT      DeviceObject;

	PEXT2_VCB           Vcb = NULL;
	PEXT2_FCB           Fcb = NULL;
	PEXT2_CCB           Ccb = NULL;
	PEXT2_MCB           Mcb = NULL;

	PUCHAR  UserEaList;
	ULONG   UserEaListLength;
	ULONG   UserEaIndex;

	BOOLEAN RestartScan;
	BOOLEAN ReturnSingleEntry;
	BOOLEAN IndexSpecified;

	BOOLEAN             MainResourceAcquired = FALSE;
	BOOLEAN             XattrRefAcquired = FALSE;

	NTSTATUS            Status = STATUS_UNSUCCESSFUL;

	struct ext4_xattr_ref xattr_ref;
	PCHAR UserBuffer;

	ULONG UserBufferLength = 0;
	ULONG RemainingUserBufferLength = 0;

	PFILE_FULL_EA_INFORMATION FullEa, LastFullEa = NULL;

	__try {

		Ccb = IrpContext->Ccb;
		ASSERT(Ccb != NULL);
		ASSERT((Ccb->Identifier.Type == EXT2CCB) &&
			(Ccb->Identifier.Size == sizeof(EXT2_CCB)));
		DeviceObject = IrpContext->DeviceObject;
		Vcb = (PEXT2_VCB)DeviceObject->DeviceExtension;
		Fcb = IrpContext->Fcb;
		Mcb = Fcb->Mcb;
		Irp = IrpContext->Irp;
		IrpSp = IoGetCurrentIrpStackLocation(Irp);

		Irp->IoStatus.Information = 0;

		/* Receive input parameter from caller */
		UserBuffer = Ext2GetUserBuffer(Irp);
		if (!UserBuffer) {
			Status = STATUS_INSUFFICIENT_RESOURCES;
			__leave;
		}
		UserBufferLength = IrpSp->Parameters.QueryEa.Length;
		RemainingUserBufferLength = UserBufferLength;
		UserEaList = IrpSp->Parameters.QueryEa.EaList;
		UserEaListLength = IrpSp->Parameters.QueryEa.EaListLength;
		UserEaIndex = IrpSp->Parameters.QueryEa.EaIndex;
		RestartScan = BooleanFlagOn(IrpSp->Flags, SL_RESTART_SCAN);
		ReturnSingleEntry = BooleanFlagOn(IrpSp->Flags, SL_RETURN_SINGLE_ENTRY);
		IndexSpecified = BooleanFlagOn(IrpSp->Flags, SL_INDEX_SPECIFIED);

		if (!Mcb)
			__leave;

		/* readers share the inode; Ext2SetEa takes it exclusive */
		if (!ExAcquireResourceSharedLite(
			&Fcb->MainResource,
			IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT))) {
			Status = STATUS_PENDING;
			__leave;
		}
		MainResourceAcquired = TRUE;

		Status = Ext2WinntError(ext4_fs_get_xattr_ref(IrpContext, Vcb, Fcb->Mcb, &xattr_ref));
		if (!NT_SUCCESS(Status)) {
			DbgPrint("ext4_fs_get_xattr_ref() failed!\n");
			__leave;
		}

		FullEa = (PFILE_FULL_EA_INFORMATION)UserBuffer;

		XattrRefAcquired = TRUE;

		if (RemainingUserBufferLength)
			RtlZeroMemory(FullEa, RemainingUserBufferLength);

		if (UserEaList != NULL) {
			ULONG Offset = 0;
			ULONG Count = 0;
			ULONG ErrorOffset = 0;

			Status = Ext2CheckGetEaList(UserEaList, UserEaListLength, &ErrorOffset);
			if (!NT_SUCCESS(Status)) {
				Irp->IoStatus.Information = ErrorOffset;
				__leave;
			}

			/* one entry per name asked for, in the caller's order; a name
			   the file does not have comes back with an empty value, as
			   NTFS and FAT answer it */
			for (;;) {
				PFILE_GET_EA_INFORMATION GetEa =
					(PFILE_GET_EA_INFORMATION)(UserEaList + Offset);
				struct ext4_xattr_item *Item =
					Ext2FindEa(&xattr_ref, GetEa->EaName, GetEa->EaNameLength);
				ULONG ValueLength = Item ? (ULONG)min(Item->data_size, MAXUSHORT) : 0;
				BOOLEAN IsLast = GetEa->NextEntryOffset == 0 || ReturnSingleEntry;
				ULONG EaEntrySize = FIELD_OFFSET(FILE_FULL_EA_INFORMATION, EaName) +
						    GetEa->EaNameLength + 1 + ValueLength;

				if (!IsLast)
					EaEntrySize = ALIGN_UP(EaEntrySize, ULONG);
				if (EaEntrySize > RemainingUserBufferLength) {
					Status = Count ? STATUS_BUFFER_OVERFLOW : STATUS_BUFFER_TOO_SMALL;
					__leave;
				}

				FullEa->NextEntryOffset = 0;
				FullEa->Flags = 0;
				FullEa->EaNameLength = GetEa->EaNameLength;
				FullEa->EaValueLength = (USHORT)ValueLength;
				RtlCopyMemory(&FullEa->EaName[0], &GetEa->EaName[0], GetEa->EaNameLength);
				FullEa->EaName[GetEa->EaNameLength] = 0;
				if (ValueLength)
					RtlCopyMemory(&FullEa->EaName[GetEa->EaNameLength + 1],
						      Item->data, ValueLength);

				if (LastFullEa)
					LastFullEa->NextEntryOffset = (ULONG)((PCHAR)FullEa -
									      (PCHAR)LastFullEa);
				LastFullEa = FullEa;
				FullEa = (PFILE_FULL_EA_INFORMATION)((PCHAR)FullEa + EaEntrySize);
				RemainingUserBufferLength -= EaEntrySize;
				Count++;

				if (IsLast)
					break;
				Offset += GetEa->NextEntryOffset;
			}
			Status = STATUS_SUCCESS;
		} else if (IndexSpecified) {
			struct EaIterator EaIterator;
			/* The user supplied an index into the Ea list. */
			if (RemainingUserBufferLength)
				RtlZeroMemory(FullEa, RemainingUserBufferLength);

			EaIterator.OverFlow = FALSE;
			EaIterator.RemainingUserBufferLength = UserBufferLength;
			/* In this case, return only an entry. */
			EaIterator.ReturnSingleEntry = TRUE;
			EaIterator.FullEa = (PFILE_FULL_EA_INFORMATION)UserBuffer;
			EaIterator.LastFullEa = NULL;
			EaIterator.UserBufferLength = UserBufferLength;
			EaIterator.EaIndex = UserEaIndex;
			EaIterator.EaIndexCounter = 1;

			xattr_ref.iter_arg = &EaIterator;
			ext4_fs_xattr_iterate(&xattr_ref, Ext2IterateAllEa);

			RemainingUserBufferLength = EaIterator.RemainingUserBufferLength;

			Status = STATUS_SUCCESS;

			/* It seems that the item isn't found */
			if (RemainingUserBufferLength == UserBufferLength)
				Status = STATUS_OBJECTID_NOT_FOUND;

			if (EaIterator.OverFlow) {
				if (RemainingUserBufferLength == UserBufferLength)
					Status = STATUS_BUFFER_TOO_SMALL;
				else
					Status = STATUS_BUFFER_OVERFLOW;
			}

		} else {
			struct EaIterator EaIterator;
			/* Else perform a simple scan, taking into account the restart
			   flag and the position of the next Ea stored in the Ccb. */
			if (RestartScan)
				Ccb->EaIndex = 1;

			if (RemainingUserBufferLength)
				RtlZeroMemory(FullEa, RemainingUserBufferLength);

			EaIterator.OverFlow = FALSE;
			EaIterator.RemainingUserBufferLength = UserBufferLength;
			EaIterator.ReturnSingleEntry = ReturnSingleEntry;
			EaIterator.FullEa = (PFILE_FULL_EA_INFORMATION)UserBuffer;
			EaIterator.LastFullEa = NULL;
			EaIterator.UserBufferLength = UserBufferLength;
			EaIterator.EaIndex = Ccb->EaIndex;
			EaIterator.EaIndexCounter = 1;

			xattr_ref.iter_arg = &EaIterator;
			ext4_fs_xattr_iterate(&xattr_ref, Ext2IterateAllEa);

			RemainingUserBufferLength = EaIterator.RemainingUserBufferLength;

			if (Ccb->EaIndex < EaIterator.EaIndexCounter)
				Ccb->EaIndex = EaIterator.EaIndexCounter;

			Status = STATUS_SUCCESS;

			if (EaIterator.OverFlow) {
				if (RemainingUserBufferLength == UserBufferLength)
					Status = STATUS_BUFFER_TOO_SMALL;
				else
					Status = STATUS_BUFFER_OVERFLOW;
			}

		}
	}
	__finally {

		if (XattrRefAcquired) {
			if (!NT_SUCCESS(Status)) {
				xattr_ref.dirty = FALSE;
				(void)ext4_fs_put_xattr_ref(&xattr_ref);	/* a release: nothing written */
			}
			else
				Status = Ext2WinntError(ext4_fs_put_xattr_ref(&xattr_ref));
		}

		if (MainResourceAcquired) {
			ExReleaseResourceLite(&Fcb->MainResource);
		}

		/* a query changes nothing: no directory notification */
		if (NT_SUCCESS(Status) || Status == STATUS_BUFFER_OVERFLOW)
			Irp->IoStatus.Information = UserBufferLength - RemainingUserBufferLength;

		if (!AbnormalTermination()) {
			if (Status == STATUS_PENDING || Status == STATUS_CANT_WAIT) {
				Status = Ext2QueueRequest(IrpContext);
			}
			else {
				Ext2CompleteIrpContext(IrpContext, Status);
			}
		}
	}

	return Status;
}

BOOLEAN
Ext2IsEaNameValid(
	IN OEM_STRING Name
)
{
	ULONG Index;
	UCHAR Char;

	/* Empty names are not valid */

	if (Name.Length == 0)
		return FALSE;

	/* Do not allow EA name longer than 255 bytes */
	if (Name.Length > 255)
		return FALSE;

	for (Index = 0; Index < (ULONG)Name.Length; Index += 1) {

		Char = Name.Buffer[Index];

		/* Skip over and Dbcs chacters */
		if (FsRtlIsLeadDbcsCharacter(Char)) {

			ASSERT(Index != (ULONG)(Name.Length - 1));
			Index += 1;
			continue;
		}

		/* Make sure this character is legal, and if a wild card, that
		   wild cards are permissible. */
		if (!FsRtlIsAnsiCharacterLegalFat(Char, FALSE))
			return FALSE;

	}

	return TRUE;
}

NTSTATUS
Ext2SetEa (
	IN PEXT2_IRP_CONTEXT    IrpContext
)
{
	PIRP                Irp = NULL;
	PIO_STACK_LOCATION  IrpSp;

	PDEVICE_OBJECT      DeviceObject;

	PEXT2_VCB           Vcb = NULL;
	PEXT2_FCB           Fcb = NULL;
	PEXT2_CCB           Ccb = NULL;
	PEXT2_MCB           Mcb = NULL;

	BOOLEAN             MainResourceAcquired = FALSE;
	BOOLEAN             FcbLockAcquired = FALSE;
	BOOLEAN             XattrRefAcquired = FALSE;

	NTSTATUS            Status = STATUS_UNSUCCESSFUL;

	struct ext4_xattr_ref xattr_ref;
	PCHAR UserBuffer;
	ULONG UserBufferLength;

	PFILE_FULL_EA_INFORMATION FullEa;

	__try {

		Ccb = IrpContext->Ccb;
		ASSERT(Ccb != NULL);
		ASSERT((Ccb->Identifier.Type == EXT2CCB) &&
			(Ccb->Identifier.Size == sizeof(EXT2_CCB)));
		DeviceObject = IrpContext->DeviceObject;
		Vcb = (PEXT2_VCB)DeviceObject->DeviceExtension;
		Fcb = IrpContext->Fcb;
		Mcb = Fcb->Mcb;
		Irp = IrpContext->Irp;
		IrpSp = IoGetCurrentIrpStackLocation(Irp);

		Irp->IoStatus.Information = 0;

		/* Receive input parameter from caller */
		UserBufferLength = IrpSp->Parameters.SetEa.Length;
		UserBuffer = Irp->UserBuffer;

		/* Check if the EA buffer provided is valid */
		Status = IoCheckEaBufferValidity((PFILE_FULL_EA_INFORMATION)UserBuffer,
			UserBufferLength,
			(PULONG)&Irp->IoStatus.Information);
		if (!NT_SUCCESS(Status))
			__leave;

		ExAcquireResourceExclusiveLite(&Vcb->FcbLock, TRUE);
		FcbLockAcquired = TRUE;

		if (!Mcb)
			__leave;

		/* We do not allow multiple instance gaining EA access to the same file */
		if (!ExAcquireResourceExclusiveLite(
			&Fcb->MainResource,
			IsFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT))) {
			Status = STATUS_PENDING;
			__leave;
		}
		MainResourceAcquired = TRUE;

		Status = Ext2WinntError(ext4_fs_get_xattr_ref(IrpContext, Vcb, Fcb->Mcb, &xattr_ref));
		if (!NT_SUCCESS(Status)) {
			DbgPrint("ext4_fs_get_xattr_ref() failed!\n");
			__leave;
		}

		XattrRefAcquired = TRUE;

		/* NtSetEaFile semantics, as NTFS implements them: each entry in
		   the buffer adds or replaces the EA of that name, an entry with
		   an empty value deletes it, and every EA not named is left alone.
		   Windows EAs live in the "user." namespace; the other namespaces
		   of the inode (POSIX ACLs, security labels, capabilities) are
		   never touched from here. The old code purged everything first,
		   so a single EA written by Windows - Smart App Control tags every
		   copied executable - wiped the Linux ACLs of the file. */
		xattr_ref.dirty = TRUE;
		Status = STATUS_SUCCESS;

		/* Iterate the whole EA buffer to do inspection */
		for (FullEa = (PFILE_FULL_EA_INFORMATION)UserBuffer;
			FullEa < (PFILE_FULL_EA_INFORMATION)&UserBuffer[UserBufferLength];
			FullEa = (PFILE_FULL_EA_INFORMATION)(FullEa->NextEntryOffset == 0 ?
				&UserBuffer[UserBufferLength] :
				(PCHAR)FullEa + FullEa->NextEntryOffset)) {

			OEM_STRING EaName;

			EaName.MaximumLength = EaName.Length = FullEa->EaNameLength;
			EaName.Buffer = &FullEa->EaName[0];

			/* Check if EA's name is valid */
			if (!Ext2IsEaNameValid(EaName)) {
				Irp->IoStatus.Information = (PCHAR)FullEa - UserBuffer;
				Status = STATUS_INVALID_EA_NAME;
				__leave;
			}
		}

		/* Now add EA entries to the inode */
		for (FullEa = (PFILE_FULL_EA_INFORMATION)UserBuffer;
			FullEa < (PFILE_FULL_EA_INFORMATION)&UserBuffer[UserBufferLength];
			FullEa = (PFILE_FULL_EA_INFORMATION)(FullEa->NextEntryOffset == 0 ?
				&UserBuffer[UserBufferLength] :
				(PCHAR)FullEa + FullEa->NextEntryOffset)) {

				OEM_STRING EaName;

				EaName.MaximumLength = EaName.Length = FullEa->EaNameLength;
				EaName.Buffer = &FullEa->EaName[0];

				/* drop the old value in whatever case it was stored
				   (absent is fine), then store the new one unless the
				   caller asked for deletion */
				for (;;) {
					struct ext4_xattr_item *Old =
						Ext2FindEa(&xattr_ref, EaName.Buffer, EaName.Length);
					if (!Old || ext4_fs_remove_xattr(&xattr_ref, EXT4_XATTR_INDEX_USER,
									 Old->name, Old->name_len))
						break;
				}
				if (FullEa->EaValueLength == 0)
					continue;

				Status = Ext2WinntError(
					ext4_fs_set_xattr_ordered(&xattr_ref,
						EXT4_XATTR_INDEX_USER,
						EaName.Buffer,
						EaName.Length,
						&FullEa->EaName[0] + FullEa->EaNameLength + 1,
						FullEa->EaValueLength));
				if (!NT_SUCCESS(Status))
					__leave;

		}
	} __finally {

		if (XattrRefAcquired) {
			if (!NT_SUCCESS(Status)) {
				xattr_ref.dirty = FALSE;
				(void)ext4_fs_put_xattr_ref(&xattr_ref);	/* a release: nothing written */
			} else
				Status = Ext2WinntError(ext4_fs_put_xattr_ref(&xattr_ref));
		}

		if (FcbLockAcquired) {
			ExReleaseResourceLite(&Vcb->FcbLock);
			FcbLockAcquired = FALSE;
		}

		if (MainResourceAcquired) {
			ExReleaseResourceLite(&Fcb->MainResource);
		}

		if (NT_SUCCESS(Status)) {
			Ext2NotifyReportChange(
				IrpContext,
				Vcb,
				Mcb,
				FILE_NOTIFY_CHANGE_EA,
				FILE_ACTION_MODIFIED);
		}

		if (!AbnormalTermination()) {
			if (Status == STATUS_PENDING || Status == STATUS_CANT_WAIT) {
				Status = Ext2QueueRequest(IrpContext);
			}
			else {
				Ext2CompleteIrpContext(IrpContext, Status);
			}
		}
	}
	return Status;
}
