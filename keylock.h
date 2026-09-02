/*
 * Key lock (master tempo): change tempo without changing pitch.
 *
 * NOTHING to do with musical key detection, despite the name - "key" here means perceived pitch.
 * The deck never needs to know what key a track is in; it only has to undo the pitch shift that
 * varispeed just introduced. So there is no analysis, no metadata and nothing cached.
 *
 * Ordinary playback resamples: reading the source at rate r scales tempo AND pitch by r. Key lock
 * instead reads at the native rate - which preserves pitch - and time-scales by r with SOLA
 * overlap-add, choosing each grain's alignment by cross-correlation against the previous grain's
 * tail (the WSOLA refinement) so successive grains stay waveform-aligned instead of phase-cancelling.
 *
 * The reference is ALWAYS the track's own recorded pitch - 0.0 on the fader, ratio 1.0 - never
 * "whatever pitch it happened to be at when key lock came on". Load and start a track at +2% and it
 * plays 2% faster at its original pitch; go to +6% and it is still the original pitch, just faster.
 * That falls out of the design rather than needing handling: the grain reader ALWAYS reads at the
 * native rate, so the pitch it emits cannot be anything other than the source's own. Only the hop
 * between grains carries the tempo change (owner's call, 2026-09-02).
 *
 * Time domain deliberately, not a phase vocoder: no FFT, no windowing latency beyond one grain, and
 * comfortably affordable next to xwax's realtime thread. The trade is that quality falls away at
 * extreme ratios, which does not matter - see keylock_applicable().
 */

#ifndef KEYLOCK_H
#define KEYLOCK_H

#include <stdbool.h>

#include "track.h"

#define KEYLOCK_CHANNELS 2

/*
 * Synthesis hop, in output samples. Sets both the artefact character and the added latency, since a
 * grain is generated whole: 512 is ~10.7ms at 48kHz, of which the listener sees about half on
 * average. Larger sounds smoother and costs more delay between platter and audio, which on a DVS
 * is the thing you cannot give away.
 */
#define KEYLOCK_GRAIN 512

/*
 * Cross-fade between grains, in output samples.
 *
 * 256 (5.3ms), raised from 128 on hearing it: a cross-fade shorter than one period of the material
 * cannot smooth a mismatch in that material, and 128 samples is under one period of anything below
 * ~375Hz - which is most of a pad.
 */
#define KEYLOCK_OVERLAP 256

/*
 * WSOLA alignment search, +/- source samples around the ideal hop.
 *
 * 512 (~10.7ms), raised from 128 on hearing it wobble on sustained melodic content at +/-8%
 * (owner-reported, 2026-09-02). The search can only phase-align to a period it can actually see:
 * +/-128 samples is +/-2.7ms, while a 100Hz pad note has a 480-sample period. Given less than half a
 * cycle to look at, the correlation picks whatever is least bad and the alignment error alternates
 * grain to grain - which is heard as a wobble at the grain rate rather than as a click. 512 covers
 * fundamentals down to ~47Hz.
 */
#define KEYLOCK_SEARCH 512

/*
 * Two-stage search: sweep the whole range coarsely, then refine to sample accuracy around the
 * winner. A single coarse pass is what the wide range would otherwise cost, and coarse alone is not
 * enough - at 1kHz a 48-sample period means an 8-sample step is already 60 degrees of phase error,
 * which is precisely the residual that pads expose.
 */
#define KEYLOCK_SEARCH_COARSE 16
#define KEYLOCK_SEARCH_FINE 1
#define KEYLOCK_CORR_STEP 4
#define KEYLOCK_CORR_STEP_FINE 2

/* Must exceed KEYLOCK_GRAIN plus the largest audio block we are ever asked for. */
#define KEYLOCK_FIFO 2048

/* Seconds of unexplained position change that re-seats the grain engine - see keylock_build(). */
#define KEYLOCK_RESEAT 0.01

struct keylock {
    bool primed;      /* a tail exists to cross-fade against */
    double read;      /* IDEAL source cursor for the next grain, advanced by exactly the hop */
    double expect;    /* elapsed we expect to be handed next call, for jump detection */
    unsigned fill;    /* output samples ready in the fifo */
    unsigned head;    /* fifo read index */

    float tail[KEYLOCK_OVERLAP][KEYLOCK_CHANNELS];
    /* Grain scratch lives in the struct, not on the stack - this runs on the realtime thread. */
    float win[KEYLOCK_GRAIN + KEYLOCK_OVERLAP][KEYLOCK_CHANNELS];
    signed short fifo[KEYLOCK_FIFO][KEYLOCK_CHANNELS];
};

void keylock_init(struct keylock *kl);
void keylock_reset(struct keylock *kl);

/*
 * Whether key lock should engage at this pitch.
 *
 * Steady forward playback only. Scratching, reverse and needle drops fall back to plain varispeed:
 * SOLA has no meaningful answer for a discontinuous, direction-changing position, and key-locked
 * scratching sounds wrong anyway - real decks drop it there too.
 */
bool keylock_applicable(double pitch);

/*
 * Mirrors build_pcm(): fills `samples` frames and returns seconds advanced in the source, so the
 * caller's position bookkeeping is completely unchanged.
 */
double keylock_build(struct keylock *kl, signed short *pcm, unsigned samples,
                     double sample_dt, struct track *tr, double position,
                     double pitch, double start_vol, double end_vol);

#endif
