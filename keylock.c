/*
 * Key lock (master tempo) - see keylock.h for what this is and, importantly, what it is not.
 */

#include <limits.h>
#include <math.h>
#include <string.h>

#include "interpolate.h"
#include "keylock.h"

void keylock_init(struct keylock *kl)
{
    memset(kl, 0, sizeof *kl);
}

void keylock_reset(struct keylock *kl)
{
    /* No memset: `primed` false means the tail is never read, so clearing it would be busywork on
     * the realtime thread. This is called on every block that runs unlocked. */
    kl->primed = false;
    kl->fill = 0;
    kl->head = 0;
}

bool keylock_applicable(double pitch)
{
    if (pitch < 0.5 || pitch > 2.0)
        return false;

    /* Near nominal there is nothing to correct, and correcting anyway comb-filters the output -
     * see KEYLOCK_DEADBAND. player_collect() resets the grain engine on this path, so re-entering
     * above the deadband starts from a clean tail rather than splicing onto a stale one. */
    return fabs(pitch - 1.0) > KEYLOCK_DEADBAND;
}

static inline signed short clamp(double v)
{
    if (v > SHRT_MAX)
        return SHRT_MAX;
    if (v < SHRT_MIN)
        return SHRT_MIN;
    return (signed short)v;
}

/*
 * Read `n` frames from the track starting at source sample `start`, advancing `step` per frame.
 */
static void read_window(struct track *tr, double start, double step, unsigned n,
                        float out[][KEYLOCK_CHANNELS], unsigned length)
{
    double sample = start;
    unsigned s;

    for (s = 0; s < n; s++) {
        signed short i[KEYLOCK_CHANNELS][4];
        int sa, q, c;
        double f;

        sa = (int)sample;
        if (sample < 0.0)
            sa--;
        f = sample - sa;
        sa--;

        for (q = 0; q < 4; q++, sa++) {
            if (sa < 0 || sa >= (int)length) {
                for (c = 0; c < KEYLOCK_CHANNELS; c++)
                    i[c][q] = 0;
            } else {
                signed short *ts = track_get_sample(tr, sa);
                for (c = 0; c < KEYLOCK_CHANNELS; c++)
                    i[c][q] = ts[c];
            }
        }

        for (c = 0; c < KEYLOCK_CHANNELS; c++)
            out[s][c] = (float)cubic_interpolate(i[c], f);

        sample += step;
    }
}

/*
 * Where to actually start the next grain.
 *
 * The ideal cursor gives the right AVERAGE rate; this picks the offset within +/-KEYLOCK_SEARCH
 * whose opening frames best match the tail we are about to cross-fade into, so the splice lands on
 * a similar part of the waveform instead of fighting it. Normalised by the candidate's own energy,
 * otherwise the loudest window wins regardless of shape.
 */
/*
 * Score one candidate offset: normalised correlation of its opening frames against the tail we are
 * about to cross-fade into. Normalised by the candidate's own energy, or the loudest window wins
 * regardless of shape.
 */
static double score_offset(struct keylock *kl, struct track *tr, double read, double step,
                           unsigned length, int off, unsigned corr_step)
{
    double corr = 0.0, energy = 0.0;
    unsigned s;

    for (s = 0; s < KEYLOCK_OVERLAP; s += corr_step) {
        double src = read + off + s * step, v, t;
        signed short *ts;
        int sa;

        sa = (int)src;
        if (src < 0.0)
            sa--;
        if (sa < 0 || sa >= (int)length)
            continue;

        /* Mono sum: half the arithmetic, and stereo grains want a common alignment anyway. */
        ts = track_get_sample(tr, sa);
        v = (double)ts[0] + ts[1];
        t = kl->tail[s][0] + kl->tail[s][1];

        corr += v * t;
        energy += v * v;
    }

    return corr / sqrt(energy + 1.0);
}

/*
 * Where to actually start the next grain.
 *
 * The ideal cursor gives the right AVERAGE rate; this picks the offset whose opening frames best
 * match the tail, so the splice lands on a similar part of the waveform instead of fighting it.
 * Coarse sweep first, then a sample-accurate refinement around the winner - see keylock.h for why
 * both stages earn their place.
 */
