/*
 * Copyright (C) 2026 Mark Hills <mark@xwax.org>
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

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "device.h"
#include "player.h"
#include "track.h"
#include "timecoder.h"

/* Bend playback speed to compensate for the difference between our
 * current position and that given by the timecode */

#define SYNC_TIME (1.0 / 2) /* time taken to reach sync */
#define SYNC_PITCH 0.05 /* don't sync at low pitches */
#define SYNC_RC 0.05 /* filter to 1.0 when no timecodes available */

/* If the difference between our current position and that given by
 * the timecode is greater than this value, recover by jumping
 * straight to the position given by the timecode. */

#define SKIP_THRESHOLD (1.0 / 8) /* before dropping audio */

/* The base volume level. A value of 1.0 leaves no headroom to play
 * louder when the record is going faster than 1.0. */

#define VOLUME (7.0/8)

#define SQ(x) ((x)*(x))
#define TARGET_UNKNOWN INFINITY

/*
 * Return: the cubic interpolation of the sample at position 2 + mu
 */

static inline double cubic_interpolate(signed short y[4], double mu)
{
    signed long a0, a1, a2, a3;
    double mu2;

    mu2 = SQ(mu);
    a0 = y[3] - y[2] - y[0] + y[1];
    a1 = y[0] - y[1] - a0;
    a2 = y[2] - y[0];
    a3 = y[1];

    return (mu * mu2 * a0) + (mu2 * a1) + (mu * a2) + a3;
}

/*
 * Return: Random dither, between -0.5 and 0.5
 */

static double dither(void)
{
    unsigned int bit, v;
    static unsigned int x = 0xbeefface;

    /* Maximum length LFSR sequence with 32-bit state */

    bit = (x ^ (x >> 1) ^ (x >> 21) ^ (x >> 31)) & 1;
    x = x << 1 | bit;

    /* We can adjust the balance between randomness and performance
     * by our chosen bit permutation; here we use a 12 bit subset
     * of the state */

    v = (x & 0x0000000f)
        | ((x & 0x000f0000) >> 12)
        | ((x & 0x0f000000) >> 16);

    return (double)v / 4096 - 0.5; /* not quite whole range */
}

/*
 * Build a block of PCM audio, resampled from the track
 *
 * This is just a basic resampler which has a small amount of aliasing
 * where pitch > 1.0.
 *
 * Return: number of seconds advanced in the source audio track
 * Post: buffer at pcm is filled with the given number of samples
 */

static double build_pcm(signed short *pcm, unsigned samples, double sample_dt,
                        struct track *tr, double position, double pitch,
                        double start_vol, double end_vol)
{
    int s;
    double sample, step, vol, gradient;

    sample = position * tr->rate;
    step = sample_dt * pitch * tr->rate;

    vol = start_vol;
    gradient = (end_vol - start_vol) / samples;

    for (s = 0; s < samples; s++) {
        int c, sa, q;
        double f;
        signed short i[PLAYER_CHANNELS][4];

        /* 4-sample window for interpolation */

        sa = (int)sample;
        if (sample < 0.0)
            sa--;
        f = sample - sa;
        sa--;

        for (q = 0; q < 4; q++, sa++) {
            if (sa < 0 || sa >= tr->length) {
                for (c = 0; c < PLAYER_CHANNELS; c++)
                    i[c][q] = 0;
            } else {
                signed short *ts;
                int c;

                ts = track_get_sample(tr, sa);
                for (c = 0; c < PLAYER_CHANNELS; c++)
                    i[c][q] = ts[c];
            }
        }

        for (c = 0; c < PLAYER_CHANNELS; c++) {
            double v;

            v = vol * cubic_interpolate(i[c], f) + dither();

            if (v > SHRT_MAX) {
                *pcm++ = SHRT_MAX;
            } else if (v < SHRT_MIN) {
                *pcm++ = SHRT_MIN;
            } else {
                *pcm++ = (signed short)v;
            }
        }

        sample += step;
        vol += gradient;
    }

    return sample_dt * pitch * samples;
}

/*
 * Equivalent to build_pcm, but for use when the track is
 * not available
 *
 * Return: number of seconds advanced in the audio track
 * Post: buffer at pcm is filled with silence
 */

