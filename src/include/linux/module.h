/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * COPYRIGHT:        See COPYRIGHT.TXT
 * PROJECT:          Ext2 File System Driver for WinNT/2K/XP
 * FILE:             Modules.h
 * PURPOSE:          Header file: nls structures & linux kernel ...
 * PROGRAMMER:       Matt Wu <mattwu@163.com>
 * HOMEPAGE:         http://www.ext2fsd.com
 * UPDATE HISTORY:
 */

#ifndef _EXT2_MODULE_HEADER_
#define _EXT2_MODULE_HEADER_

struct buffer_head;

/* INCLUDES *************************************************************/

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/rbtree.h>
#include <linux/fs.h>
#include <linux/log2.h>

/* STRUCTS ******************************************************/

#ifndef offsetof
# define offsetof(type, member) ((ULONG_PTR)&(((type *)0)->member))
#endif

#ifndef container_of
#define container_of(ptr, type, member)                  \
                ((type *)((char *)ptr - (char *)offsetof(type, member)))
#endif

/* Byte order swapping routines */

/* use the runtime routine or compiler's implementation */
#if (defined(_M_IX86) && (_MSC_FULL_VER > 13009037)) || \
    ((defined(_M_AMD64) || defined(_M_IA64)) &&         \
     (_MSC_FULL_VER > 13009175))
#ifdef __cplusplus
extern "C" {
#endif
#ifdef __cplusplus
}
#endif
#pragma intrinsic(_byteswap_ushort)
#pragma intrinsic(_byteswap_ulong)
#pragma intrinsic(_byteswap_uint64)

#define RtlUshortByteSwap(_x)    _byteswap_ushort((USHORT)(_x))
#define RtlUlongByteSwap(_x)     _byteswap_ulong((_x))
#define RtlUlonglongByteSwap(_x) _byteswap_uint64((_x))

#else

#ifndef RtlUshortByteSwap
USHORT
FASTCALL
RtlUshortByteSwap(
    IN USHORT Source
);
#endif

#ifndef RtlUlongByteSwap
ULONG
FASTCALL
RtlUlongByteSwap(
    IN ULONG Source
);
#endif

#ifndef RtlUlonglongByteSwap
ULONGLONG
FASTCALL
RtlUlonglongByteSwap(
    IN ULONGLONG Source
);
#endif

#endif

#define __swab16(x) RtlUshortByteSwap(x)
#define __swab32(x) RtlUlongByteSwap(x)
#define __swab64(x) RtlUlonglongByteSwap(x)

#define __cpu_to_le64(x) ((__u64)(x))
#define __le64_to_cpu(x) ((__u64)(x))
#define __cpu_to_le32(x) ((__u32)(x))
#define __le32_to_cpu(x) ((__u32)(x))
#define __cpu_to_le16(x) ((__u16)(x))
#define __le16_to_cpu(x) ((__u16)(x))
#define __cpu_to_be64(x) __swab64((x))
#define __be64_to_cpu(x) __swab64((x))
#define __cpu_to_be32(x) __swab32((x))
#define __be32_to_cpu(x) __swab32((x))
#define __cpu_to_be16(x) __swab16((x))
#define __be16_to_cpu(x) __swab16((x))

#ifndef cpu_to_le64
#define cpu_to_le64 __cpu_to_le64
#define le64_to_cpu __le64_to_cpu
#define cpu_to_le32 __cpu_to_le32
#define le32_to_cpu __le32_to_cpu
#define cpu_to_le16 __cpu_to_le16
#define le16_to_cpu __le16_to_cpu
#endif

#define cpu_to_be64 __cpu_to_be64
#define be64_to_cpu __be64_to_cpu
#define cpu_to_be32 __cpu_to_be32
#define be32_to_cpu __be32_to_cpu
#define cpu_to_be16 __cpu_to_be16
#define be16_to_cpu __be16_to_cpu

/* val may be negative: le16_add_cpu(&count, -1) is how Linux decrements */
static inline void le16_add_cpu(__le16 *var, int val)
{
	*var = cpu_to_le16((u16)(le16_to_cpu(*var) + val));
}

static inline void le32_add_cpu(__le32 *var, int val)
{
	*var = cpu_to_le32((u32)(le32_to_cpu(*var) + val));
}

/* Network to host byte swap functions */

