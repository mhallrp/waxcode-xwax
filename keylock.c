/*
 * Key lock (master tempo) - see keylock.h for what this is and, importantly, what it is not.
 */

#include <limits.h>
#include <math.h>
#include <string.h>

#include "interpolate.h"
#include "keylock.h"

/*
 * Faster engine, short window, threading off.
 *
 * EngineFiner sounds better on a bounce but is far heavier, and this runs on the realtime thread of
 * a Pi driving two decks. ThreadingNever because xwax already owns its realtime thread and a
 * library spawning its own inside it is a scheduling problem, not a speedup. WindowShort keeps the
 * latency a DVS can least afford down.
 */
static const RubberBandOptions KEYLOCK_OPTIONS =
    RubberBandOptionProcessRealTime
    | RubberBandOptionEngineFaster
    | RubberBandOptionWindowShort
    | RubberBandOptionThreadingNever
    | RubberBandOptionTransientsCrisp;

void keylock_init(struct keylock *kl)
{
    memset(kl, 0, sizeof *kl);
    for (int c = 0; c < KEYLOCK_CHANNELS; c++) {
        kl->in_ptr[c] = kl->in[c];
        kl->out_ptr[c] = kl->out[c];
    }
}

void keylock_clear(struct keylock *kl)
{
    if (kl->rb != NULL) {
        rubberband_delete(kl->rb);
        kl->rb = NULL;
    }
}

void keylock_reset(struct keylock *kl)
{
    /* Steadiness deliberately NOT cleared: this is called on every bypassed block, and zeroing it
     * there would stop the timer ever accumulating, so key lock could never engage at all. */
    kl->primed = false;
    if (kl->rb != NULL)
        rubberband_reset(kl->rb);
}

bool keylock_applicable(struct keylock *kl, double pitch, double dt)
{
    /* Tracked whatever the speed, so a scrub passing through the working range does not arrive
     * already looking settled. */
    if (fabs(pitch - kl->last_pitch) > KEYLOCK_STEADY_TOLERANCE)
        kl->steady_for = 0.0;
    else
        kl->steady_for += dt;
    kl->last_pitch = pitch;

    if (pitch < 0.5 || pitch > 2.0)
        return false;

    /* Near nominal there is nothing to correct - see KEYLOCK_DEADBAND. */
    if (fabs(pitch - 1.0) <= KEYLOCK_DEADBAND)
        return false;

    return kl->steady_for >= KEYLOCK_STEADY_SECONDS;
}

static inline signed short clamp(double v)
{
    if (v > SHRT_MAX)
        return SHRT_MAX;
    if (v < SHRT_MIN)
        return SHRT_MIN;
    return (signed short)v;
}

/* Reads `n` frames from the track at its NATIVE rate - which is what preserves pitch. */
static void read_native(struct track *tr, double start, double step, unsigned n,
                        float in[KEYLOCK_CHANNELS][KEYLOCK_FEED], unsigned length)
{
    double sample = start;

    for (unsigned s = 0; s < n; s++) {
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

        /* Rubber Band wants float in -1..1, planar. */
        for (c = 0; c < KEYLOCK_CHANNELS; c++)
            in[c][s] = (float)(cubic_interpolate(i[c], f) / 32768.0);

        sample += step;
    }
}

static bool ensure_stretcher(struct keylock *kl, unsigned int rate)
{
    if (kl->rb != NULL && kl->rate == rate)
        return true;

    keylock_clear(kl);
    kl->rb = rubberband_new(rate, KEYLOCK_CHANNELS, KEYLOCK_OPTIONS, 1.0, 1.0);
    if (kl->rb == NULL)
        return false;
    /* Told up front so it never allocates on the realtime thread. */
    rubberband_set_max_process_size(kl->rb, KEYLOCK_FEED);
    kl->rate = rate;
    kl->primed = false;
    return true;
}

double keylock_build(struct keylock *kl, signed short *pcm, unsigned samples,
                     double sample_dt, struct track *tr, double position,
                     double pitch, double start_vol, double end_vol)
{
    double step, vol, gradient, advanced;
    unsigned length, produced;

    /* Paired with track.c's __ATOMIC_RELEASE store on tr->length - see that comment. */
    length = __atomic_load_n(&tr->length, __ATOMIC_ACQUIRE);

    if (samples > KEYLOCK_MAX_BLOCK || !ensure_stretcher(kl, tr->rate)) {
        /* Nothing sensible to do but pass the audio through unstretched. */
        memset(pcm, '\0', sizeof(*pcm) * KEYLOCK_CHANNELS * samples);
        return sample_dt * pitch * samples;
    }

    step = sample_dt * tr->rate;

    /*
     * Re-seat on anything that moved position other than our own playback: a seek, a loop wrap, a
     * needle drop, a fresh track. A stretcher carries state across its window, so feeding it a
     * discontinuity splices two unrelated parts of the track together.
     */
    if (!kl->primed || fabs(position - kl->expect) > KEYLOCK_RESEAT) {
        keylock_reset(kl);
        kl->read = position * tr->rate;
        kl->primed = true;
    }

    /* Time ratio is output over input: playing FASTER means less output per input. */
    rubberband_set_time_ratio(kl->rb, pitch != 0.0 ? 1.0 / fabs(pitch) : 1.0);

    /* Feed until it can give us the block. Bounded so a pathological ratio cannot spin here. */
    for (int guard = 0; rubberband_available(kl->rb) < (int)samples && guard < 64; guard++) {
        unsigned want = rubberband_get_samples_required(kl->rb);

        if (want == 0 || want > KEYLOCK_FEED)
            want = KEYLOCK_FEED;

        read_native(tr, kl->read, step, want, kl->in, length);
        kl->read += want * step;
        rubberband_process(kl->rb, (const float *const *)kl->in_ptr, want, 0);
    }

    produced = rubberband_retrieve(kl->rb, kl->out_ptr, samples);

    vol = start_vol;
    gradient = (end_vol - start_vol) / samples;

    for (unsigned s = 0; s < samples; s++) {
        for (int c = 0; c < KEYLOCK_CHANNELS; c++) {
            /* Short of output only while priming; silence there beats repeating a stale block. */
            double v = s < produced ? kl->out[c][s] * 32768.0 : 0.0;
            *pcm++ = clamp(vol * v);
        }
        vol += gradient;
    }

    advanced = sample_dt * pitch * samples;
    kl->expect = position + advanced;

    return advanced;
}
