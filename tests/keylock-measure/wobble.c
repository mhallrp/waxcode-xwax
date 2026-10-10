/*
 * Warble measured as PITCH instability, in cents - which is what an ear calls warbling.
 *
 * The first warble metric measured amplitude modulation of a chord. It ranked the configurations in
 * the right order but could not be trusted to judge a candidate fix, because amplitude wobble is not
 * what anyone means by "warble". Phasiness moves the apparent FREQUENCY of a partial around, and the
 * ear is far more sensitive to that than to a level wobble.
 *
 * Quadrature demodulation: multiply the output by cos/sin at the nominal frequency, low-pass, and the
 * derivative of atan2 is the instantaneous frequency deviation. Exact for a steady tone, and the
 * result is in cents - a perceptual unit. Anything under ~5 cents is inaudible; 20+ is obvious.
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <rubberband/rubberband-c.h>

#define RATE 48000
#define BLOCK 512
#define LEN (RATE * 6)
#define F0 220.0

static void wobble(const char *name, const float *x, int n, double expected)
{
    /* Quadrature at the frequency the output SHOULD be, then the phase slope is the error. */
    double i_lp = 0.0, q_lp = 0.0, prev = 0.0;
    const double a = 0.002;            /* ~15Hz low-pass: keeps wobble, drops the 2*f0 image */
    int start = RATE, frames = 0;
    double sum = 0.0, sumsq = 0.0, worst = 0.0;

    for (int k = 0; k < n; k++) {
        double t = (double)k / RATE;
        i_lp += (x[k] * cos(2.0 * M_PI * expected * t) - i_lp) * a;
        q_lp += (x[k] * -sin(2.0 * M_PI * expected * t) - q_lp) * a;
        if (k < start) continue;

        /* Over a WINDOW, not per sample. A per-sample difference of a low-passed signal is dominated
         * by numerical noise, which the RATE/2pi scaling then multiplies by about 7600 - the first
         * version of this measured its own rounding error and reported 113 cents on a clean tone. */
        if ((k - start) % 2048 != 0) continue;
        double ph = atan2(q_lp, i_lp);
        double d = ph - prev;
        prev = ph;
        while (d > M_PI) d -= 2 * M_PI;
        while (d < -M_PI) d += 2 * M_PI;

        double hz = d * RATE / (2.0 * M_PI * 2048.0);
        double cents = 1200.0 * log2((expected + hz) / expected);
        if (!isfinite(cents) || fabs(cents) > 400) continue;
        sum += cents; sumsq += cents * cents; frames++;
        if (fabs(cents) > worst) worst = fabs(cents);
    }
    if (!frames) { printf("  %-16s (no signal)\n", name); return; }
    double mean = sum / frames;
    double sd = sqrt(sumsq / frames - mean * mean);
    printf("  %-16s wobble %6.2f cents rms   worst %6.1f cents\n", name, sd, worst);
}

int main(int argc, char **argv)
{
    double pitch = argc > 1 ? atof(argv[1]) : 1.08;
    double ratio = 1.0 / pitch;

    float *src = malloc(LEN * sizeof *src);
    for (int n = 0; n < LEN; n++) {
        double t = (double)n / RATE;
        /* A pure sine. Harmonics beat into the quadrature and swamp the thing being measured; the
         * question here is only how steady ONE partial is held. */
        src[n] = (float)(0.45 * sin(2 * M_PI * F0 * t));
    }

    /* Dry: resampled, so it plays at F0*pitch. That is the floor - any wobble here is the harness. */
    float *dry = calloc(LEN, sizeof *dry);
    for (int n = 0; n < LEN; n++) {
        double s = n * pitch; int i = (int)s; double f = s - i;
        dry[n] = (i + 1 < LEN) ? (float)(src[i] * (1 - f) + src[i + 1] * f) : 0.0f;
    }

    struct { const char *name; RubberBandOptions opts; } cfg[] = {
      { "R2+Crisp",  RubberBandOptionEngineFaster | RubberBandOptionChannelsTogether | RubberBandOptionWindowShort | RubberBandOptionTransientsCrisp },
      { "R2+Smooth", RubberBandOptionEngineFaster | RubberBandOptionChannelsTogether | RubberBandOptionWindowShort | RubberBandOptionTransientsSmooth },
      { "R3",        RubberBandOptionEngineFiner  | RubberBandOptionChannelsTogether },
    };

    printf("pitch %+.0f%%   (key lock holds pitch at %.1f Hz; dry plays at %.1f Hz)\n",
           (pitch - 1.0) * 100.0, F0, F0 * pitch);
    wobble("dry (no keylock)", dry, LEN, F0 * pitch);

    for (unsigned c = 0; c < sizeof cfg / sizeof *cfg; c++) {
        RubberBandState rb = rubberband_new(RATE, 1, RubberBandOptionProcessRealTime
            | RubberBandOptionThreadingNever | cfg[c].opts, ratio, 1.0);
        rubberband_set_time_ratio(rb, ratio);
        float *wet = calloc(LEN, sizeof *wet);
        int fed = 0, got = 0;
        while (fed < LEN && got < LEN) {
            const float *in[1] = { src + fed };
            unsigned want = (unsigned)(LEN - fed < BLOCK ? LEN - fed : BLOCK);
            rubberband_process(rb, in, want, 0); fed += want;
            int avail;
            while ((avail = rubberband_available(rb)) > 0 && got < LEN) {
                float *out[1] = { wet + got };
                got += rubberband_retrieve(rb, out, (unsigned)(avail > LEN - got ? LEN - got : avail));
            }
        }
        wobble(cfg[c].name, wet, got, F0);   /* key lock holds the ORIGINAL pitch */
        rubberband_delete(rb); free(wet);
    }
    return 0;
}
