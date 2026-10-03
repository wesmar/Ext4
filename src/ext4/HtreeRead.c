/**
 * HtreeRead.c - hashed directory (htree) lookup and ordered readdir.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "htree_internal.h"

/*
 * These functions convert from the major/minor hash to an f_pos
 * value.
 *
 * Currently we only use major hash numer.  This is unfortunate, but
 * on 32-bit machines, the same VFS interface is used for lseek and
 * llseek, so if we use the 64 bit offset, then the 32-bit versions of
 * lseek/telldir/seekdir will blow out spectacularly, and from within
 * the ext2 low-level routine, we don't know if we're being called by
 * a 64-bit version of the system call or the 32-bit version of the
 * system call.  Worse yet, NFSv2 only allows for a 32-bit readdir
 * cookie.  Sigh.
 */
#define hash2pos(major, minor)	(major >> 1)
#define pos2maj_hash(pos)	((pos << 1) & 0xffffffff)
#define pos2min_hash(pos)	(0)

/*
 * This structure holds the nodes of the red-black tree used to store
 * the directory entry in hash order.
 */
struct fname {
    __u32		hash;
    __u32		minor_hash;
    struct rb_node	rb_hash;
    struct fname	*next;
    __u32		inode;
    __u8		name_len;
    __u8		file_type;
    char		name[0];
};

/*
 * This functoin implements a non-recursive way of freeing all of the
 * nodes in the red-black tree.
 */
static void free_rb_tree_fname(struct rb_root *root)
{
    struct rb_node	*n = root->rb_node;
    struct rb_node	*parent;
    struct fname	*fname;

    while (n) {
        /* Do the node's children first */
        if ((n)->rb_left) {
            n = n->rb_left;
            continue;
        }
        if (n->rb_right) {
            n = n->rb_right;
            continue;
        }
        /*
         * The node has no children; free it, and then zero
         * out parent's link to it.  Finally go to the
         * beginning of the loop and try to free the parent
         * node.
         */
        parent = rb_parent(n);
        fname = rb_entry(n, struct fname, rb_hash);
        while (fname) {
            struct fname * old = fname;
            fname = fname->next;
            kfree (old);
        }
        if (!parent)
            root->rb_node = NULL;
        else if (parent->rb_left == n)
            parent->rb_left = NULL;
        else if (parent->rb_right == n)
            parent->rb_right = NULL;
        n = parent;
    }
    root->rb_node = NULL;
}

static struct dir_private_info *create_dir_info(loff_t pos)
{
    struct dir_private_info *p;

    p = kmalloc(sizeof(struct dir_private_info), GFP_KERNEL);
    if (!p)
        return NULL;
    p->root.rb_node = NULL;
    p->curr_node = NULL;
    p->extra_fname = NULL;
    p->last_pos = 0;
    p->curr_hash = (__u32)pos2maj_hash(pos);
    p->curr_minor_hash = (__u32)pos2min_hash(pos);
    p->next_hash = 0;
    return p;
}

void ext3_htree_free_dir_info(struct dir_private_info *p)
{
    free_rb_tree_fname(&p->root);
    kfree(p);
}

/*
 * Given a directory entry, enter it into the fname rb tree.
 */
int ext3_htree_store_dirent(struct file *dir_file, __u32 hash,
                            __u32 minor_hash,
                            struct ext3_dir_entry_2 *dirent)
{
    struct rb_node **p, *parent = NULL;
    struct fname * fname, *new_fn;
    struct dir_private_info *info;
    int extra_data = 0;
    int len;

    info = (struct dir_private_info *) dir_file->private_data;
    p = &info->root.rb_node;

    /* Create and allocate the fname structure */
    if (dirent->file_type & EXT3_DIRENT_LUFID)
        extra_data = ext3_get_dirent_data_len(dirent);

    len = sizeof(struct fname) + dirent->name_len + extra_data;
    new_fn = kmalloc(len, GFP_KERNEL);
    if (!new_fn)
        return -ENOMEM;
    memset(new_fn, 0, len);
    new_fn->hash = hash;
    new_fn->minor_hash = minor_hash;
    new_fn->inode = le32_to_cpu(dirent->inode);
    new_fn->name_len = dirent->name_len;
    new_fn->file_type = dirent->file_type;
    memcpy(&new_fn->name[0], &dirent->name[0],
           dirent->name_len + extra_data);
    new_fn->name[dirent->name_len] = 0;

