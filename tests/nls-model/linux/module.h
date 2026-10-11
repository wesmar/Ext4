/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef EXT4_NLS_MODEL_MODULE_H
#define EXT4_NLS_MODEL_MODULE_H
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#ifndef UNREFERENCED_PARAMETER
#define UNREFERENCED_PARAMETER(x) ((void)(x))
#endif
#define __init
#define __exit
#define THIS_MODULE NULL
struct module;
struct nls_table {
    char *charset;
    char *alias;
    int (*uni2char)(wchar_t, unsigned char *, int);
    int (*char2uni)(const unsigned char *, int, wchar_t *);
    const unsigned char *charset2lower;
    const unsigned char *charset2upper;
    struct module *owner;
    struct nls_table *next;
};
static inline int register_nls(struct nls_table *nls) { return nls ? 0 : -EINVAL; }
static inline int unregister_nls(struct nls_table *nls) { return nls ? 0 : -EINVAL; }
#define module_init(fn) \
    const struct nls_table *MODEL_GETTER(void) { (void)fn(); return &table; }
#define module_exit(fn) void MODEL_EXIT(void) { fn(); }
#define MODULE_LICENSE(x)
#define MODULE_ALIAS_NLS(x)
#endif
