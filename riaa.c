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
 * Matched-Z rather than the bilinear transform, which was tried first and rejected: bilinear warps
 * frequency near Nyquist and put the response +6dB out at 20kHz, which the mixer's phono stage
 * would hand straight back as harshness. Matched-Z places every pole and zero at exactly its own
 * frequency; what remains is a gentle shortfall at the very top (-0.5dB at 10k, -2dB at 20k),
 * where a record carries around 35dB less energy than at 1kHz anyway.
 *
 * ATTENUATION IS THE LIMITING FACTOR, not the curve. Dropping a full-scale track to the few
 * millivolts a phono input expects costs roughly 50dB, and every dB of that is a dB of the output
 * word thrown away before the mixer amplifies it back. On a 16-bit path that leaves around 50dB of
 * signal-to-noise where a line output has 96dB. The arithmetic below is kept in double precision
 * so that raising the output path's bit depth later improves this without touching this file.
 *
 * Because so few bits survive, how the result is quantised matters more here than it would at full
 * level, and the first version got it wrong in a way that was audible as crackle on quiet passages:
 * a bare cast truncates toward zero, which puts a deadband around silence. Rounded and dithered
 * now. Dither trades a little noise for the removal of quantisation DISTORTION, which is the right
 * trade at 7-odd effective bits - distortion tracks the signal and is heard, where noise does not.
 */

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

/*
 * Pre: attenuate_db is zero (inactive) or positive (dB of attenuation)
 * Post: ri is ready for riaa_apply()
 */

void riaa_init(struct riaa *ri, unsigned int rate, double attenuate_db)
{
    double z1, z3, p2, p4, scale;

    memset(ri, 0, sizeof *ri);

    if (attenuate_db <= 0.0)
        return;

    /* Matched-Z: each analogue pole and zero maps to z = e^(-1/(T*rate)), landing at exactly the
     * frequency its time constant describes. T4's corner sits above Nyquist at this rate, which
     * costs nothing - its only job is to bound a boost that is already bounded by the band. */
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

    /* Distinct seeds: identical dither on both channels would correlate into the centre of the
     * image rather than spreading, which is audible as a change in width on quiet passages. */
    ri->dither[0] = 0x9e3779b9u;
    ri->dither[1] = 0x85ebca6bu;

    ri->active = true;
}

/*
 * Apply in place to interleaved stereo
 *
 * Pre: pcm holds frames stereo samples
 * Post: pcm is pre-emphasised and attenuated, if this filter is active
 */

/*
 * Uniform random in [0,1), from a cheap xorshift.
 *
 * In the audio thread, so it must not allocate, lock, or call into libc's rand(), which is neither
 * fast nor reentrant. Quality beyond "no audible pattern" is not required of dither.
 */
static double dither_uniform(uint32_t *state)
{
    uint32_t x = *state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;

    return (double)x / 4294967296.0;
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

            /* Feedback keeps the unclamped, undithered value: clamping into the state would
             * distort the filter itself rather than just its output, and dither fed back would
             * accumulate. Flushed to zero when it decays below audibility, because a denormal in
             * this loop costs far more than the sample is worth. */
            ri->y[c][0] = (out > -1e-20 && out < 1e-20) ? 0.0 : out;

            /* TPDF: two uniforms summed, spanning one LSB either side. Triangular rather than
             * rectangular so the noise floor stops depending on the signal. */
            dithered = out + (dither_uniform(&ri->dither[c]) + dither_uniform(&ri->dither[c]) - 1.0);

            /* The boost is large at the top of the band, so a bright transient can exceed full
             * scale even after attenuation. Clamp rather than let it wrap. */
            if (dithered > 32767.0)
                dithered = 32767.0;
            else if (dithered < -32768.0)
                dithered = -32768.0;

            /* Round, do NOT truncate. A cast rounds toward zero, which leaves a deadband around
             * silence - heard as crackle on quiet passages, and much worse here than usual because
             * attenuation leaves so few bits in play. */
            pcm[n * RIAA_CHANNELS + c] = (signed short)floor(dithered + 0.5);
        }
    }
}