static double build_silence(signed short *pcm, unsigned samples,
                            double sample_dt, double pitch)
{
    memset(pcm, '\0', sizeof(*pcm) * PLAYER_CHANNELS * samples);
    return sample_dt * pitch * samples;
}

/*
 * Change the timecoder used by this playback
 */

void player_set_timecoder(struct player *pl, struct timecoder *tc)
{
    assert(tc != NULL);
    pl->timecoder = tc;
    pl->recalibrate = true;
    pl->timecode_control = true;
}

/*
 * Post: player is initialised
 */

void player_init(struct player *pl, unsigned int sample_rate,
                 struct track *track, struct timecoder *tc,
                 double cue_offset)
{
    assert(track != NULL);
    assert(sample_rate != 0);

    spin_init(&pl->lock);

    pl->sample_dt = 1.0 / sample_rate;
    pl->track = track;
    player_set_timecoder(pl, tc);

    pl->position = 0.0;
    /* Pi DVS: cue_offset is a fixed, deliberately-configured
     * calibration constant (see xwax.c's --cue-offset), not a dynamic
     * recue - it compensates for a small, consistent discrepancy
     * confirmed on real hardware between true needle-drop position 0
     * and the first position xwax can actually decode (decoding a
     * valid absolute position requires VALID_BITS consecutive correct
     * bits - see timecoder.c - which inherently takes a small amount
     * of groove/time to accumulate, during which the needle keeps
     * moving). Defaults to 0.0 (no correction, matching prior
     * behaviour) until empirically tuned. */
    pl->offset = cue_offset;
    pl->target_position = TARGET_UNKNOWN;
    pl->last_difference = 0.0;

    /* Pi DVS: no real-time buffer has run yet, so there is genuinely
     * no signal to speak of - false is the honest starting value, not
     * just a placeholder (see struct player's own doc comment). */
    pl->timecode_valid = false;
    pl->relative_mode = false;
    pl->relative_awaiting_signal = false;
    pl->loop_active = false;
    pl->loop_start = 0.0;
    pl->loop_end = 0.0;
    pl->cue_point = pl->offset;
    pl->timecode_disabled = false;

    pl->pitch = 0.0;
    pl->sync_pitch = 1.0;
    pl->volume = 0.0;
}

/*
 * Pre: player is initialised
 * Post: no resources are allocated by the player
 */

void player_clear(struct player *pl)
{
    spin_clear(&pl->lock);
    track_release(pl->track);
}

/*
 * Enable or disable timecode control
 */

void player_set_timecode_control(struct player *pl, bool on)
{
    if (on && !pl->timecode_control)
        pl->recalibrate = true;
    pl->timecode_control = on;
}

/*
 * Toggle timecode control
 *
 * Return: the new state of timecode control
 */

bool player_toggle_timecode_control(struct player *pl)
{
    pl->timecode_control = !pl->timecode_control;
    if (pl->timecode_control)
        pl->recalibrate = true;
    return pl->timecode_control;
}

void player_set_internal_playback(struct player *pl)
{
    pl->timecode_control = false;
    pl->pitch = 1.0;
}

/*
 * Pi DVS: enable or disable relative mode (owner's spec, 2026-08-01) -
 * see struct player's own doc comment for the full design. Turning it
 * off schedules the SAME plain position-snap player_set_timecode_control()
 * already uses when re-enabling absolute mode from off, by reusing
 * `recalibrate` - relative mode leaves timecode_control itself
 * untouched throughout, so that's the only signal retarget() has to
 * know a snap is due.
 */
void player_set_relative_mode(struct player *pl, bool on)
{
    pl->relative_mode = on;
    if (!on) {
        pl->recalibrate = true;
        /* Pi DVS: a loop only ever applies in relative mode (see
         * player_collect()'s own gate) - clearing it here rather than
         * just leaving loop_active stale means re-entering relative
         * mode later never silently resumes an old loop the owner
         * never re-requested. */
        pl->loop_active = false;
    }
}

