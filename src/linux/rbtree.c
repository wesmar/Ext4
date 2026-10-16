/**
 * rbtree.c - red-black tree balancing (include/linux/rbtree.h).
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * The rules kept: the root is black, a red node has no red child, and every
 * path from a node down to an empty slot meets as many black nodes. Each
 * repair is written once for both mirror images: "side" is 0 for left and 1
 * for right, and Slot(n, side) is n's child link on that side.
 * Tested by tests/rbtree-model.ps1 against a sorted array, rules checked
 * after every change.
 */

#include <linux/module.h>

#define LEFT    0
#define RIGHT   1

static __inline struct rb_node **Slot(struct rb_node *n, int side)
{
    return side == RIGHT ? &n->rb_right : &n->rb_left;
}

static __inline int IsRed(const struct rb_node *n)
{
    return n != NULL && rb_is_red(n);
}

static __inline void Paint(struct rb_node *n, int red)
{
    n->rb_parent_color = red ? (n->rb_parent_color | RB_RED_BIT)
                             : (n->rb_parent_color & ~RB_RED_BIT);
}

static __inline void SetParent(struct rb_node *n, struct rb_node *parent)
{
    n->rb_parent_color = (n->rb_parent_color & RB_RED_BIT) | (ULONG_PTR)parent;
}

/* the link that holds n: a slot of its parent, or the root */
static __inline struct rb_node **Holder(struct rb_node *n, struct rb_root *root)
{
    struct rb_node *parent = rb_parent(n);

    if (parent == NULL)
        return &root->rb_node;
    return parent->rb_left == n ? &parent->rb_left : &parent->rb_right;
}

/* n's child on side rises into n's place; n becomes its child on the other
   side, and takes over the grandchild that sat between them */
static void Rotate(struct rb_root *root, struct rb_node *n, int side)
{
    struct rb_node *up = *Slot(n, side);
    struct rb_node *between = *Slot(up, !side);

    *Holder(n, root) = up;
    SetParent(up, rb_parent(n));
    *Slot(up, !side) = n;
    SetParent(n, up);
    *Slot(n, side) = between;
    if (between != NULL)
        SetParent(between, n);
}

/*
 * node was just linked red. The only rule it can break is a red parent; the
 * fix either recolours and moves the question two levels up (red uncle) or
 * ends it with one or two rotations (black uncle).
 */
void rb_insert_color(struct rb_node *node, struct rb_root *root)
{
    for (;;) {
        struct rb_node *parent = rb_parent(node), *grand, *uncle;
        int             side;

        if (parent == NULL) {
            Paint(node, FALSE);             /* the root */
            return;
        }
        if (!IsRed(parent))
            return;

        /* a red parent is not the root: the grandparent exists */
        grand = rb_parent(parent);
        side = grand->rb_right == parent ? RIGHT : LEFT;
        uncle = *Slot(grand, !side);

        if (IsRed(uncle)) {
            Paint(parent, FALSE);
            Paint(uncle, FALSE);
            Paint(grand, TRUE);
            node = grand;
            continue;
        }

        /* an inner grandchild is turned into an outer one first */
        if (*Slot(parent, !side) == node) {
            Rotate(root, parent, !side);
            node = parent;
            parent = rb_parent(node);
        }
        Paint(parent, FALSE);
        Paint(grand, TRUE);
        Rotate(root, grand, side);
        return;
    }
}

/*
 * A black node left the tree: the paths through child (which took its place,
 * possibly empty) below parent are one black short. Push the shortage up
 * until a red node absorbs it or a rotation borrows a black from the sibling.
 */
static void EraseRepair(struct rb_root *root, struct rb_node *child, struct rb_node *parent)
{
    while (child != root->rb_node && !IsRed(child)) {

        /* the side is told by the sibling when child is an empty slot: a
           path one black short below a black node has a non-empty sibling */
        int             side = parent->rb_left == child ? LEFT : RIGHT;
        struct rb_node *sibling = *Slot(parent, !side);

        if (IsRed(sibling)) {
            /* make the sibling black: rotate the red one above parent */
            Paint(sibling, FALSE);
            Paint(parent, TRUE);
            Rotate(root, parent, !side);
            sibling = *Slot(parent, !side);
        }

        if (!IsRed(sibling->rb_left) && !IsRed(sibling->rb_right)) {
            /* the sibling's side gives up a black too: one level up */
            Paint(sibling, TRUE);
            child = parent;
            parent = rb_parent(child);
            continue;
        }

        if (!IsRed(*Slot(sibling, !side))) {
            /* only the inner nephew is red: make it the sibling */
            Paint(*Slot(sibling, side), FALSE);
            Paint(sibling, TRUE);
            Rotate(root, sibling, side);
            sibling = *Slot(parent, !side);
        }

        /* a red outer nephew: the sibling rises into parent's place and
           colour, parent and the nephew become black - the shortage is gone */
        Paint(sibling, IsRed(parent));
        Paint(parent, FALSE);
        Paint(*Slot(sibling, !side), FALSE);
        Rotate(root, parent, !side);
        child = root->rb_node;
        break;
    }
    if (child != NULL)
        Paint(child, FALSE);
}

void rb_erase(struct rb_node *node, struct rb_root *root)
{
    struct rb_node *child, *parent;
    int             black;

    if (node->rb_left != NULL && node->rb_right != NULL) {

        /* two children: the successor - leftmost on the right, with no left
           child - leaves its own place and takes node's, colour included;
           the shortage, if any, is where the successor was */
        struct rb_node *next = node->rb_right;

        while (next->rb_left != NULL)
            next = next->rb_left;
        black = !IsRed(next);
        child = next->rb_right;

        if (rb_parent(next) == node) {
            parent = next;
        } else {
            parent = rb_parent(next);
            parent->rb_left = child;
            if (child != NULL)
                SetParent(child, parent);
            next->rb_right = node->rb_right;
            SetParent(node->rb_right, next);
        }
        next->rb_left = node->rb_left;
        SetParent(node->rb_left, next);
        *Holder(node, root) = next;
        next->rb_parent_color = node->rb_parent_color;

    } else {

        /* at most one child: it takes node's place */
        child = node->rb_left != NULL ? node->rb_left : node->rb_right;
        parent = rb_parent(node);
        black = !IsRed(node);
        *Holder(node, root) = child;
        if (child != NULL)
            SetParent(child, parent);
    }

    if (black)
        EraseRepair(root, child, parent);
}

struct rb_node *rb_first(struct rb_root *root)
{
    struct rb_node *n = root->rb_node;

    if (n == NULL)
        return NULL;
    while (n->rb_left != NULL)
        n = n->rb_left;
    return n;
}

struct rb_node *rb_next(struct rb_node *node)
{
    struct rb_node *parent;

    /* the leftmost of the right subtree, or else the first ancestor that
       has node's subtree on its left */
    if (node->rb_right != NULL) {
        node = node->rb_right;
        while (node->rb_left != NULL)
            node = node->rb_left;
        return node;
    }
    while ((parent = rb_parent(node)) != NULL && parent->rb_right == node)
        node = parent;
    return parent;
}

void rb_insert(struct rb_root *root, struct rb_node *node,
               int (*cmp)(struct rb_node *, struct rb_node *))
{
    struct rb_node **link = &root->rb_node, *parent = NULL;

    while (*link != NULL) {
        int order = cmp(node, *link);

        if (order == 0)
            return;
        parent = *link;
        link = order < 0 ? &parent->rb_left : &parent->rb_right;
    }
    rb_link_node(node, parent, link);
    rb_insert_color(node, root);
}
