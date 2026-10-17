/**
 * HtreeDirectory.c - indexed directories in the driver: lookups, adds and
 * listings in hash order, through DirIndex.c.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * DirIndex.c walks and grows the index over blocks this file reads and
 * writes: index blocks through ext3_bread_index (checked as index blocks,
 * their checksum verified), leaves through ext3_bread, new blocks through
 * ext3_append; a changed block gets its checksum and is marked dirty, which
 * puts it in the running journal transaction. Names are hashed by
 * NameHash.c, a casefolded directory's folded first, as Linux hashes them.
 *
 * An index found not sound - a bad root or node, a path that comes back on
 * itself - is ERR_BAD_DX_DIR to the callers, which then read the directory
 * as a plain one, as Linux does; a leaf entry that is not sound is
 * recorded as corruption.
 *
 * A listing goes in hash order (the order of the index): the names of the
 * leaves from a hash on are gathered in a tree sorted by (hash, minor hash),
 * names of one hash pair chained, and handed out from the position the
 * handle is at - half the hash, which is what the position of an entry
 * reported to Windows is.
 */

#include "ext4fs.h"
#include "NameHash.h"
#include "DirectoryInternal.h"

#define HT_POS(hash)        ((hash) >> 1)           /* a listing position */
#define HT_HASH(pos)        ((__u32)((pos) << 1))   /* and back */
#define HT_NO_MORE          (~0u)
#define HT_FOLD_STACK       512
#define HT_TAG              'thxE'

typedef struct _HT_DIR {
    DX_DIR              D;
    struct ext2_icb    *Icb;
    struct inode       *Dir;
    int                 IndexCorrupt;   /* an index block was not sound */
} HT_DIR, *PHT_DIR;

/* ---- the directory as DirBlock.c and DirIndex.c see it ---- */

VOID Ext4DirGeometry(IN struct inode *Dir, OUT DB_GEOMETRY *G)
{
    struct super_block *sb = Dir->i_sb;

    G->BlockSize = (unsigned int)sb->s_blocksize;
    G->Csum = ext4_has_metadata_csum(sb) ? 1 : 0;
    G->InodesCount = le32_to_cpu(EXT3_SB(sb)->s_es->s_inodes_count);
    G->FileTypes = EXT3_HAS_INCOMPAT_FEATURE(sb, EXT3_FEATURE_INCOMPAT_FILETYPE) ? 1 : 0;
}

int Ext4DirMatch(void *Context, const unsigned char *Entry)
{
    PEXT4_DIR_NAME n = (PEXT4_DIR_NAME)Context;
    return ext4_match(n->Dir, n->Length, n->Name, (struct ext3_dir_entry_2 *)Entry);
}

int Ext4DirTouched(IN struct ext2_icb *Icb, IN struct inode *Dir)
{
    Dir->i_mtime = Dir->i_ctime = ext3_current_time(Dir);
    ext3_update_dx_flag(Dir);
    Dir->i_version++;
    return ext3_mark_inode_dirty(Icb, Dir);
}

VOID Ext4DirCorrupt(IN struct inode *Dir, IN struct buffer_head *Bh)
{
    Ext2ReportCorruption((PEXT2_VCB)Dir->i_sb->s_priv, (ULONG)Dir->i_ino,
                         Bh ? (ULONGLONG)Bh->b_blocknr : 0, -EFSCORRUPTED);
}

/* the hash of a name in Dir: a casefolded directory hashes the folded form
   (one that is not UTF-8, or folds too long, as it is - so does Linux) */
static int HtHashName(struct inode *Dir, const unsigned char *Name, unsigned int Length,
                      unsigned int Version, unsigned int *Major, unsigned int *Minor)
{
    struct ext4_sb_info *sbi = EXT3_SB(Dir->i_sb);
    unsigned char        stack[HT_FOLD_STACK];
    unsigned char       *pool = NULL, *folded = stack;
    int                  length = -1, rc;

    if (Length && ext4_casefolded(Dir)) {
        length = Ext4Utf8Casefold((const char *)Name, (int)Length, stack, sizeof(stack));
        if (length == EXT4_UTF8_TOO_LONG) {
            pool = Ext2AllocatePool(PagedPool, EXT4_UTF8_FOLDED_MAX, HT_TAG);
            if (!pool)
                return -ENOMEM;
            folded = pool;
            length = Ext4Utf8Casefold((const char *)Name, (int)Length, pool, EXT4_UTF8_FOLDED_MAX);
        }
    }
    if (length >= 0)
        rc = NhHash(folded, (unsigned int)length, Version, sbi->s_hash_seed, Major, Minor);
    else
        rc = NhHash(Name, Length, Version, sbi->s_hash_seed, Major, Minor);
    if (pool)
        Ext2FreePool(pool, HT_TAG);
    return rc ? -EINVAL : 0;
}