/*
 * Pi DVS: activate a loop between [start_seconds, end_seconds) of the
 * track's own elapsed time (owner's spec, 2026-08-02 - a simple loop
 * toggle for relative mode, the app decides what range to send, eg. a
 * bar's worth from the beat grid). Converts to position-space once
 * here (adding the player's own fixed `offset`) so player_collect()'s
 * own realtime wraparound check never needs to repeat that arithmetic
 * every buffer. Only takes effect while relative_mode is on - see
 * player_collect()'s own gate; absolute mode's position is dictated
 * by the physical needle, there's nothing here to loop against.
 *
 * Plain field writes, not lock-protected - called from handle_loop(),
 * already on the realtime thread itself (see control.c's own
 * IMPORTANT #1), so there's no genuine cross-thread race to guard
 * against here, same reasoning as player_set_relative_mode() above.
 * xwax's own spin_lock() restricts itself from being called on a
 * realtime thread at all (see spin.h) - that restriction simply
 * doesn't apply to plain field writes like these.
 */
void player_set_loop(struct player *pl, double start_seconds, double end_seconds)
{
    pl->loop_start = pl->offset + start_seconds;
    pl->loop_end = pl->offset + end_seconds;
    pl->loop_active = true;
}

/*
 * Pi DVS: deactivate a loop - playback continues from wherever
 * `position` currently is, no jump (owner's spec, 2026-08-02: "press
 * again to stop looping, continue playing").
 */
void player_clear_loop(struct player *pl)
{
    pl->loop_active = false;
}

double player_get_position(struct player *pl)
{
    return pl->position;
}

double player_get_elapsed(struct player *pl)
{
    return pl->position - pl->offset;
}

double player_get_remain(struct player *pl)
{
    return (double)pl->track->length / pl->track->rate
        + pl->offset - pl->position;
}

bool player_is_active(const struct player *pl)
{
    return (fabs(pl->pitch) > 0.01);
}

/*
 * Cue to the zero position of the track
 */

void player_recue(struct player *pl)
{
    pl->offset = pl->position;
}

/*
 * Pi DVS: shared primitive behind player_seek_to_elapsed()/player_cue()
 * below - jump `position` directly to `to` (position-space, offset
 * already applied) and pause there (owner's spec, 2026-08-03).
 * Originally player_cue_to_start() (retired - see player_seek_to_
 * elapsed()'s own doc comment), generalized from a fixed jump-to-
 * `offset` to an arbitrary target.
 *
 * Locked, unlike player_seek_to()/player_recue() elsewhere in this
 * file - those only ever write `offset`, but this writes `position`
 * directly, the same field player_collect() reads to build audio.
 *
 * MUST use spin_try_lock(), not spin_lock() - confirmed as a real,
 * hardware-crashing bug (2026-08-01, on the original player_cue_to_
 * start()): unlike player_set_track() (whose spin_lock() call is
 * legitimate - it runs on the dedicated LOAD worker thread, see
 * control.c's own IMPORTANT #2), this runs directly on the realtime
 * thread itself (called from handle_seek()/handle_goto_cue(), same as
 * player_collect()). spin_lock()'s own precondition is "current thread
 * is not realtime" (see spin.h) - it calls rt_not_allowed()
 * unconditionally, which aborts the whole process the instant a real
 * command arrived, crashing xwax and restarting it with nothing loaded
 * (read as "the track got unloaded" from the app). spin_try_lock() is
 * what player_collect() already uses for this exact reason; on the
 * rare miss (worker thread mid-swap in player_set_track()) this just
 * silently no-ops rather than blocking - acceptable, the same tradeoff
 * player_collect() already makes.
 *
 * Pi DVS (owner's spec, 2026-08-01, carried over from player_cue_to_
 * start()): if the needle is down and providing a real reading right
 * now, this jump starts the track playing from the new position
 * immediately, following the live pitch as usual - no extra work
 * needed here, sync_to_timecode_relative() already drives pitch from
 * the needle every buffer regardless of this call. But if the needle
 * is currently up (not providing a valid reading), relative mode's
 * normal "keep playing through a lift" behaviour would otherwise carry
 * the OLD pitch straight through this jump, silently auto-playing from
 * the new position with nobody's hand on the record - same "inheriting
 * stale state" problem player_set_track() already solves for a fresh
 * load via relative_awaiting_signal (see struct player's own doc
 * comment), so this reuses exactly that flag: pitch holds at 0
 * (paused, ready for the needle to be dropped) until the needle
 * actually provides its next real reading. Outside the lock, same
 * reasoning as player_set_track()'s own write to this field - a plain
 * flag, not one the lock protects.
 */
