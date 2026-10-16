/* Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * The driver's red-black tree (src/linux/rbtree.c) against a sorted array:
 * every insertion order of up to 8 keys, each tree then emptied in three
 * orders; and long random runs of inserts and erases over a few thousand
 * keys. After every change the tree must keep the rules - black root, no
 * red child of a red node, one black height, parents that match - and walk
 * (rb_first, rb_next) exactly the keys the array holds, in order.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/linux/rbtree.c"

typedef struct { struct rb_node Node; int Key; int In; } ITEM;

#define KEYS 4096
static ITEM Items[KEYS];
static struct rb_root Root;
static unsigned Count, Checks;
static uint32_t RandomState;

static uint32_t Random(void)
{
    RandomState ^= RandomState << 13;
    RandomState ^= RandomState >> 17;
    RandomState ^= RandomState << 5;
    return RandomState;
}

static int Key(struct rb_node *n) { return rb_entry(n, ITEM, Node)->Key; }

static unsigned Rules(struct rb_node *n, struct rb_node *parent, int *last)
{
    unsigned left, right;
    if (n == NULL) return 1;
    assert(rb_parent(n) == parent);
    if (rb_is_red(n)) {
        assert(n->rb_left == NULL || rb_is_black(n->rb_left));
        assert(n->rb_right == NULL || rb_is_black(n->rb_right));
    }
    left = Rules(n->rb_left, n, last);
    assert(Key(n) > *last);
    *last = Key(n);
    right = Rules(n->rb_right, n, last);
    assert(left == right);
    return left + (rb_is_black(n) ? 1u : 0u);
}

static void Verify(int keys)
{
    struct rb_node *n;
    int last = -1, k = 0;
    unsigned seen = 0;
    if (Root.rb_node) assert(rb_is_black(Root.rb_node) && rb_parent(Root.rb_node) == NULL);
    Rules(Root.rb_node, NULL, &last);
    for (n = rb_first(&Root); n != NULL; n = rb_next(n)) {
        while (k < keys && !Items[k].In) k++;
        assert(k < keys && Key(n) == k);
        k++;
        seen++;
    }
    while (k < keys) assert(!Items[k++].In);
    assert(seen == Count);
    Checks++;
}

static void Insert(int key)
{
    struct rb_node **link = &Root.rb_node, *parent = NULL;
    assert(!Items[key].In);
    while (*link) {
        parent = *link;
        link = key < Key(parent) ? &parent->rb_left : &parent->rb_right;
    }
    Items[key].Key = key;
    rb_link_node(&Items[key].Node, parent, link);
    rb_insert_color(&Items[key].Node, &Root);
    Items[key].In = 1;
    Count++;
}

static int Compare(struct rb_node *a, struct rb_node *b) { return Key(a) - Key(b); }

static void InsertByCallback(int key)
{
    Items[key].Key = key;
    rb_insert(&Root, &Items[key].Node, Compare);
    Items[key].In = 1;
    Count++;
}

static void Erase(int key)
{
    assert(Items[key].In);
    rb_erase(&Items[key].Node, &Root);
    memset(&Items[key].Node, 0xcc, sizeof(Items[key].Node));
    Items[key].In = 0;
    Count--;
}

static int Next(int *a, int n)          /* the next permutation, in place */
{
    int i = n - 2, j = n - 1, t;
    while (i >= 0 && a[i] >= a[i + 1]) i--;
    if (i < 0) return 0;
    while (a[j] <= a[i]) j--;
    t = a[i]; a[i] = a[j]; a[j] = t;
    for (i++, j = n - 1; i < j; i++, j--) { t = a[i]; a[i] = a[j]; a[j] = t; }
    return 1;
}

int main(int argc, char **argv)
{
    unsigned trees = 0, round, op;
    int n, i, order[8];

    RandomState = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 10) : 0x2545f491u;
    if (!RandomState) RandomState = 1;

    for (n = 1; n <= 8; n++) {
        for (i = 0; i < n; i++) order[i] = i;
        do {
            int way;
            for (way = 0; way < 3; way++) {
                for (i = 0; i < n; i++) {
                    if (way == 2 && (i & 1)) InsertByCallback(order[i]); else Insert(order[i]);
                    Verify(n);
                }
                for (i = 0; i < n; i++) {
                    /* the same order, the reverse, and from the middle out */
                    int key = way == 0 ? order[i] : way == 1 ? order[n - 1 - i] : (order[i] + n / 2) % n;
                    Erase(key);
                    Verify(n);
                }
                assert(Root.rb_node == NULL && Count == 0);
                trees++;
            }
        } while (Next(order, n));
    }

    for (round = 0; round < 40; round++) {
        int keys = round < 20 ? 64 : KEYS;
        for (op = 0; op < 20000; op++) {
            int key = (int)(Random() % (uint32_t)keys);
            /* lean towards growing, then towards emptying */
            int grow = (Random() % 100) < (op < 10000 ? 70u : 30u);
            if (!Items[key].In && grow) Insert(key);
            else if (Items[key].In && !grow) Erase(key);
            else continue;
            if (keys == 64 || op % 97 == 0) Verify(keys);
        }
        for (i = 0; i < keys; i++) if (Items[i].In) { Erase(i); if (i % 31 == 0) Verify(keys); }
        Verify(keys);
        assert(Root.rb_node == NULL);
    }

    printf("RBTREE-MODEL: %u trees built and emptied, %u states verified: ALL PASSED\n", trees, Checks);
    return 0;
}
