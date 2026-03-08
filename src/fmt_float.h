/**
 * @file fmt_float.h
 * @brief Integer-only float-to-string formatting.
 *
 * ARM Compiler 6 default standard library may link a reduced printf that
 * silently drops the decimal portion of %f / %.Nf, causing diagnostic
 * output to be truncated.  This header provides ff() which formats a
 * float using only %u (always available) and manual digit extraction.
 *
 * Usage:
 *   snprintf(buf, sz, "val=%s", ff(3.14f, 4));  // → "val=3.1400"
 *
 * Up to 4 concurrent ff() results may be used in a single expression
 * (4 rotating static buffers).  NOT thread-safe.
 */
#ifndef FMT_FLOAT_H
#define FMT_FLOAT_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Format @p v with @p prec decimal digits.  Returns pointer to an
 * internal static buffer (one of 4 rotating slots).
 * @param v     Value to format (|v| < 4 000 000 000).
 * @param prec  Decimal places, 0-10.
 */
static inline const char* ff(float v, int prec)
{
    static char bufs[4][28];   /* sign + 10 int digits + '.' + 10 frac + '\0' */
    static int idx = 0;
    char *buf = bufs[idx++ & 3];
    char *p = buf;

    if (v < 0.0f) { *p++ = '-'; v = -v; }

    /* Use double arithmetic so that 10^prec multiplier is exact. */
    double m = 1.0;
    for (int i = 0; i < prec; i++) m *= 10.0;

    uint64_t sv = (uint64_t)((double)v * m + 0.5);
    uint64_t dv = (uint64_t)m;
    uint32_t ip = (uint32_t)(sv / dv);
    uint32_t fp = (uint32_t)(sv % dv);

    /* Integer part — %u is always supported */
    int n = snprintf(p, 12, "%u", (unsigned)ip);
    p += n;

    if (prec > 0)
    {
        *p++ = '.';
        /* Fractional digits with leading zeros */
        for (int d = prec - 1; d >= 0; d--)
        {
            uint32_t pw = 1;
            for (int j = 0; j < d; j++) pw *= 10;
            *p++ = (char)('0' + (int)((fp / pw) % 10));
        }
    }
    *p = '\0';
    return buf;
}

#ifdef __cplusplus
}
#endif

#endif /* FMT_FLOAT_H */