/* kernel printk flags */

#define KERN_EMERG      "<0>"   /* system is unusable                   */
#define KERN_ALERT      "<1>"   /* action must be taken immediately     */
#define KERN_CRIT       "<2>"   /* critical conditions                  */
#define KERN_ERR        "<3>"   /* error conditions                     */
#define KERN_WARNING    "<4>"   /* warning conditions                   */
#define KERN_NOTICE     "<5>"   /* normal but significant condition     */
#define KERN_INFO       "<6>"   /* informational                        */
#define KERN_DEBUG      "<7>"   /* debug-level messages                 */

#define printk  DbgPrint

/*
 * error pointer
 */
#define MAX_ERRNO	4095
#define IS_ERR_VALUE(x) ((x) >= (unsigned long)-MAX_ERRNO)

static inline void *ERR_PTR(long error)
{
	return (void *)(long_ptr_t) error;
}

static inline long PTR_ERR(const void *ptr)
{
	return (long)(long_ptr_t) ptr;
}

static inline long IS_ERR(const void *ptr)
{
	return IS_ERR_VALUE((unsigned long)(long_ptr_t)ptr);
}

#define BUG_ON(c) assert(!(c))

/* Linux module definitions */

#define likely
#define unlikely

#define __init
#define __exit

#define THIS_MODULE NULL
#define MODULE_LICENSE(x)
#define MODULE_ALIAS_NLS(x)

#define try_module_get(x) (TRUE)
#define module_put(x)

#define module_init(X) int  __init module_##X() {return X();}
#define module_exit(X) void __exit module_##X() {X();}

#define DECLARE_INIT(X) int  __init  module_##X(void)
#define DECLARE_EXIT(X) void __exit  module_##X(void)

#define LOAD_MODULE(X) do {                             \
                            rc = module_##X();          \
                       } while(0)

#define UNLOAD_MODULE(X) do {                           \
                            module_##X();               \
                         } while(0)

#define LOAD_NLS    LOAD_MODULE
#define UNLOAD_NLS  UNLOAD_MODULE

/* spinlocks ..... */

typedef struct _spinlock_t {

    KSPIN_LOCK  lock;
    KIRQL       irql;
} spinlock_t;

#define spin_lock_init(sl)    KeInitializeSpinLock(&((sl)->lock))
#define spin_lock(sl)         KeAcquireSpinLock(&((sl)->lock), &((sl)->irql))
#define spin_unlock(sl)       KeReleaseSpinLock(&((sl)->lock), (sl)->irql)

/* bit operations */

/*
 * Atomic bit operations on arrays of unsigned long (32 bits on Windows).
 * set_bit and clear_bit return the previous value of the bit, so they are
 * also test_and_set_bit and test_and_clear_bit. The Interlocked primitives
 * are declared on LONG; the bit pattern is the same.
 */
#define BIT_WORD_OF(nr)     ((nr) >> ORDER_PER_LONG)
#define BIT_MASK_OF(nr)     (1UL << ((nr) & (BITS_PER_LONG - 1)))

static inline int set_bit(int nr, volatile unsigned long *addr)
{
    unsigned long mask = BIT_MASK_OF(nr);

    return (InterlockedOr((volatile LONG *)&addr[BIT_WORD_OF(nr)], (LONG)mask) & mask) != 0;
}

static inline int clear_bit(int nr, volatile unsigned long *addr)
{
    unsigned long mask = BIT_MASK_OF(nr);

    return (InterlockedAnd((volatile LONG *)&addr[BIT_WORD_OF(nr)], ~(LONG)mask) & mask) != 0;
}

static inline int test_bit(int nr, volatile const unsigned long *addr)
{
    return (addr[BIT_WORD_OF(nr)] & BIT_MASK_OF(nr)) != 0;
}

#define test_and_set_bit(nr, addr)      set_bit(nr, addr)
#define test_and_clear_bit(nr, addr)    clear_bit(nr, addr)

/* list definition ... */

#include <linux/list.h>

/*********************************************
 *  linux scheduler related structures      *
*********************************************/

/* task structure */

#define TASK_INTERRUPTIBLE      1
#define TASK_UNINTERRUPTIBLE    2

struct task_struct {
    pid_t pid;
    pid_t tid;
    char comm[32];
    void * journal_info;
};