    while (*p) {
        parent = *p;
        fname = rb_entry(parent, struct fname, rb_hash);

        /*
         * If the hash and minor hash match up, then we put
         * them on a linked list.  This rarely happens...
         */
        if ((new_fn->hash == fname->hash) &&
                (new_fn->minor_hash == fname->minor_hash)) {
            new_fn->next = fname->next;
            fname->next = new_fn;
            return 0;
        }

        if (new_fn->hash < fname->hash)
            p = &(*p)->rb_left;
        else if (new_fn->hash > fname->hash)
            p = &(*p)->rb_right;
        else if (new_fn->minor_hash < fname->minor_hash)
            p = &(*p)->rb_left;
        else /* if (new_fn->minor_hash > fname->minor_hash) */
            p = &(*p)->rb_right;
    }

    rb_link_node(&new_fn->rb_hash, parent, p);
    rb_insert_color(&new_fn->rb_hash, &info->root);
    return 0;
}

/*
 * This is a helper function for ext3_dx_readdir.  It calls filldir
 * for all entres on the fname linked list.  (Normally there is only
 * one entry on the linked list, unless there are 62 bit hash collisions.)
 */
static int call_filldir(struct file * filp, void * cookie,
                        filldir_t filldir, struct fname *fname)
{
    struct dir_private_info *info = filp->private_data;
    loff_t	curr_pos;
    struct inode *inode = filp->f_dentry->d_inode;
    struct super_block * sb;
    int error;

    sb = inode->i_sb;

    if (!fname) {
        printk("call_filldir: called with null fname?!?\n");
        return 0;
    }
    curr_pos = hash2pos(fname->hash, fname->minor_hash);
    while (fname) {
        error = filldir(cookie, fname->name,
                        fname->name_len, (ULONG)curr_pos,
                        fname->inode, get_dtype(sb, fname->file_type));
        if (error) {
            filp->f_pos = curr_pos;
            info->extra_fname = fname;
            return error;
        }
        fname = fname->next;
    }
    return 0;
}

/*
 * Probe for a directory leaf block to search.
 *
 * dx_probe can return ERR_BAD_DX_DIR, which means there was a format
 * error in the directory index, and the caller should fall back to
 * searching the directory normally.  The callers of dx_probe **MUST**
 * check for this error code, and make sure it never gets reflected
 * back to userspace.
 */
struct dx_frame *
            dx_probe(struct ext2_icb *icb, struct dentry *dentry, struct inode *dir,
                     struct dx_hash_info *hinfo, struct dx_frame *frame_in, int *err)
{
    unsigned count, indirect;
    struct dx_entry *at, *entries, *p, *q, *m;
    struct dx_root *root;
    struct buffer_head *bh;
    struct dx_frame *frame = frame_in;
    u32 hash;

    /* every level unused until filled: dx_release stops at the first */
    memset(frame_in, 0, EXT4_HTREE_LEVEL * sizeof(frame_in[0]));
    if (dentry)
        dir = dentry->d_parent->d_inode;
    if (!(bh = ext3_bread (icb, dir, 0, err)))
        goto fail;
    root = (struct dx_root *) bh->b_data;
    if (root->info.hash_version != DX_HASH_TEA &&
            root->info.hash_version != DX_HASH_HALF_MD4 &&
            root->info.hash_version != DX_HASH_LEGACY) {
        ext3_warning(dir->i_sb, __FUNCTION__,
                     "Unrecognised inode hash code %d",
                     root->info.hash_version);
        __brelse(bh);
        *err = ERR_BAD_DX_DIR;
        goto fail;
    }
    hinfo->hash_version = root->info.hash_version;
    hinfo->seed = EXT3_SB(dir->i_sb)->s_hash_seed;
    if (dentry)
        ext4_dir_hash(dir, dentry->d_name.name, dentry->d_name.len, hinfo);
    hash = hinfo->hash;