static void player_jump_to_position(struct player *pl, double to)
{
    if (spin_try_lock(&pl->lock)) {
        pl->position = to;
        spin_unlock(&pl->lock);
    }
    if (!pl->timecode_valid)
        pl->relative_awaiting_signal = true;

    /* Pi DVS: no longer clears the loop here (2026-08-04, reverted the
     * SAME day it was added) - owner's revised call: a SEEK/GOTO_CUE
     * landing outside the loop's own range should leave the loop armed,
     * not cancel it outright, resuming automatically once real
     * playback/scratching brings position back inside [loop_start,
     * loop_end). See player_collect()'s own was_in_loop gate, which is
     * what actually fixes the original "loop fights the jump" bug now
     * - by no longer touching pl->position at all while it's outside
     * the loop's range, not by clearing loop_active. */
}

/*
 * Pi DVS: jump to an arbitrary elapsed-time offset within the track
 * and pause there (owner's spec, 2026-08-03) - the primitive behind
 * tap-to-seek and drag-to-position (see player_jump_to_position()'s
 * own doc comment for the pause mechanics). Elapsed-space (not
 * position-space) since that's the convention every caller above this
 * layer already thinks in - player_get_elapsed(), STATUS's own
 * <remain> field, LOOP's start/end.
 */
void player_seek_to_elapsed(struct player *pl, double elapsed_seconds)
{
    player_jump_to_position(pl, pl->offset + elapsed_seconds);
}

/*
 * Pi DVS: store an explicit elapsed-time offset as the cue point
 * (owner's spec, 2026-08-03) - SET_CUE. Takes the target directly
 * rather than always using the CURRENT position (the original design)
 * - the app needs to be able to snap a cue point to the nearest beat-
 * grid tick before storing it, and xwax itself has no notion of tempo/
 * bars to do that snapping here (same "the caller decides the range,
 * xwax just applies it" reasoning as player_set_loop()'s own doc
 * comment). Position-space internally (offset already applied),
 * matching player_set_loop()'s own loop_start/loop_end convention, so
 * player_cue()/player_cue_play() below never need to repeat that
 * arithmetic.
 *
 * Plain field write, not lock-protected - called from handle_set_cue(),
 * already on the realtime thread itself, same reasoning as
 * player_set_loop()/player_set_relative_mode() above.
 */
void player_set_cue_point(struct player *pl, double elapsed_seconds)
{
    pl->cue_point = pl->offset + elapsed_seconds;
}

/*
 * Pi DVS: the cue point's own elapsed-time value (owner's spec,
 * 2026-08-03) - reported back via STATUS so the app can draw a cue
 * marker without needing to track what it last set client-side (which
 * would go stale across a reconnect, or across leaving and re-entering
 * relative mode - see server/src/deck-status-poller.js's own doc
 * comment on why STATUS fields generally get read back rather than
 * tracked locally). Reads `cue_point`/`offset` unlocked, same
 * reasoning as player_get_elapsed() elsewhere in this file.
 */
double player_get_cue_point_elapsed(struct player *pl)
{
    return pl->cue_point - pl->offset;
}

/*
 * Pi DVS: the active loop's own state, reported back via STATUS
 * (owner's spec, 2026-08-04) - same "read it back rather than track it
 * locally" idiom as player_get_cue_point_elapsed() above, fixing a
 * real bug: the app's own loop-active flag was pure client @State with
 * nothing to read it back FROM, so it silently went stale (always
 * false) across an app restart while xwax kept faithfully looping
 * underneath it - confusing, and left the loop's own green waveform
 * highlight missing despite the loop still very much being active.
 * Start/end fixed at 0.000 whenever inactive, matching cue_point's own
 * "meaningless while inactive" convention, rather than reporting
 * whatever range happened to be set last. Reads unlocked, same
 * reasoning as player_get_cue_point_elapsed() above.
 */
