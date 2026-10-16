/**
 * rbtree.h - an intrusive red-black tree, with the interface the ported
 * Linux code expects of <linux/rbtree.h>.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * A node lives inside the structure it orders; rb_entry gets back to that.
 * The caller owns the order: it walks down from rb_root.rb_node comparing
 * keys itself, links the new node into the empty slot it reached
 * (rb_link_node) and lets rb_insert_color restore the balance. rb_insert
 * does both with a comparison callback.
 *
 * A node's parent and colour share one word: nodes are at least 4-aligned,
 * so the low bit is free and marks a red node.
 */

#ifndef _LINUX_RBTREE_H
#define _LINUX_RBTREE_H

struct rb_node {
    ULONG_PTR       rb_parent_color;    /* parent | RB_RED_BIT */
    struct rb_node *rb_right;
    struct rb_node *rb_left;
};

struct rb_root {
    struct rb_node *rb_node;
};

#define RB_RED_BIT          ((ULONG_PTR)1)

#define rb_parent(n)        ((struct rb_node *)((n)->rb_parent_color & ~(ULONG_PTR)3))
#define rb_is_red(n)        (((n)->rb_parent_color & RB_RED_BIT) != 0)
#define rb_is_black(n)      (!rb_is_red(n))

#define rb_entry(ptr, type, member) container_of(ptr, type, member)

/* a new node: red, no children, in the slot *link below parent */
static __inline void rb_link_node(struct rb_node *node, struct rb_node *parent,
                                  struct rb_node **link)
{
    node->rb_parent_color = (ULONG_PTR)parent | RB_RED_BIT;
    node->rb_left = node->rb_right = NULL;
    *link = node;
}

void rb_insert_color(struct rb_node *node, struct rb_root *root);
void rb_erase(struct rb_node *node, struct rb_root *root);
struct rb_node *rb_first(struct rb_root *root);
struct rb_node *rb_next(struct rb_node *node);

/* link node by cmp (its result for node against a node in the tree), unless
   an equal node is there already */
void rb_insert(struct rb_root *root, struct rb_node *node,
               int (*cmp)(struct rb_node *, struct rb_node *));

#endif /* _LINUX_RBTREE_H */