    if (root->info.unused_flags & 1) {
        ext3_warning(dir->i_sb, __FUNCTION__,
                     "Unimplemented inode hash flags: %#06x",
                     root->info.unused_flags);
        brelse(bh);
        *err = ERR_BAD_DX_DIR;
        goto fail;
    }

    /* two levels below the root with large_dir, one without */
    if ((indirect = root->info.indirect_levels) >= (unsigned)ext4_dir_htree_level(dir->i_sb)) {
        ext3_warning(dir->i_sb, __FUNCTION__,
                     "Unimplemented inode hash depth: %#06x",
                     root->info.indirect_levels);
        brelse(bh);
        *err = ERR_BAD_DX_DIR;
        goto fail;
    }

    entries = (struct dx_entry *) (((char *)&root->info) +
                                   root->info.info_length);

    if (dx_get_limit(entries) != dx_root_limit(dir,
            root->info.info_length)) {
        ext3_warning(dir->i_sb, __FUNCTION__,
                     "dx entry: limit != root limit");
        brelse(bh);
        *err = ERR_BAD_DX_DIR;
        goto fail;
    }

    dxtrace(printk("Look up %x", hash));
    while (1)
    {
        count = dx_get_count(entries);
        if (!count || count > dx_get_limit(entries)) {
            ext3_warning(dir->i_sb, __FUNCTION__,
                         "dx entry: no count or count > limit");
            brelse(bh);
            *err = ERR_BAD_DX_DIR;
            goto fail2;
        }

        p = entries + 1;
        q = entries + count - 1;
        while (p <= q)
        {
            m = p + (q - p)/2;
            if (dx_get_hash(m) > hash)
                q = m - 1;
            else
                p = m + 1;
        }

        if (0) /* linear search cross check */
        {
            unsigned n = count - 1;
            at = entries;
            while (n--)
            {
                if (dx_get_hash(++at) > hash)
                {
                    at--;
                    break;
                }
            }
            ASSERT(at == p - 1);
        }

        at = p - 1;
        frame->bh = bh;
        frame->entries = entries;
        frame->at = at;
        if (!indirect--) return frame;
        if (!(bh = ext3_bread(icb, dir, dx_get_block(at), err)))
            goto fail2;
        at = entries = ((struct dx_node *) bh->b_data)->entries;
        if (dx_get_limit(entries) != dx_node_limit (dir)) {
            ext3_warning(dir->i_sb, __FUNCTION__,
                         "dx entry: limit != node limit");
            brelse(bh);
            *err = ERR_BAD_DX_DIR;
            goto fail2;
        }
        frame++;
        frame->bh = NULL;
    }
fail2:
    while (frame >= frame_in) {
        brelse(frame->bh);
        frame->bh = NULL;
        frame--;
    }
fail:
    if (*err == ERR_BAD_DX_DIR)
        ext3_warning(dir->i_sb, __FUNCTION__,
                     "Corrupt dir inode %ld, running e2fsck is "
                     "recommended.", dir->i_ino);
    return NULL;
}

void dx_release (struct dx_frame *frames)
{
    unsigned levels, i;

    if (frames[0].bh == NULL)
        return;

    /* read before the root's buffer may go */
    levels = ((struct dx_root *) frames[0].bh->b_data)->info.indirect_levels;
    for (i = 0; i <= levels && i < EXT4_HTREE_LEVEL; i++) {
        if (frames[i].bh == NULL)
            break;
        brelse(frames[i].bh);
        frames[i].bh = NULL;
    }
}

/*
 * This function increments the frame pointer to search the next leaf
 * block, and reads in the necessary intervening nodes if the search
 * should be necessary.  Whether or not the search is necessary is
 * controlled by the hash parameter.  If the hash value is even, then
 * the search is only continued if the next block starts with that
 * hash value.  This is used if we are searching for a specific file.
 *
 * If the hash value is HASH_NB_ALWAYS, then always go to the next block.
 *
 * This function returns 1 if the caller should continue to search,
 * or 0 if it should not.  If there is an error reading one of the
 * index blocks, it will a negative error code.
 *
 * If start_hash is non-null, it will be filled in with the starting
 * hash of the next page.
 */