bool player_get_loop_active(struct player *pl)
{
    return pl->loop_active;
}

double player_get_loop_start_elapsed(struct player *pl)
{
    return pl->loop_active ? pl->loop_start - pl->offset : 0.0;
}

double player_get_loop_end_elapsed(struct player *pl)
{
    return pl->loop_active ? pl->loop_end - pl->offset : 0.0;
}

/*
 * Pi DVS: "full internal mode" (owner's spec, 2026-08-04) - see
 * struct player's own doc comment on timecode_disabled. Plain field
 * write, not lock-protected - called from handle_timecode() below,
 * already on the realtime thread itself, same reasoning as
 * player_set_loop()/player_set_relative_mode() above. Deliberately NOT
 * reset by player_set_track() (unlike loop_active/cue_point) - this is
 * a persistent, box-wide mode choice, not something scoped to one
 * track's own load, same treatment relative_mode itself already gets.
 */
void player_set_timecode_disabled(struct player *pl, bool on)
{
    pl->timecode_disabled = on;
}

bool player_get_timecode_disabled(struct player *pl)
{
    return pl->timecode_disabled;
}

/*
 * Pi DVS: jump to the stored cue point and pause there (owner's spec,
 * 2026-08-03) - GOTO_CUE, replacing the old fixed player_cue_to_start()
 * (which this now subsumes: `cue_point` defaults to the track's own
 * start point - see struct player's own doc comment - so this does
 * exactly what the old function did until SET_CUE is ever sent).
 */
void player_cue(struct player *pl)
{
    player_jump_to_position(pl, pl->cue_point);
}

/*
 * Pi DVS: jump to the stored cue point and start playing FROM there,
 * even with no real needle signal present (owner's spec, 2026-08-03) -
 * PLAY_CUE, a genuine digital/software-driven playback, unlike every
 * other position-setting function in this file which pauses.
 *
 * Deliberately does NOT use player_jump_to_position() above - that
 * always SETS relative_awaiting_signal when the needle isn't valid
 * (pausing); this does the opposite, CLEARING it. Relative mode's
 * existing "lift the needle, keep playing" behaviour (see
 * sync_to_timecode_relative()) already holds pitch at 1.0 the moment
 * relative_awaiting_signal is false and no real needle signal is
 * present - that's exactly the digital playback this button needs, no
 * new pitch-source machinery required, this just reuses the mechanism
 * that already exists for "scratch, then lift the needle and let it
 * keep going". If the needle IS down and valid, sync_to_timecode_
 * relative() uses the real reading regardless, same as always - this
 * never fights a real, present signal.
 */
void player_cue_play(struct player *pl)
{
    if (spin_try_lock(&pl->lock)) {
        pl->position = pl->cue_point;
        spin_unlock(&pl->lock);
    }
    pl->relative_awaiting_signal = false;

    /* Pi DVS: no longer clears the loop here either (2026-08-04,
     * reverted the same day it was added) - same revised reasoning as
     * player_jump_to_position()'s own doc comment. */
}

/*
 * Set the track used for the playback
 *
 * Pre: caller holds reference on track
 * Post: caller does not hold reference on track
 */

