/* SPDX-License-Identifier: GPL-2.0-only
 * Production revoke/registration/flush functions with allocation and I/O faults.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define IN
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS 0
#define STATUS_INSUFFICIENT_RESOURCES (-1)
#define NT_SUCCESS(s) ((s)>=0)
#define JF_ACTIVE 1
#define JF_ABORTED 2
#define JREC_NORMAL 0
#define JREC_REVOKE 1
#define JTXN_RUNNING 0
#define tid_geq(a,b) ((a)>=(b))
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
typedef uint32_t ULONG;
typedef uint64_t ULONGLONG;
typedef int BOOLEAN,NTSTATUS,KIRQL;
typedef struct _LIST { struct _LIST *Flink,*Blink; } LIST_ENTRY;
struct rb_node { struct rb_node *rb_left,*rb_right; };
struct rb_root { struct rb_node *rb_node; };
struct buffer_head { struct rb_node b_rb_node; ULONGLONG b_blocknr; int b_count,b_jrefs; ULONG b_jtid; void *b_jrec; };
typedef struct _REC { LIST_ENTRY Link; struct buffer_head *bh; ULONGLONG Block; int Type; void *Snapshot; } REC,*PEXT2_JREC;
typedef struct { LIST_ENTRY Records; ULONG Tid,NumRevokes,NumNormal,NumRecords; int State; } TXN,*PEXT2_JTXN;
typedef struct { TXN *Running; ULONG Flags,MaxTxnBlocks,CommitRequest; int RecLookaside,WakeEvent; } JOURNAL,*PEXT2_JOURNAL;
struct block_device { struct rb_root bd_bh_root; int bd_bh_lock; void *bd_priv; volatile long bd_write_error; };
typedef struct { JOURNAL *Journal; struct block_device bd; } VCB,*PEXT2_VCB;
static TXN txn;
static JOURNAL journal;
static VCB vcb;
static struct buffer_head buffer;
static unsigned checks,aborts,allocated,freed,flushes,disk_flushes;
static int fail_allocate,fail_activate;
static NTSTATUS flush_status,disk_status;
static REC *last_record;
static void check(int yes,const char *name) { checks++;if(!yes) { fprintf(stderr,"FAIL: %s\n",name);exit(1); } }
static void *ExAllocateFromNPagedLookasideList(int *p) { (void)p;if(fail_allocate)return NULL;allocated++;return calloc(1,sizeof(REC)); }
static void ExFreeToNPagedLookasideList(int *p,void *r) { (void)p;freed++;free(r); }
static void ExAcquireResourceSharedLite(int *p,int wait) { (void)p;(void)wait; }
static void ExReleaseResourceLite(int *p) { (void)p; }
static void get_bh(struct buffer_head *b) { b->b_count++; }
static void put_bh(struct buffer_head *b) { b->b_count--; }
static struct rb_node *rb_first(struct rb_root *r) { return r->rb_node; }
static struct rb_node *rb_next(struct rb_node *n) { (void)n;return NULL; }
static TXN *JnlActivate(JOURNAL *j) { return fail_activate?NULL:j->Running; }
#define JnlLock(j,irql) do { (void)(j);(irql)=0; } while(0)
#define JnlUnlock(j,irql) do { (void)(j);(void)(irql); } while(0)
static void JnlAbort(JOURNAL *j,NTSTATUS s) { (void)s;aborts++;j->Flags|=JF_ABORTED; }
static void InsertTailList(LIST_ENTRY *h,LIST_ENTRY *e) { h->Flink=e;last_record=container_of(e,REC,Link); }
static void KeSetEvent(int *e,int p,int w) { (void)e;(void)p;(void)w; }
static long InterlockedCompareExchange(volatile long *p,long n,long old) { long previous=*p;if(previous==old)*p=n;return previous; }
static NTSTATUS Ext2FlushVolume(void *c,VCB *v,int shutdown) { (void)c;(void)v;(void)shutdown;flushes++;return flush_status; }
static NTSTATUS Ext2FlushDisk(VCB *v) { (void)v;disk_flushes++;return disk_status; }
static int Ext2LinuxError(NTSTATUS s) { return s; }
BOOLEAN Ext2JournalDirtyBuffer(PEXT2_VCB,struct buffer_head*);
BOOLEAN Ext2JournalRevokeBlocks(PEXT2_VCB,ULONGLONG,ULONG);
int sync_blockdev(struct block_device*);
static void reset(void) {
    if(last_record) { free(last_record);freed++;last_record=NULL; }
    check(allocated==freed,"no record leaks");
    memset(&txn,0,sizeof(txn));memset(&journal,0,sizeof(journal));memset(&vcb,0,sizeof(vcb));memset(&buffer,0,sizeof(buffer));
    journal.Running=&txn;journal.Flags=JF_ACTIVE;journal.MaxTxnBlocks=32;txn.Tid=1;
    vcb.Journal=&journal;vcb.bd.bd_priv=&vcb;vcb.bd.bd_bh_root.rb_node=&buffer.b_rb_node;
    buffer.b_blocknr=100;buffer.b_count=1;buffer.b_jrefs=1;
    fail_allocate=fail_activate=0;aborts=flushes=disk_flushes=0;flush_status=disk_status=0;
}
int main(void) {
    REC existing={0};
    reset();fail_allocate=1;check(!Ext2JournalRevokeBlocks(&vcb,100,1),"missing revoke allocation reports failure");check(aborts==1&&(journal.Flags&JF_ABORTED)&&!buffer.b_jrec&&buffer.b_count==1,"missing revoke aborts without losing buffer reference");
    reset();fail_allocate=1;existing.Type=JREC_NORMAL;buffer.b_jrec=&existing;txn.NumNormal=1;
    check(Ext2JournalRevokeBlocks(&vcb,100,1),"existing record requires no new allocation");check(existing.Type==JREC_REVOKE&&txn.NumNormal==0&&txn.NumRevokes==1&&!aborts,"existing normal record becomes revoke");
    reset();fail_allocate=1;buffer.b_jrefs=0;check(Ext2JournalRevokeBlocks(&vcb,100,1)&&!aborts,"unjournaled block requires no revoke record");
    reset();fail_activate=1;check(!Ext2JournalRevokeBlocks(&vcb,100,1),"unavailable transaction reports revoke failure");
    reset();check(Ext2JournalRevokeBlocks(&vcb,100,1),"old journaled block gets revoke");check(last_record&&last_record->Type==JREC_REVOKE&&last_record->Block==100&&buffer.b_jrefs==2&&txn.NumRevokes==1,"new revoke record registered");
    reset();journal.Flags|=JF_ABORTED;check(!Ext2JournalRevokeBlocks(&vcb,100,1),"aborted journal refuses free");
    reset();vcb.Journal=NULL;check(Ext2JournalRevokeBlocks(&vcb,100,1),"unjournaled volume may free");
    reset();fail_allocate=1;check(Ext2JournalDirtyBuffer(&vcb,&buffer)&&aborts==1,"registration OOM cannot select legacy write path");
    reset();journal.Flags|=JF_ABORTED;check(Ext2JournalDirtyBuffer(&vcb,&buffer),"aborted engine cannot select legacy write path");
    reset();buffer.b_jrefs=0;check(Ext2JournalDirtyBuffer(&vcb,&buffer),"normal buffer registration");check(last_record&&txn.NumNormal==1&&buffer.b_jrefs==1,"normal record accounting");
    reset();vcb.bd.bd_write_error=-9;check(sync_blockdev(&vcb.bd)==-9&&!flushes&&!disk_flushes,"retained write failure reaches recovery");
    reset();flush_status=-7;check(sync_blockdev(&vcb.bd)==-7&&flushes==1&&!disk_flushes,"cache flush failure reaches recovery");
    reset();disk_status=-8;check(sync_blockdev(&vcb.bd)==-8&&disk_flushes==1,"device flush failure reaches recovery");
    reset();check(!sync_blockdev(&vcb.bd)&&flushes==1&&disk_flushes==1,"recovery flush reaches device cache");
    reset();puts("JOURNAL-ERROR: ALL PASSED");printf("%u checks\n",checks);return 0;
}