extern struct task_struct *current;

/* scheduler routines */

static inline int cond_resched() {
    return FALSE;
}

#define might_sleep()  do {} while(0)

/* mutex */

typedef struct mutex {
    FAST_MUTEX  lock;
} mutex_t;

#define mutex_init(x)   ExInitializeFastMutex(&((x)->lock))
/* The "unsafe" variants keep IRQL at PASSIVE_LEVEL and only need normal
   kernel APCs disabled, so synchronous disk I/O issued while a mutex is
   held (journal superblock updates) can still complete. */
#define mutex_lock(x)   do { KeEnterCriticalRegion(); ExAcquireFastMutexUnsafe(&((x)->lock)); } while (0)
#define mutex_unlock(x) do { ExReleaseFastMutexUnsafe(&((x)->lock)); KeLeaveCriticalRegion(); } while (0)

/* wait queues: jbd2 initialises its heads and wakes them; nothing waits on them here */

struct __wait_queue_head {
    spinlock_t lock;
    struct list_head task_list;
};
typedef struct __wait_queue_head wait_queue_head_t;

void init_waitqueue_head(wait_queue_head_t *q);
int wake_up(wait_queue_head_t *queue);

/* timer structure */

struct timer_list {
    struct list_head entry;
    unsigned long expires;

    void (*function)(unsigned long);
    unsigned long data;

#ifdef CONFIG_TIMER_STATS
    void *start_site;
    char start_comm[16];
    int start_pid;
#endif
};

typedef struct kmem_cache kmem_cache_t;

struct block_device {

    unsigned long           bd_flags;   /* flags */
    atomic_t		        bd_count;   /* reference count */
    PDEVICE_OBJECT          bd_dev;     /* device object */
    ANSI_STRING             bd_name;    /* name in ansi string */
    DISK_GEOMETRY           bd_geo;     /* disk geometry */
    PARTITION_INFORMATION   bd_part;    /* partition information */
    void *                  bd_priv;    /* pointers to EXT2_VCB
                                           NULL if it's a journal dev */
    PFILE_OBJECT            bd_volume;  /* streaming object file */
    LARGE_MCB               bd_extents; /* dirty extents */

    kmem_cache_t *          bd_bh_cache;/* memory cache for buffer_head */
    ERESOURCE               bd_bh_lock; /* lock for bh tree and reaper list */
    struct rb_root          bd_bh_root; /* buffer_head red-black tree root */
    LIST_ENTRY              bd_bh_free; /* reaper list */
    KSPIN_LOCK              bd_bh_free_lock; /* the reaper list, for a release
                                                holding bd_bh_lock shared */
    KEVENT                  bd_bh_notify; /* set when the reaper has none of ours */
    LONG                    bd_bh_reaping; /* bhs the reaper took and has not freed yet */
    volatile LONG           bd_write_error; /* first metadata write error; cleared only at mount */
};

/* page information */

typedef struct page {
    void           *addr;
    void           *mapping;
    void           *private;
    atomic_t        count;
    __u32           index;
    __u32           flags;
} mem_map_t;

#define PG_locked		 0	/* Page is locked. Don't touch. */
#define PG_error		 1
#define PG_referenced		 2
#define PG_uptodate		 3
#define PG_dirty		 4
#define PG_unused		 5
#define PG_lru			 6
#define PG_active		 7
#define PG_slab			 8
#define PG_skip			10
#define PG_highmem		11
#define PG_checked		12	/* kill me in 2.5.<early>. */
#define PG_arch_1		13
#define PG_reserved		14
#define PG_launder		15	/* written out by VM pressure.. */
#define PG_fs_1			16	/* Filesystem specific */

#ifndef arch_set_page_uptodate
#define arch_set_page_uptodate(page)
#endif

#define __GFP_HIGHMEM   0x02

#define __GFP_WAIT	0x10	/* Can wait and reschedule? */
#define __GFP_HIGH	0x20	/* Should access emergency pools? */
#define __GFP_IO	0x40	/* Can start low memory physical IO? */
#define __GFP_HIGHIO	0x80	/* Can start high mem physical IO? */
#define __GFP_FS	0x100	/* Can call down to low-level FS? */

#define GFP_KERNEL	(__GFP_HIGH | __GFP_WAIT | __GFP_IO | __GFP_HIGHIO | __GFP_FS)
#define GFP_NOFS    0
#define __GFP_NOFAIL 0

