/**
 * ext4fs_global.h - object identifiers and the driver-wide global state.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _EXT4_EXT4FS_GLOBAL_H_
#define _EXT4_EXT4FS_GLOBAL_H_

/* Ext2Fsd Driver Definitions */

/* EXT2_IDENTIFIER_TYPE

   Identifiers used to mark the structures */

typedef enum _EXT2_IDENTIFIER_TYPE {
#ifdef _MSC_VER
    EXT2FGD  = ':DGF',
    EXT2VCB  = ':BCV',
    EXT2FCB  = ':BCF',
    EXT2CCB  = ':BCC',
    EXT2ICX  = ':XCI',
    EXT2FSD  = ':DSF',
    EXT2MCB  = ':BCM',
    EXT2ICB  = ':BCI',
    EXT4CRY  = ':YRC',      /* disk device of an unlocked LUKS volume */
    EXT4LVD  = ':DVL'       /* disk device of an LVM volume inside one */
#else
    EXT2FGD  = 0xE2FD0001,
    EXT2VCB  = 0xE2FD0002,
    EXT2FCB  = 0xE2FD0003,
    EXT2CCB  = 0xE2FD0004,
    EXT2ICX  = 0xE2FD0005,
    EXT2FSD  = 0xE2FD0006,
    EXT2MCB  = 0xE2FD0007,
    EXT2ICB  = 0xE2FD0008,
    EXT4CRY  = 0xE2FD0009,
    EXT4LVD  = 0xE2FD000A
#endif
} EXT2_IDENTIFIER_TYPE;

/* EXT2_IDENTIFIER

   Header used to mark the structures */
typedef struct _EXT2_IDENTIFIER {
    EXT2_IDENTIFIER_TYPE     Type;
    ULONG                    Size;
} EXT2_IDENTIFIER, *PEXT2_IDENTIFIER;


#define NodeType(Ptr) (*((EXT2_IDENTIFIER_TYPE *)(Ptr)))

typedef struct _EXT2_MCB  EXT2_MCB, *PEXT2_MCB;
typedef struct _EXT2_ICB  EXT2_ICB, *PEXT2_ICB;

/* one lock of a striped set (core\stripes.c), alone on its cache lines:
   stripes are taken by different processors at once, and two sharing a
   line would bounce it between them as one lock does */
typedef struct DECLSPEC_CACHEALIGN _EXT2_LOCK_STRIPE {
    ERESOURCE   Lock;
} EXT2_LOCK_STRIPE, *PEXT2_LOCK_STRIPE;

/* the locks of the block groups that fall on one stripe (core\stripes.c):
   the block bitmap and the inode bitmap are changed apart - a delete frees
   both at once - and the descriptor they share is checksummed as one step */
typedef struct DECLSPEC_CACHEALIGN _EXT2_GROUP_STRIPE {
    ERESOURCE   BlockLock;
    ERESOURCE   InodeLock;
    EX_PUSH_LOCK DescLock;      /* not a spin lock: the descriptor is in a pageable cache view */
} EXT2_GROUP_STRIPE, *PEXT2_GROUP_STRIPE;

/* buckets of the per-volume inode node hash (EXT2_VCB.IcbTable): as many
   as the name hash has, since every cached name holds an Icb. With 256 the
   chains grew to ~100 nodes at 25 000 cached names, walked under the IcbLock
   spin lock by every new name - opens/s fell by a third. Inode numbers are
   allocated densely, so the low bits spread them evenly (power of two: the
   modulo is a mask). */
#define EXT2_ICB_BUCKETS    4096


typedef PVOID   PBCB;


/* EXT2_GLOBAL_DATA

   Data that is not specific to a mounted volume */

typedef VOID (*EXT2_REAPER_RELEASE)(PVOID);

typedef struct _EXT2_REAPER {
        PETHREAD                Thread;
        KEVENT                  Engine;
        KEVENT                  Wait;
        EXT2_REAPER_RELEASE     Free;
        ULONG                   Flags;
        volatile LONG           Armed;      /* sleeping without a deadline */
} EXT2_REAPER, *PEXT2_REAPER;

#define EXT2_REAPER_FLAG_STOP   (1 << 0)