void player_set_track(struct player *pl, struct track *track)
{
    struct track *x;

    assert(track != NULL);
    assert(track->refcount > 0);

    spin_lock(&pl->lock); /* Synchronise with the playback thread */
    x = pl->track;
    pl->track = track;

    /* Pi DVS (owner's call, 2026-07-29): if the needle isn't currently
     * providing a valid timecode reading (lifted, or never dropped),
     * `position` is just whatever was last measured for a PREVIOUS
     * load - it has no relationship to this new one. Left alone, the
     * new track would silently start playback wherever that stale
     * reading happens to land, not at its own beginning - confirmed
     * as a real, confusing thing on real hardware (lift the needle
     * after playing a track, load a new one, it starts mid-track at
     * the old track's last position). Resetting `position` to exactly
     * `offset` makes player_get_elapsed() (position - offset) read 0,
     * ie. "start of track" - not touching `offset` itself, which must
     * stay the fixed calibration constant it's always been (see its
     * own doc comment). The instant the needle DOES provide a real
     * reading again, retarget()/sync_to_timecode() overwrite this
     * immediately with the real absolute position, same as any other
     * needle drop - this is only ever the starting value while
     * genuinely nothing better is known. */
    if (!pl->timecode_valid)
        pl->position = pl->offset;

    spin_unlock(&pl->lock);

    /* Pi DVS: same "don't silently inherit stale state from the
     * previous load" reasoning as the position reset just above, for
     * relative mode's pitch instead of absolute mode's position - see
     * struct player's own doc comment on relative_awaiting_signal.
     * Outside the lock, matching every other simple flag this player
     * struct already reads/writes cross-thread unlocked (relative_mode,
     * timecode_valid, recalibrate) - the lock protects the
     * position/track pair specifically, not every field. Harmless to
     * set unconditionally even outside relative mode - only ever
     * consulted from sync_to_timecode_relative(). */
    pl->relative_awaiting_signal = true;

    /* Pi DVS: a loop bound to the PREVIOUS track's own timing is
     * meaningless for whatever's being loaded now - same "don't
     * silently inherit stale state" reasoning as relative_awaiting_
     * signal just above. Plain flag, same lock status as that field. */
    pl->loop_active = false;

    /* Pi DVS: same "don't silently inherit stale state" reasoning
     * again - a cue point bound to the previous track's own timing is
     * meaningless here too. Defaults to `offset` (this new track's own
     * start point), matching player_init()'s own default. */
    pl->cue_point = pl->offset;

    track_release(x); /* discard the old track */
}

/*
 * Set the playback of one player to match another, used
 * for "instant doubles" and beat juggling
 */

void player_clone(struct player *pl, const struct player *from)
{
    double elapsed;
    struct track *x, *t;

    elapsed = from->position - from->offset;
    pl->offset = pl->position - elapsed;

    t = from->track;
    track_acquire(t);

    spin_lock(&pl->lock);
    x = pl->track;
    pl->track = t;
    spin_unlock(&pl->lock);

    track_release(x);
}

/*
 * Synchronise to the position and speed given by the timecoder
 *
 * Return: 0 on success or -1 if the timecoder is not currently valid
 */

static int sync_to_timecode(struct player *pl)
{
    double when, tcpos;
    signed int timecode;

    timecode = timecoder_get_position(pl->timecoder, &when);

    /* Pi DVS: the one place this gets set - see struct player's own
     * doc comment. Deliberately BEFORE the safe-zone early return
     * below, so a needle that's down but past the safe zone still
     * correctly counts as "signal present" here (it's genuinely
     * reading something, just not something sync_to_timecode() itself
     * wants to act on) - distinct from timecode == -1, which means no
     * reading at all. */
    pl->timecode_valid = (timecode != -1);

    /* Instruct the caller to disconnect the timecoder if the needle
     * is outside the 'safe' zone of the record */

    if (timecode != -1 && timecode > timecoder_get_safe(pl->timecoder))
        return -1;

    /* If the timecoder is alive, use the pitch from the sine wave */

    pl->pitch = timecoder_get_pitch(pl->timecoder);

    /* If we can read an absolute time from the timecode, then use it */

    if (timecode == -1) {
        pl->target_position = TARGET_UNKNOWN;
    } else {
        tcpos = (double)timecode / timecoder_get_resolution(pl->timecoder);
        pl->target_position = tcpos + pl->pitch * when;
    }

    return 0;
}

