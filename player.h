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
        offset, /* track start point in timecode - fixed for the life of the player, never adjusted after init */
        last_difference, /* last known position minus target_position */
        pitch, /* from timecoder */
        sync_pitch, /* pitch required to sync to timecode signal */
        volume;

    /* Timecode control */

    struct timecoder *timecoder;
    bool timecode_control,
        recalibrate, /* re-sync offset at next opportunity */
        timecode_valid, /* needle down and reading a valid locked position - not the same as pitch~=0, which is also true when paused */
        relative_mode, /* needle drives live pitch/scratch but its absolute position is never consulted - see PROTOCOL.md's RELATIVE */
        relative_awaiting_signal, /* true from a fresh load until the needle's first valid reading - holds pitch at 0 instead of relative mode's usual "lift needle, keep playing" 1.0, so a new load doesn't inherit the previous track's pitch */
        loop_active; /* [loop_start, loop_end) below - only consulted while relative_mode is also on */

    double loop_start, loop_end; /* seconds, position-space, valid only while loop_active */

    double cue_point; /* position-space; defaults to `offset` until SET_CUE is ever sent - see PROTOCOL.md */
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

/* Reported back via STATUS (see PROTOCOL.md) - 0/0.000/0.000 whenever loop_active is false. */
bool player_get_loop_active(struct player *pl);
double player_get_loop_start_elapsed(struct player *pl);
double player_get_loop_end_elapsed(struct player *pl);

void player_set_track(struct player *pl, struct track *track);
void player_clone(struct player *pl, const struct player *from);

double player_get_position(struct player *pl);
double player_get_elapsed(struct player *pl);
double player_get_remain(struct player *pl);
bool player_is_active(const struct player *pl);

void player_seek_to(struct player *pl, double seconds);
void player_recue(struct player *pl);

/* Single settable cue point - see PROTOCOL.md. Writes `position` directly rather than mutating `offset` (deck.c/cues.c's upstream cue system does the latter, which conflicts with this fork's "offset never changes after init" invariant). */
void player_seek_to_elapsed(struct player *pl, double elapsed_seconds);
void player_set_cue_point(struct player *pl, double elapsed_seconds);
double player_get_cue_point_elapsed(struct player *pl);
void player_cue(struct player *pl);
void player_cue_play(struct player *pl);

void player_collect(struct player *pl, signed short *pcm, unsigned samples);

#endif
