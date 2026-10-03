/**
 * HtreeInsert.c - hashed directory (htree) insertion and leaf splitting.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"
#include "htree_internal.h"

#ifndef swap
#define swap(type, x, y) do { type z = x; x = y; y = z; } while (0)
#endif

static inline void dx_set_hash (struct dx_entry *entry, unsigned value)
{
    entry->hash = cpu_to_le32(value);
}

static inline void dx_set_count (struct dx_entry *entries, unsigned value)
{
    ((struct dx_countlimit *) entries)->count = cpu_to_le16(value);
}

static inline void dx_set_limit (struct dx_entry *entries, unsigned value)
{
    ((struct dx_countlimit *) entries)->limit = cpu_to_le16(value);
}

/*
 * Directory block splitting, compacting
 */

/*
 * Create map of hash values, offsets, and sizes, stored at end of block.
 * Returns number of entries mapped.
 */
static int dx_make_map (struct inode *dir, struct ext3_dir_entry_2 *de, int size,
                        struct dx_hash_info *hinfo, struct dx_map_entry *map_tail)
{
    int count = 0;
    char *base = (char *) de;
    struct dx_hash_info h = *hinfo;

    while ((char *) de < base + size)
    {
        if (de->name_len && de->inode) {
            ext4_dir_hash(dir, de->name, de->name_len, &h);
            map_tail--;
            map_tail->hash = h.hash;
            map_tail->offs = (u16) ((char *) de - base);
            map_tail->size = le16_to_cpu(de->rec_len);
            count++;
            cond_resched();
        }
        /* XXX: do we need to check rec_len == 0 case? -Chris */
        de = (struct ext3_dir_entry_2 *) ((char *) de + le16_to_cpu(de->rec_len));
    }
    return count;
}

/* Sort map by hash value */
static void dx_sort_map (struct dx_map_entry *map, unsigned count)
{
    struct dx_map_entry *p, *q, *top = map + count - 1;
    int more;
    /* Combsort until bubble sort doesn't suck */
    while (count > 2)
    {
        count = count*10/13;
        if (count - 9 < 2) /* 9, 10 -> 11 */
            count = 11;
        for (p = top, q = p - count; q >= map; p--, q--)
            if (p->hash < q->hash)
                swap(struct dx_map_entry, *p, *q);
    }
    /* Garden variety bubble sort */
    do {
        more = 0;
        q = top;
        while (q-- > map)
        {
            if (q[1].hash >= q[0].hash)
                continue;
            swap(struct dx_map_entry, *(q+1), *q);
            more = 1;
        }
    } while (more);
}

static void dx_insert_block(struct dx_frame *frame, u32 hash, u32 block)
{
    struct dx_entry *entries = frame->entries;
    struct dx_entry *old = frame->at, *new = old + 1;
    unsigned int count = dx_get_count(entries);

    ASSERT(count < dx_get_limit(entries));
    ASSERT(old < entries + count);
    memmove(new + 1, new, (char *)(entries + count) - (char *)(new));
    dx_set_hash(new, hash);
    dx_set_block(new, block);
    dx_set_count(entries, count + 1);
}

/*
 * Add an entry to an indexed directory. Returns 0 for success, or a
 * negative error value.
 *
 * As in Linux: when the leaf is full it is split; when the index block
 * above it is full too, the lowest index block that still has room above
 * it takes the split (a level is added under the root when none has: one
 * level without large_dir, two with it). Splitting an index block or
 * adding a level changes the path to the leaf, so the probe starts again.
 */
int ext3_dx_add_entry(struct ext2_icb *icb, struct dentry *dentry,
                      struct inode *inode)
{
    struct dx_frame frames[EXT4_HTREE_LEVEL], *frame;
    struct dx_entry *entries, *at;
    struct dx_hash_info hinfo;
    struct buffer_head * bh;
    struct inode *dir = dentry->d_parent->d_inode;
    struct super_block * sb = dir->i_sb;
    struct ext3_dir_entry_2 *de;
    int err, restart;

again:
    restart = 0;
    bh = NULL;
    frame = dx_probe(icb, dentry, NULL, &hinfo, frames, &err);
    if (!frame)
        return err;
    entries = frame->entries;
    at = frame->at;

    if (!(bh = ext3_bread(icb, dir, dx_get_block(frame->at), &err)))
        goto cleanup;

    err = add_dirent_to_buf(icb, dentry, inode, NULL, bh);
    if (err != -ENOSPC) {
        bh = NULL;
        goto cleanup;
    }
    err = 0;

