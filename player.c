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
    unsigned int length;
    double sample, step, vol, gradient;

    /* Paired with track.c's __ATOMIC_RELEASE store on tr->length - see that comment. */
    length = __atomic_load_n(&tr->length, __ATOMIC_ACQUIRE);

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
            if (sa < 0 || sa >= length) {
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
    /* cue_offset is a fixed calibration constant (--cue-offset), not a dynamic recue - see DEVLOG.md. */
    pl->offset = cue_offset;
    pl->target_position = TARGET_UNKNOWN;
    pl->last_difference = 0.0;

    pl->timecode_valid = false;
    pl->relative_mode = false;
    pl->relative_playing = false;
    pl->loop_active = false;
    pl->loop_start = 0.0;
    pl->loop_end = 0.0;
    pl->cue_point = pl->offset;

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

/* Relative mode on/off - see PROTOCOL.md's RELATIVE. Turning off reuses timecode_control's own recalibrate snap. */
void player_set_relative_mode(struct player *pl, bool on)
{
    pl->relative_mode = on;
    if (!on) {
        pl->recalibrate = true;
        pl->loop_active = false; /* a loop only ever applies in relative mode */
    }
}

/* Activate a loop over [start_seconds, end_seconds) of elapsed time - see PROTOCOL.md's LOOP. Plain field writes: already on the realtime thread (control.c), not lock-protected. */
void player_set_loop(struct player *pl, double start_seconds, double end_seconds)
{
    pl->loop_start = pl->offset + start_seconds;
    pl->loop_end = pl->offset + end_seconds;
    pl->loop_active = true;
}

/* Deactivate a loop - playback continues from wherever `position` currently is, no jump. */
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
    /* Deliberately NOT clamped to the track duration, so this reads as MORE than the duration
     * while the needle is still in the cue offset's pre-roll (position < offset).
     *
     * Clamping it here was tried on 2026-08-22 and made no difference, because the client already
     * floors its own derived position at zero - so all the clamp achieved was destroying the
     * information the client needs to tell "before the track starts" from "at the start". It needs
     * that to know not to extrapolate the playhead forward through a stretch where the track isn't
     * advancing; without it the estimate crept forward and got yanked back every STATUS tick,
     * which is what the jitter actually was. See DEVLOG 2026-08-22. */
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

/* Shared primitive behind player_seek_to_elapsed()/player_cue() - jumps `position` and pauses,
 * unconditionally in relative mode (owner's call, 2026-08-06 - see relative_playing's own doc
 * comment). Must use spin_try_lock(), not spin_lock() - this runs on the realtime thread, and
 * spin_lock() aborts the process if called there (see DEVLOG.md for the real crash this caused). */
static void player_jump_to_position(struct player *pl, double to)
{
    if (spin_try_lock(&pl->lock)) {
        pl->position = to;
        spin_unlock(&pl->lock);
    }
    pl->relative_playing = false;

    /* Does NOT clear the loop (reverted 2026-08-04) - a jump outside the loop's range now just
     * leaves it armed rather than cancelling it; see player_collect()'s was_in_loop gate. */
}

/* Jump to an elapsed-time offset and pause - the primitive behind tap-to-seek/drag-to-position. */
void player_seek_to_elapsed(struct player *pl, double elapsed_seconds)
{
    player_jump_to_position(pl, pl->offset + elapsed_seconds);
}

/* Jump to an elapsed-time offset WITHOUT touching play/pause state - unlike
 * player_seek_to_elapsed()/player_jump_to_position(), doesn't force a pause: whatever's currently
 * holding (playing or paused) keeps holding. RELOCATE (see PROTOCOL.md) - used to keep a shrinking
 * loop's own position inside its new bounds without interrupting playback if it was already
 * playing (or start it paused if it wasn't - either way, this only moves `position`). */
void player_relocate(struct player *pl, double elapsed_seconds)
{
    if (spin_try_lock(&pl->lock)) {
        pl->position = pl->offset + elapsed_seconds;
        spin_unlock(&pl->lock);
    }
}

/* Store an explicit elapsed-time cue point - SET_CUE (see PROTOCOL.md). Plain field write,
 * already on the realtime thread. Takes the target directly since xwax has no beat-grid of its own. */
void player_set_cue_point(struct player *pl, double elapsed_seconds)
{
    pl->cue_point = pl->offset + elapsed_seconds;
}

/* Reported back via STATUS rather than tracked client-side (see PROTOCOL.md). */
double player_get_cue_point_elapsed(struct player *pl)
{
    return pl->cue_point - pl->offset;
}

/* Reported back via STATUS - see PROTOCOL.md and player.h's own doc comment. */
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

/* Jump to the stored cue point and pause - GOTO_CUE, subsumes the old fixed cue-to-start. */
void player_cue(struct player *pl)
{
    player_jump_to_position(pl, pl->cue_point);
}

/* Jump to the cue point and start playing immediately - PLAY_CUE. Deliberately doesn't use
 * player_jump_to_position() - sets relative_playing instead of clearing it, so relative mode's
 * live-needle pitch kicks in (or holds at 1.0 with the needle up) right away. Resets pitch to 1.0
 * as this play's own starting baseline - see sync_to_timecode_relative()'s own comment for why
 * that function no longer does this itself once already playing. */
void player_cue_play(struct player *pl)
{
    if (spin_try_lock(&pl->lock)) {
        pl->position = pl->cue_point;
        spin_unlock(&pl->lock);
    }
    pl->relative_playing = true;
    pl->pitch = 1.0;

    /* Loop also not cleared here (reverted 2026-08-04) - same reasoning as player_jump_to_position(). */
}

/* Resume digital playback from wherever `position` already is - PLAY. Same relative_playing set
 * and pitch reset as player_cue_play(), but no jump: unlike Play from Cue, this isn't tied to the
 * cue point at all. The needle only modulates pitch from here - see relative_playing's own doc
 * comment. */
void player_play(struct player *pl)
{
    pl->relative_playing = true;
    pl->pitch = 1.0;
}

/* Pause at wherever `position` already is - PAUSE. Holds regardless of needle position - see
 * relative_playing's own doc comment for why this changed from the old "only holds if the
 * needle's up" behaviour (owner's call, 2026-08-06). */
void player_pause(struct player *pl)
{
    pl->relative_playing = false;
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

    /* If the needle isn't currently valid, `position` is stale from a previous load - reset to
     * `offset` (track start) rather than silently starting mid-track. Not touching `offset` itself. */
    if (!pl->timecode_valid)
        pl->position = pl->offset;

    spin_unlock(&pl->lock);

    /* A fresh load starts paused - no reason a newly loaded track should play itself, and this
     * also stops it inheriting the previous track's pitch. */
    pl->relative_playing = false;

    /* A loop bound to the previous track's timing is meaningless for whatever's loading now. */
    pl->loop_active = false;

    /* Same reasoning again - a cue point from the previous track is meaningless here too. */
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

    /* Set before the safe-zone check below, so a needle past the safe zone still counts as "signal present" here. */
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

/* Relative-mode equivalent of sync_to_timecode() - never sets target_position, so retarget()
 * never pulls `position` back to an absolute reading (position free-runs from pitch instead).
 *
 * relative_playing (PLAY/PLAY_CUE vs PAUSE/SEEK/GOTO_CUE/a fresh load) is checked first and is
 * the only thing that can hold pitch at a hard 0 - the needle never gets a vote on whether
 * playback is running, only on its speed once it already is (owner's call, 2026-08-06: the vinyl
 * is a controller for pitch/mixing, not a play/pause switch - see relative_playing's own doc
 * comment in player.h). While playing, pitch takes the live needle reading whenever the needle's
 * down, and simply holds at whatever it last was while the needle's lifted - not a fixed 1.0
 * (owner's call, 2026-08-06: dropping the needle back down should resume at the pitch it was
 * left at, not silently snap the track back to its original tempo). player_play()/
 * player_cue_play() reset pitch to 1.0 as each play's own starting baseline, so a deck that's
 * never had a needle reading yet still plays at normal speed rather than sitting at whatever
 * pitch happened to be left over from before. */
static void sync_to_timecode_relative(struct player *pl)
{
    double when;
    signed int timecode;

    timecode = timecoder_get_position(pl->timecoder, &when);
    pl->timecode_valid = (timecode != -1);

    if (!pl->relative_playing) {
        pl->pitch = 0.0;
    } else if (pl->timecode_valid) {
        pl->pitch = timecoder_get_pitch(pl->timecoder);
    }
    /* else: needle's up while playing - hold the last known pitch (from a live reading, or the
     * 1.0 baseline set by player_play()/player_cue_play()), don't reset it. */
}

/*
 * Synchronise to the position given by the timecoder without
 * affecting the audio playback position
 */

static void calibrate_to_timecode_position(struct player *pl)
{
    assert(pl->target_position != TARGET_UNKNOWN);

    /* Deliberately does NOT adjust ->offset (unlike upstream xwax) - see DEVLOG.md for why
     * auto-recalibrating offset to the needle's first lock position was wrong for this project. */
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

    /* Relative mode is an independent override, checked first - timecode_control's own
     * absolute/internal-playback behaviour is otherwise completely unchanged. */
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

    /* Captured before pl->position advances below - see the wraparound block's was_in_loop gate. */
    bool was_in_loop = pl->relative_mode && pl->loop_active
        && pl->position >= pl->loop_start && pl->position < pl->loop_end;

    pl->position += r;
    pl->volume = target_volume;

    /* Loop wraparound (relative mode only). fmod-based, not a flat snap, so a buffer that jumps
     * more than the loop's own length still lands at the correct phase (matters when scratching).
     * Gated on was_in_loop (the PRE-advance position) so a deliberate SEEK/GOTO_CUE/PLAY_CUE
     * landing outside the loop just plays on rather than fighting the jump every buffer - see
     * DEVLOG.md for the real bug this fixes. */
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
