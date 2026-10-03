/**
 * ext4fs_fsd.h - IRP handlers: create, read, write, information, directories, FSCTL, IOCTL, PnP, fast I/O.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _EXT4_EXT4FS_FSD_H_
#define _EXT4_EXT4FS_FSD_H_

int Ext2CheckInodeAccess(PEXT2_VCB Vcb, struct inode *in, int attempt);

/* chattr +i and +a, honoured as Linux honours them: an immutable inode is
   not written, truncated, renamed, deleted or linked, and an immutable
   directory takes and loses no entries; an append-only file grows only at
   its end and is not deleted, an append-only directory only takes entries */
#define Ext4IsImmutable(in)     (((in)->i_flags & EXT4_IMMUTABLE_FL) != 0)
#define Ext4IsAppendOnly(in)    (((in)->i_flags & EXT4_APPEND_FL) != 0)
#define Ext4IsSealed(in)        (((in)->i_flags & (EXT4_IMMUTABLE_FL | EXT4_APPEND_FL)) != 0)

int Ext2CheckFileAccess (PEXT2_VCB Vcb, PEXT2_MCB Mcb, int attempt);

NTSTATUS
Ext2Cleanup (IN PEXT2_IRP_CONTEXT IrpContext);

NTSTATUS
Ext2Close (IN PEXT2_IRP_CONTEXT IrpContext);

BOOLEAN
Ext2AcquireForLazyWrite (
    IN PVOID    Context,
    IN BOOLEAN  Wait );

VOID
Ext2ReleaseFromLazyWrite (IN PVOID Context);

BOOLEAN
Ext2AcquireForReadAhead (
    IN PVOID    Context,
    IN BOOLEAN  Wait );

VOID
Ext2ReleaseFromReadAhead (IN PVOID Context);

BOOLEAN
Ext2NoOpAcquire (
    IN PVOID Fcb,
    IN BOOLEAN Wait );

VOID
Ext2NoOpRelease (IN PVOID Fcb);

BOOLEAN
Ext2IsNameValid(PUNICODE_STRING FileName);

NTSTATUS
Ext2FollowLink (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_MCB            Parent,
    IN PEXT2_MCB            Mcb,
    IN ULONG                Linkdep
);

NTSTATUS
Ext2ScanDir (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_MCB            Parent,
    IN PUNICODE_STRING      FileName,
    OUT PULONG              Inode,
    struct dentry         **dentry
);

VOID
Ext2ResolveLink (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_MCB            Mcb,
    IN ULONG                Linkdep,
    IN BOOLEAN              NotFollow
);

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
);

BOOLEAN
Ext2IsSpecialSystemFile(
    IN PUNICODE_STRING FileName,
    IN BOOLEAN         bDirectory
);

#define EXT2_LOOKUP_FLAG_MASK   (0xFF00000)

#define EXT2_LOOKUP_NOT_FOLLOW  (0x8000000)

NTSTATUS
Ext2LookupFile (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PUNICODE_STRING      FullName,
    IN PEXT2_MCB            Parent,
    OUT PEXT2_MCB *         Ext2Mcb,
    IN ULONG                Linkdep
);

NTSTATUS
Ext2CreateFile(
    IN PEXT2_IRP_CONTEXT IrpContext,
    IN PEXT2_VCB Vcb,
    OUT PBOOLEAN OpPostIrp
);

NTSTATUS
Ext2CreateVolume(
    IN PEXT2_IRP_CONTEXT IrpContext,
    IN PEXT2_VCB Vcb );

NTSTATUS
Ext2Create (IN PEXT2_IRP_CONTEXT IrpContext);

NTSTATUS
Ext2CreateInode(
    IN PEXT2_IRP_CONTEXT   IrpContext,
    IN PEXT2_VCB           Vcb,
    IN PEXT2_FCB           pParentFcb,
    IN ULONG               Type,
    IN ULONG               FileAttr,
    IN PUNICODE_STRING     FileName,
    OUT PULONG             NewIno OPTIONAL,
    OUT struct dentry    **NewEntry OPTIONAL);

