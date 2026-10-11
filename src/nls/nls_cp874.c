/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * linux/fs/nls_cp874.c
 *
 * Charset cp874 translation tables.
 * Generated automatically from the Unicode and charset
 * tables from the Unicode Organization (www.unicode.org).
 * The Unicode to charset table has only exact mappings.
 */

#include <linux/module.h>
#include <linux/errno.h>
#include "CharsetMap.h"

/* Generated from the original mappings; all implicit zero slots are retained. */
static const NLS_WORD MapWords[13] = {
    0x2026, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
    0x00a0, 0x0000, 0x0000, 0x0000, 0x0000,
};
static const NLS_BYTE MapBytes[14] = {
    0xa0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x96, 0x97,
    0x91, 0x92, 0x93, 0x94, 0x95, 0x85,
};
static const NLS_MAP_EXCEPTIONS MapExceptions[10] = {
    { 0x00fe0020u, 0x00000000u, 0u },
    { 0x00000001u, 0x00010e00u, 8u },
    { 0x78000000u, 0x00010e20u, 9u },
    { 0xf0000000u, 0x00010e40u, 9u },
    { 0x00000001u, 0x00000000u, 0u },
    { 0x00000001u, 0x000100a0u, 1u },
    { 0x78000000u, 0x000100c0u, 2u },
    { 0xf0000000u, 0x000100e0u, 2u },
    { 0x33180000u, 0x00000000u, 6u },
    { 0x00000044u, 0x00000000u, 12u },
};

static const NLS_BITS charset2uni[8] = {
    0x80000000u, 0x80000020u, 0x80000040u, 0x80000060u, 0xc0000000u, 0xc0000001u, 0xc0000002u, 0xc0000003u,
};

static const NLS_BITS page00[8] = {
    0x80000000u, 0x80000020u, 0x80000040u, 0x80000060u, 0x40000000u, 0xc0000004u, 0x40000000u, 0x40000000u,
};

static const NLS_BITS page0e[8] = {
    0xc0000005u, 0xc0000006u, 0xc0000007u, 0x40000000u, 0x40000000u, 0x40000000u, 0x40000000u, 0x40000000u,
};

static const NLS_BITS page20[8] = {
    0xc0000008u, 0xc0000009u, 0x40000000u, 0x40000000u, 0x40000000u, 0x40000000u, 0x40000000u, 0x40000000u,
};

static const NLS_BITS *const page_uni2charset[256] = {
    page00, NULL,   NULL,   NULL,   NULL,   NULL,   NULL,   NULL,
    NULL,   NULL,   NULL,   NULL,   NULL,   NULL,   page0e, NULL,
    NULL,   NULL,   NULL,   NULL,   NULL,   NULL,   NULL,   NULL,
    NULL,   NULL,   NULL,   NULL,   NULL,   NULL,   NULL,   NULL,
    page20, NULL,   NULL,   NULL,   NULL,   NULL,   NULL,   NULL,
};

static const unsigned char charset2lower[256] = {
    NLS_ASCENDING_8(0x00),
    NLS_ASCENDING_8(0x08),
    NLS_ASCENDING_8(0x10),
    NLS_ASCENDING_8(0x18),
    NLS_ASCENDING_8(0x20),
    NLS_ASCENDING_8(0x28),
    NLS_ASCENDING_8(0x30),
    NLS_ASCENDING_8(0x38),
    0x40, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67,
    NLS_ASCENDING_8(0x68),
    NLS_ASCENDING_8(0x70),
    0x78, 0x79, 0x7a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f,
    NLS_ASCENDING_8(0x60),
    NLS_ASCENDING_8(0x68),
    NLS_ASCENDING_8(0x70),
    NLS_ASCENDING_8(0x78),
    0x00, 0x00, 0x00, 0x00, 0x00, 0x85, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    NLS_ASCENDING_8(0xa0),
    NLS_ASCENDING_8(0xa8),
    NLS_ASCENDING_8(0xb0),
    NLS_ASCENDING_8(0xb8),
    NLS_ASCENDING_8(0xc0),
    NLS_ASCENDING_8(0xc8),
    NLS_ASCENDING_8(0xd0),
    0xd8, 0xd9, 0xda, 0x00, 0x00, 0x00, 0x00, 0xdf,
    NLS_ASCENDING_8(0xe0),
    NLS_ASCENDING_8(0xe8),
    NLS_ASCENDING_8(0xf0),
    0xf8, 0xf9, 0xfa, 0xfb, 0x00, 0x00, 0x00, 0x00,
};

