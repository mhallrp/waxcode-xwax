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

/** Key lock (master tempo): change tempo without changing pitch. */

#ifndef KEYLOCK_H
#define KEYLOCK_H

#include <stdbool.h>

#include <rubberband/rubberband-c.h>

#include "keylock-rb.h"

#include "track.h"

#define KEYLOCK_CHANNELS 2

/** Largest block this will ever be asked for, and the most source it will pull in one go. */
#define KEYLOCK_MAX_BLOCK 2048
#define KEYLOCK_FEED 512

/** How far from nominal speed the platter must be before the engine is used at all. */
#define KEYLOCK_DISENGAGE 0.004

/** How long the pitch must STAY inside the disengage threshold before key lock actually lets go. */
#define KEYLOCK_DISENGAGE_SECONDS 1.5

/**...and how far it must get before engaging again. */
#define KEYLOCK_ENGAGE 0.012

/** How steady the platter must be before key lock engages, and for how long. */
#define KEYLOCK_STEADY_TOLERANCE 0.02
#define KEYLOCK_STEADY_SECONDS 0.15

/** How long the speed must keep moving before it counts as a hand on the platter. */
#define KEYLOCK_UNSTEADY_SECONDS 0.03

/* Seconds of unexplained position change that re-seats the engine - see keylock_build(). */
#define KEYLOCK_RESEAT 0.01

/** The time ratios the stretcher is warmed for at construction, and clamped to in service. */
#define KEYLOCK_RATIO_MIN 0.5
#define KEYLOCK_RATIO_MAX 2.0

/** How many ratios across that range are walked at construction. */
#define KEYLOCK_PREWARM_POINTS 24

/** Don't hand Rubber Band a new time ratio for a change smaller than this. */
#define KEYLOCK_RATIO_EPSILON 0.001

struct keylock {
    RubberBandState rb;
    unsigned int rate;      /* what rb was built for; rebuilt if a track differs */
    bool primed;            /* fed enough to have started producing */
    bool engaged;           /* latched, with hysteresis - see KEYLOCK_ENGAGE */
    double settled_for;     /* seconds inside DISENGAGE - see KEYLOCK_DISENGAGE_SECONDS */
    double read;            /* source cursor, in samples, at the NATIVE rate */
    double expect;          /* elapsed we expect next call, for jump detection */
    double steady_for;      /* seconds the speed has held still */
    double unsteady_for;    /* seconds it has been moving - see KEYLOCK_UNSTEADY_SECONDS */
    double last_pitch;

    /** Set when Rubber Band threw. */
    bool failed;
    double last_ratio;      /* what the stretcher was last told - see KEYLOCK_RATIO_EPSILON */

    /* Scratch, in the struct rather than on the stack - this runs on the realtime thread. */
    float in[KEYLOCK_CHANNELS][KEYLOCK_FEED];
    float out[KEYLOCK_CHANNELS][KEYLOCK_MAX_BLOCK];
    float *in_ptr[KEYLOCK_CHANNELS];
    float *out_ptr[KEYLOCK_CHANNELS];
};

void keylock_init(struct keylock *kl);
void keylock_reset(struct keylock *kl);
void keylock_clear(struct keylock *kl);

/** Whether key lock should be engaged right now. */
bool keylock_applicable(struct keylock *kl, double pitch, double dt);

/** Mirrors build_pcm(): fills `samples` frames and returns seconds advanced in the source */
double keylock_build(struct keylock *kl, signed short *pcm, unsigned samples,
                     double sample_dt, struct track *tr, double position,
                     double pitch, double start_vol, double end_vol);

#endif