NTSTATUS
Ext2SupersedeOrOverWriteFile(
    IN PEXT2_IRP_CONTEXT IrpContext,
    IN PFILE_OBJECT      FileObject,
    IN PEXT2_VCB         Vcb,
    IN PEXT2_FCB         Fcb,
    IN PLARGE_INTEGER    AllocationSize,
    IN ULONG             Disposition
);

/* debug levels */
#define DL_NVR 0

NTSTATUS
Ext2ProcessGlobalProperty(
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PEXT2_VOLUME_PROPERTY3 Property,
    IN  ULONG Length
);

NTSTATUS
Ext2ProcessVolumeProperty(
    IN  PEXT2_VCB              Vcb,
    IN  PEXT2_VOLUME_PROPERTY3 Property,
    IN  ULONG Length
);

NTSTATUS
Ext2ProcessUserProperty(
    IN PEXT2_IRP_CONTEXT        IrpContext,
    IN PEXT2_VOLUME_PROPERTY3   Property,
    IN ULONG                    Length
);

NTSTATUS
Ext2ProcessUserPerfStat(
    IN PEXT2_IRP_CONTEXT        IrpContext,
    IN PEXT2_QUERY_PERFSTAT     QueryPerf,
    IN ULONG                    Length
);

NTSTATUS
Ext2ProcessMountPoint(
    IN PEXT2_IRP_CONTEXT        IrpContext,
    IN PEXT2_MOUNT_POINT        MountPoint,
    IN ULONG                    Length
);

NTSTATUS
Ext2DeviceControlNormal (IN PEXT2_IRP_CONTEXT IrpContext);

NTSTATUS
Ext2DeviceControl (IN PEXT2_IRP_CONTEXT IrpContext);

ULONG
Ext2GetInfoLength(IN FILE_INFORMATION_CLASS  FileInformationClass);

NTSTATUS
Ext2ProcessEntry(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_FCB            Dcb,
    IN FILE_INFORMATION_CLASS  FileInformationClass,
    IN ULONG                in,
    IN PVOID                Buffer,
    IN ULONG                UsedLength,
    IN ULONG                Length,
    IN ULONG                FileIndex,
    IN PUNICODE_STRING      pName,
    OUT PULONG              EntrySize,
    IN BOOLEAN              Single
);

BOOLEAN
Ext2IsWearingCloak(
    IN  PEXT2_VCB       Vcb,
    IN  POEM_STRING     OeName
);

NTSTATUS Ext2QueryDirectory (IN PEXT2_IRP_CONTEXT IrpContext);

NTSTATUS
Ext2NotifyChangeDirectory (
    IN PEXT2_IRP_CONTEXT IrpContext
);

VOID
Ext2NotifyReportChange (
    IN PEXT2_IRP_CONTEXT IrpContext,
    IN PEXT2_VCB         Vcb,
    IN PEXT2_MCB         Mcb,
    IN ULONG             Filter,
    IN ULONG             Action
);

NTSTATUS
Ext2DirectoryControl (IN PEXT2_IRP_CONTEXT IrpContext);

BOOLEAN
Ext2IsDirectoryEmpty (
    IN PEXT2_IRP_CONTEXT IrpContext,
    IN PEXT2_VCB Vcb,
    IN PEXT2_MCB Mcb
);

NTSTATUS
Ext2QueryEa(
	IN PEXT2_IRP_CONTEXT    IrpContext
);

BOOLEAN
Ext2IsEaNameValid(
	IN OEM_STRING Name
);

NTSTATUS
Ext2SetEa(
	IN PEXT2_IRP_CONTEXT    IrpContext
);

NTSTATUS
Ext2BlockMap(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_MCB            Mcb,
    IN ULONG                Index,
    IN BOOLEAN              bAlloc,
    OUT PULONGLONG          pBlock,
    OUT PULONG              Number
);

FAST_IO_POSSIBLE
Ext2IsFastIoPossible(
    IN PEXT2_FCB Fcb
);