static int HtGet(void *Context, unsigned int Block, int Index, unsigned char **Data, void **Handle)
{
    PHT_DIR             h = (PHT_DIR)Context;
    struct buffer_head *bh;
    int                 err = 0;

    bh = Index ? ext3_bread_index(h->Icb, h->Dir, Block, &err) : ext3_bread(h->Icb, h->Dir, Block, &err);
    if (!bh)
        return err ? err : -EIO;
    *Data = (unsigned char *)bh->b_data;
    *Handle = bh;
    return 0;
}

static void HtPut(void *Context, void *Handle)
{
    UNREFERENCED_PARAMETER(Context);
    brelse((struct buffer_head *)Handle);
}

static int HtAppend(void *Context, unsigned int *Block, unsigned char **Data, void **Handle)
{
    PHT_DIR             h = (PHT_DIR)Context;
    struct buffer_head *bh;
    ext3_lblk_t         block;
    int                 err = 0;

    bh = ext3_append(h->Icb, h->Dir, &block, &err);
    if (!bh)
        return err ? err : -EIO;
    *Block = (unsigned int)block;
    *Data = (unsigned char *)bh->b_data;
    *Handle = bh;
    return 0;
}

static int HtDirty(void *Context, void *Handle, int Index)
{
    PHT_DIR             h = (PHT_DIR)Context;
    struct buffer_head *bh = (struct buffer_head *)Handle;

    if (Index)
        ext4_dx_csum_set(h->Dir, (struct ext4_dir_entry *)bh->b_data);
    else
        ext4_dirent_csum_set(h->Dir, (struct ext4_dir_entry *)bh->b_data);
    mark_buffer_dirty(bh);
    return 0;
}

static int HtHash(void *Context, const unsigned char *Name, unsigned int Length, unsigned int Version,
                  unsigned int *Major, unsigned int *Minor)
{
    return HtHashName(((PHT_DIR)Context)->Dir, Name, Length, Version, Major, Minor);
}

static void *HtAllocate(void *Context, unsigned int Bytes)
{
    UNREFERENCED_PARAMETER(Context);
    return Ext2AllocatePool(PagedPool, Bytes, HT_TAG);
}

static void HtRelease(void *Context, void *Memory)
{
    UNREFERENCED_PARAMETER(Context);
    Ext2FreePool(Memory, HT_TAG);
}

static void HtCorrupt(void *Context, unsigned int Block, int Index)
{
    PHT_DIR h = (PHT_DIR)Context;

    UNREFERENCED_PARAMETER(Block);
    if (Index) {
        h->IndexCorrupt = 1;    /* Linux warns and reads the directory plainly */
    } else {
        Ext4DirCorrupt(h->Dir, NULL);
    }
}

static void HtSetup(PHT_DIR h, struct ext2_icb *Icb, struct inode *Dir)
{
    RtlZeroMemory(h, sizeof(*h));
    h->Icb = Icb;
    h->Dir = Dir;
    Ext4DirGeometry(Dir, &h->D.G);
    h->D.MaxLevels = (unsigned int)ext4_dir_htree_level(Dir->i_sb);
    h->D.UnsignedDelta = (unsigned int)EXT3_SB(Dir->i_sb)->s_hash_unsigned;
    h->D.Io.Context = h;
    h->D.Io.Get = HtGet;
    h->D.Io.Put = HtPut;
    h->D.Io.Append = HtAppend;
    h->D.Io.Dirty = HtDirty;
    h->D.Io.Hash = HtHash;
    h->D.Io.Allocate = HtAllocate;
    h->D.Io.Release = HtRelease;
    h->D.Io.Corrupt = HtCorrupt;
}

/* the version a new index is made with: the volume's default (0-2) */
static unsigned int HtNewVersion(struct inode *Dir)
{
    int version = EXT3_SB(Dir->i_sb)->s_def_hash_version;
    return version >= DX_HASH_LEGACY && version <= DX_HASH_TEA ? (unsigned int)version : DX_HASH_HALF_MD4;
}

/* ---- lookups and adds ---- */

struct buffer_head *
ext3_dx_find_entry(struct ext2_icb *icb, struct dentry *dentry,
                   struct ext3_dir_entry_2 **res_dir, int *err)
{
    HT_DIR          h;
    EXT4_DIR_NAME   name;
    void           *handle;
    unsigned char  *data;
    unsigned int    offset;
    int             rc;