#define KM_USER0 0

/* buffer head definitions */

enum bh_state_bits {
    BH_Uptodate,	        /* Contains valid data */
    BH_Dirty,	            /* Is dirty */
    BH_Verified,	 /* Is verified */
    BH_Lock,	            /* Is locked */
    BH_Req,		            /* Has been submitted for I/O */
    BH_Uptodate_Lock,       /* Used by the first bh in a page, to serialise
			                 * IO completion of other buffers in the page
			                 */

    BH_Mapped,	            /* Has a disk mapping */
    BH_New,		            /* Disk mapping was newly created by get_block */
    BH_Async_Read,	        /* Is under end_buffer_async_read I/O */
    BH_Async_Write,	        /* Is under end_buffer_async_write I/O */
    BH_Delay,	            /* Buffer is not yet allocated on disk */
    BH_Boundary,	        /* Block is followed by a discontiguity */
    BH_Write_EIO,	        /* I/O error on write */
    BH_Ordered,	            /* ordered write */
    BH_Eopnotsupp,	        /* operation not supported (barrier) */
    BH_Unwritten,	        /* Buffer is allocated on disk but not written */
    BH_Virtual,	            /* No block of the volume: a view built in memory
                             * (an inline directory); never written, freed
                             * with its last reference */

    BH_PrivateStart,        /* not a state bit, but the first bit available
			                 * for private allocation by other entities
			                 */
};

#define PAGE_CACHE_SHIFT (12)

typedef void (bh_end_io_t)(struct buffer_head *bh, int uptodate);

/*
 * Historically, a buffer_head was used to map a single block
 * within a page, and of course as the unit of I/O through the
 * filesystem and block layers.  Nowadays the basic I/O unit
 * is the bio, and buffer_heads are used for extracting block
 * mappings (via a get_block_t call), for tracking state within
 * a page (via a page_mapping) and for wrapping bio submission
 * for backward compatibility reasons (e.g. submit_bh).
 */
struct buffer_head {
    LIST_ENTRY    b_link;                   /* to be added to reaper list */
    unsigned long b_state;		            /* buffer state bitmap (see above) */
    struct page *b_page;                    /* the page this bh is mapped to */
    PMDL         b_mdl;                     /* MDL of the locked buffer */
    void	    *b_bcb;                     /* BCB of the buffer */

    /* kdev_t b_dev;                        /* device (B_FREE = free) * / */
    struct block_device *b_bdev;            /* block device object */

    blkcnt_t b_blocknr;		        /* start block number */
    size_t        b_size;			        /* size of mapping */
    char *        b_data;			        /* pointer to data within the page */
    bh_end_io_t *b_end_io;		        /* I/O completion */
    void *b_private;		                /* reserved for b_end_io */
    /* struct list_head b_assoc_buffers;    /* associated with another mapping * /
       struct address_space *b_assoc_map;   /* mapping this buffer is associated with * / */
    atomic_t b_count;		                /* users using this buffer_head */
    struct rb_node b_rb_node;               /* Red-black tree node entry */

    LARGE_INTEGER  b_ts_creat;              /* creation time*/
    LARGE_INTEGER  b_ts_drop;               /* drop time (to be released) */

    /* write-ahead journal bookkeeping (jbd2\commit.c) */
    void          *b_jrec;                  /* record in the running transaction */
    unsigned int   b_jtid;                  /* newest transaction that touched us */
    unsigned int   b_jctid;                 /* newest committed transaction that holds us */
    LONG           b_jrefs;                 /* references held by journal records */
    LONG           b_jpin;                  /* long-term holders (group desc cache) */
};

/*
 * macro tricks to expand the set_buffer_foo(), clear_buffer_foo()
 * and buffer_foo() functions.
 */