BOOLEAN
Ext2FastIoCheckIfPossible (
    IN PFILE_OBJECT         FileObject,
    IN PLARGE_INTEGER       FileOffset,
    IN ULONG                Length,
    IN BOOLEAN              Wait,
    IN ULONG                LockKey,
    IN BOOLEAN              CheckForReadOperation,
    OUT PIO_STATUS_BLOCK    IoStatus,
    IN PDEVICE_OBJECT       DeviceObject
);

BOOLEAN
Ext2FastIoRead (IN PFILE_OBJECT FileObject,
                IN PLARGE_INTEGER       FileOffset,
                IN ULONG                Length,
                IN BOOLEAN              Wait,
                IN ULONG                LockKey,
                OUT PVOID               Buffer,
                OUT PIO_STATUS_BLOCK    IoStatus,
                IN PDEVICE_OBJECT       DeviceObject);

BOOLEAN
Ext2FastIoWrite (
    IN PFILE_OBJECT         FileObject,
    IN PLARGE_INTEGER       FileOffset,
    IN ULONG                Length,
    IN BOOLEAN              Wait,
    IN ULONG                LockKey,
    OUT PVOID               Buffer,
    OUT PIO_STATUS_BLOCK    IoStatus,
    IN PDEVICE_OBJECT       DeviceObject);

BOOLEAN
Ext2FastIoQueryBasicInfo (
    IN PFILE_OBJECT             FileObject,
    IN BOOLEAN                  Wait,
    OUT PFILE_BASIC_INFORMATION Buffer,
    OUT PIO_STATUS_BLOCK        IoStatus,
    IN PDEVICE_OBJECT           DeviceObject);

BOOLEAN
Ext2FastIoQueryStandardInfo (
    IN PFILE_OBJECT                 FileObject,
    IN BOOLEAN                      Wait,
    OUT PFILE_STANDARD_INFORMATION  Buffer,
    OUT PIO_STATUS_BLOCK            IoStatus,
    IN PDEVICE_OBJECT               DeviceObject);

BOOLEAN
Ext2FastIoLock (
    IN PFILE_OBJECT         FileObject,
    IN PLARGE_INTEGER       FileOffset,
    IN PLARGE_INTEGER       Length,
    IN PEPROCESS            Process,
    IN ULONG                Key,
    IN BOOLEAN              FailImmediately,
    IN BOOLEAN              ExclusiveLock,
    OUT PIO_STATUS_BLOCK    IoStatus,
    IN PDEVICE_OBJECT       DeviceObject
);

BOOLEAN
Ext2FastIoUnlockSingle (
    IN PFILE_OBJECT         FileObject,
    IN PLARGE_INTEGER       FileOffset,
    IN PLARGE_INTEGER       Length,
    IN PEPROCESS            Process,
    IN ULONG                Key,
    OUT PIO_STATUS_BLOCK    IoStatus,
    IN PDEVICE_OBJECT       DeviceObject
);

BOOLEAN
Ext2FastIoUnlockAll (
    IN PFILE_OBJECT         FileObject,
    IN PEPROCESS            Process,
    OUT PIO_STATUS_BLOCK    IoStatus,
    IN PDEVICE_OBJECT       DeviceObject
);

BOOLEAN
Ext2FastIoUnlockAllByKey (
    IN PFILE_OBJECT         FileObject,
    IN PEPROCESS            Process,
    IN ULONG                Key,
    OUT PIO_STATUS_BLOCK    IoStatus,
    IN PDEVICE_OBJECT       DeviceObject
);

BOOLEAN
Ext2FastIoQueryNetworkOpenInfo (
    IN PFILE_OBJECT                     FileObject,
    IN BOOLEAN                          Wait,
    OUT PFILE_NETWORK_OPEN_INFORMATION  Buffer,
    OUT PIO_STATUS_BLOCK                IoStatus,
    IN PDEVICE_OBJECT                   DeviceObject );

BOOLEAN
Ext2FastIoQueryNetworkOpenInfo (
    IN PFILE_OBJECT                     FileObject,
    IN BOOLEAN                          Wait,
    OUT PFILE_NETWORK_OPEN_INFORMATION  Buffer,
    OUT PIO_STATUS_BLOCK                IoStatus,
    IN PDEVICE_OBJECT                   DeviceObject);

