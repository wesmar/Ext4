/* SPDX-License-Identifier: GPL-2.0-only
 * Fault-injection harness for the production block builder and symlink writer.
 * write-path-model.ps1 appends the current production functions verbatim.
 */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#define IN
#define OUT
#define TRUE 1
#define FALSE 0
#define NULL_CONTEXT ((PEXT2_IRP_CONTEXT)0)
#define MAXULONG UINT32_MAX
#define MAXULONGLONG UINT64_MAX
#ifndef min
#define min(a,b) ((a)<(b)?(a):(b))
#endif
#define NT_SUCCESS(s) ((s)>=0)
#define STATUS_SUCCESS 0
#define STATUS_UNSUCCESSFUL (-1)
#define STATUS_DISK_CORRUPT_ERROR (-2)
#define STATUS_INSUFFICIENT_RESOURCES (-3)
#define STATUS_INVALID_BUFFER_SIZE (-4)
#define STATUS_INVALID_PARAMETER (-5)
#define STATUS_UNEXPECTED_IO_ERROR (-6)
#define STATUS_NAME_TOO_LONG (-7)
#define PagedPool 0
#define IRP_NOCACHE 1
#define IRP_PAGING_IO 2
#define EXT2_LINKLEN_IN_INODE 60
#define EXT4_EXTENTS_FL 0x80000
#define S_ISDIR(mode) ((mode)==2)
#define ClearFlag(field,flag) ((field)&=~(flag))
#define ClearLongFlag ClearFlag
#define ICB_ZONE_INITED 1
#define ASSERT(test) do { if(!(test)) abort(); } while(0)
#define RtlCopyMemory memcpy
#define RtlZeroMemory(p,n) memset((p),0,(n))
typedef uint32_t ULONG,*PULONG;
typedef uint64_t ULONGLONG,*PULONGLONG;
typedef int64_t LONGLONG;
typedef int32_t NTSTATUS;
typedef unsigned char BOOLEAN,*PUCHAR;
typedef void *PVOID;
typedef struct { int Flags; } IRP;
typedef struct { IRP *Irp; } IRP_CONTEXT,*PEXT2_IRP_CONTEXT;
typedef IRP *PIRP;
#define VOID void
#include "write_internal.h"
typedef struct { unsigned Bits; ULONGLONG Blocks; } VCB,*PEXT2_VCB;
typedef struct { LONGLONG i_size; ULONG i_blocks,i_flags,i_mode,i_block[15]; } INODE;
typedef struct { ULONG Flags; int Extents; } ICB;
typedef struct { INODE *Inode; ICB *Icb; int FullName; } MCB,*PEXT2_MCB;
typedef struct _EXTENT { LONGLONG Lba; ULONG Length,Offset; struct _EXTENT *Next; } EXTENT,*PEXT2_EXTENT;
typedef struct { LONGLONG QuadPart,Reserved; } LARGE_INTEGER;
#define BLOCK_BITS (Vcb->Bits)
#define BLOCK_SIZE (1ULL<<BLOCK_BITS)
#define SECTOR_SIZE 512U
#define TOTAL_BLOCKS (Vcb->Blocks)
#define IsZoneInited(m) ((m)->Icb->Flags & ICB_ZONE_INITED)
static ULONGLONG map[64];
static NTSTATUS initialize_status,map_status,raw_status;
static unsigned map_calls,save_calls,inode_calls,truncate_calls;
static unsigned fail_save,fail_inode,fail_extent;
static unsigned live_extents;
static unsigned char saved[32768];
static unsigned checks;
static void *Ext2AllocatePool(int pool,size_t size,unsigned tag) { (void)pool;(void)tag;return malloc(size); }
static void Ext2FreePool(void *p,unsigned tag) { (void)tag;free(p); }
static void check(int yes,const char *name) { checks++; if(!yes) { fprintf(stderr,"FAIL: %s\n",name); exit(1); } }
static NTSTATUS Ext2InitializeZone(PEXT2_IRP_CONTEXT c,PEXT2_VCB v,PEXT2_MCB m) { (void)c;(void)v;if(NT_SUCCESS(initialize_status))m->Icb->Flags|=ICB_ZONE_INITED;return initialize_status; }
static BOOLEAN Ext2LookupBlockExtent(PEXT2_VCB v,PEXT2_MCB m,ULONG i,PULONGLONG b,PULONG n) { (void)v;(void)m;map_calls++; *b=i<64?map[i]:0; *n=1;return TRUE; }
static NTSTATUS Ext2BlockMap(PEXT2_IRP_CONTEXT c,PEXT2_VCB v,PEXT2_MCB m,ULONG i,BOOLEAN a,PULONGLONG b,PULONG n) { (void)c;(void)v;map_calls++;if(!NT_SUCCESS(map_status))return map_status;*b=i<64?map[i]:0;if(a&&!*b) { *b=map[i]=200+i;m->Inode->i_blocks+=8; } *n=1;return STATUS_SUCCESS; }
static BOOLEAN Ext2AddBlockExtent(PEXT2_VCB v,PEXT2_MCB m,ULONG i,ULONGLONG b,ULONG n) { (void)v;(void)m;(void)i;(void)b;(void)n;return TRUE; }
static void Ext2ClearAllExtents(int *p) { *p=0; }
static PEXT2_EXTENT Ext2AllocateExtent(void) { PEXT2_EXTENT e;if(fail_extent)return NULL;e=calloc(1,sizeof(*e));if(e)live_extents++;return e; }
static void Ext2DestroyExtentChain(PEXT2_EXTENT e) { while(e) { PEXT2_EXTENT n=e->Next;free(e);live_extents--;e=n; } }
static NTSTATUS Ext2ReadWriteBlocks(PEXT2_IRP_CONTEXT c,PEXT2_VCB v,PEXT2_EXTENT e,ULONG n) { (void)c;(void)v;(void)e;(void)n;return raw_status; }
static BOOLEAN Ext2SaveBuffer(PEXT2_IRP_CONTEXT c,PEXT2_VCB v,LONGLONG p,ULONG n,PVOID b) { (void)c;(void)v;(void)p;save_calls++;if(fail_save==save_calls)return FALSE;check(n<=sizeof(saved),"save capacity");memcpy(saved,b,n);return TRUE; }
static BOOLEAN Ext2InodeHoldsNoData(INODE *i) { return i->i_blocks==0; }   /* no xattr block in the model */
static BOOLEAN Ext2SaveInode(PEXT2_IRP_CONTEXT c,PEXT2_VCB v,INODE *i) { (void)c;(void)v;(void)i;inode_calls++;return fail_inode!=inode_calls; }
static NTSTATUS Ext2TruncateFile(PEXT2_IRP_CONTEXT c,PEXT2_VCB v,PEXT2_MCB m,LARGE_INTEGER *n) { (void)c;(void)v;truncate_calls++;m->Inode->i_size=n->QuadPart;m->Inode->i_blocks=0;memset(map,0,sizeof(map));return STATUS_SUCCESS; }
NTSTATUS Ext2BuildExtents(PEXT2_IRP_CONTEXT,PEXT2_VCB,PEXT2_MCB,ULONGLONG,ULONG,BOOLEAN,PEXT2_EXTENT*);
NTSTATUS Ext2WriteInode(PEXT2_IRP_CONTEXT,PEXT2_VCB,PEXT2_MCB,ULONGLONG,PVOID,ULONG,BOOLEAN,PULONG);
NTSTATUS Ext2WriteSymlink(PEXT2_IRP_CONTEXT,PEXT2_VCB,PEXT2_MCB,PVOID,ULONG,PULONG);
static void reset(INODE *inode,ICB *icb) { check(!live_extents,"no extent leaks");memset(inode,0,sizeof(*inode));memset(icb,0,sizeof(*icb));memset(map,0,sizeof(map));memset(saved,0,sizeof(saved));initialize_status=map_status=raw_status=0;map_calls=save_calls=inode_calls=truncate_calls=0;fail_save=fail_inode=fail_extent=0; }
int main(void) {
    VCB v={12,10000};INODE inode={0};ICB icb={0};MCB m={&inode,&icb,0};
    IRP irp={0};IRP_CONTEXT context={&irp};PEXT2_EXTENT chain=NULL;ULONG written;NTSTATUS s;
    unsigned char target[8192];memset(target,'x',sizeof(target));
    check(Ext4PagingWriteLength(1024,1000,512)==0,"paging offset between EOF and allocation returns zero");
    check(Ext4PagingWriteLength(1000,1000,512)==0,"paging at EOF returns zero");
    check(Ext4PagingWriteLength(512,1000,512)==488,"partial EOF page returns logical bytes");
    check(Ext4PagingWriteLength(0,0,UINT32_MAX)==0,"empty file paging write returns zero");
    check(Ext4PagingWriteLength(-1,1000,512)==0,"negative paging offset rejected");
    check(Ext4PagingWriteLength(0,INT64_MAX,UINT32_MAX)==UINT32_MAX,"wide file length remains bounded");
    reset(&inode,&icb);inode.i_size=12288;map[0]=100;map[2]=101;
    s=Ext2BuildExtents(NULL_CONTEXT,&v,&m,0,12288,FALSE,&chain);
    check(NT_SUCCESS(s),"sparse builder status");check(chain&&chain->Length==4096&&chain->Offset==0,"first sparse run");
    check(chain&&chain->Next&&chain->Next->Lba==101LL*4096&&chain->Next->Offset==8192&&chain->Next->Length==4096,"adjacent physical runs across hole stay separate");Ext2DestroyExtentChain(chain);
    reset(&inode,&icb);inode.i_size=4096;map[0]=100;initialize_status=STATUS_INSUFFICIENT_RESOURCES;
    s=Ext2BuildExtents(NULL_CONTEXT,&v,&m,0,4096,FALSE,&chain);check(s==initialize_status&&!chain&&!map_calls,"zone error reaches caller");
    reset(&inode,&icb);inode.i_size=4096;map[0]=v.Blocks;
    s=Ext2BuildExtents(NULL_CONTEXT,&v,&m,0,4096,FALSE,&chain);check(!NT_SUCCESS(s)&&!chain,"out-of-volume cached mapping rejected");
    reset(&inode,&icb);inode.i_size=8192;map[0]=100;map[1]=101;
    s=Ext2BuildExtents(NULL_CONTEXT,&v,&m,0,8192,FALSE,&chain);check(NT_SUCCESS(s)&&chain&&chain->Length==8192&&!chain->Next,"contiguous logical and physical ranges merge");Ext2DestroyExtentChain(chain);
    reset(&inode,&icb);inode.i_size=UINT32_MAX;irp.Flags=IRP_NOCACHE;
    s=Ext2BuildExtents(&context,&v,&m,0,UINT32_MAX,FALSE,&chain);check(s==STATUS_INVALID_BUFFER_SIZE&&!chain,"sector-rounding overflow rejected");irp.Flags=0;
    reset(&inode,&icb);inode.i_size=4096;map[0]=100;fail_save=1;
    s=Ext2WriteInode(NULL_CONTEXT,&v,&m,0,target,60,FALSE,&written);check(s==STATUS_UNEXPECTED_IO_ERROR&&written==0,"buffer failure is not a successful write");
    reset(&inode,&icb);s=Ext2WriteInode(NULL_CONTEXT,&v,&m,0,target,60,FALSE,&written);check(!NT_SUCCESS(s)&&written==0,"empty chain cannot report a nonempty write");
    for(unsigned size=59;size<=61;size++) { reset(&inode,&icb);s=Ext2WriteSymlink(NULL_CONTEXT,&v,&m,target,size,&written);check(NT_SUCCESS(s)&&written==size&&inode.i_size==size,"symlink boundary size");check(size<60?inode.i_blocks==0:inode.i_blocks>0,"symlink boundary allocation");check(!memcmp(size<60?(void*)inode.i_block:(void*)saved,target,size),"symlink target persisted"); }
    check(saved[61]==0,"block symlink has a trailing NUL");
    reset(&inode,&icb);inode.i_mode=2;s=Ext2WriteSymlink(NULL_CONTEXT,&v,&m,target,127,&written);check(NT_SUCCESS(s)&&written==127&&inode.i_blocks>0,"directory inode can become a block-backed symlink");
    reset(&inode,&icb);s=Ext2WriteSymlink(NULL_CONTEXT,&v,&m,target,4096,&written);check(s==STATUS_NAME_TOO_LONG&&written==0&&!map_calls&&!inode_calls,"oversized symlink rejected before mutation");
    reset(&inode,&icb);fail_save=1;s=Ext2WriteSymlink(NULL_CONTEXT,&v,&m,target,61,&written);check(!NT_SUCCESS(s)&&written==0&&truncate_calls==1&&inode.i_size==0,"failed symlink write releases partial target");
    reset(&inode,&icb);fail_inode=1;s=Ext2WriteSymlink(NULL_CONTEXT,&v,&m,target,61,&written);check(!NT_SUCCESS(s)&&written==0&&save_calls==0,"initial inode error propagated");
    reset(&inode,&icb);fail_inode=2;s=Ext2WriteSymlink(NULL_CONTEXT,&v,&m,target,61,&written);check(!NT_SUCCESS(s)&&written==0,"final inode error propagated");
    reset(&inode,&icb);inode.i_size=4096;map[0]=100;raw_status=STATUS_UNEXPECTED_IO_ERROR;s=Ext2WriteInode(&context,&v,&m,0,NULL,4096,TRUE,&written);check(s==raw_status&&written==0,"direct I/O error propagated");
    reset(&inode,&icb);puts("WRITE-PATH: ALL PASSED");printf("%u checks\n",checks);return 0;
}
