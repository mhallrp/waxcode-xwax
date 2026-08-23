/*
 * Inverse RIAA equalisation and attenuation for the playback output.
 *
 * The playback curve a phono stage applies is defined by three time constants - 3180us, 318us and
 * 75us. Undoing it in advance means applying their inverse, plus the 3.18us constant that bounds
 * the high-frequency boost (the "Neumann" pole); without that fourth term the response rises
 * without limit and the filter has more zeros than poles, which is not realisable.
 *
 *   H(s) = (1 + sT1)(1 + sT3) / ((1 + sT2)(1 + sT4))
 *
 * Second order over second order, so one biquad covers it. Coefficients are derived here from the
 * time constants rather than written out, so a rate other than 48kHz stays correct.
 *
 * Accuracy is limited by the bilinear transform's frequency warping near Nyquist - close to exact
 * through the midrange and a couple of dB down at the top of the band. That is the same trade the
 * previous implementation made, and it measured within 0.15dB to 5kHz.
 *
 * ATTENUATION IS THE LIMITING FACTOR, not the curve. Dropping a full-scale track to the few
 * millivolts a phono input expects costs roughly 50dB, and every dB of that is a dB of the output
 * word thrown away before the mixer amplifies it back. On a 16-bit path that leaves around 50dB of
 * signal-to-noise where a line output has 96dB. The arithmetic below is kept in double precision
 * so that raising the output path's bit depth later improves this without touching this file.
 */

#include <math.h>
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

/*
 * Pre: attenuate_db is zero (inactive) or positive (dB of attenuation)
 * Post: ri is ready for riaa_apply()
 */

void riaa_init(struct riaa *ri, unsigned int rate, double attenuate_db)
{
    double k, bs[3], as[3], scale;

    memset(ri, 0, sizeof *ri);

    if (attenuate_db <= 0.0)
        return;

    /* Expanded from the product of the two bracketed pairs above. */
    bs[0] = 1.0;
    bs[1] = T1 + T3;
    bs[2] = T1 * T3;
    as[0] = 1.0;
    as[1] = T2 + T4;
    as[2] = T2 * T4;

    /* Bilinear transform, s = k(1 - z^-1)/(1 + z^-1) */
    k = 2.0 * (double)rate;

    ri->b[0] = bs[0] + bs[1] * k + bs[2] * k * k;
    ri->b[1] = 2.0 * (bs[0] - bs[2] * k * k);
    ri->b[2] = bs[0] - bs[1] * k + bs[2] * k * k;
    ri->a[0] = as[0] + as[1] * k + as[2] * k * k;
    ri->a[1] = 2.0 * (as[0] - as[2] * k * k);
    ri->a[2] = as[0] - as[1] * k + as[2] * k * k;

    /* Normalise so a[0] is unity, which riaa_apply() assumes. */
    ri->b[0] /= ri->a[0];
    ri->b[1] /= ri->a[0];
    ri->b[2] /= ri->a[0];
    ri->a[1] /= ri->a[0];
    ri->a[2] /= ri->a[0];
    ri->a[0] = 1.0;

    /* 0dB at 1kHz, then the requested attenuation on top. */
    scale = pow(10.0, -attenuate_db / 20.0)
        / response_at(ri->b, ri->a, REFERENCE_HZ, rate);

    ri->b[0] *= scale;
    ri->b[1] *= scale;
    ri->b[2] *= scale;

    ri->active = true;
}

/*
 * Apply in place to interleaved stereo
 *
 * Pre: pcm holds frames stereo samples
 * Post: pcm is pre-emphasised and attenuated, if this filter is active
 */

void riaa_apply(struct riaa *ri, signed short *pcm, size_t frames)
{
    size_t n;
    unsigned int c;

    if (!ri->active)
        return;

    for (n = 0; n < frames; n++) {
        for (c = 0; c < RIAA_CHANNELS; c++) {
            double in, out;

            in = (double)pcm[n * RIAA_CHANNELS + c];
            out = ri->b[0] * in
                + ri->b[1] * ri->x[c][0]
                + ri->b[2] * ri->x[c][1]
                - ri->a[1] * ri->y[c][0]
                - ri->a[2] * ri->y[c][1];

            ri->x[c][1] = ri->x[c][0];
            ri->x[c][0] = in;
            ri->y[c][1] = ri->y[c][0];
            ri->y[c][0] = out;

            /* The boost is large at the top of the band, so a bright transient can exceed full
             * scale even after attenuation. Clamp rather than let it wrap. */
            if (out > 32767.0)
                out = 32767.0;
            else if (out < -32768.0)
                out = -32768.0;

            pcm[n * RIAA_CHANNELS + c] = (signed short)out;
        }
    }
}
