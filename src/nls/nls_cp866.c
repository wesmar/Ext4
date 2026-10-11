/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * linux/fs/nls_cp866.c
 *
 * Charset cp866 translation tables.
 * Generated automatically from the Unicode and charset
 * tables from the Unicode Organization (www.unicode.org).
 * The Unicode to charset table has only exact mappings.
 */

#include <linux/module.h>
#include <linux/errno.h>
#include "CharsetMap.h"

/* Generated from the original mappings; all implicit zero slots are retained. */
static const NLS_WORD MapWords[63] = {
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255d, 0x255c, 0x255b, 0x2510,
    0x2514, 0x2534, 0x252c, 0x251c, 0x2500, 0x253c, 0x255e, 0x255f,
    0x255a, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256c, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256b,
    0x256a, 0x2518, 0x250c, 0x2588, 0x2584, 0x258c, 0x2590, 0x2580,
    0x0401, 0x0404, 0x0454, 0x0407, 0x0457, 0x040e, 0x045e, 0x00b0,
    0x2219, 0x00b7, 0x221a, 0x2116, 0x00a4, 0x25a0, 0x00a0,
};
static const NLS_BYTE MapBytes[87] = {
    0xff, 0xfd, 0xf8, 0xfa, 0x00, 0xf0, 0x00, 0x00,
    0xf2, 0x00, 0x00, 0xf4, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xf6, 0x00, 0x00, 0x00, 0x00, 0xf3,
    0x00, 0x00, 0xf5, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xf7, 0x00, 0xfc, 0xf9, 0xfb, 0xc4, 0xb3,
    0xda, 0xbf, 0xc0, 0xd9, 0xc3, 0xb4, 0xc2, 0xc1,
    0xc5, 0xcd, 0xba, 0xd5, 0xd6, 0xc9, 0xb8, 0xb7,
    0xbb, 0xd4, 0xd3, 0xc8, 0xbe, 0xbd, 0xbc, 0xc6,
    0xc7, 0xcc, 0xb5, 0xb6, 0xb9, 0xd1, 0xd2, 0xcb,
    0xcf, 0xd0, 0xca, 0xd8, 0xd7, 0xce, 0xdf, 0xdc,
    0xdb, 0xdd, 0xde, 0xb0, 0xb1, 0xb2, 0xfe,
};
static const NLS_MAP_EXCEPTIONS MapExceptions[13] = {
    { 0xffff0000u, 0x00010430u, 0u },
    { 0xfffd0000u, 0x00010440u, 48u },
    { 0x00810011u, 0x00000000u, 0u },
    { 0x0000ffffu, 0x00010070u, 4u },
    { 0xfffd0000u, 0x000100e0u, 20u },
    { 0x00400000u, 0x00000000u, 35u },
    { 0x06000000u, 0x00000000u, 36u },
    { 0x11111005u, 0x00000000u, 38u },
    { 0x10101010u, 0x00000000u, 45u },
    { 0xffff0000u, 0x00000000u, 49u },
    { 0x00001fffu, 0x00000000u, 65u },
    { 0x000f1111u, 0x00000000u, 78u },
    { 0x00000001u, 0x00000000u, 86u },
};

static const NLS_BITS charset2uni[8] = {
    0x80000000u, 0x80000020u, 0x80000040u, 0x80000060u, 0x80000410u, 0xc0000000u, 0x00000010u, 0xc0000001u,
};

static const NLS_BITS page00[8] = {
    0x80000000u, 0x80000020u, 0x80000040u, 0x80000060u, 0x40000000u, 0xc0000002u, 0x40000000u, 0x40000000u,
};

static const NLS_BITS page04[8] = {
    0xc0000003u, 0x80000090u, 0xc0000004u, 0x40000000u, 0x40000000u, 0x40000000u, 0x40000000u, 0x40000000u,
};

static const NLS_BITS page21[8] = {
    0xc0000005u, 0x40000000u, 0x40000000u, 0x40000000u, 0x40000000u, 0x40000000u, 0x40000000u, 0x40000000u,
};

static const NLS_BITS page22[8] = {
    0xc0000006u, 0x40000000u, 0x40000000u, 0x40000000u, 0x40000000u, 0x40000000u, 0x40000000u, 0x40000000u,
};

static const NLS_BITS page25[8] = {
    0xc0000007u, 0xc0000008u, 0xc0000009u, 0xc000000au, 0xc000000bu, 0xc000000cu, 0x40000000u, 0x40000000u,
};

static const NLS_BITS *const page_uni2charset[256] = {
    page00, NULL,   NULL,   NULL,   page04, NULL,   NULL,   NULL,
    NULL,   NULL,   NULL,   NULL,   NULL,   NULL,   NULL,   NULL,
    NULL,   NULL,   NULL,   NULL,   NULL,   NULL,   NULL,   NULL,
    NULL,   NULL,   NULL,   NULL,   NULL,   NULL,   NULL,   NULL,
    NULL,   page21, page22, NULL,   NULL,   page25, NULL,   NULL,
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
    NLS_ASCENDING_8(0xa0),
    NLS_ASCENDING_8(0xa8),
    NLS_ASCENDING_8(0xe0),
    NLS_ASCENDING_8(0xe8),
    NLS_ASCENDING_8(0xa0),
    NLS_ASCENDING_8(0xa8),
    NLS_ASCENDING_8(0xb0),
    NLS_ASCENDING_8(0xb8),
    NLS_ASCENDING_8(0xc0),
    NLS_ASCENDING_8(0xc8),
    NLS_ASCENDING_8(0xd0),
    NLS_ASCENDING_8(0xd8),
    NLS_ASCENDING_8(0xe0),
    NLS_ASCENDING_8(0xe8),
    0xf1, 0xf1, 0xf3, 0xf3, 0xf5, 0xf5, 0xf7, 0xf7,
    NLS_ASCENDING_8(0xf8),
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
    NLS_ASCENDING_8(0x80),
    NLS_ASCENDING_8(0x88),
    NLS_ASCENDING_8(0x90),
    NLS_ASCENDING_8(0x98),
    NLS_ASCENDING_8(0x80),
    NLS_ASCENDING_8(0x88),
    NLS_ASCENDING_8(0xb0),
    NLS_ASCENDING_8(0xb8),
    NLS_ASCENDING_8(0xc0),
    NLS_ASCENDING_8(0xc8),
    NLS_ASCENDING_8(0xd0),
    NLS_ASCENDING_8(0xd8),
    NLS_ASCENDING_8(0x90),
    NLS_ASCENDING_8(0x98),
    0xf0, 0xf0, 0xf2, 0xf2, 0xf4, 0xf4, 0xf6, 0xf6,
    NLS_ASCENDING_8(0xf8),
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
    /* .charset */ "cp866",
    /* .alias */   0,
    /* .uni2char */ uni2char,
    /* .char2uni */ char2uni,
    /* .charset2lower */ charset2lower,
    /* .charset2upper */ charset2upper,
    /* .owner */ THIS_MODULE,
};

static int __init init_nls_cp866(void)
{
    return register_nls(&table);
}

static void __exit exit_nls_cp866(void)
{
    unregister_nls(&table);
}

module_init(init_nls_cp866)
module_exit(exit_nls_cp866)

MODULE_LICENSE("Dual BSD/GPL");