/*
 * Pi DVS: update pitch from the timecoder while in relative mode.
 * Unlike sync_to_timecode(), this never sets target_position - so
 * retarget() never runs and never pulls `position` back toward an
 * absolute reading, which is the whole point of relative mode
 * (position just free-runs from whatever `pitch` integrates to in
 * build_pcm(), the same "clock decoupled from timecode" mechanism
 * every mode already uses, just never corrected back).
 *
 * When the needle isn't providing a valid reading (lifted), pitch
 * holds at 1.0 rather than adopting the timecoder's own raw/filtered
 * value - that filter (see pitch.h) is continuously fed "no movement"
 * observations while lifted and decays toward 0, which is exactly the
 * "stop" behaviour absolute mode wants but relative mode explicitly
 * shouldn't have (owner's spec: lifting the needle should leave the
 * track playing, not pause it) - EXCEPT immediately after a fresh
 * load (relative_awaiting_signal - see struct player's own doc
 * comment), where holding at 1.0 would wrongly auto-play a track that
 * was never actually cued by a real needle reading, just inheriting
 * whatever the PREVIOUS track happened to be doing. Pitch holds at 0
 * (paused) instead until the needle actually provides a first real
 * reading for THIS load, at which point normal "keep playing through
 * a lift" behaviour resumes as usual.
 */
static void sync_to_timecode_relative(struct player *pl)
{
    double when;
    signed int timecode;

    /* Pi DVS: "full internal mode" (owner's spec, 2026-08-04) - while
     * timecode_disabled, never consult the real timecoder reading at
     * all, behaving exactly as if the needle were permanently lifted
     * (see the "needle isn't providing a valid reading" branch below,
     * same logic) regardless of what's actually on the ADC8x input. */
    if (pl->timecode_disabled) {
        pl->timecode_valid = false;
        pl->pitch = pl->relative_awaiting_signal ? 0.0 : 1.0;
        return;
    }

    timecode = timecoder_get_position(pl->timecoder, &when);
    pl->timecode_valid = (timecode != -1);

    if (pl->timecode_valid) {
        pl->pitch = timecoder_get_pitch(pl->timecoder);
        pl->relative_awaiting_signal = false;
    } else if (pl->relative_awaiting_signal) {
        pl->pitch = 0.0;
    } else {
        pl->pitch = 1.0;
    }
}

/*
 * Synchronise to the position given by the timecoder without
 * affecting the audio playback position
 */

static void calibrate_to_timecode_position(struct player *pl)
{
    assert(pl->target_position != TARGET_UNKNOWN);

    /* Pi DVS: deliberately do NOT adjust ->offset here. Upstream
     * xwax shifts it to make wherever the needle happens to be at
     * first lock into "track position 0" - a silent auto-recue to
     * "now" rather than to the record's real, fixed start. offset
     * must stay at its initialised value (0.0 by default, or a fixed
     * --cue-offset calibration constant - see player_init()) forever,
     * so that (position - offset) always equals the true, absolute,
     * decoded timecode position, corrected by the same fixed constant
     * every time - vinyl position 0 is always the same track
     * position, on every copy, permanently (see CLAUDE.md's
     * "Needle position" section). The position snap below is still
     * correct and wanted: it's an instant, accurate jump to the real
     * position on first lock, not a gradual catch-up. */
    pl->position = pl->target_position;
}

void retarget(struct player *pl)
{
    double diff;

    if (pl->recalibrate) {
        calibrate_to_timecode_position(pl);
        pl->recalibrate = false;
    }

    /* Calculate the pitch compensation required to get us back on
     * track with the absolute timecode position */

    diff = pl->position - pl->target_position;
    pl->last_difference = diff; /* to print in user interface */

    if (fabs(diff) > SKIP_THRESHOLD) {

        /* Jump the track to the time */

        pl->position = pl->target_position;
        fprintf(stderr, "Seek to new position %.2lfs.\n", pl->position);

    } else if (fabs(pl->pitch) > SYNC_PITCH) {

        /* Re-calculate the drift between the timecoder pitch from
         * the sine wave and the timecode values */

        pl->sync_pitch = pl->pitch / (diff / SYNC_TIME + pl->pitch);

    }
}

/*
 * Seek to the given position
 */

void player_seek_to(struct player *pl, double seconds)
{
    pl->offset = pl->position - seconds;
}

/*
 * Get a block of PCM audio data to send to the soundcard
 *
 * This is the main function which retrieves audio for playback.  The
 * clock of playback is decoupled from the clock of the timecode
 * signal.
 *
 * Post: buffer at pcm is filled with the given number of samples
 */