    name.Dir = dentry->d_parent->d_inode;
    name.Name = dentry->d_name.name;
    name.Length = dentry->d_name.len;
    HtSetup(&h, icb, name.Dir);
    rc = DxLookup(&h.D, (const unsigned char *)name.Name, (unsigned int)name.Length,
                  Ext4DirMatch, &name, &handle, &data, &offset);
    if (rc == 0) {
        *res_dir = (struct ext3_dir_entry_2 *)(data + offset);
        *err = 0;
        return (struct buffer_head *)handle;
    }
    /* a damaged index or leaf: the plain scan takes over, as in Linux */
    *err = rc == DB_ECORRUPT ? ERR_BAD_DX_DIR : rc;
    return NULL;
}

int ext3_dx_add_entry(struct ext2_icb *icb, struct dentry *dentry, struct inode *inode)
{
    HT_DIR          h;
    EXT4_DIR_NAME   name;
    int             rc;

    name.Dir = dentry->d_parent->d_inode;
    name.Name = dentry->d_name.name;
    name.Length = dentry->d_name.len;
    HtSetup(&h, icb, name.Dir);
    rc = DxAdd(&h.D, (const unsigned char *)name.Name, (unsigned int)name.Length,
               inode ? (unsigned int)inode->i_ino : 0, inode ? ext3_type_by_mode(inode->i_mode) : 0,
               Ext4DirMatch, &name);
    if (rc == 0)
        return Ext4DirTouched(icb, name.Dir);
    if (rc == DB_ECORRUPT)
        return h.IndexCorrupt ? ERR_BAD_DX_DIR : -EFSCORRUPTED;
    return rc;
}

/* the one-block directory in bh, full, becomes indexed with the name added;
   bh is released */
int make_indexed_dir(struct ext2_icb *icb, struct dentry *dentry,
                     struct inode *inode, struct buffer_head *bh)
{
    HT_DIR          h;
    struct inode   *dir = dentry->d_parent->d_inode;
    int             rc, converted = 0, written;

    HtSetup(&h, icb, dir);
    rc = DxMakeIndexed(&h.D, bh, (unsigned char *)bh->b_data, HtNewVersion(dir),
                       (const unsigned char *)dentry->d_name.name, (unsigned int)dentry->d_name.len,
                       inode ? (unsigned int)inode->i_ino : 0, inode ? ext3_type_by_mode(inode->i_mode) : 0,
                       &converted);
    brelse(bh);
    if (!converted)
        return rc == DB_ECORRUPT ? -EFSCORRUPTED : rc;
    /* block 0 is a root now: the directory is indexed, whatever came after */
    EXT3_I(dir)->i_flags |= EXT3_INDEX_FL;
    written = Ext4DirTouched(icb, dir);
    if (rc)
        return rc == DB_ECORRUPT ? -EFSCORRUPTED : rc;
    return written;
}

/* ---- listings in hash order ---- */

/* a name of the tree; the ones of one (hash, minor) pair are chained */
struct fname {
    __u32           hash;
    __u32           minor_hash;
    struct rb_node  rb_hash;
    struct fname   *next;
    __u32           inode;
    __u8            name_len;
    __u8            file_type;
    char            name[1];
};

/* the names of one fill of the tree live in chunks, released together;
   the newest stays for the next fill */
#define HT_CHUNK_BYTES  16384

struct fname_chunk {
    struct fname_chunk *next;
    ULONG               used;
    ULONG               size;
    char                data[1];
};

static struct fname *HtNewName(struct dir_private_info *info, ULONG Bytes)
{
    struct fname_chunk *c = info->chunks;
    ULONG               need = (Bytes + 7) & ~7u;

    if (!c || c->size - c->used < need) {
        ULONG size = max(HT_CHUNK_BYTES, need + (ULONG)sizeof(struct fname_chunk));
        c = Ext2AllocatePool(PagedPool, size, HT_TAG);
        if (!c)
            return NULL;
        c->next = info->chunks;
        c->used = 0;
        c->size = size - FIELD_OFFSET(struct fname_chunk, data);
        info->chunks = c;
    }
    c->used += need;
    return (struct fname *)(c->data + c->used - need);
}

/* the tree emptied; Everything: the chunks given back as well */
static void HtEmptyTree(struct dir_private_info *info, BOOLEAN Everything)
{
    struct fname_chunk *c = info->chunks, *gone;

    if (c) {
        for (gone = Everything ? c : c->next; gone; ) {
            struct fname_chunk *next = gone->next;
            Ext2FreePool(gone, HT_TAG);
            gone = next;
        }
        if (Everything) {
            info->chunks = NULL;
        } else {
            c->next = NULL;
            c->used = 0;
        }
    }
    info->root.rb_node = NULL;
}

