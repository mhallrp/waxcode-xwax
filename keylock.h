/*
 * Key lock (master tempo): change tempo without changing pitch.
 *
 * NOTHING to do with musical key detection, despite the name - "key" here means perceived pitch.
 * The deck never needs to know what key a track is in; it only has to undo the pitch shift that
 * varispeed just introduced. So there is no analysis, no metadata and nothing cached.
 *
 * The reference is ALWAYS the track's own recorded pitch - 0.0 on the fader - never "whatever pitch
 * it happened to be at when key lock came on". Load and start a track at +2% and it plays 2% faster
 * at its original pitch. That falls out of the design: the source is always read at its native
 * rate, so the pitch it emits cannot be anything but the recording's own. Only the time ratio
 * carries the tempo change (owner's call, 2026-09-02).
 *
 * The stretching itself is Rubber Band's, in real-time mode. It replaced a hand-rolled SOLA on
 * 2026-09-15. That version was tuned across three rounds on real hardware - grain size up, then a
 * correlation window decoupled from the cross-fade, then grain size down - and each round improved
 * something, but snares and claps stayed wrong. They always would have: WSOLA splices where a
 * waveform REPEATS, a clap is broadband noise with no period to find, so the aligner picks
 * arbitrarily and the splice either flams the attack or clips it. Fixing that means detecting
 * transients and refusing to splice across them, which is exactly the part a library has already
 * solved and the part worth not writing twice.
 */

#ifndef KEYLOCK_H
#define KEYLOCK_H

#include <stdbool.h>

#include <rubberband/rubberband-c.h>

#include "track.h"

#define KEYLOCK_CHANNELS 2

/*
 * Largest block this will ever be asked for, and the most source it will pull in one go.
 *
 * Both are fixed so nothing allocates on the audio thread: Rubber Band is told the maximum up
 * front, and the scratch buffers below are sized for it.
 */
#define KEYLOCK_MAX_BLOCK 2048
#define KEYLOCK_FEED 512

/*
 * How far from nominal speed the platter must be before the engine is used at all.
 *
 * At 1.0 there is nothing to correct: tempo and pitch are already the recording's own. Running a
 * stretcher anyway can only add its own artefacts to a signal that needed none. These are 0.4% and
 * 1.2% - 7 and 21 cents, both inaudible as pitch error and far below any deliberate nudge.
 *
 * This is how far from nominal the platter must fall before key lock lets go...
 */
#define KEYLOCK_DISENGAGE 0.004

/*
 * ...and how far it must get before engaging again. The gap between the two is hysteresis, and it
 * is not optional.
 *
 * A platter wows by around 0.3% once per revolution, so a fader parked anywhere near a single
 * threshold has its real pitch wandering back and forth ACROSS it. Without hysteresis that switched
 * between varispeed and a freshly-reset stretcher hundreds of times a second, each switch emitting
 * a partial block while the stretcher re-primed - which is not a subtle artefact, it is loud
 * electronic noise, and it is what the owner heard around 0.0 (2026-09-15).
 *
 * The band between them is wide enough to swallow wow whole.
 */
#define KEYLOCK_ENGAGE 0.012

/*
 * How steady the platter must be before key lock engages, and for how long.
 *
 * A speed WITHIN the working range is not the same as playback. Back-cueing sweeps the pitch
 * through the range continuously, and a stretcher fed a hand-moved platter produces nonsense - it
 * is being asked to time-stretch something whose timebase keeps reversing. Real decks disable key
 * lock while scratching for the same reason.
 */
#define KEYLOCK_STEADY_TOLERANCE 0.02
#define KEYLOCK_STEADY_SECONDS 0.15

/* Seconds of unexplained position change that re-seats the engine - see keylock_build(). */
#define KEYLOCK_RESEAT 0.01

struct keylock {
    RubberBandState rb;
    unsigned int rate;      /* what rb was built for; rebuilt if a track differs */
    bool primed;            /* fed enough to have started producing */
    bool engaged;           /* latched, with hysteresis - see KEYLOCK_ENGAGE */
    double read;            /* source cursor, in samples, at the NATIVE rate */
    double expect;          /* elapsed we expect next call, for jump detection */
    double steady_for;      /* seconds the speed has held still */
    double last_pitch;

    /* Scratch, in the struct rather than on the stack - this runs on the realtime thread. */
    float in[KEYLOCK_CHANNELS][KEYLOCK_FEED];
    float out[KEYLOCK_CHANNELS][KEYLOCK_MAX_BLOCK];
    float *in_ptr[KEYLOCK_CHANNELS];
    float *out_ptr[KEYLOCK_CHANNELS];
};

void keylock_init(struct keylock *kl);
void keylock_reset(struct keylock *kl);
void keylock_clear(struct keylock *kl);

/*
 * Whether key lock should be engaged right now.
 *
 * Takes the whole state, not just the instantaneous pitch, because "is this playback or a scrub?"
 * cannot be answered from one sample. `dt` is the block's duration and advances the steadiness
 * timer, so this must be called once per block.
 */
bool keylock_applicable(struct keylock *kl, double pitch, double dt);

/*
 * Mirrors build_pcm(): fills `samples` frames and returns seconds advanced in the source, so the
 * caller's position bookkeeping is completely unchanged.
 */
double keylock_build(struct keylock *kl, signed short *pcm, unsigned samples,
                     double sample_dt, struct track *tr, double position,
                     double pitch, double start_vol, double end_vol);

#endif
