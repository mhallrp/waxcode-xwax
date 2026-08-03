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

#ifndef PLAYER_H
#define PLAYER_H

#include <stdbool.h>

#include "spin.h"
#include "track.h"

#define PLAYER_CHANNELS 2

struct player {
    double sample_dt;

    spin lock;
    struct track *track;

    /* Current playback parameters */

    double position, /* seconds */
        target_position, /* seconds, or TARGET_UNKNOWN */
        offset, /* track start point in timecode - see player_init()'s
                 * cue_offset parameter. Fixed for the life of the
                 * player once set; never adjusted again after init (in
                 * particular, calibrate_to_timecode_position() in
                 * player.c deliberately does not touch it - see that
                 * function's own comment) */
        last_difference, /* last known position minus target_position */
        pitch, /* from timecoder */
        sync_pitch, /* pitch required to sync to timecode signal */
        volume;

    /* Timecode control */

    struct timecoder *timecoder;
    bool timecode_control,
        recalibrate, /* re-sync offset at next opportunity */
        /* Pi DVS: whether the timecoder currently has a genuinely
         * valid, locked absolute position (needle down and reading,
         * see timecoder_get_position()'s -1 sentinel) - NOT the same
         * thing as `pitch` being near zero, which is also true for a
         * needle resting stationary on a still-valid position (paused,
         * not lifted). Updated every real-time buffer inside
         * sync_to_timecode(), read (lock-free, same established
         * pattern as `pitch` elsewhere - see control.c's STATUS reply)
         * by player_set_track() on the LOAD worker thread to decide
         * whether a fresh load should start at track position 0
         * rather than silently inheriting wherever the timecode
         * happened to leave off from whatever was loaded before. */
        timecode_valid,
        /* Pi DVS: "relative mode" (owner's spec, 2026-08-01) - unlike
         * timecode_control's absolute mode, the needle still drives
         * live pitch/scratch while it's down (see
         * sync_to_timecode_relative() in player.c), but its absolute
         * position is never consulted, so lifting the needle simply
         * leaves the track playing rather than stopping it. Checked
         * ahead of timecode_control in player_collect() - an
         * independent override, not a variant of the existing
         * absolute/internal binary, so that binary's own behaviour
         * stays provably unchanged for the (currently unused, but not
         * removed) case where something else still relies on it.
         * Returning to absolute mode is a plain position snap using
         * the SAME recalibrate/calibrate_to_timecode_position() path
         * timecode_control already uses when re-enabled from off -
         * see player_set_relative_mode() - deliberately not the
         * offset-preserving continuity an earlier design of this
         * feature considered and the owner decided against. */
        relative_mode,
        /* Pi DVS: set on every fresh player_set_track() load, cleared
         * the first time sync_to_timecode_relative() sees a genuinely
         * valid reading since that load. While set AND the needle
         * isn't currently valid, pitch is held at 0 (paused) instead
         * of relative mode's usual "lift the needle, keep playing" 1.0
         * - owner's spec, 2026-08-01: a freshly loaded track
         * inheriting whatever pitch the PREVIOUS track happened to be
         * moving at (eg. immediately auto-playing because the needle
         * was lifted and idling at 1.0 before this load) is a real
         * bug, not the intended "keep playing" behaviour, which is
         * only meant to apply once THIS track has actually had a real
         * needle reading at least once. Irrelevant outside relative
         * mode - only ever consulted from
         * sync_to_timecode_relative(). */
        relative_awaiting_signal,
        /* Pi DVS: a loop is active between [loop_start, loop_end)
         * below (owner's spec, 2026-08-02) - see player_set_loop()'s
         * own doc comment. Only ever consulted from player_collect(),
         * and only while relative_mode is also on - absolute mode's
         * position is dictated by the physical needle, there's
         * nothing here to loop against. */
        loop_active;

    double loop_start, loop_end; /* seconds, position-space (already
                                   * offset-adjusted) - valid only
                                   * while loop_active */

    /* Pi DVS: a single, settable cue point (owner's spec, 2026-08-03) -
     * see player_set_cue_point()/player_cue()/player_cue_play()'s own
     * doc comments. Position-space (offset already applied), matching
     * loop_start/loop_end's own convention. Defaults to `offset` (the
     * track's own start point) at init and on every fresh load - see
     * player_init()/player_set_track() - so CUE/CUE_PLAY do something
     * sensible ("go to the start") even before SET is ever pressed. */
    double cue_point;
};

void player_init(struct player *pl, unsigned int sample_rate,
                 struct track *track, struct timecoder *timecoder,
                 double cue_offset);
void player_clear(struct player *pl);

void player_set_timecoder(struct player *pl, struct timecoder *tc);
void player_set_timecode_control(struct player *pl, bool on);
bool player_toggle_timecode_control(struct player *pl);
void player_set_internal_playback(struct player *pl);
void player_set_relative_mode(struct player *pl, bool on);
void player_set_loop(struct player *pl, double start_seconds, double end_seconds);
void player_clear_loop(struct player *pl);

void player_set_track(struct player *pl, struct track *track);
void player_clone(struct player *pl, const struct player *from);

double player_get_position(struct player *pl);
double player_get_elapsed(struct player *pl);
double player_get_remain(struct player *pl);
bool player_is_active(const struct player *pl);

void player_seek_to(struct player *pl, double seconds);
void player_recue(struct player *pl);

/* Pi DVS (owner's spec, 2026-08-03): a single settable cue point,
 * replacing the earlier fixed "cue to start" (player_cue_to_start(),
 * retired) - see each function's own doc comment in player.c. NOT
 * built on the upstream cue-points system already present in deck.c/
 * cues.c (deck_cue() et al) - that mechanism works by mutating
 * `offset` dynamically, which conflicts directly with this fork's own
 * "offset never changes after init" invariant (see struct player's own
 * doc comment on `offset`, and calibrate_to_timecode_position()'s) -
 * these write `position` directly instead, the same approach
 * player_cue_to_start() already used safely. */
void player_seek_to_elapsed(struct player *pl, double elapsed_seconds);
void player_set_cue_point(struct player *pl, double elapsed_seconds);
double player_get_cue_point_elapsed(struct player *pl);
void player_cue(struct player *pl);
void player_cue_play(struct player *pl);

void player_collect(struct player *pl, signed short *pcm, unsigned samples);

#endif
