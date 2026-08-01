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
    if (!on)
        pl->recalibrate = true;
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
 * Pi DVS: jump back to the track's own start point - the opposite of
 * player_recue() above (which redefines "here" as the new start;
 * this jumps TO the existing one). Needed specifically for relative
 * mode: absolute mode always has a real needle position to fall back
 * on, but relative mode's position free-runs with nothing to
 * physically return it to zero, so a manual cue is the only way back
 * to the beginning (owner's spec, 2026-08-01).
 *
 * Locked, unlike player_seek_to()/player_recue() above - those only
 * ever write `offset`, but this writes `position` directly, the same
 * field player_collect() reads under this same lock (via
 * spin_try_lock) to build audio - see player_set_track()'s own
 * position write for the established precedent.
 */
void player_cue_to_start(struct player *pl)
{
    spin_lock(&pl->lock);
    pl->position = pl->offset;
    spin_unlock(&pl->lock);
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

    pl->position += r;
    pl->volume = target_volume;
}