    /* Block full, should compress but for now just split */
    dxtrace(printk("using %u of %u node entries\n",
                   dx_get_count(entries), dx_get_limit(entries)));
    /* Need to split index? */
    if (dx_get_count(entries) == dx_get_limit(entries)) {
        u32 newblock;
        int levels = (int)(frame - frames) + 1;
        unsigned icount;
        int add_level = 1;
        struct dx_entry *entries2;
        struct dx_node *node2;
        struct buffer_head *bh2;

        /* the lowest full index block whose parent has room splits */
        while (frame > frames) {
            if (dx_get_count((frame - 1)->entries) <
                dx_get_limit((frame - 1)->entries)) {
                add_level = 0;
                break;
            }
            frame--;
            at = frame->at;
            entries = frame->entries;
            restart = 1;
        }
        if (add_level && levels == ext4_dir_htree_level(sb)) {
            ext3_warning(sb, __FUNCTION__,
                         "Directory (ino: %lu) index full, reach max htree level :%d",
                         dir->i_ino, levels);
            err = -ENOSPC;
            goto cleanup;
        }
        icount = dx_get_count(entries);
        bh2 = ext3_append (icb, dir, &newblock, &err);
        if (!(bh2))
            goto cleanup;
        node2 = (struct dx_node *)(bh2->b_data);
        entries2 = node2->entries;
        memset(&node2->fake, 0, sizeof(struct fake_dirent));
        node2->fake.rec_len = cpu_to_le16(sb->s_blocksize);

        if (!add_level) {
            unsigned icount1 = icount/2, icount2 = icount - icount1;
            unsigned hash2 = dx_get_hash(entries + icount1);
            dxtrace(printk("Split index %i/%i\n", icount1, icount2));

            memcpy ((char *) entries2, (char *) (entries + icount1),
                    icount2 * sizeof(struct dx_entry));
            dx_set_count (entries, icount1);
            dx_set_count (entries2, icount2);
            dx_set_limit (entries2, dx_node_limit(dir));

            /* Which index block gets the new entry? */
            if ((unsigned int)(at - entries) >= icount1) {
                frame->at = at = at - entries - icount1 + entries2;
                frame->entries = entries = entries2;
                swap(struct buffer_head *, frame->bh, bh2);
            }
            dx_insert_block ((frame - 1), hash2, newblock);
            ext4_dx_csum_set(dir, (struct ext4_dir_entry *)bh2->b_data);
            mark_buffer_dirty(bh2);
            brelse (bh2);
            ext4_dx_csum_set(dir, (struct ext4_dir_entry *)(frame - 1)->bh->b_data);
            mark_buffer_dirty((frame - 1)->bh);
            ext4_dx_csum_set(dir, (struct ext4_dir_entry *)frame->bh->b_data);
            mark_buffer_dirty(frame->bh);
            if (restart)
                goto cleanup;
        } else {
            struct dx_root *dxroot;

            dxtrace(printk("Creating %d level index...\n", levels));
            memcpy((char *) entries2, (char *) entries,
                   icount * sizeof(struct dx_entry));
            dx_set_limit(entries2, dx_node_limit(dir));

            /* Set up root */
            dx_set_count(entries, 1);
            dx_set_block(entries + 0, newblock);
            dxroot = (struct dx_root *)frames[0].bh->b_data;
            dxroot->info.indirect_levels += 1;

            ext4_dx_csum_set(dir, (struct ext4_dir_entry *)frame->bh->b_data);
            mark_buffer_dirty(frame->bh);
            ext4_dx_csum_set(dir, (struct ext4_dir_entry *)bh2->b_data);
            mark_buffer_dirty(bh2);
            brelse(bh2);
            restart = 1;
            goto cleanup;
        }
    }
    de = do_split(icb, dir, &bh, frame, &hinfo, &err);
    if (!de)
        goto cleanup;
    err = add_dirent_to_buf(icb, dentry, inode, de, bh);
    bh = NULL;

cleanup:
    if (bh)
        brelse(bh);
    dx_release(frames);
    /* the path through the index changed: probe it again */
    if (restart && err == 0)
        goto again;
    return err;
}
/*
 * Move count entries from end of map between two memory locations.
 * Returns pointer to last entry moved.
 */
