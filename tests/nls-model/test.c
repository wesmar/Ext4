/* SPDX-License-Identifier: GPL-2.0-only
 * Compare every conversion result, error and output byte against the original
 * production implementation; benchmark separately from correctness checks.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "linux/module.h"
#include "../../src/nls/CharsetMap.h"
#include "profiles.h"

static unsigned long long Checks;
static volatile unsigned long long Sink;
void VerifyDentry(void);

static double NowMilliseconds(void)
{
    LARGE_INTEGER count, frequency;
    if (!QueryPerformanceCounter(&count) || !QueryPerformanceFrequency(&frequency)) exit(2);
    return 1000.0 * (double)count.QuadPart / (double)frequency.QuadPart;
}

static void Check(int yes, const char *page, const char *operation, unsigned input, int bound)
{
    Checks++;
    if (!yes) {
        fprintf(stderr, "FAIL: %s %s input=%04x bound=%d\n", page, operation, input, bound);
        exit(1);
    }
}

static void Verify(const PROFILE *profile)
{
    const struct nls_table *old = profile->Before();
    const struct nls_table *now = profile->After();
    static const int bounds[] = {-1, 0, 1, 2, 3};
    unsigned input, b;
    Check(sizeof(wchar_t) == 2 && !strcmp(old->charset, now->charset), profile->Name, "registration", 0, 0);
    Check((!old->alias && !now->alias) || (old->alias && now->alias && !strcmp(old->alias, now->alias)), profile->Name, "alias", 0, 0);
    Check(!memcmp(old->charset2lower, now->charset2lower, 256) &&
          !memcmp(old->charset2upper, now->charset2upper, 256), profile->Name, "case maps", 0, 0);
    for (input = 0; input < 65536; input++) {
        for (b = 0; b < sizeof(bounds)/sizeof(bounds[0]); b++) {
            unsigned char before[8], after[8];
            int a, c;
            memset(before, 0xa5, sizeof(before));
            memset(after, 0xa5, sizeof(after));
            a = old->uni2char((wchar_t)input, before, bounds[b]);
            c = now->uni2char((wchar_t)input, after, bounds[b]);
            Check(a == c && !memcmp(before, after, sizeof(before)), profile->Name, "encode", input, bounds[b]);
        }
    }
    /* Every two-byte input, including single-byte prefixes and malformed pairs. */
    for (input = 0; input < 65536; input++) {
        unsigned char raw[2] = {(unsigned char)(input >> 8), (unsigned char)input};
        for (b = 0; b < sizeof(bounds)/sizeof(bounds[0]); b++) {
            wchar_t before = 0xface, after = 0xface;
            int a = old->char2uni(raw, bounds[b], &before);
            int c = now->char2uni(raw, bounds[b], &after);
            Check(a == c && before == after, profile->Name, "decode", input, bounds[b]);
        }
    }
    printf("%s: every BMP encode and byte-pair decode, bounds -1/0/1/2/3: passed\n", profile->Name);
}

typedef struct { wchar_t Unicode; unsigned char Raw[2]; } PROBE;

static double TimeEncode(const struct nls_table *table, const PROBE *probes, unsigned count)
{
    unsigned char out[8] = {0};
    unsigned long long sum = 0;
    double begin = NowMilliseconds();
    for (unsigned repeat = 0; repeat < 800; repeat++)
        for (unsigned i = 0; i < count; i++) {
            int rc = table->uni2char(probes[i].Unicode, out, 3);
            sum += (unsigned)(rc + 128) + out[0] + out[1];
        }
    Sink += sum;
    return NowMilliseconds() - begin;
}

static double TimeDecode(const struct nls_table *table, const PROBE *probes, unsigned count)
{
    wchar_t out = 0;
    unsigned long long sum = 0;
    double begin = NowMilliseconds();
    for (unsigned repeat = 0; repeat < 800; repeat++)
        for (unsigned i = 0; i < count; i++) {
            int rc = table->char2uni(probes[i].Raw, 2, &out);
            sum += (unsigned)(rc + 128) + out;
        }
    Sink += sum;
    return NowMilliseconds() - begin;
}