int ext3_htree_next_block(struct ext2_icb *icb, struct inode *dir,
                          __u32 hash, struct dx_frame *frame,
                          struct dx_frame *frames, __u32 *start_hash)
{
    struct dx_frame *p;
    struct buffer_head *bh;
    int err, num_frames = 0;
    __u32 bhash;

    p = frame;
    /*
     * Find the next leaf page by incrementing the frame pointer.
     * If we run out of entries in the interior node, loop around and
     * increment pointer in the parent node.  When we break out of
     * this loop, num_frames indicates the number of interior
     * nodes need to be read.
     */
    while (1) {
        if (++(p->at) < p->entries + dx_get_count(p->entries))
            break;
        if (p == frames)
            return 0;
        num_frames++;
        p--;
    }

    /*
     * If the hash is 1, then continue only if the next page has a
     * continuation hash of any value.  This is used for readdir
     * handling.  Otherwise, check to see if the hash matches the
     * desired contiuation hash.  If it doesn't, return since
     * there's no point to read in the successive index pages.
     */
    bhash = dx_get_hash(p->at);
    if (start_hash)
        *start_hash = bhash;
    if ((hash & 1) == 0) {
        if ((bhash & ~1) != hash)
            return 0;
    }
    /*
     * If the hash is HASH_NB_ALWAYS, we always go to the next
     * block so no check is necessary
     */
    while (num_frames--) {
        if (!(bh = ext3_bread(icb, dir, dx_get_block(p->at), &err)))
            return err; /* Failure */
        p++;
        brelse (p->bh);
        p->bh = bh;
        p->at = p->entries = ((struct dx_node *) bh->b_data)->entries;
    }
    return 1;
}

/*
 * This function fills a red-black tree with information from a
 * directory block.  It returns the number directory entries loaded
 * into the tree.  If there is an error it is returned in err.
 */
int htree_dirblock_to_tree(struct ext2_icb *icb, struct file *dir_file,
                           struct inode *dir, int block,
                           struct dx_hash_info *hinfo,
                           __u32 start_hash, __u32 start_minor_hash)
{
    struct buffer_head *bh;
    struct ext3_dir_entry_2 *de, *top;
    int err, count = 0;

    dxtrace(printk("In htree dirblock_to_tree: block %d\n", block));
    if (!(bh = ext3_bread (icb, dir, block, &err)))
        return err;

    de = (struct ext3_dir_entry_2 *) bh->b_data;
    top = (struct ext3_dir_entry_2 *) ((char *) de +
                                       dir->i_sb->s_blocksize -
                                       EXT3_DIR_REC_LEN(0));
    for (; de < top; de = ext3_next_entry(de)) {
        if (!ext3_check_dir_entry("htree_dirblock_to_tree", dir, de, bh,
                                  ((unsigned long)block<<EXT3_BLOCK_SIZE_BITS(dir->i_sb))
                                  + (unsigned long)((char *)de - bh->b_data))) {
            /* On error, skip the f_pos to the next block. */
            dir_file->f_pos = (dir_file->f_pos |
                               (dir->i_sb->s_blocksize - 1)) + 1;
            brelse (bh);
            return count;
        }
        ext4_dir_hash(dir, de->name, de->name_len, hinfo);
        if ((hinfo->hash < start_hash) ||
                ((hinfo->hash == start_hash) &&
                 (hinfo->minor_hash < start_minor_hash)))
            continue;
        if (de->inode == 0)
            continue;
        if ((err = ext3_htree_store_dirent(dir_file,
                                           hinfo->hash, hinfo->minor_hash, de)) != 0) {
            brelse(bh);
            return err;
        }
        count++;
    }
    brelse(bh);
    return count;
}

/*
 * This function fills a red-black tree with information from a
 * directory.  We start scanning the directory in hash order, starting
 * at start_hash and start_minor_hash.
 *
 * This function returns the number of entries inserted into the tree,
 * or a negative error code.
 */
