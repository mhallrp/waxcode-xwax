/*
 * v2: adds PRE-ECHO, and runs on real music.
 *
 * v1 measured attack time, crest and peak on a synthetic kick and found only 0.3-0.6 dB of loss -
 * near the edge of audibility, which did not match the owner's report. The likely reason is that
 * those metrics cannot see the classic phase-vocoder artefact: a window is CENTRED on the frame, so a
 * transient is spread BACKWARDS as well as forwards. Energy arriving before the attack masks it, and
 * is heard as mud or loss of definition rather than as a slower attack.
 *
 * Reads raw mono f32 at 48k on stdin, so ffmpeg can hand it any track.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <rubberband/rubberband-c.h>

#define RATE 48000
#define BLOCK 512

static void envelope(const float *x, int n, float *env)
{
    double e = 0.0;
    for (int i = 0; i < n; i++) {
        double m = fabs(x[i]);
        e += (m - e) * (m > e ? 0.55 : 0.002);
        env[i] = (float)e;
    }
}

/* Onsets: a sharp rise well above the running level, spaced so one hit is not counted twice. */
static int onsets(const float *env, int n, int *out, int max)
{
    double slow = 0.0;
    int found = 0, last = -RATE;
    for (int i = 1; i < n && found < max; i++) {
        slow += (env[i] - slow) * 0.0006;
        if (env[i] > slow * 2.2 && env[i] > env[i - 1] * 1.04 && i - last > RATE / 8 && slow > 1e-4) {
            out[found++] = i;
            last = i;
        }
    }
    return found;
}

static void measure(const char *label, const float *x, int n)
{
    float *env = malloc(n * sizeof *env);
    envelope(x, n, env);

    int *hits = malloc(512 * sizeof *hits);
    int count = onsets(env, n, hits, 512);
    if (count == 0) { printf("  %-12s (no onsets found)\n", label); free(env); free(hits); return; }

    double attack = 0.0, pre = 0.0, crest = 0.0;
    int used = 0;

    for (int h = 0; h < count; h++) {
        int at = hits[h];
        int peak_at = at;
        int end = at + RATE / 20; if (end > n) end = n;
        for (int i = at; i < end; i++) if (env[i] > env[peak_at]) peak_at = i;
        double peak = env[peak_at];
        if (peak < 1e-3) continue;

        int lo = peak_at;
        while (lo > 0 && env[lo] > peak * 0.10) lo--;
        int hi = lo;
        while (hi < n && env[hi] < peak * 0.90) hi++;
        attack += (hi - lo) * 1000.0 / RATE;

        /* PRE-ECHO: energy in the 25ms before the attack starts, against the peak that follows.
         * A clean recording is near silence there; a smeared one is not. */
        int pfrom = lo - RATE / 40; if (pfrom < 0) pfrom = 0;
        double psum = 0.0; int pn = 0;
        for (int i = pfrom; i < lo; i++) { psum += (double)x[i] * x[i]; pn++; }
        double prms = pn ? sqrt(psum / pn) : 0.0;
        pre += 20.0 * log10(prms > 1e-9 ? prms / peak : 1e-9);

        double sum = 0.0; int cn = 0; double rawpk = 0.0;
        for (int i = lo; i < end; i++) { sum += (double)x[i] * x[i]; cn++; if (fabs(x[i]) > rawpk) rawpk = fabs(x[i]); }
        double rms = cn ? sqrt(sum / cn) : 0.0;
        crest += 20.0 * log10(rms > 0 ? rawpk / rms : 1.0);
        used++;
    }

    if (used) {
        printf("  %-12s attack %5.2f ms   crest %5.2f dB   pre-echo %6.1f dB   (%d hits)\n",
               label, attack / used, crest / used, pre / used, used);
    }
    free(env); free(hits);
}

int main(int argc, char **argv)
{
    double pitch = argc > 1 ? atof(argv[1]) : 1.08;
    double ratio = 1.0 / pitch;

    size_t cap = RATE * 60, n = 0;
    float *src = malloc(cap * sizeof *src);
    n = fread(src, sizeof *src, cap, stdin);
    if (n < RATE) { fprintf(stderr, "need at least a second of audio\n"); return 1; }

    float *dry = calloc(n, sizeof *dry);
    for (size_t i = 0; i < n; i++) {
        double s = i * pitch; size_t k = (size_t)s; double f = s - k;
        dry[i] = (k + 1 < n) ? (float)(src[k] * (1 - f) + src[k + 1] * f) : 0.0f;
    }

    /* Every configuration worth comparing, measured against the same audio. The R2 crisp ones are
     * the gap: we went from R2+TransientsSmooth (the worst possible for attacks) straight to R3, and
     * never measured the mode actually designed for percussive material. */
    struct { const char *name; RubberBandOptions opts; } cfg[] = {
      { "Crisp",  RubberBandOptionEngineFaster | RubberBandOptionChannelsTogether | RubberBandOptionWindowShort | RubberBandOptionTransientsCrisp },
      { "Mixed",  RubberBandOptionEngineFaster | RubberBandOptionChannelsTogether | RubberBandOptionWindowShort | RubberBandOptionTransientsMixed },
      { "Smooth", RubberBandOptionEngineFaster | RubberBandOptionChannelsTogether | RubberBandOptionWindowShort | RubberBandOptionTransientsSmooth },
      { "Smooth+SoftDet", RubberBandOptionEngineFaster | RubberBandOptionChannelsTogether | RubberBandOptionWindowShort | RubberBandOptionTransientsSmooth | RubberBandOptionDetectorSoft },
      { "Crisp+SoftDet",  RubberBandOptionEngineFaster | RubberBandOptionChannelsTogether | RubberBandOptionWindowShort | RubberBandOptionTransientsCrisp | RubberBandOptionDetectorSoft },
    };

    printf("pitch %+.0f%%\n", (pitch - 1.0) * 100.0);
    measure("key lock OFF", dry, (int)n);

    for (unsigned c = 0; c < sizeof cfg / sizeof *cfg; c++) {
    const RubberBandOptions opts = RubberBandOptionProcessRealTime
        | RubberBandOptionThreadingNever | cfg[c].opts;
    RubberBandState rb = rubberband_new(RATE, 1, opts, ratio, 1.0);
    rubberband_set_time_ratio(rb, ratio);

    float *wet = calloc(n, sizeof *wet);
    size_t fed = 0, got = 0; (void)0;
    while (fed < n && got < n) {
        const float *in[1] = { src + fed };
        unsigned want = (unsigned)(n - fed < BLOCK ? n - fed : BLOCK);
        rubberband_process(rb, in, want, 0);
        fed += want;
        int avail;
        while ((avail = rubberband_available(rb)) > 0 && got < n) {
            float *out[1] = { wet + got };
            unsigned take = (unsigned)((size_t)avail > n - got ? n - got : (size_t)avail);
            got += rubberband_retrieve(rb, out, take);
        }
    }

    measure(cfg[c].name, wet, (int)got);
    rubberband_delete(rb);
    free(wet);
    }
    return 0;
}
