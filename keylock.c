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

/** Key lock (master tempo): change tempo without changing pitch, via Rubber Band R3. */

#include <limits.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

#include "interpolate.h"
#include "keylock.h"

/** R3, chosen as the better all-rounder - and it IS a compromise, measured rather than assumed. */
static const RubberBandOptions KEYLOCK_OPTIONS =
    RubberBandOptionProcessRealTime
    | RubberBandOptionEngineFiner
    | RubberBandOptionThreadingNever
    | RubberBandOptionChannelsTogether;

void keylock_init(struct keylock *kl)
{
    memset(kl, 0, sizeof *kl);
    for (int c = 0; c < KEYLOCK_CHANNELS; c++) {
        kl->in_ptr[c] = kl->in[c];
        kl->out_ptr[c] = kl->out[c];
    }
    kl->last_ratio = 0.0;
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
    /** Steadiness deliberately NOT cleared: this is called on every bypassed block, and zeroing it there would stop the timer ever accumulating */
    if (!kl->primed)
        return;

    kl->primed = false;
    kl->last_ratio = 0.0;
    /** Clears `failed` too: a reseat is a fresh start, and it is the point at which retrying the stretcher is reasonable again rather than hopeful. */
    kl->failed = false;
    if (kl->rb != NULL)
        kl_rb_reset(kl->rb);
}

bool keylock_applicable(struct keylock *kl, double pitch, double dt)
{
    /** Asked before anything else: a stretcher that just threw gets left alone until a reseat */
    if (kl->failed)
        return false;

    /** Tracked whatever the speed, so a scrub passing through the working range does not arrive already looking settled. */
    if (fabs(pitch - kl->last_pitch) > KEYLOCK_STEADY_TOLERANCE) {
        kl->unsteady_for += dt;
        if (kl->unsteady_for >= KEYLOCK_UNSTEADY_SECONDS)
            kl->steady_for = 0.0;
    } else {
        kl->unsteady_for = 0.0;
        kl->steady_for += dt;
    }
    kl->last_pitch = pitch;

    if (pitch < 0.5 || pitch > 2.0) {
        kl->engaged = false;
        return false;
    }

    /** Latched with hysteresis - see KEYLOCK_ENGAGE. */
    {
        double off_nominal = fabs(pitch - 1.0);

        if (kl->engaged) {
            /** Has to PERSIST. */
            if (off_nominal < KEYLOCK_DISENGAGE) {
                kl->settled_for += dt;
                if (kl->settled_for >= KEYLOCK_DISENGAGE_SECONDS)
                    kl->engaged = false;
            } else {
                kl->settled_for = 0.0;
            }
        } else if (off_nominal > KEYLOCK_ENGAGE) {
            kl->engaged = true;
            kl->settled_for = 0.0;
        }
    }

    if (!kl->engaged)
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

    /** And the RATIO extremes, here, for the same reason - which max_process_size does not cover. */
    {
        double ratios[KEYLOCK_PREWARM_POINTS];
        const double span = KEYLOCK_RATIO_MAX - KEYLOCK_RATIO_MIN;

        for (int i = 0; i < KEYLOCK_PREWARM_POINTS; i++) {
            ratios[i] = KEYLOCK_RATIO_MIN
                + span * (double)i / (double)(KEYLOCK_PREWARM_POINTS - 1);
        }

        /** A stretcher that cannot be warmed is one that would allocate while playing, so it is no use to us */
        if (kl_rb_prewarm(kl->rb, ratios, KEYLOCK_PREWARM_POINTS) != 0) {
            fprintf(stderr, "keylock: could not pre-allocate the stretcher; key lock unavailable\n");
            keylock_clear(kl);
            return false;
        }
    }

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

    /** Re-seat on anything that moved position other than our own playback: a seek, a loop wrap, a needle drop, a fresh track. */
    if (!kl->primed || fabs(position - kl->expect) > KEYLOCK_RESEAT) {
        keylock_reset(kl);
        kl->read = position * tr->rate;
        kl->primed = true;
    }

    /** Time ratio is output over input: playing FASTER means less output per input. */
    {
        /** CLAMPED to the range the stretcher was warmed for. */
        double ratio = pitch != 0.0 ? 1.0 / fabs(pitch) : 1.0;
        if (ratio < KEYLOCK_RATIO_MIN)
            ratio = KEYLOCK_RATIO_MIN;
        else if (ratio > KEYLOCK_RATIO_MAX)
            ratio = KEYLOCK_RATIO_MAX;

        if (kl->last_ratio <= 0.0
            || fabs(ratio - kl->last_ratio) > kl->last_ratio * KEYLOCK_RATIO_EPSILON) {
            if (kl_rb_set_time_ratio(kl->rb, ratio) != 0) {
                kl->failed = true;
                return 0.0;
            }
            kl->last_ratio = ratio;
        }
    }

    /* Feed until it can give us the block. Bounded so a pathological ratio cannot spin here. */
    for (int guard = 0; guard < 64; guard++) {
        int available;
        unsigned want;

        if (kl_rb_available(kl->rb, &available) != 0) {
            kl->failed = true;
            return 0.0;
        }
        if (available >= (int)samples)
            break;

        if (kl_rb_required(kl->rb, &want) != 0) {
            kl->failed = true;
            return 0.0;
        }
        if (want == 0 || want > KEYLOCK_FEED)
            want = KEYLOCK_FEED;

        read_native(tr, kl->read, step, want, kl->in, length);
        kl->read += want * step;
        if (kl_rb_process(kl->rb, (const float *const *)kl->in_ptr, want, 0) != 0) {
            kl->failed = true;
            return 0.0;
        }
    }

    if (kl_rb_retrieve(kl->rb, kl->out_ptr, samples, &produced) != 0) {
        kl->failed = true;
        return 0.0;
    }

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