VOID
Ext2AcquireForCreateSection (
    IN PFILE_OBJECT FileObject
);

VOID
Ext2ReleaseForCreateSection (
    IN PFILE_OBJECT FileObject
);

NTSTATUS
Ext2AcquireFileForModWrite (
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER EndingOffset,
    OUT PERESOURCE *ResourceToRelease,
    IN PDEVICE_OBJECT DeviceObject
);

NTSTATUS
Ext2ReleaseFileForModWrite (
    IN PFILE_OBJECT FileObject,
    IN PERESOURCE ResourceToRelease,
    IN PDEVICE_OBJECT DeviceObject
);

NTSTATUS
Ext2AcquireFileForCcFlush (
    IN PFILE_OBJECT FileObject,
    IN PDEVICE_OBJECT DeviceObject
);

NTSTATUS
Ext2ReleaseFileForCcFlush (
    IN PFILE_OBJECT FileObject,
    IN PDEVICE_OBJECT DeviceObject
);

NTSTATUS
Ext2PreAcquireForCreateSection(
    IN PFS_FILTER_CALLBACK_DATA cd,
    OUT PVOID *cc
    );

NTSTATUS
Ext2QueryFileInformation (IN PEXT2_IRP_CONTEXT IrpContext);

NTSTATUS
Ext2SetFileInformation (IN PEXT2_IRP_CONTEXT IrpContext);

ULONG
Ext2TotalBlocks(
    PEXT2_VCB         Vcb,
    PLARGE_INTEGER    Size,
    PULONG            pMeta
);

NTSTATUS
Ext2ExpandFile(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB         Vcb,
    PEXT2_MCB         Mcb,
    PLARGE_INTEGER    Size
);

NTSTATUS
Ext2TruncateFile (
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB Vcb,
    PEXT2_MCB Mcb,
    PLARGE_INTEGER AllocationSize );

NTSTATUS
Ext2IsFileRemovable(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_FCB            Fcb,
    IN PEXT2_CCB            Ccb
);

NTSTATUS
Ext2SetDispositionInfo(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB Vcb,
    PEXT2_FCB Fcb,
    PEXT2_CCB Ccb,
    BOOLEAN bDelete
);

NTSTATUS
Ext2SetRenameInfo(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB Vcb,
    PEXT2_FCB Fcb,
    PEXT2_CCB Ccb
);

NTSTATUS
Ext2SetLinkInfo(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB Vcb,
    PEXT2_FCB Fcb,
    PEXT2_CCB Ccb
);

ULONG
Ext2InodeType(PEXT2_MCB Mcb);

NTSTATUS
Ext2DeleteFile(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB Vcb,
    PEXT2_FCB Fcb,
    PEXT2_MCB Mcb
);

NTSTATUS
Ext2FlushFiles(
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN BOOLEAN              bShutDown
);

NTSTATUS
Ext2FlushVolume (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN BOOLEAN              bShutDown
);

NTSTATUS
Ext2FlushFile (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_FCB            Fcb,
    IN PEXT2_CCB            Ccb
);

NTSTATUS
Ext2Flush (IN PEXT2_IRP_CONTEXT IrpContext);

NTSTATUS
Ext2ReadSymlink (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_MCB            Mcb,
    IN PVOID                Buffer,
    IN ULONG                Size,
    OUT PULONG              BytesRead
    );

NTSTATUS
Ext2WriteSymlink (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_MCB            Mcb,
    IN PVOID                Buffer,
    IN ULONG                Size,
    OUT PULONG              BytesWritten
);

NTSTATUS
Ext2TruncateSymlink(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB         Vcb,
    PEXT2_MCB         Mcb,
    ULONG             Size
);

NTSTATUS
Ext2AllowExtendedDasdIo(IN PEXT2_IRP_CONTEXT IrpContext);

NTSTATUS
Ext2OplockRequest (IN PEXT2_IRP_CONTEXT IrpContext);