int ext3_htree_fill_tree(struct ext2_icb *icb, struct file *dir_file,
                         __u32 start_hash, __u32 start_minor_hash,
                         __u32 *next_hash)
{
    struct dx_hash_info hinfo;
    struct ext3_dir_entry_2 *de;
    struct dx_frame frames[EXT4_HTREE_LEVEL], *frame;
    int block, err = 0;
    struct inode *dir;
    int count = 0;
    int ret;
    __u32 hashval;

    dxtrace(printk("In htree_fill_tree, start hash: %x:%x\n", start_hash,
                   start_minor_hash));
    dir = dir_file->f_dentry->d_inode;
    if (!(EXT3_I(dir)->i_flags & EXT3_INDEX_FL)) {
        hinfo.hash_version = EXT3_SB(dir->i_sb)->s_def_hash_version;
        hinfo.seed = EXT3_SB(dir->i_sb)->s_hash_seed;
        count = htree_dirblock_to_tree(icb, dir_file, dir, 0, &hinfo,
                                       start_hash, start_minor_hash);
        *next_hash = ~0U;
        return count;
    }

    hinfo.hash = start_hash;
    hinfo.minor_hash = 0;
    frame = dx_probe(icb, NULL, dir_file->f_dentry->d_inode, &hinfo, frames, &err);
    if (!frame)
        return err;

    /* Add '.' and '..' from the htree header */
    if (!start_hash && !start_minor_hash) {
        de = (struct ext3_dir_entry_2 *) frames[0].bh->b_data;
        if ((err = ext3_htree_store_dirent(dir_file, 0, 0, de)) != 0)
            goto errout;
        count++;
    }
    if (start_hash < 2 || (start_hash ==2 && start_minor_hash==0)) {
        de = (struct ext3_dir_entry_2 *) frames[0].bh->b_data;
        de = ext3_next_entry(de);
        if ((err = ext3_htree_store_dirent(dir_file, 2, 0, de)) != 0)
            goto errout;
        count++;
    }

    while (1) {
        block = dx_get_block(frame->at);
        ret = htree_dirblock_to_tree(icb, dir_file, dir, block, &hinfo,
                                     start_hash, start_minor_hash);
        if (ret < 0) {
            err = ret;
            goto errout;
        }
        count += ret;
        hashval = ~0U;
        ret = ext3_htree_next_block(icb, dir, HASH_NB_ALWAYS,
                                    frame, frames, &hashval);
        *next_hash = hashval;
        if (ret < 0) {
            err = ret;
            goto errout;
        }
        /*
         * Stop if:  (a) there are no more entries, or
         * (b) we have inserted at least one entry and the
         * next hash value is not a continuation
         */
        if ((ret == 0) ||
                (count && ((hashval & 1) == 0)))
            break;
    }
    dx_release(frames);
    dxtrace(printk("Fill tree: returned %d entries, next hash: %x\n",
                   count, *next_hash));
    return count;
errout:
    dx_release(frames);

    return (err);
}

struct buffer_head *
            ext3_dx_find_entry(struct ext2_icb *icb, struct dentry *dentry,
                               struct ext3_dir_entry_2 **res_dir, int *err)
{
    struct super_block * sb;
    struct dx_hash_info	hinfo = {0};
    u32 hash;
    struct dx_frame frames[EXT4_HTREE_LEVEL], *frame;
    struct ext3_dir_entry_2 *de, *top;
    struct buffer_head *bh;
    unsigned long block;
    int retval;
    int namelen = dentry->d_name.len;
    const char *name = dentry->d_name.name;
    struct inode *dir = dentry->d_parent->d_inode;