struct ext3_dir_entry_2 *
            dx_move_dirents(char *from, char *to, struct dx_map_entry *map, int count)
{
    unsigned rec_len = 0;

    while (count--) {
        struct ext3_dir_entry_2 *de = (struct ext3_dir_entry_2 *) (from + map->offs);
        rec_len = EXT3_DIR_REC_LEN(de->name_len);
        memcpy (to, de, rec_len);
        ((struct ext3_dir_entry_2 *) to)->rec_len =
            cpu_to_le16(rec_len);
        de->inode = 0;
        map++;
        to += rec_len;
    }
    return (struct ext3_dir_entry_2 *) (to - rec_len);
}

/*
 * Compact each dir entry in the range to the minimal rec_len.
 * Returns pointer to last entry in range.
 */
struct ext3_dir_entry_2* dx_pack_dirents(char *base, int size)
{
    struct ext3_dir_entry_2 *next, *to, *prev, *de = (struct ext3_dir_entry_2 *) base;
    unsigned rec_len = 0;

    prev = to = de;
    while ((char*)de < base + size) {
        next = (struct ext3_dir_entry_2 *) ((char *) de +
                                            le16_to_cpu(de->rec_len));
        if (de->inode && de->name_len) {
            rec_len = EXT3_DIR_REC_LEN(de->name_len);
            if (de > to)
                memmove(to, de, rec_len);
            to->rec_len = cpu_to_le16(rec_len);
            prev = to;
            to = (struct ext3_dir_entry_2 *) (((char *) to) + rec_len);
        }
        de = next;
    }
    return prev;
}

/*
 * Split a full leaf block to make room for a new dir entry.
 * Allocate a new block, and move entries so that they are approx. equally full.
 * Returns pointer to de in block into which the new entry will be inserted.
 */
struct ext3_dir_entry_2 *
            do_split(struct ext2_icb *icb, struct inode *dir,
                     struct buffer_head **bh,struct dx_frame *frame,
                     struct dx_hash_info *hinfo, int *error)
{
    unsigned blocksize = dir->i_sb->s_blocksize;
    unsigned count, continued;
    struct buffer_head *bh2;
    u32 newblock;
    u32 hash2;
    struct dx_map_entry *map;
    char *data1 = (*bh)->b_data, *data2;
    unsigned split, move, size;
    struct ext3_dir_entry_2 *de = NULL, *de2;
    struct ext4_dir_entry_tail *t;
    int	csum_size = 0;
    int i;

    if (ext4_has_metadata_csum(dir->i_sb))
        csum_size = sizeof(struct ext4_dir_entry_tail);

    bh2 = ext3_append (icb, dir, &newblock, error);
    if (!(bh2)) {
        brelse(*bh);
        *bh = NULL;
        goto errout;
    }

    data2 = bh2->b_data;

    /* create map in the end of data2 block */
    map = (struct dx_map_entry *) (data2 + blocksize);
    count = dx_make_map (dir, (struct ext3_dir_entry_2 *) data1,
                         blocksize, hinfo, map);
    map -= count;
    dx_sort_map (map, count);
    /* Split the existing block in the middle, size-wise */
    size = 0;
    move = 0;
    for (i = count-1; i >= 0; i--) {
        /* is more than half of this entry in 2nd half of the block? */
        if (size + map[i].size/2 > blocksize/2)
            break;
        size += map[i].size;
        move++;
    }
    /* map index at which we will split */
    split = count - move;
    hash2 = map[split].hash;
    continued = hash2 == map[split - 1].hash;
    dxtrace(printk("Split block %i at %x, %i/%i\n",
                   dx_get_block(frame->at), hash2, split, count-split));

    /* Fancy dance to stay within two buffers */
    de2 = dx_move_dirents(data1, data2, map + split, count - split);
    de = dx_pack_dirents(data1,blocksize);
    de->rec_len = cpu_to_le16(data1 + (blocksize - csum_size) - (char *) de);
    de2->rec_len = cpu_to_le16(data2 + (blocksize - csum_size) - (char *) de2);
    if (csum_size) {
        t = EXT4_DIRENT_TAIL(data1, blocksize);
        initialize_dirent_tail(t, blocksize);

        t = EXT4_DIRENT_TAIL(data2, blocksize);
        initialize_dirent_tail(t, blocksize);
    }
    dxtrace(dx_show_leaf (icb, hinfo, (struct ext3_dir_entry_2 *) data1, blocksize, 1));
    dxtrace(dx_show_leaf (icb, hinfo, (struct ext3_dir_entry_2 *) data2, blocksize, 1));