NTSTATUS
Ext2QueryExtentMappings(
    IN PEXT2_IRP_CONTEXT   IrpContext,
    IN PEXT2_VCB           Vcb,
    IN PEXT2_FCB           Fcb,
    IN PLARGE_INTEGER      RequestVbn,
    OUT PLARGE_INTEGER *   pMappedRuns
);

NTSTATUS
Ext2QueryRetrievalPointers(IN PEXT2_IRP_CONTEXT IrpContext);

NTSTATUS
Ext2GetRetrievalPointers(IN PEXT2_IRP_CONTEXT IrpContext);

NTSTATUS
Ext2GetRetrievalPointerBase(IN PEXT2_IRP_CONTEXT IrpContext);

NTSTATUS
Ext2UserFsRequest (IN PEXT2_IRP_CONTEXT IrpContext);

NTSTATUS
Ext2FileSystemControl (IN PEXT2_IRP_CONTEXT IrpContext);

NTSTATUS
Ext2LockControl (IN PEXT2_IRP_CONTEXT IrpContext);

NTSTATUS
Ext2QueryHardLinks (
    IN PEXT2_VCB        Vcb,
    IN PEXT2_MCB        Mcb,
    OUT PVOID           Buffer,
    IN ULONG            Length,
    IN BOOLEAN          FullId,
    OUT PULONG_PTR      Information
);

NTSTATUS
Ext2CompleteIrpContext (
    IN PEXT2_IRP_CONTEXT IrpContext,
    IN NTSTATUS Status );

NTSTATUS
Ext2Pnp(IN PEXT2_IRP_CONTEXT IrpContext);

NTSTATUS
Ext2PnpQueryRemove(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB         Vcb      );

NTSTATUS
Ext2PnpRemove(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB         Vcb      );

NTSTATUS
Ext2PnpCancelRemove(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB Vcb              );

NTSTATUS
Ext2PnpSurpriseRemove(
    PEXT2_IRP_CONTEXT IrpContext,
    PEXT2_VCB Vcb              );

NTSTATUS
Ext2ReadInode (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_MCB            Mcb,
    IN ULONGLONG            Offset,
    IN PVOID                Buffer,
    IN ULONG                Size,
    IN BOOLEAN              bDirectIo,
    OUT PULONG              dwReturn
);

NTSTATUS
Ext2Read (IN PEXT2_IRP_CONTEXT IrpContext);

NTSTATUS
Ext2ShutDown (IN PEXT2_IRP_CONTEXT IrpContext);

NTSTATUS
Ext2QueryVolumeInformation (IN PEXT2_IRP_CONTEXT IrpContext);

NTSTATUS
Ext2SetVolumeInformation (IN PEXT2_IRP_CONTEXT IrpContext);

typedef struct _EXT2_RW_CONTEXT {
    PIRP                MasterIrp;
    KEVENT              Event;
    LONG                Blocks;
    ULONG               Length;
    PERESOURCE          Resource;
    ERESOURCE_THREAD    ThreadId;
    PFILE_OBJECT        FileObject;
    ULONG               Flags;
    BOOLEAN             Wait;

} EXT2_RW_CONTEXT, *PEXT2_RW_CONTEXT;

#define EXT2_RW_CONTEXT_WRITE   1

NTSTATUS
Ext2WriteInode (
    IN PEXT2_IRP_CONTEXT    IrpContext,
    IN PEXT2_VCB            Vcb,
    IN PEXT2_MCB            Mcb,
    IN ULONGLONG            Offset,
    IN PVOID                Buffer,
    IN ULONG                Size,
    IN BOOLEAN              bDirectIo,
    OUT PULONG              dwReturn
);

BOOLEAN
Ext2ZeroData (
    IN PEXT2_IRP_CONTEXT IrpContext,
    IN PEXT2_VCB Vcb,
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER Start,
    IN PLARGE_INTEGER End );

NTSTATUS
Ext2Write (IN PEXT2_IRP_CONTEXT IrpContext);

#endif /* _EXT4_EXT4FS_FSD_H_ */