    sb = dir->i_sb;
    /* NFS may look up ".." - look at dx_root directory block */
    if (namelen > 2 || name[0] != '.'||(name[1] != '.' && name[1] != '\0')) {
        if (!(frame = dx_probe(icb, dentry, NULL, &hinfo, frames, err)))
            return NULL;
    } else {
        frame = frames;
        frame->bh = NULL;			/* for dx_release() */
        frame->at = (struct dx_entry *)frames;	/* a one-entry frame pointing at block 0: a linear directory read through the htree code */
        dx_set_block(frame->at, 0);		/* dx_root block is 0 */
    }
    hash = hinfo.hash;
    do {
        block = dx_get_block(frame->at);
        if (!(bh = ext3_bread (icb, dir, block, err)))
            goto errout;
        de = (struct ext3_dir_entry_2 *) bh->b_data;
        top = (struct ext3_dir_entry_2 *) ((char *) de + sb->s_blocksize -
                                           EXT3_DIR_REC_LEN(0));
        for (; de < top; de = ext3_next_entry(de))
            if (ext4_match (dir, namelen, name, de)) {
                if (!ext3_check_dir_entry("ext3_find_entry",
                                          dir, de, bh,
                                          (block<<EXT3_BLOCK_SIZE_BITS(sb))
                                          + (unsigned long)((char *)de - bh->b_data))) {
                    brelse (bh);
                    goto errout;
                }
                *res_dir = de;
                dx_release (frames);
                return bh;
            }
        brelse (bh);
        /* Check to see if we should continue to search */
        retval = ext3_htree_next_block(icb, dir, hash, frame,
                                       frames, NULL);
        if (retval < 0) {
            ext3_warning(sb, __FUNCTION__,
                         "error reading index page in directory #%lu",
                         dir->i_ino);
            *err = retval;
            goto errout;
        }
    } while (retval == 1);

    *err = -ENOENT;
errout:
    dxtrace(printk("%s not found\n", name));
    dx_release (frames);
    return NULL;
}

int ext3_dx_readdir(struct file *filp, filldir_t filldir,
                    void * context)
{
    struct dir_private_info *info = filp->private_data;
    struct inode *inode = filp->f_dentry->d_inode;
    struct fname *fname;
    PEXT2_FILLDIR_CONTEXT fc = context;
    int	ret;

    if (!info) {
        info = create_dir_info(filp->f_pos);
        if (!info)
            return -ENOMEM;
        filp->private_data = info;
    }

    if (filp->f_pos == EXT3_HTREE_EOF)
        return 0;	/* EOF */

    /* Some one has messed with f_pos; reset the world */
    if (info->last_pos != filp->f_pos) {
        free_rb_tree_fname(&info->root);
        info->curr_node = NULL;
        info->extra_fname = NULL;
        info->curr_hash = (__u32)pos2maj_hash(filp->f_pos);
        info->curr_minor_hash = (__u32)pos2min_hash(filp->f_pos);
    }

    /*
     * If there are any leftover names on the hash collision
     * chain, return them first.
     */
    if (info->extra_fname) {
        if (call_filldir(filp, context, filldir, info->extra_fname))
            goto finished;
        info->extra_fname = NULL;
        goto next_node;
    } else if (!info->curr_node)
        info->curr_node = rb_first(&info->root);

    while (1) {
        /*
         * Fill the rbtree if we have no more entries,
         * or the inode has changed since we last read in the
         * cached entries.
         */
        if ((!info->curr_node) ||
                (filp->f_version != inode->i_version)) {
            info->curr_node = NULL;
            free_rb_tree_fname(&info->root);
            filp->f_version = inode->i_version;
            ret = ext3_htree_fill_tree(fc->efc_irp, filp, info->curr_hash,
                                       info->curr_minor_hash, &info->next_hash);
            if (ret < 0)
                return ret;
            if (ret == 0) {
                filp->f_pos = EXT3_HTREE_EOF;
                break;
            }
            info->curr_node = rb_first(&info->root);
        }

        fname = rb_entry(info->curr_node, struct fname, rb_hash);
        info->curr_hash = fname->hash;
        info->curr_minor_hash = fname->minor_hash;
        if (call_filldir(filp, context, filldir, fname))
            break;
next_node:
        info->curr_node = rb_next(info->curr_node);
        if (info->curr_node) {
            fname = rb_entry(info->curr_node, struct fname,
                             rb_hash);
            info->curr_hash = fname->hash;
            info->curr_minor_hash = fname->minor_hash;
        } else {
            if (info->next_hash == ~0) {
                filp->f_pos = EXT3_HTREE_EOF;
                break;
            }
            info->curr_hash = info->next_hash;
            info->curr_minor_hash = 0;
        }
    }
finished:
    info->last_pos = filp->f_pos;
    return 0;
}

int ext3_release_dir (struct inode * inode, struct file * filp)
{
    UNREFERENCED_PARAMETER(inode);
    if (filp->private_data) {
        ext3_htree_free_dir_info(filp->private_data);
        filp->private_data = NULL;
    }

    return 0;
}