static const unsigned char charset2upper[256] = {
    NLS_ASCENDING_8(0x00),
    NLS_ASCENDING_8(0x08),
    NLS_ASCENDING_8(0x10),
    NLS_ASCENDING_8(0x18),
    NLS_ASCENDING_8(0x20),
    NLS_ASCENDING_8(0x28),
    NLS_ASCENDING_8(0x30),
    NLS_ASCENDING_8(0x38),
    NLS_ASCENDING_8(0x40),
    NLS_ASCENDING_8(0x48),
    NLS_ASCENDING_8(0x50),
    NLS_ASCENDING_8(0x58),
    0x60, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
    NLS_ASCENDING_8(0x48),
    NLS_ASCENDING_8(0x50),
    0x58, 0x59, 0x5a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x85, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    NLS_ASCENDING_8(0xa0),
    NLS_ASCENDING_8(0xa8),
    NLS_ASCENDING_8(0xb0),
    NLS_ASCENDING_8(0xb8),
    NLS_ASCENDING_8(0xc0),
    NLS_ASCENDING_8(0xc8),
    NLS_ASCENDING_8(0xd0),
    0xd8, 0xd9, 0xda, 0x00, 0x00, 0x00, 0x00, 0xdf,
    NLS_ASCENDING_8(0xe0),
    NLS_ASCENDING_8(0xe8),
    NLS_ASCENDING_8(0xf0),
    0xf8, 0xf9, 0xfa, 0xfb, 0x00, 0x00, 0x00, 0x00,
};

static int uni2char(wchar_t uni, unsigned char *out, int boundlen)
{
    const NLS_BITS *uni2charset;
    unsigned char cl = uni & 0x00ff;
    unsigned char ch = (uni & 0xff00) >> 8;

    if (boundlen <= 0)
        return -ENAMETOOLONG;

    /* Exact identity run from the original reverse map. */
    if (uni >= 1 && uni < 128) {
        out[0] = (unsigned char)uni;
        return 1;
    }

    uni2charset = page_uni2charset[ch];
    if (uni2charset && NlsMapByte(uni2charset, MapBytes, MapExceptions, cl))
        out[0] = NlsMapByte(uni2charset, MapBytes, MapExceptions, cl);
    else
        return -EINVAL;
    return 1;
}

static int char2uni(const unsigned char *rawstring, int boundlen, wchar_t *uni)
{
    UNREFERENCED_PARAMETER(boundlen);
    if (*rawstring < 0x80) {
        *uni = *rawstring;
    } else {
        *uni = NlsMapWord(charset2uni, MapWords, MapExceptions, *rawstring);
    }
    if (*uni == 0x0000)
        return -EINVAL;
    return 1;
}

static struct nls_table table = {
    /* .charset */ "cp874",
    /* .alias */ "tis-620",
    /* .uni2char */ uni2char,
    /* .char2uni */ char2uni,
    /* .charset2lower */ charset2lower,
    /* .charset2upper */ charset2upper,
    /* .owner */ THIS_MODULE,
};

static int __init init_nls_cp874(void)
{
    return register_nls(&table);
}

static void __exit exit_nls_cp874(void)
{
    unregister_nls(&table);
}

module_init(init_nls_cp874)
module_exit(exit_nls_cp874)

MODULE_LICENSE("Dual BSD/GPL");
MODULE_ALIAS_NLS(tis-620);
