/**
 * cc_callbacks.c - cache manager and memory manager acquire/release callbacks.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"

#define FASTIO_DEBUG_LEVEL DL_NVR

#define CMCB_DEBUG_LEVEL DL_NVR

VOID
Ext2AcquireForCreateSection (
    IN PFILE_OBJECT FileObject
)

{
    PEXT2_FCB Fcb = FileObject->FsContext;

    if (Fcb->Header.Resource != NULL) {
        ExAcquireResourceExclusiveLite(Fcb->Header.Resource, TRUE);
    }

    DEBUG(FASTIO_DEBUG_LEVEL, ("Ext2AcquireForCreateSection:  Fcb=%p\n", Fcb));
}

VOID
Ext2ReleaseForCreateSection (
    IN PFILE_OBJECT FileObject
)
{
    PEXT2_FCB Fcb = FileObject->FsContext;

    DEBUG(FASTIO_DEBUG_LEVEL, ("Ext2ReleaseForCreateSection:  Fcb=%p\n", Fcb));

    if (Fcb->Header.Resource != NULL) {
        ExReleaseResourceLite(Fcb->Header.Resource);
    }
}

NTSTATUS
Ext2AcquireFileForModWrite (
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER EndingOffset,
    OUT PERESOURCE *ResourceToRelease,
    IN PDEVICE_OBJECT DeviceObject
)

{
    UNREFERENCED_PARAMETER(EndingOffset);
    UNREFERENCED_PARAMETER(DeviceObject);
    BOOLEAN ResourceAcquired = FALSE;

    PEXT2_FCB Fcb = FileObject->FsContext;

    *ResourceToRelease = Fcb->Header.Resource;
    ResourceAcquired = ExAcquireResourceExclusiveLite(*ResourceToRelease, FALSE);
    if (!ResourceAcquired) {
        *ResourceToRelease = NULL;
    }

    DEBUG(FASTIO_DEBUG_LEVEL, ("Ext2AcquireFileForModWrite:  Fcb=%p Acquired=%d\n",
                             Fcb, ResourceAcquired));

    return (ResourceAcquired ? STATUS_SUCCESS : STATUS_CANT_WAIT);
}

NTSTATUS
Ext2ReleaseFileForModWrite (
    IN PFILE_OBJECT FileObject,
    IN PERESOURCE ResourceToRelease,
    IN PDEVICE_OBJECT DeviceObject
)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    PEXT2_FCB Fcb = FileObject->FsContext;

    DEBUG(FASTIO_DEBUG_LEVEL, ("Ext2ReleaseFileForModWrite: Fcb=%p\n", Fcb));

    if (ResourceToRelease != NULL) {
        ASSERT(ResourceToRelease == Fcb->Header.Resource);
        ExReleaseResourceLite(ResourceToRelease);
    } else {
        DbgBreak();
    }

    return STATUS_SUCCESS;
}

NTSTATUS
Ext2AcquireFileForCcFlush (
    IN PFILE_OBJECT FileObject,
    IN PDEVICE_OBJECT DeviceObject
)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    PEXT2_FCB Fcb = FileObject->FsContext;

    if (Fcb->Header.Resource != NULL) {
        ExAcquireResourceExclusiveLite(Fcb->Header.Resource, TRUE);
    }

    DEBUG(FASTIO_DEBUG_LEVEL, ("Ext2AcquireFileForCcFlush: Fcb=%p\n", Fcb));

    return STATUS_SUCCESS;
}

NTSTATUS
Ext2ReleaseFileForCcFlush (
    IN PFILE_OBJECT FileObject,
    IN PDEVICE_OBJECT DeviceObject
)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    PEXT2_FCB Fcb = FileObject->FsContext;

    DEBUG(FASTIO_DEBUG_LEVEL, ("Ext2ReleaseFileForCcFlush: Fcb=%p\n", Fcb));

    if (Fcb->Header.Resource != NULL) {
        ExReleaseResourceLite(Fcb->Header.Resource);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
Ext2PreAcquireForCreateSection(
    IN PFS_FILTER_CALLBACK_DATA cd,
    OUT PVOID *cc
    )
{
    UNREFERENCED_PARAMETER(cc);
    PEXT2_FCB Fcb = (PEXT2_FCB)cd->FileObject->FsContext;
    NTSTATUS        status;

    ASSERT(cd->Operation == FS_FILTER_ACQUIRE_FOR_SECTION_SYNCHRONIZATION);
    ExAcquireResourceExclusiveLite(Fcb->Header.Resource, TRUE);
    if (cd->Parameters.AcquireForSectionSynchronization.SyncType != SyncTypeCreateSection) {
        status = STATUS_FSFILTER_OP_COMPLETED_SUCCESSFULLY;
    } else if (Fcb->ShareAccess.Writers == 0) {
        status = STATUS_FILE_LOCKED_WITH_ONLY_READERS;
    } else {
        status = STATUS_FILE_LOCKED_WITH_WRITERS;
    }

    return status;
}

BOOLEAN
Ext2AcquireForLazyWrite (
    IN PVOID    Context,
    IN BOOLEAN  Wait)
{
    PEXT2_FCB    Fcb;

    Fcb = (PEXT2_FCB) Context;
    ASSERT(Fcb != NULL);
    ASSERT((Fcb->Identifier.Type == EXT2FCB) &&
           (Fcb->Identifier.Size == sizeof(EXT2_FCB)));
#if EXT2_DEBUG
    DEBUG(CMCB_DEBUG_LEVEL, ("Ext2AcquireForLazyWrite: %s %s Fcb=%p\n",
                             Ext2GetCurrentProcessName(), "ACQUIRE_FOR_LAZY_WRITE", Fcb));
#endif
    if (!ExAcquireResourceExclusiveLite(Fcb->Header.Resource, Wait)) {
        return FALSE;
    }

    ASSERT(Fcb->LazyWriterThread == NULL);
    Fcb->LazyWriterThread = PsGetCurrentThread();

    ASSERT(IoGetTopLevelIrp() == NULL);
    IoSetTopLevelIrp((PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);

    return TRUE;
}

VOID
Ext2ReleaseFromLazyWrite (IN PVOID Context)
{
    PEXT2_FCB Fcb = (PEXT2_FCB) Context;

    ASSERT(Fcb != NULL);

    ASSERT((Fcb->Identifier.Type == EXT2FCB) &&
           (Fcb->Identifier.Size == sizeof(EXT2_FCB)));
#if EXT2_DEBUG
    DEBUG(CMCB_DEBUG_LEVEL, ( "Ext2ReleaseFromLazyWrite: %s %s Fcb=%p\n",
                              Ext2GetCurrentProcessName(), "RELEASE_FROM_LAZY_WRITE", Fcb));
#endif
    ASSERT(Fcb->LazyWriterThread == PsGetCurrentThread());
    Fcb->LazyWriterThread = NULL;

    ExReleaseResourceLite(Fcb->Header.Resource);

    ASSERT(IoGetTopLevelIrp() == (PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);
    IoSetTopLevelIrp( NULL );
}

BOOLEAN
Ext2AcquireForReadAhead (IN PVOID    Context,
                         IN BOOLEAN  Wait)
{
    PEXT2_FCB    Fcb = (PEXT2_FCB) Context;

    ASSERT(Fcb != NULL);
    ASSERT((Fcb->Identifier.Type == EXT2FCB) &&
           (Fcb->Identifier.Size == sizeof(EXT2_FCB)));

    DEBUG(CMCB_DEBUG_LEVEL, ("Ext2AcquireForReadAhead: i=%xh Fcb=%p\n",
                             Fcb->Mcb->Inode->i_ino, Fcb));

    if (!ExAcquireResourceSharedLite(Fcb->Header.Resource, Wait))
        return FALSE;
    ASSERT(IoGetTopLevelIrp() == NULL);
    IoSetTopLevelIrp((PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);

    return TRUE;
}

VOID
Ext2ReleaseFromReadAhead (IN PVOID Context)
{
    PEXT2_FCB Fcb = (PEXT2_FCB) Context;

    ASSERT(Fcb != NULL);

    ASSERT((Fcb->Identifier.Type == EXT2FCB) &&
           (Fcb->Identifier.Size == sizeof(EXT2_FCB)));

    DEBUG(CMCB_DEBUG_LEVEL, ("Ext2ReleaseFromReadAhead: i=%xh Fcb=%p\n",
                             Fcb->Mcb->Inode->i_ino, Fcb));

    IoSetTopLevelIrp(NULL);
    ExReleaseResourceLite(Fcb->Header.Resource);
}

BOOLEAN
Ext2NoOpAcquire (
    IN PVOID Fcb,
    IN BOOLEAN Wait
)
{
    UNREFERENCED_PARAMETER(Fcb);
    UNREFERENCED_PARAMETER(Wait);
    ASSERT(IoGetTopLevelIrp() == NULL);
    IoSetTopLevelIrp((PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);
    return TRUE;
}

VOID
Ext2NoOpRelease (
    IN PVOID Fcb
)
{
    UNREFERENCED_PARAMETER(Fcb);
    ASSERT(IoGetTopLevelIrp() == (PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);
    IoSetTopLevelIrp( NULL );

    return;
}