typedef struct _EXT2_GLOBAL {

    /* Identifier for this structure */
    EXT2_IDENTIFIER             Identifier;

    /* Syncronization primitive for this structure */
    ERESOURCE                   Resource;

    /* Global flags for the driver: I put it since
       FastIoDispatch isn't 8bytes aligned.  */
    ULONG                       Flags;

    /* Table of pointers to the fast I/O entry points */
    FAST_IO_DISPATCH            FastIoDispatch;

    /* Filter callbacks */
    FS_FILTER_CALLBACKS         FilterCallbacks;

    /* Table of pointers to the Cache Manager callbacks */
    CACHE_MANAGER_CALLBACKS     CacheManagerCallbacks;
    CACHE_MANAGER_CALLBACKS     CacheManagerNoOpCallbacks;

    /* Pointer to the disk device object */
    PDEVICE_OBJECT              DiskdevObject;

    /* Pointer to the cdrom device object */
    PDEVICE_OBJECT              CdromdevObject;

    /* List of mounted volumes */
    LIST_ENTRY                  VcbList;

    /* Volumes dismounted but not yet destroyed: their cached Fcbs still
       hold references, and the Fcb reaper drains them (memory.c) */
    LIST_ENTRY                  DismountingVcbList;
    LIST_ENTRY                  SwapVpbList;    /* VPBs of ours still on devices (dismount.c) */
    KSPIN_LOCK                  SwapVpbLock;

    /* Cleaning thread related: resource cleaner */
    EXT2_REAPER                 FcbReaper;
    EXT2_REAPER                 McbReaper;
    EXT2_REAPER                 bhReaper;

    /* names the last McbReaper sweep could not bring under the low water
       mark: allocations wake it again only above this (0: no floor) */
    LONG                        McbReapFloor;

    /* \KernelObjects\LowMemoryCondition: the reapers give back everything
       idle while it is signaled */
    PKEVENT                     LowMemory;
    HANDLE                      LowMemoryHandle;

    /* bare "sc stop" support (devctl.c, "Bare sc stop support") */
#define EXT2_UNLOAD_IDLE        0   /* nothing seen yet */
#define EXT2_UNLOAD_PROBING     1   /* probe work item queued or running */
#define EXT2_UNLOAD_DRAINING    2   /* sc stop seen, drain thread owns the rest */
    HANDLE                      SelfHandle;     /* our own open of \ext4 */
    PEX_TIMER                   UnloadTimer;    /* probe timer */
    PIO_WORKITEM                UnloadWorkItem; /* the probe, at PASSIVE_LEVEL */
    EX_RUNDOWN_REF              UnloadRundown;  /* probes queued or running */
    volatile LONG               UnloadState;    /* EXT2_UNLOAD_IDLE/PROBING/DRAINING */
    BOOLEAN                     UnloadFlagUsable; /* DEVOBJ_EXTENSION layout verified */
    PETHREAD                    UnloadThread;   /* the drain thread, once seen */
    KEVENT                      VolumeReleased; /* a volume may have become idle */
    KEVENT                      UnloadStarted;  /* set for good once sc stop is seen */
    volatile LONG               MountsInFlight; /* Ext2MountVolume calls not yet done */
    volatile LONG               VcbTeardownsInFlight; /* unlinked VCBs still being destroyed */

    /* drive letters for ext volumes (letter.c) */
    PVOID                       VolumeNotifyEntry;    /* PnP volume-interface watch */
    PVOID                       PartitionNotifyEntry; /* same for hidden partitions */
    EX_RUNDOWN_REF              LetterRundown;     /* probes and assignments in flight */
    volatile LONG               LetterStopping;    /* the rundown wait has been started */
    KEVENT                      LetterStopped;     /* ... and is over */

    /* Look Aside table of IRP_CONTEXT, FCB, MCB, CCB */
    NPAGED_LOOKASIDE_LIST       Ext2IrpContextLookasideList;
    NPAGED_LOOKASIDE_LIST       Ext2FcbLookasideList;
    NPAGED_LOOKASIDE_LIST       Ext2CcbLookasideList;
    NPAGED_LOOKASIDE_LIST       Ext2McbLookasideList;
    NPAGED_LOOKASIDE_LIST       Ext2ExtLookasideList;
    NPAGED_LOOKASIDE_LIST       Ext2DentryLookasideList;
    USHORT                      MaxDepth;

    /* User specified global codepage name */
    struct {
        WCHAR                   PageName[CODEPAGE_MAXLEN];
        CHAR                    AnsiName[CODEPAGE_MAXLEN];
        struct nls_table *      PageTable;
    } Codepage;

    /* global hiding patterns */
    WCHAR                       wHidingPrefix[HIDINGPAT_LEN];
    WCHAR                       wHidingSuffix[HIDINGPAT_LEN];
    BOOLEAN                     bHidingPrefix;
    CHAR                        sHidingPrefix[HIDINGPAT_LEN];
    BOOLEAN                     bHidingSuffix;
    CHAR                        sHidingSuffix[HIDINGPAT_LEN];

    /* Registery path */
    UNICODE_STRING              RegistryPath;

    /* global memory and i/o statistics and memory allocations
       of various sturctures */

    EXT2_PERF_STATISTICS_V2     PerfStat;

} EXT2_GLOBAL, *PEXT2_GLOBAL;

/* Flags for EXT2_GLOBAL_DATA */

#define EXT2_UNLOAD_PENDING     0x00000001
#define EXT2_SUPPORT_WRITING    0x00000002
#define EXT3_FORCE_WRITING      0x00000004
#define EXT2_CHECKING_BITMAP    0x00000008
#define EXT2_AUTO_MOUNT         0x00000010
#define EXT2_DEVICES_DELETED    0x00000020  /* control devices already gone (devctl.c) */

/* Glboal Ext2Fsd Memory Block */

extern PEXT2_GLOBAL Ext2Global;

/* memory allocation statistics */


#define INC_MEM_COUNT(_i, _p, _s) do { ASSERT(_p); Ext2TraceMemory(TRUE, (int) (_i), (PVOID)(_p), (LONG)(_s)); } while(0)
#define DEC_MEM_COUNT(_i, _p, _s) do { ASSERT(_p); Ext2TraceMemory(FALSE, (int) (_i), (PVOID)(_p), (LONG)(_s)); } while(0)
#define INC_IRP_COUNT(IrpContext) Ext2TraceIrpContext(TRUE, (IrpContext))
#define DEC_IRP_COUNT(IrpContext) Ext2TraceIrpContext(FALSE, (IrpContext))

/* Driver Extension define */

#define IsExt2FsDevice(DO) ((DO == Ext2Global->DiskdevObject) || \
                            (DO == Ext2Global->CdromdevObject) )


typedef struct _EXT2_FCBVCB {

    /* Command header for Vcb and Fcb */
    FSRTL_ADVANCED_FCB_HEADER   Header;

    FAST_MUTEX                  Mutex;

    /* Ext2Fsd identifier */
    EXT2_IDENTIFIER             Identifier;


    /* Locking resources */
    ERESOURCE                   MainResource;
    ERESOURCE                   PagingIoResource;

} EXT2_FCBVCB, *PEXT2_FCBVCB;

#endif /* _EXT4_EXT4FS_GLOBAL_H_ */