    /* Which block gets the new entry? */
    if (hinfo->hash >= hash2)
    {
        swap(struct buffer_head *, *bh, bh2);
        de = de2;
    }
    dx_insert_block (frame, hash2 + continued, newblock);
    ext4_dx_csum_set(dir, (struct ext4_dir_entry *)frame->bh->b_data);
    ext4_dirent_csum_set(dir, (struct ext4_dir_entry *)bh2->b_data);
    mark_buffer_dirty(bh2);
    mark_buffer_dirty(frame->bh);

    brelse (bh2);
    dxtrace(dx_show_index ("frame", frame->entries));
errout:
    return de;
}

/*
 * This converts a one block unindexed directory to a 3 block indexed
 * directory, and adds the dentry to the indexed directory.
 */
int make_indexed_dir(struct ext2_icb *icb, struct dentry *dentry,
                     struct inode *inode, struct buffer_head *bh)
{
    struct inode	*dir = dentry->d_parent->d_inode;
    const char	*name = dentry->d_name.name;
    int		namelen = dentry->d_name.len;
    struct buffer_head *bh2;
    struct dx_root	*root;
    struct dx_frame	frames[EXT4_HTREE_LEVEL] = {0}, *frame;
    struct dx_entry *entries;
    struct ext3_dir_entry_2	*de, *de2;
    char		*data1, *top;
    unsigned	len;
    int		retval;
    unsigned	blocksize;
    struct dx_hash_info hinfo;
    ext3_lblk_t  block;
    struct fake_dirent *fde;
    struct ext4_dir_entry_tail *t;
    int csum_size = 0;

    if (ext4_has_metadata_csum(inode->i_sb))
        csum_size = sizeof(struct ext4_dir_entry_tail);

    blocksize =  dir->i_sb->s_blocksize;
    dxtrace(printk("Creating index: inode %lu\n", dir->i_ino));

    root = (struct dx_root *) bh->b_data;

    /* The 0th block becomes the root, move the dirents out */
    fde = &root->dotdot;
    de = (struct ext3_dir_entry_2 *)((char *)fde +
                                     ext3_rec_len_from_disk(fde->rec_len));
    if ((char *) de >= (((char *) root) + blocksize)) {
        DEBUG(DL_ERR, (__FUNCTION__  ": invalid rec_len for '..' in inode %lu",
                       dir->i_ino));
        brelse(bh);
        return -EIO;
    }
    len = (unsigned int)((char *) root + (blocksize - csum_size) - (char *) de);

    /* Allocate new block for the 0th block's dirents */
    bh2 = ext3_append(icb, dir, &block, &retval);
    if (!(bh2)) {
        brelse(bh);
        return retval;
    }
    EXT3_I(dir)->i_flags |= EXT3_INDEX_FL;
    data1 = bh2->b_data;

    memcpy (data1, de, len);
    de = (struct ext3_dir_entry_2 *) data1;
    top = data1 + len;
    while ((char *)(de2 = ext3_next_entry(de)) < top)
        de = de2;
    de->rec_len = ext3_rec_len_to_disk((blocksize - csum_size) + (__u32)(data1 - (char *)de));

    if (csum_size) {
        t = EXT4_DIRENT_TAIL(data1, blocksize);
        initialize_dirent_tail(t, blocksize);
    }

    /* Initialize the root; the dot dirents already exist */
    de = (struct ext3_dir_entry_2 *) (&root->dotdot);
    de->rec_len = ext3_rec_len_to_disk(blocksize - EXT3_DIR_REC_LEN(2));
    memset (&root->info, 0, sizeof(root->info));
    root->info.info_length = sizeof(root->info);
    root->info.hash_version = (__u8)(EXT3_SB(dir->i_sb)->s_def_hash_version);
    entries = root->entries;
    dx_set_block(entries, 1);
    dx_set_count(entries, 1);
    dx_set_limit(entries, dx_root_limit(dir, sizeof(root->info)));

    /* Initialize as for dx_probe */
    hinfo.hash_version = root->info.hash_version;
    hinfo.seed = EXT3_SB(dir->i_sb)->s_hash_seed;
    ext4_dir_hash(dir, name, namelen, &hinfo);
    frame = frames;
    frame->entries = entries;
    frame->at = entries;
    frame->bh = bh;
    ext4_dx_csum_set(dir, (struct ext4_dir_entry *)bh->b_data);
    ext4_dirent_csum_set(dir, (struct ext4_dir_entry *)bh2->b_data);
    bh = bh2;
    /* bh and bh2 are to be marked as dirty in do_split */
    de = do_split(icb, dir, &bh, frame, &hinfo, &retval);
    dx_release (frames);
    if (!(de))
        return retval;

    return add_dirent_to_buf(icb, dentry, inode, de, bh);
}
