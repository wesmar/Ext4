/**
 * nls_euc-jp.c - EUC-JP, converted through code page 932.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: MIT
 *
 * A character goes through Shift JIS (cp932) and then between Shift JIS and
 * EUC-JP as the OSF/JVC code set conversion specification between Japanese
 * EUC and Shift JIS lays them side by side:
 *   JIS X 0208, leads 81..9F, E0..EA  rows A1..FE, by row and cell
 *   half-width katakana A1..DF        SS2 and the same byte
 *   user rows F0..F4                  rows F5..FE
 *   user rows F5..F9                  SS3, rows F5..FE
 *   IBM extensions FA40..FC4B         EucJpTables.h: JIS X 0208, JIS X 0212
 *                                     or the G3 user rows F3, F4 after SS3
 * The NEC-selected rows ED, EE hold IBM extensions as well and are written as
 * those. The results, the errors for short bounds included, are those of the
 * converter the driver has always used (tests/nls-model.ps1).
 */

#include <linux/module.h>
#include <linux/errno.h>
#include "CharsetMap.h"

/* Names are converted at PASSIVE_LEVEL only: the tables live in PAGERO, pageable and read-only. */
#pragma const_seg("PAGERO")
#include "EucJpTables.h"
#pragma const_seg()

#define EUC_SS2         0x8e            /* single shift: half-width katakana */
#define EUC_SS3         0x8f            /* single shift: the G3 set */
#define SJIS_ROW        (0xfc - 0x40)   /* trail bytes of a Shift JIS lead, 7F left out */
#define IBM_ROW         (0xfd - 0x40)   /* the same with 7F, as EucJpFromIbm counts */
#define NEC_FIRST_IBM   (0x5c - 0x40)   /* ED40 holds the IBM character FA5C */

static struct nls_table *Sjis;

static int IsTrail(unsigned char byte)
{
    return byte >= 0x40 && byte <= 0xfc && byte != 0x7f;
}

static int IsEuc(unsigned char byte)
{
    return byte >= 0xa1 && byte <= 0xfe;
}

/* Shift JIS rows from lead `first` to EUC rows from `row`: two EUC rows a lead. */
static void SjisToEucRows(unsigned char lead, unsigned char low, unsigned first, unsigned row,
                          unsigned char *euc)
{
    euc[0] = (unsigned char)(row + (lead - first) * 2 + (low >= 0x9f));
    euc[1] = (unsigned char)(low >= 0x9f ? low + 2 : low + (low >= 0x7f ? 0x60 : 0x61));
}

static void EucToSjisRows(unsigned char high, unsigned char low, unsigned row, unsigned first,
                          unsigned char *sjis)
{
    sjis[0] = (unsigned char)(high / 2 + first - row / 2 - !(high & 1));
    sjis[1] = (unsigned char)(!(high & 1) ? low - 2 : low - (low >= 0xe0 ? 0x60 : 0x61));
}

/* The NEC-selected rows hold the IBM extensions FA5C.. in order; their last
 * cells are FA40..FA49, the not sign 81CA and FA55..FA57.
 */
static void NecToIbm(unsigned char *sjis)
{
    unsigned count, code = sjis[0] << 8 | sjis[1];

    if (sjis[0] < 0xed || sjis[0] > 0xee || !IsTrail(sjis[1]))
        return;
    if (code == 0xeef9) {
        sjis[0] = 0x81;
        sjis[1] = 0xca;
        return;
    }
    if (sjis[0] == 0xee && sjis[1] >= 0xef)
        count = code - (sjis[1] <= 0xf9 ? 0xeeef : 0xeeef - 10);
    else
        count = (sjis[0] - 0xed) * SJIS_ROW + sjis[1] - 0x40 + NEC_FIRST_IBM - (sjis[1] >= 0x7f);
    sjis[0] = (unsigned char)(0xfa + count / SJIS_ROW);
    sjis[1] = (unsigned char)(0x40 + count % SJIS_ROW);
    if (sjis[1] >= 0x7f)
        sjis[1]++;
}