void player_collect(struct player *pl, signed short *pcm, unsigned samples)
{
    double r, pitch, dt, target_volume;

    dt = pl->sample_dt * samples;

    /* Pi DVS: relative mode is checked first and is an independent
     * override, not a variant of timecode_control - see struct
     * player's own doc comment. timecode_control's existing absolute/
     * internal-playback behaviour is otherwise completely unchanged. */
    if (pl->relative_mode) {
        sync_to_timecode_relative(pl);
    } else if (pl->timecode_control) {
        if (sync_to_timecode(pl) == -1)
            pl->timecode_control = false;
    }

    if (pl->target_position != TARGET_UNKNOWN) {

        /* Bias the pitch towards a known target, and acknowledge that
         * we did so */

        retarget(pl);
        pl->target_position = TARGET_UNKNOWN;

    } else {

        /* Without a known target, tend sync_pitch towards 1.0, to
         * avoid using outlier values from scratching for too long */

        pl->sync_pitch += dt / (SYNC_RC + dt) * (1.0 - pl->sync_pitch);
    }

    target_volume = fabs(pl->pitch) * VOLUME;
    if (target_volume > 1.0)
        target_volume = 1.0;

    /* Sync pitch is applied post-filtering */

    pitch = pl->pitch * pl->sync_pitch;

    /* We must return audio immediately to stay realtime. A spin
     * lock protects us from changes to the audio source */

    if (!spin_try_lock(&pl->lock)) {
        r = build_silence(pcm, samples, pl->sample_dt, pitch);
    } else {
        r = build_pcm(pcm, samples, pl->sample_dt, pl->track,
                      pl->position - pl->offset, pitch,
                      pl->volume, target_volume);
        spin_unlock(&pl->lock);
    }

    /* Pi DVS (2026-08-04): captured BEFORE pl->position advances below,
     * deliberately - see the wraparound block's own doc comment for
     * why this gates it. */
    bool was_in_loop = pl->relative_mode && pl->loop_active
        && pl->position >= pl->loop_start && pl->position < pl->loop_end;

    pl->position += r;
    pl->volume = target_volume;

    /* Pi DVS: loop wraparound (owner's spec, 2026-08-02, revised
     * 2026-08-04) - relative mode only, see player_set_loop()'s own
     * doc comment for why. fmod-based rather than a flat snap to
     * loop_start/loop_end, so a single buffer that advances (or,
     * scratching backward, retreats) past more than the loop's own
     * length still wraps to the correct PHASE within the loop instead
     * of always landing exactly on its edge - matters at high scratch
     * speeds, negligible at normal playback pitch. Symmetric: handles
     * both a forward loop-end crossing and a backward loop-start
     * crossing, since relative mode's pitch (and therefore this loop)
     * can run in either direction under a real scratch.
     *
     * Gated on was_in_loop (owner's spec, 2026-08-04: "the loop stays
     * active but is only acted upon if the position marker is within
     * the loop area... it just activates again if the position goes
     * inside the loop area again") - revised from an earlier version
     * that applied this unconditionally to ANY out-of-range position,
     * which fought a deliberate SEEK/GOTO_CUE/PLAY_CUE landing outside
     * the loop every single buffer (confirmed as a real, disorienting
     * bug on real hardware - see player_jump_to_position()/player_cue_
     * play(), which used to call player_clear_loop() to work around
     * exactly this, now reverted since the loop no longer needs
     * clearing to escape it). Checking the PRE-advance position (not
     * the post-advance one already written to pl->position above)
     * means a jump that lands outside the loop just plays on normally
     * - was_in_loop is false for every subsequent buffer until real
     * playback/scratching brings position back inside [loop_start,
     * loop_end), at which point it naturally starts wrapping at the
     * edges again, same as if the loop had never been left. */
    if (was_in_loop) {
        double loop_length = pl->loop_end - pl->loop_start;

        if (loop_length > 0) {
            if (pl->position >= pl->loop_end)
                pl->position = pl->loop_start + fmod(pl->position - pl->loop_start, loop_length);
            else if (pl->position < pl->loop_start)
                pl->position = pl->loop_end - fmod(pl->loop_start - pl->position, loop_length);
        }
    }
}
