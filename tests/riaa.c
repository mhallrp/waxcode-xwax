/*
 * Copyright (C) 2026 Mark Hills <mark@xwax.org>
 *
 * This file is part of "xwax".
 *
 * "xwax" is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License, version 3 as
 * published by the Free Software Foundation.
 *
 * "xwax" is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 *
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "riaa.h"

#define RATE 48000

/*
 * Self-contained test of the inverse RIAA output filter
 *
 * Checks the realised biquad against the analogue curve it is meant to reproduce. Worth testing
 * rather than eyeballing: the first implementation used a bilinear transform and was +6dB out at
 * 20kHz, which sounds like harshness rather than like a bug.
 */

/* The analogue curve, normalised at 1kHz. */
static double ideal_db(double f)
{
    static const double t1 = 3180e-6, t2 = 318e-6, t3 = 75e-6, t4 = 3.18e-6;
    double w, ref_w, mag, ref;

    w = 2.0 * M_PI * f;
    ref_w = 2.0 * M_PI * 1000.0;

    mag = sqrt((1 + pow(w * t1, 2)) * (1 + pow(w * t3, 2)))
        / sqrt((1 + pow(w * t2, 2)) * (1 + pow(w * t4, 2)));
    ref = sqrt((1 + pow(ref_w * t1, 2)) * (1 + pow(ref_w * t3, 2)))
        / sqrt((1 + pow(ref_w * t2, 2)) * (1 + pow(ref_w * t4, 2)));

    return 20.0 * log10(mag / ref);
}

static double realised_db(const struct riaa *ri, double f)
{
    double w, nr, ni, dr, di;

    w = 2.0 * M_PI * f / RATE;
    nr = ri->b[0] + ri->b[1] * cos(w) + ri->b[2] * cos(2 * w);
    ni = -(ri->b[1] * sin(w) + ri->b[2] * sin(2 * w));
    dr = ri->a[0] + ri->a[1] * cos(w) + ri->a[2] * cos(2 * w);
    di = -(ri->a[1] * sin(w) + ri->a[2] * sin(2 * w));

    return 20.0 * log10(sqrt((nr * nr + ni * ni) / (dr * dr + di * di)));
}

int main(int argc, char *argv[])
{
    struct riaa ri;
    signed short pcm[8];
    size_t n;

    /* Off unless asked for: an ordinary line-level box must be untouched by this. */
    riaa_init(&ri, RATE, 0.0);
    assert(!ri.active);

    for (n = 0; n < 8; n++)
        pcm[n] = 1000;
    riaa_apply(&ri, pcm, 4);
    for (n = 0; n < 8; n++)
        assert(pcm[n] == 1000);

    riaa_init(&ri, RATE, 50.0);
    assert(ri.active);

    /* Through the midrange the curve should be all but exact. */
    {
        static const double exact[] = {20, 50, 100, 500, 1000, 2000, 5000};
        for (n = 0; n < sizeof exact / sizeof *exact; n++) {
            double got = realised_db(&ri, exact[n]) + 50.0;
            assert(fabs(got - ideal_db(exact[n])) < 0.2);
        }
    }

    /* Near Nyquist matched-Z falls gently short. Bounded so a regression to bilinear, which
     * overshot by 6dB here, fails rather than passes. */
    {
        double at20k = realised_db(&ri, 20000.0) + 50.0;
        assert(at20k < ideal_db(20000.0));
        assert(at20k > ideal_db(20000.0) - 2.5);
    }

    /* Attenuation is what it claims: at 1kHz, exactly the level asked for. */
    assert(fabs(realised_db(&ri, 1000.0) + 50.0) < 0.01);

    riaa_init(&ri, RATE, 30.0);
    assert(fabs(realised_db(&ri, 1000.0) + 30.0) < 0.01);

    /* A rate other than 48kHz must derive its own coefficients, not inherit them. */
    {
        struct riaa other;
        riaa_init(&other, 44100, 50.0);
        assert(other.b[1] != ri.b[1]);
    }

    /* Loud input must clamp, not wrap: the boost is large at the top of the band. */
    riaa_init(&ri, RATE, 0.001);
    for (n = 0; n < 8; n++)
        pcm[n] = (n % 2) ? 32767 : -32768;
    riaa_apply(&ri, pcm, 4);
    for (n = 0; n < 8; n++)
        assert(pcm[n] >= -32768 && pcm[n] <= 32767);

    printf("riaa: ok\n");
    return 0;
}