#define BUFFER_FNS(bit, name)						\
static inline void set_buffer_##name(struct buffer_head *bh)		\
{									\
	set_bit(BH_##bit, &(bh)->b_state);				\
}									\
static inline void clear_buffer_##name(struct buffer_head *bh)		\
{									\
	clear_bit(BH_##bit, &(bh)->b_state);				\
}									\
static inline int buffer_##name(const struct buffer_head *bh)		\
{									\
	return test_bit(BH_##bit, &(bh)->b_state);			\
}

/*
 * test_set_buffer_foo() and test_clear_buffer_foo()
 */
#define TAS_BUFFER_FNS(bit, name)					\
static inline int test_set_buffer_##name(struct buffer_head *bh)	\
{									\
	return test_and_set_bit(BH_##bit, &(bh)->b_state);		\
}									\
static inline int test_clear_buffer_##name(struct buffer_head *bh)	\
{									\
	return test_and_clear_bit(BH_##bit, &(bh)->b_state);		\
}									\
 
/*
 * Emit the buffer bitops functions.   Note that there are also functions
 * of the form "mark_buffer_foo()".  These are higher-level functions which
 * do something in addition to setting a b_state bit.
 */
BUFFER_FNS(Uptodate, uptodate)
BUFFER_FNS(Dirty, dirty)
TAS_BUFFER_FNS(Dirty, dirty)
BUFFER_FNS(Verified, verified)
BUFFER_FNS(Lock, locked)
TAS_BUFFER_FNS(Lock, locked)
BUFFER_FNS(Req, req)
TAS_BUFFER_FNS(Req, req)
BUFFER_FNS(Mapped, mapped)
BUFFER_FNS(New, new)
BUFFER_FNS(Async_Read, async_read)
BUFFER_FNS(Async_Write, async_write)
BUFFER_FNS(Delay, delay)
BUFFER_FNS(Boundary, boundary)
BUFFER_FNS(Write_EIO, write_io_error)
BUFFER_FNS(Ordered, ordered)
BUFFER_FNS(Eopnotsupp, eopnotsupp)
BUFFER_FNS(Unwritten, unwritten)
BUFFER_FNS(Virtual, virtual)

/* If we *know* page->private refers to buffer_heads */

/*
 * Declarations
 */

void mark_buffer_dirty(struct buffer_head *bh);
struct buffer_head *alloc_page_buffers(struct page *page, unsigned long size,
                                                   int retry);

int sync_blockdev(struct block_device *bdev);
void __wait_on_buffer(struct buffer_head *);
struct super_block *freeze_bdev(struct block_device *);
struct buffer_head *get_block_bh(struct block_device *bdev, sector_t block,
                                 unsigned long size, int zero);
struct buffer_head *__getblk(struct block_device *bdev, sector_t block,
                                         unsigned long size);
void __brelse(struct buffer_head *);
/* a buffer of its own, not a block of the volume (BH_Virtual) */
struct buffer_head *alloc_virtual_bh(struct block_device *bdev, size_t size);
void __bforget(struct buffer_head *);
struct buffer_head *__bread(struct block_device *, sector_t block, unsigned size);
struct buffer_head *alloc_buffer_head(gfp_t gfp_flags);
void free_buffer_head(struct buffer_head * bh);
void unlock_buffer(struct buffer_head *bh);
void __lock_buffer(struct buffer_head *bh);
void ll_rw_block(int, int, struct buffer_head * bh[]);
int submit_bh(int, struct buffer_head *);
int bh_submit_read(struct buffer_head *bh);
/* They are separately managed  */
struct buffer_head *extents_bread(struct super_block *sb, sector_t block);
struct buffer_head *extents_bwrite(struct super_block *sb, sector_t block);
void extents_mark_buffer_dirty(struct buffer_head *bh);
void extents_brelse(struct buffer_head *bh);
void extents_bforget(struct buffer_head *bh);
void buffer_head_remove(struct block_device *bdev, struct buffer_head *bh);

/*
 * Generic address_space_operations implementations for buffer_head-backed
 * address_spaces.
 */

/*
 * inline definitions
 */

static inline void get_bh(struct buffer_head *bh)
{
    atomic_inc(&bh->b_count);
}

static inline void put_bh(struct buffer_head *bh)
{
    if (bh)
        __brelse(bh);
}

static inline void brelse(struct buffer_head *bh)
{
    if (bh)
        __brelse(bh);
}

static inline void fini_bh(struct buffer_head **bh)
{
    if (bh && *bh) {
        brelse(*bh);
        *bh = NULL;
    }
}

static inline void bforget(struct buffer_head *bh)
{
    if (bh)
        __bforget(bh);
}

static inline struct buffer_head *
            sb_getblk(struct super_block *sb, sector_t block)
{
    return get_block_bh(sb->s_bdev, block, sb->s_blocksize, 0);
}

static inline struct buffer_head *
            sb_getblk_zero(struct super_block *sb, sector_t block)
{
    return get_block_bh(sb->s_bdev, block, sb->s_blocksize, 1);
}

/*
 * Calling wait_on_buffer() for a zero-ref buffer is illegal, so we call into
 * __wait_on_buffer() just to trip a debug check.  Because debug code in inline
 * functions is bloaty.
 */

static inline void wait_on_buffer(struct buffer_head *bh)
{
    might_sleep();
    if (buffer_locked(bh) || atomic_read(&bh->b_count) == 0)
        __wait_on_buffer(bh);
}

static inline void lock_buffer(struct buffer_head *bh)
{
    might_sleep();
    if (test_set_buffer_locked(bh))
        __lock_buffer(bh);
}

/* unicode character */

struct nls_table {
    char *charset;
    char *alias;
    int (*uni2char) (wchar_t uni, unsigned char *out, int boundlen);
    int (*char2uni) (const unsigned char *rawstring, int boundlen,
                     wchar_t *uni);
    struct module *owner;
    struct nls_table *next;
};

/* this value hold the maximum octet of charset */
#define NLS_MAX_CHARSET_SIZE 6 /* for UTF-8 */

/* nls.c */
extern int register_nls(struct nls_table *);
extern int unregister_nls(struct nls_table *);
extern struct nls_table *load_nls(char *);
extern void unload_nls(struct nls_table *);

extern int utf8_mbtowc(wchar_t *, const __u8 *, int);
extern int utf8_wctomb(__u8 *, wchar_t, int);

/* kernel jiffies */

#define HZ  (100)

static inline __u32 JIFFIES()
{
    LARGE_INTEGER Tick;

    KeQueryTickCount(&Tick);
    Tick.QuadPart *= KeQueryTimeIncrement();
    Tick.QuadPart /= (10000000 / HZ);

    return Tick.LowPart;
}

#define jiffies JIFFIES()

/* memory routines */

PVOID Ext2AllocatePool(
    IN POOL_TYPE PoolType,
    IN SIZE_T NumberOfBytes,
    IN ULONG Tag
);

VOID
Ext2FreePool(
    IN PVOID P,
    IN ULONG Tag
);

void *kzalloc(size_t size, int flags);
#define kmalloc(size, gfp) Ext2AllocatePool(NonPagedPool, size, 'JBDM')
#define kfree(p) Ext2FreePool(p, 'JBDM')

/* memory slab */

#define	SLAB_HWCACHE_ALIGN	0x00002000U	/* align objs on a h/w cache lines */
#define SLAB_KERNEL         0x00000001U
#define SLAB_TEMPORARY      0x00000002U

typedef void (*kmem_cache_cb_t)(void*, kmem_cache_t *, unsigned long);

struct kmem_cache {
    CHAR                    name[32];
    ULONG                   flags;
    ULONG                   size;
    atomic_t                count;
    atomic_t                acount;
    NPAGED_LOOKASIDE_LIST   la;
    kmem_cache_cb_t         constructor;
};

kmem_cache_t *
kmem_cache_create(
    const char *name,
    size_t size,
    size_t offset,
    unsigned long flags,
    kmem_cache_cb_t ctor
);

void* kmem_cache_alloc(kmem_cache_t *kc, int flags);
void  kmem_cache_free(kmem_cache_t *kc, void *p);
int   kmem_cache_destroy(kmem_cache_t *kc);

/* block device */

#define BDEVNAME_SIZE      32      /* Largest string for a blockdev identifier */

/* ll_rw_block .... */

#define RW_MASK         1
#define RWA_MASK        2
#define READ 0
#define WRITE 1
#define READA 2         /* read-ahead  - don't block if no resources */
#define SWRITE 3        /* for ll_rw_block() - wait for buffer lock */

/* timer routines */

#define smp_rmb()  do {}while(0)

static inline __u32 do_div64 (__u64 * n, __u64 b)
{
    __u64 mod;

    mod = *n % b;
    *n = *n / b;
    return (__u32) mod;
}
#define do_div(n, b) do_div64(&(n), (__u64)b)

#endif /* _EXT2_MODULE_HEADER_ */