static double best_alignment(struct keylock *kl, struct track *tr, double read,
                             double step, unsigned length)
{
    double best_score = -INFINITY;
    int best_off = 0, off, lo, hi;

    for (off = -KEYLOCK_SEARCH; off <= KEYLOCK_SEARCH; off += KEYLOCK_SEARCH_COARSE) {
        double score = score_offset(kl, tr, read, step, length, off, KEYLOCK_CORR_STEP);

        if (score > best_score) {
            best_score = score;
            best_off = off;
        }
    }

    lo = best_off - KEYLOCK_SEARCH_COARSE;
    hi = best_off + KEYLOCK_SEARCH_COARSE;
    best_score = -INFINITY;

    for (off = lo; off <= hi; off += KEYLOCK_SEARCH_FINE) {
        double score = score_offset(kl, tr, read, step, length, off, KEYLOCK_CORR_STEP_FINE);

        if (score > best_score) {
            best_score = score;
            best_off = off;
        }
    }

    return read + best_off;
}

/*
 * Generate one grain into the fifo.
 *
 * `step` is source samples per output frame at NATIVE pitch - sample-rate conversion only, which is
 * what keeps pitch unchanged. `hop` is how far the ideal cursor moves between grains, and carries
 * the whole tempo change.
 */
static void make_grain(struct keylock *kl, struct track *tr, double step, double hop)
{
    unsigned length, s;
    int c;
    double read;

    /* Paired with track.c's __ATOMIC_RELEASE store on tr->length - see that comment. */
    length = __atomic_load_n(&tr->length, __ATOMIC_ACQUIRE);

    read = kl->primed ? best_alignment(kl, tr, kl->read, step, length) : kl->read;
    read_window(tr, read, step, KEYLOCK_GRAIN + KEYLOCK_OVERLAP, kl->win, length);

    if (kl->primed) {
        for (s = 0; s < KEYLOCK_OVERLAP; s++) {
            float w = (float)s / KEYLOCK_OVERLAP;

            for (c = 0; c < KEYLOCK_CHANNELS; c++)
                kl->win[s][c] = kl->tail[s][c] * (1.0f - w) + kl->win[s][c] * w;
        }
    }

    for (s = 0; s < KEYLOCK_GRAIN; s++) {
        unsigned w = (kl->head + kl->fill + s) % KEYLOCK_FIFO;

        for (c = 0; c < KEYLOCK_CHANNELS; c++)
            kl->fifo[w][c] = clamp(kl->win[s][c]);
    }
    kl->fill += KEYLOCK_GRAIN;

    for (s = 0; s < KEYLOCK_OVERLAP; s++)
        for (c = 0; c < KEYLOCK_CHANNELS; c++)
            kl->tail[s][c] = kl->win[KEYLOCK_GRAIN + s][c];

    /* Advance the IDEAL cursor by exactly the hop. The search offset above is deliberately not
     * folded back in - if it were, each grain's alignment nudge would accumulate and playback would
     * drift away from the position the rest of the player believes it is at. */
    kl->read += hop;
    kl->primed = true;
}

double keylock_build(struct keylock *kl, signed short *pcm, unsigned samples,
                     double sample_dt, struct track *tr, double position,
                     double pitch, double start_vol, double end_vol)
{
    double step, hop, vol, gradient, advanced;
    unsigned s;

    step = sample_dt * tr->rate;
    hop = KEYLOCK_GRAIN * step * pitch;

    /*
     * Re-seat on anything that moved position other than our own playback: a seek, a loop wrap, a
     * needle drop, a fresh track. Continuity is the whole basis of overlap-add, so carrying a tail
     * across a jump would splice two unrelated parts of the track together.
     */
    if (!kl->primed || fabs(position - kl->expect) > KEYLOCK_RESEAT) {
        keylock_reset(kl);
        kl->read = position * tr->rate;
    }

    while (kl->fill < samples)
        make_grain(kl, tr, step, hop);

    vol = start_vol;
    gradient = (end_vol - start_vol) / samples;

    for (s = 0; s < samples; s++) {
        int c;

        for (c = 0; c < KEYLOCK_CHANNELS; c++)
            *pcm++ = clamp(vol * kl->fifo[kl->head][c]);

        kl->head = (kl->head + 1) % KEYLOCK_FIFO;
        kl->fill--;
        vol += gradient;
    }

    advanced = sample_dt * pitch * samples;
    kl->expect = position + advanced;

    return advanced;
}