void ext3_htree_free_dir_info(struct dir_private_info *p)
{
    HtEmptyTree(p, TRUE);
    Ext2FreePool(p, HT_TAG);
}

int ext3_release_dir(struct inode *inode, struct file *filp)
{
    UNREFERENCED_PARAMETER(inode);
    if (filp->private_data) {
        ext3_htree_free_dir_info(filp->private_data);
        filp->private_data = NULL;
    }
    return 0;
}

/* an entry into the tree, by (hash, minor hash) */
static int HtStore(struct dir_private_info *info, __u32 Hash, __u32 Minor, const unsigned char *Entry)
{
    struct rb_node **p = &info->root.rb_node, *parent = NULL;
    unsigned int     length = Entry[DB_E_NAME_LEN];
    struct fname    *n = HtNewName(info, FIELD_OFFSET(struct fname, name) + length + 1);

    if (!n)
        return -ENOMEM;
    RtlZeroMemory(n, FIELD_OFFSET(struct fname, name));
    n->hash = Hash;
    n->minor_hash = Minor;
    n->inode = DbGet32(Entry + DB_E_INODE);
    n->name_len = (__u8)length;
    n->file_type = Entry[DB_E_TYPE];
    RtlCopyMemory(n->name, Entry + DB_E_NAME, length);
    n->name[length] = 0;

    while (*p) {
        struct fname *at = rb_entry(*p, struct fname, rb_hash);

        parent = *p;
        if (Hash == at->hash && Minor == at->minor_hash) {
            n->next = at->next;     /* a pair already there: chained */
            at->next = n;
            return 0;
        }
        if (Hash < at->hash || (Hash == at->hash && Minor < at->minor_hash))
            p = &(*p)->rb_left;
        else
            p = &(*p)->rb_right;
    }
    rb_link_node(&n->rb_hash, parent, p);
    rb_insert_color(&n->rb_hash, &info->root);
    return 0;
}

/* the names of leaf Block at or after (Start, StartMinor): their number, or an error */
static int HtLeafToTree(PHT_DIR h, struct dir_private_info *info, unsigned int Block,
                        unsigned int Version, __u32 Start, __u32 StartMinor)
{
    struct buffer_head *bh;
    unsigned char      *data;
    unsigned int        size = h->D.G.BlockSize, at;
    int                 err = 0, count = 0;

    bh = ext3_bread(h->Icb, h->Dir, Block, &err);
    if (!bh)
        return err ? err : -EIO;
    data = (unsigned char *)bh->b_data;
    for (at = 0; at < size - DB_REC_LEN(0); at += DbRecLen(&h->D.G, data + at)) {
        unsigned int major, minor;

        if (DbCheck(&h->D.G, data, size, at)) {
            Ext4DirCorrupt(h->Dir, bh);     /* the rest of the block is left out */
            break;
        }
        if (DbGet32(data + at + DB_E_INODE) == 0)
            continue;
        err = HtHashName(h->Dir, data + at + DB_E_NAME, data[at + DB_E_NAME_LEN], Version, &major, &minor);
        if (err) {
            brelse(bh);
            return err;     /* placed by a wrong hash, it would be skipped or repeated */
        }
        if (major < Start || (major == Start && minor < StartMinor))
            continue;
        err = HtStore(info, major, minor, data + at);
        if (err) {
            brelse(bh);
            return err;
        }
        count++;
    }
    brelse(bh);
    return count;
}

/*
 * The tree filled from (Start, StartMinor): the names of the leaves the
 * index leads to from there on, at least one leaf's worth and on as long as
 * the next leaf continues a hash. A directory of one block without an index
 * is hashed whole. Returns the number of names, or an error; *Next is the
 * first hash of the leaf that comes next (HT_NO_MORE: none).
 */
