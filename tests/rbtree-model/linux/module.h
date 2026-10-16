/* SPDX-License-Identifier: MIT */
/* rbtree.c in user mode: what it takes from the kernel headers */
#include <stdint.h>
#include <stddef.h>
typedef uintptr_t ULONG_PTR;
#define TRUE 1
#define FALSE 0
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#include "../../../src/include/linux/rbtree.h"
