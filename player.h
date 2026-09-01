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

    /* The --cue-offset calibration: where the track's start sits on the timecode record. Set once
     * and never written again - `offset` below is the working value, which relative mode moves
     * about, and this is what absolute mode restores it from. */
    double cue_offset;

    struct timecoder *timecoder;
    bool timecode_control,
        recalibrate, /* re-sync offset at next opportunity */
        timecode_valid, /* needle down and reading a valid locked position - not the same as pitch~=0, which is also true when paused */
        position_known, /* the timecoder has decoded an ABSOLUTE position since the needle last stopped. False means we know how fast the needle is moving but not where it is, and must not play - see player_collect() */
        relative_mode, /* needle drives live pitch/scratch but its absolute position is never consulted - see PROTOCOL.md's RELATIVE */
        relative_playing, /* relative mode's own play/pause state - true only from PLAY/PLAY_CUE, false from PAUSE/SEEK/GOTO_CUE/a fresh load. The needle modulates pitch while this is true (live scratch) but never starts or stops playback itself - lifting it doesn't pause, and dropping it back down doesn't resume. Owner's call, 2026-08-06: the vinyl is a controller for pitch/mixing, not a play/pause switch. */
        loop_active; /* [loop_start, loop_end) below - applies in BOTH modes; how it wraps differs, see player_collect() */

    double loop_start, loop_end; /* seconds, position-space, valid only while loop_active */

    /* The needle's last VALID absolute reading while in relative mode, where nothing else consults
     * it. Kept so that turning tracking back on can adopt the needle where it currently is and
     * leave the track where it is playing, rather than snapping. Tracked continuously rather than
     * read at the moment of the switch, so a momentary undecodable patch right as the DJ taps the
     * toggle doesn't read as "needle up". `relative_needle_known` is false until the needle has
     * been down at least once since relative mode began. */
    double relative_needle_position;
    bool relative_needle_known;

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

/* How far `offset` has been slid away from the --cue-offset calibration, in seconds. Zero means
 * the needle's position and the track's agree; non-zero means looping (or a seek, in non-tracking
 * mode) has moved the track along the record by this much. Reported via STATUS so the app can show
 * it - the DJ cannot otherwise tell, and it is the number that warns them before the track's tail
 * runs past the end of the timecode. See PROTOCOL.md. */
double player_get_offset_drift(struct player *pl);

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
void player_relocate(struct player *pl, double elapsed_seconds);
void player_set_cue_point(struct player *pl, double elapsed_seconds);
double player_get_cue_point_elapsed(struct player *pl);
void player_cue(struct player *pl);
void player_cue_play(struct player *pl);

/* PLAY/PAUSE - digital transport control at whatever position `position` already holds, no jump.
 * Contrast with player_cue()/player_cue_play(), which both jump to the cue point first. */
void player_play(struct player *pl);
void player_pause(struct player *pl);

void player_collect(struct player *pl, signed short *pcm, unsigned samples);

#endif