static int HtFillTree(struct ext2_icb *icb, struct file *filp, __u32 Start, __u32 StartMinor, __u32 *Next)
{
    struct dir_private_info *info = filp->private_data;
    struct inode            *dir = filp->f_dentry->d_inode;
    HT_DIR                   h;
    DX_PATH                  path;
    unsigned char           *root;
    int                      rc, count = 0;

    HtSetup(&h, icb, dir);
    *Next = HT_NO_MORE;
    if (!(EXT3_I(dir)->i_flags & EXT3_INDEX_FL)) {
        unsigned int version = HtNewVersion(dir);
        if (version <= DX_HASH_TEA)
            version += h.D.UnsignedDelta;
        return HtLeafToTree(&h, info, 0, version, Start, StartMinor);
    }

    rc = DxProbe(&h.D, NULL, 0, Start, &path);
    if (rc)
        return rc == DB_ECORRUPT ? ERR_BAD_DX_DIR : rc;

    /* '.' and '..' come first, from the root, at hashes 0 and 2 */
    root = path.Frame[0].Data;
    if (!Start && !StartMinor) {
        rc = HtStore(info, 0, 0, root);
        if (rc)
            goto out;
        count++;
    }
    if (Start < 2 || (Start == 2 && StartMinor == 0)) {
        rc = HtStore(info, 2, 0, root + DbRecLen(&h.D.G, root));
        if (rc)
            goto out;
        count++;
    }

    for (;;) {
        __u32 next = HT_NO_MORE;

        rc = HtLeafToTree(&h, info, DxLeaf(&path), path.Version, Start, StartMinor);
        if (rc < 0)
            goto out;
        count += rc;
        rc = DxNextLeaf(&h.D, &path, DX_NEXT_ALWAYS, &next);
        *Next = next;
        if (rc < 0)
            goto out;
        /* the end, or a leaf that starts a new hash once there are names */
        if (rc == 0 || (count && (next & DX_HASH_CONTINUED) == 0))
            break;
    }
    rc = count;
out:
    DxRelease(&h.D, &path);
    return rc;
}

/* a chain of names to the caller; a full buffer leaves the rest for later */
static int HtHandOut(struct file *filp, void *cookie, filldir_t filldir, struct fname *n)
{
    struct dir_private_info *info = filp->private_data;
    struct super_block      *sb = filp->f_dentry->d_inode->i_sb;
    loff_t                   pos = HT_POS(n->hash);

    for (; n; n = n->next) {
        if (filldir(cookie, n->name, n->name_len, (unsigned long)pos, n->inode, get_dtype(sb, n->file_type))) {
            filp->f_pos = pos;
            info->extra_fname = n;
            return 1;
        }
    }
    return 0;
}

int ext3_dx_readdir(struct file *filp, filldir_t filldir, void *context)
{
    struct dir_private_info *info = filp->private_data;
    struct inode            *dir = filp->f_dentry->d_inode;
    PEXT2_FILLDIR_CONTEXT    fc = context;
    struct fname            *n;
    int                      rc;

    if (!info) {
        info = Ext2AllocatePool(PagedPool, sizeof(*info), HT_TAG);
        if (!info)
            return -ENOMEM;
        RtlZeroMemory(info, sizeof(*info));
        info->curr_hash = HT_HASH(filp->f_pos);
        filp->private_data = info;
    }
    if (filp->f_pos == EXT3_HTREE_EOF)
        return 0;

    /* the position moved since the last call: start again from it */
    if (info->last_pos != filp->f_pos) {
        HtEmptyTree(info, FALSE);
        info->curr_node = NULL;
        info->extra_fname = NULL;
        info->curr_hash = HT_HASH(filp->f_pos);
        info->curr_minor_hash = 0;
    }

    /* the rest of a chain a full buffer cut */
    if (info->extra_fname) {
        if (HtHandOut(filp, context, filldir, info->extra_fname))
            goto done;
        info->extra_fname = NULL;
        goto next;
    }
    if (!info->curr_node)
        info->curr_node = rb_first(&info->root);

    for (;;) {
        /* no names left, or the directory changed: the tree again */
        if (!info->curr_node || filp->f_version != dir->i_version) {
            info->curr_node = NULL;
            HtEmptyTree(info, FALSE);
            filp->f_version = dir->i_version;
            rc = HtFillTree(fc->efc_irp, filp, info->curr_hash, info->curr_minor_hash, &info->next_hash);
            if (rc < 0)
                return rc;
            if (rc == 0) {
                filp->f_pos = EXT3_HTREE_EOF;
                break;
            }
            info->curr_node = rb_first(&info->root);
        }
        n = rb_entry(info->curr_node, struct fname, rb_hash);
        info->curr_hash = n->hash;
        info->curr_minor_hash = n->minor_hash;
        if (HtHandOut(filp, context, filldir, n))
            break;
next:
        info->curr_node = rb_next(info->curr_node);
        if (info->curr_node) {
            n = rb_entry(info->curr_node, struct fname, rb_hash);
            info->curr_hash = n->hash;
            info->curr_minor_hash = n->minor_hash;
        } else {
            if (info->next_hash == HT_NO_MORE) {
                filp->f_pos = EXT3_HTREE_EOF;
                break;
            }
            info->curr_hash = info->next_hash;
            info->curr_minor_hash = 0;
        }
    }
done:
    info->last_pos = filp->f_pos;
    return 0;
}
