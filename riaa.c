/*
 * Copyright (C) 2026 Matt Hall <info@waxcode.co>
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

/** Inverse RIAA equalisation and attenuation for the playback output. */

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "riaa.h"

/* Seconds. T1/T2/T3 define the RIAA curve itself; T4 bounds the boost so the filter is realisable. */
#define T1 3180e-6
#define T2 318e-6
#define T3 75e-6
#define T4 3.18e-6

/* The curve is defined as 0dB at 1kHz, so normalise there and let attenuation be the only gain. */
#define REFERENCE_HZ 1000.0

/* Magnitude of the biquad at one frequency, for normalising. */
static double response_at(const double b[3], const double a[3], double hz, unsigned int rate)
{
    double w, cos1, sin1, cos2, sin2, nr, ni, dr, di;

    w = 2.0 * M_PI * hz / (double)rate;
    cos1 = cos(w);
    sin1 = sin(w);
    cos2 = cos(2.0 * w);
    sin2 = sin(2.0 * w);

    nr = b[0] + b[1] * cos1 + b[2] * cos2;
    ni = -(b[1] * sin1 + b[2] * sin2);
    dr = a[0] + a[1] * cos1 + a[2] * cos2;
    di = -(a[1] * sin1 + a[2] * sin2);

    return sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
}

/** attenuate_db is zero (inactive) or positive (dB); leaves ri ready for riaa_apply(). */

void riaa_init(struct riaa *ri, unsigned int rate, double attenuate_db)
{
    double z1, z3, p2, p4, scale;

    memset(ri, 0, sizeof *ri);

    if (attenuate_db <= 0.0)
        return;

    /** Matched-Z: each analogue pole and zero maps to z = e^(-1/(T*rate)), landing at exactly the frequency its time constant describes. */
    z1 = exp(-1.0 / (T1 * (double)rate));
    z3 = exp(-1.0 / (T3 * (double)rate));
    p2 = exp(-1.0 / (T2 * (double)rate));
    p4 = exp(-1.0 / (T4 * (double)rate));

    ri->b[0] = 1.0;
    ri->b[1] = -(z1 + z3);
    ri->b[2] = z1 * z3;
    ri->a[0] = 1.0;
    ri->a[1] = -(p2 + p4);
    ri->a[2] = p2 * p4;

    /* 0dB at 1kHz, then the requested attenuation on top. */
    scale = pow(10.0, -attenuate_db / 20.0)
        / response_at(ri->b, ri->a, REFERENCE_HZ, rate);

    ri->b[0] *= scale;
    ri->b[1] *= scale;
    ri->b[2] *= scale;

    /** Distinct seeds: identical dither on both channels would correlate into the centre of the image rather than spreading */
    ri->dither[0] = 0x9e3779b9u;
    ri->dither[1] = 0x85ebca6bu;

    ri->active = true;
}

/** Uniform random in [0,1), from a cheap xorshift. */
static double dither_uniform(uint32_t *state)
{
    uint32_t x = *state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;

    return (double)x / 4294967296.0;
}

/** Apply in place to interleaved stereo: pcm holds `frames` stereo samples, and comes back pre-emphasised and attenuated. */

void riaa_apply(struct riaa *ri, signed short *pcm, size_t frames)
{
    size_t n;
    unsigned int c;

    if (!ri->active)
        return;

    for (n = 0; n < frames; n++) {
        for (c = 0; c < RIAA_CHANNELS; c++) {
            double in, out, dithered;

            in = (double)pcm[n * RIAA_CHANNELS + c];
            out = ri->b[0] * in
                + ri->b[1] * ri->x[c][0]
                + ri->b[2] * ri->x[c][1]
                - ri->a[1] * ri->y[c][0]
                - ri->a[2] * ri->y[c][1];

            ri->x[c][1] = ri->x[c][0];
            ri->x[c][0] = in;
            ri->y[c][1] = ri->y[c][0];

            /** Feedback keeps the unclamped, undithered value: clamping into the state would distort the filter itself rather than just its output */
            ri->y[c][0] = (out > -1e-20 && out < 1e-20) ? 0.0 : out;

            /** TPDF: two uniforms summed, spanning one LSB either side. */
            dithered = out + (dither_uniform(&ri->dither[c]) + dither_uniform(&ri->dither[c]) - 1.0);

            /** The boost is large at the top of the band, so a bright transient can exceed full scale even after attenuation. */
            if (dithered > 32767.0)
                dithered = 32767.0;
            else if (dithered < -32768.0)
                dithered = -32768.0;

            /** Round, do NOT truncate. */
            pcm[n * RIAA_CHANNELS + c] = (signed short)floor(dithered + 0.5);
        }
    }
}
