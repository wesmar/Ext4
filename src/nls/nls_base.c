/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * linux/fs/nls_base.c
 *
 * Native language support--charsets and unicode translations.
 * By Gordon Chaffee 1996, 1997
 *
 * Unicode based case conversion 1999 by Wolfram Pienkoss
 *
 */

#include <linux/module.h>
#include <linux/config.h>
#include <linux/errno.h>

struct nls_table *tables;
spinlock_t nls_lock;

/*
 * Sample implementation from Unicode home page.
 * http://www.stonehand.com/unicode/standard/fss-utf.html
 */
struct utf8_table {
    int     cmask;
    int     cval;
    int     shift;
    long    lmask;
    long    lval;
};

static struct utf8_table utf8_table[] =
{
    {0x80,  0x00,   0*6,    0x7F,           0,         /* 1 byte sequence */},
    {0xE0,  0xC0,   1*6,    0x7FF,          0x80,      /* 2 byte sequence */},
    {0xF0,  0xE0,   2*6,    0xFFFF,         0x800,     /* 3 byte sequence */},
    {0xF8,  0xF0,   3*6,    0x1FFFFF,       0x10000,   /* 4 byte sequence */},
    {0xFC,  0xF8,   4*6,    0x3FFFFFF,      0x200000,  /* 5 byte sequence */},
    {0xFE,  0xFC,   5*6,    0x7FFFFFFF,     0x4000000, /* 6 byte sequence */},
    {0,						       /* end of table    */}
};

int
utf8_mbtowc(wchar_t *p, const __u8 *s, int n)
{
    long l;
    int c0, c, nc;
    struct utf8_table *t;

    nc = 0;
    c0 = *s;
    l = c0;
    for (t = utf8_table; t->cmask; t++) {
        nc++;
        if ((c0 & t->cmask) == t->cval) {
            l &= t->lmask;
            if (l < t->lval)
                return -1;
            *p = (wchar_t)l;
            return nc;
        }
        if (n <= nc)
            return -1;
        s++;
        c = (*s ^ 0x80) & 0xFF;
        if (c & 0xC0)
            return -1;
        l = (l << 6) | c;
    }
    return -1;
}

int
utf8_wctomb(__u8 *s, wchar_t wc, int maxlen)
{
    long l;
    int c, nc;
    struct utf8_table *t;

    if (s == 0)
        return 0;

    l = wc;
    nc = 0;
    for (t = utf8_table; t->cmask && maxlen; t++, maxlen--) {
        nc++;
        if (l <= t->lmask) {
            c = t->shift;
            *s = (__u8)(t->cval | (l >> c));
            while (c > 0) {
                c -= 6;
                s++;
                *s = 0x80 | (__u8)((l >> c) & 0x3F);
            }
            return nc;
        }
    }
    return -1;
}

int register_nls(struct nls_table * nls)
{
    struct nls_table ** tmp = &tables;

    if (!nls)
        return -EINVAL;
    if (nls->next)
        return -EBUSY;

    spin_lock(&nls_lock);
    while (*tmp) {
        if (nls == *tmp) {
            spin_unlock(&nls_lock);
            return -EBUSY;
        }
        tmp = &(*tmp)->next;
    }
    nls->next = tables;
    tables = nls;
    spin_unlock(&nls_lock);
    return 0;
}

int unregister_nls(struct nls_table * nls)
{
    struct nls_table ** tmp = &tables;

    spin_lock(&nls_lock);
    while (*tmp) {
        if (nls == *tmp) {
            *tmp = nls->next;
            spin_unlock(&nls_lock);
            return 0;
        }
        tmp = &(*tmp)->next;
    }
    spin_unlock(&nls_lock);
    return -EINVAL;
}

static struct nls_table *find_nls(char *charset)
{
    struct nls_table *nls;
    spin_lock(&nls_lock);
    for (nls = tables; nls; nls = nls->next) {
        if (!strcmp(nls->charset, charset))
            break;
        if (nls->alias && !strcmp(nls->alias, charset))
            break;
    }
    if (nls && !try_module_get(nls->owner))
        nls = NULL;
    spin_unlock(&nls_lock);
    return nls;
}

struct nls_table *load_nls(char *charset)
{
    /* the tables are built in: there is no module to load one from */
    return find_nls(charset);
}

void unload_nls(struct nls_table *nls)
{
    UNREFERENCED_PARAMETER(nls);
    module_put(nls->owner);
}

MODULE_LICENSE("Dual BSD/GPL");