static void Benchmark(const PROFILE *profile)
{
    const struct nls_table *old = profile->Before(), *now = profile->After();
    PROBE probes[4096];
    unsigned count = 0;
    /* Valid non-ASCII names: evenly sampled representable BMP characters. */
    for (unsigned u = 128; u < 65536 && count < 4096; u++) {
        unsigned char raw[8] = {0};
        if (old->uni2char((wchar_t)u, raw, 3) > 0) {
            probes[count].Unicode = (wchar_t)u;
            probes[count].Raw[0] = raw[0];
            probes[count++].Raw[1] = raw[1];
        }
    }
    if (!count) return;
    double oe = 0, ne = 0, od = 0, nd = 0;
    for (unsigned pass = 0; pass < 4; pass++) {
        if (pass & 1) {
            ne += TimeEncode(now, probes, count); oe += TimeEncode(old, probes, count);
            nd += TimeDecode(now, probes, count); od += TimeDecode(old, probes, count);
        } else {
            oe += TimeEncode(old, probes, count); ne += TimeEncode(now, probes, count);
            od += TimeDecode(old, probes, count); nd += TimeDecode(now, probes, count);
        }
    }
    printf("BENCH %s non-ASCII %u probes: encode %.1f -> %.1f ms; decode %.1f -> %.1f ms\n",
           profile->Name, count, oe/4, ne/4, od/4, nd/4);
    for (unsigned i = 0; i < 96; i++) {
        probes[i].Unicode = (wchar_t)(32 + i);
        probes[i].Raw[0] = (unsigned char)(32 + i); probes[i].Raw[1] = 0;
    }
    oe = TimeEncode(old, probes, 96); ne = TimeEncode(now, probes, 96);
    od = TimeDecode(old, probes, 96); nd = TimeDecode(now, probes, 96);
    printf("BENCH %s ASCII: encode %.3f -> %.3f ms; decode %.3f -> %.3f ms\n",
           profile->Name, oe, ne, od, nd);
}

static void ExportProbes(const PROFILE *profile)
{
    const struct nls_table *old = profile->Before();
    unsigned char invalid[8] = {0};
    unsigned emitted = 0;
    Check(old->uni2char((wchar_t)0xffff, invalid, 3) < 0,
          profile->Name, "guest unrepresentable-name fixture", 0xffff, 3);
    printf("PROBE %s", old->charset);
    /* Only exact round trips safe in a Windows filename. Keep aliases out:
     * their behavior is already covered by the exhaustive converter model.
     */
    for (unsigned u = 160; u < 65536 && emitted < 32; u++) {
        unsigned char raw[8] = {0};
        wchar_t decoded = 0;
        int length = old->uni2char((wchar_t)u, raw, 3);
        if (u >= 0xd800 && u <= 0xdfff) continue;
        if (length < 1 || length > 2 || raw[0] < 32 || raw[0] == 127 ||
            strchr("\\/:*?\"<>|", raw[0]) ||
            (length == 2 && (!raw[1] || strchr("\\/", raw[1])))) continue;
        if (old->char2uni(raw, length, &decoded) != length || decoded != u) continue;
        /* A standalone byte can be a lead byte when a name follows it. */
        raw[length] = '-';
        if (old->char2uni(raw, length + 1, &decoded) != length || decoded != u) continue;
        printf(" %04X", u);
        emitted++;
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--export-probes")) {
        for (unsigned i = 0; i < sizeof(Profiles)/sizeof(Profiles[0]); i++)
            ExportProbes(&Profiles[i]);
        return 0;
    }
    VerifyDentry();
    uint32_t random = UINT32_C(0x1234fedc);
    for (unsigned i = 0; i < 1000000; i++) {
        unsigned expected = 0;
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        for (uint32_t bits = random; bits; bits &= bits-1) expected++;
        Check(NlsMapRank(random) == expected, "rank", "popcount", random, 0);
    }
    for (unsigned i = 0; i < sizeof(Profiles)/sizeof(Profiles[0]); i++) {
        Verify(&Profiles[i]);
        if (argc > 1 && !strcmp(argv[1], "--bench")) Benchmark(&Profiles[i]);
    }
    printf("NLS-MODEL: %llu checks: ALL PASSED\n", Checks);
    return 0;
}