static int uni2char(wchar_t uni, unsigned char *out, int boundlen)
{
    unsigned char lead, low;
    NLS_WORD euc;
    int n;

    if (!Sjis)
        return -EINVAL;
    n = Sjis->uni2char(uni, out, boundlen);
    if (n < 0)
        return n;
    if (n == 1) {
        if (out[0] < 0xa1 || out[0] > 0xdf)
            return 1;
        if (boundlen < 2)
            return -ENAMETOOLONG;
        out[1] = out[0];
        out[0] = EUC_SS2;
        return 2;
    }
    if (n != 2)
        return -EINVAL;
    NecToIbm(out);
    lead = out[0];
    low = out[1];
    if (!IsTrail(low))
        return -EINVAL;
    if (lead >= 0xf0 && lead <= 0xf4) {
        SjisToEucRows(lead, low, 0xf0, 0xf5, out);
        return 2;
    }
    if (lead >= 0xf5 && lead <= 0xf9) {
        if (boundlen < 3)
            return -ENAMETOOLONG;
        out[0] = EUC_SS3;
        SjisToEucRows(lead, low, 0xf5, 0xf5, out + 1);
        return 3;
    }
    if (lead >= 0xfa && lead <= 0xfc) {
        unsigned index = (lead - 0xfa) * IBM_ROW + low - 0x40;

        if (index >= sizeof(EucJpFromIbm) / sizeof(EucJpFromIbm[0]) || !EucJpFromIbm[index])
            return -EINVAL;
        euc = EucJpFromIbm[index];
        if (euc < 0x8080) {                     /* JIS X 0208 */
            out[0] = (unsigned char)(euc >> 8 | 0x80);
            out[1] = (unsigned char)(euc | 0x80);
            return 2;
        }
        if (boundlen < 3)
            return -ENAMETOOLONG;
        out[0] = EUC_SS3;
        out[1] = (unsigned char)(euc >> 8);
        out[2] = (unsigned char)euc;
        return 3;
    }
    if ((lead >= 0x81 && lead <= 0x9f) || (lead >= 0xe0 && lead <= 0xea)) {
        SjisToEucRows(lead, low, lead < 0xa0 ? 0x81 : 0xe0, lead < 0xa0 ? 0xa1 : 0xdf, out);
        return 2;
    }
    return -EINVAL;
}

/* A G3 code of an IBM extension: the user rows by position, JIS X 0212 by search. */
#define X0212_COUNT (sizeof(EucJpX0212ToIbm) / sizeof(EucJpX0212ToIbm[0]))
#define UPPER_COUNT (sizeof(EucJpUpperToIbm) / sizeof(EucJpUpperToIbm[0]))

static int G3ToIbm(unsigned char high, unsigned char low, unsigned char *sjis)
{
    unsigned code = high << 8 | low, first = 0, last = X0212_COUNT;
    int index = high == 0xf3 ? (int)code - 0xf3f3 : (int)code - 0xf4a1 + (0xf3fe - 0xf3f3 + 1);

    if (index >= 0 && index < (int)UPPER_COUNT) {
        sjis[0] = (unsigned char)(EucJpUpperToIbm[index] >> 8);
        sjis[1] = (unsigned char)EucJpUpperToIbm[index];
        return 1;
    }
    while (first < last) {
        unsigned middle = (first + last) / 2;

        if (EucJpX0212ToIbm[middle][0] < code)
            first = middle + 1;
        else
            last = middle;
    }
    if (first == X0212_COUNT || EucJpX0212ToIbm[first][0] != code)
        return 0;
    sjis[0] = (unsigned char)(EucJpX0212ToIbm[first][1] >> 8);
    sjis[1] = (unsigned char)EucJpX0212ToIbm[first][1];
    return 1;
}

/* Too short a sequence is an invalid one here, not a short bound. */
static int char2uni(const unsigned char *rawstring, int boundlen, wchar_t *uni)
{
    unsigned char sjis[2] = { rawstring[0], 0 };
    int length = 1, n;

    if (!Sjis)
        return -EINVAL;
    if (boundlen <= 0)
        return -ENAMETOOLONG;
    if (rawstring[0] == EUC_SS3) {
        if (boundlen < 3)
            return -EINVAL;
        length = 3;
        if (rawstring[1] >= 0xf5 && rawstring[1] <= 0xfe && IsEuc(rawstring[2]))
            EucToSjisRows(rawstring[1], rawstring[2], 0xf5, 0xf5, sjis);
        else if (!G3ToIbm(rawstring[1], rawstring[2], sjis))
            return -EINVAL;
    } else if (rawstring[0] > 0x7f) {
        if (boundlen < 2)
            return -EINVAL;
        length = 2;
        if (rawstring[0] == EUC_SS2 && rawstring[1] >= 0xa1 && rawstring[1] <= 0xdf)
            sjis[0] = rawstring[1];
        else if (rawstring[0] >= 0xf5 && IsEuc(rawstring[0]) && IsEuc(rawstring[1]))
            EucToSjisRows(rawstring[0], rawstring[1], 0xf5, 0xf0, sjis);
        else if (IsEuc(rawstring[0]) && IsEuc(rawstring[1])) {
            sjis[0] = (unsigned char)(((rawstring[0] - 0x5f) / 2) ^ 0xa0);
            sjis[1] = (unsigned char)(!(rawstring[0] & 1) ? rawstring[1] - 2 :
                                      rawstring[1] - (rawstring[1] < 0xe0 ? 0x61 : 0x60));
        } else
            return -EINVAL;
    }
    n = Sjis->char2uni(sjis, sizeof(sjis), uni);
    return n < 0 ? n : length;
}

static struct nls_table table = {
    .charset = "euc-jp",
    .alias = NULL,
    .uni2char = uni2char,
    .char2uni = char2uni,
    .owner = THIS_MODULE,
};

static int __init init_nls_euc_jp(void)
{
    Sjis = load_nls("cp932");
    return Sjis ? register_nls(&table) : -EINVAL;
}

static void __exit exit_nls_euc_jp(void)
{
    unregister_nls(&table);
    unload_nls(Sjis);
}

module_init(init_nls_euc_jp)
module_exit(exit_nls_euc_jp)
