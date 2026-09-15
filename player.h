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

#include "keylock.h"
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
        loop_active; /* [loop_start, loop_end) below - only consulted while relative_mode is also on */

    /*
     * Seconds spent with a turning platter whose POSITION will not decode.
     *
     * The timecoder reads speed off the control signal's sine carrier, but position off a
     * side-specific code sequence - so a record whose side does not match `--timecode` gives a
     * perfectly good pitch and no position at all. The deck then reports PLAYING with a real pitch
     * and a moving playhead while player_collect() outputs silence, which reads as a dead output
     * rather than as a misconfiguration (owner-reported, 2026-09-15: "I don't seem to be getting
     * any output on deck B... vinyl control is working fine").
     *
     * A damaged or dirty record and an outright wrong timecode format look the same from here, and
     * all three want the same message, so this counts the condition rather than naming a cause.
     * Reset the moment a position decodes, so an ordinary needle drop never accumulates.
     */
    double unreadable_seconds;

    /* Seconds below NEEDLE_STOPPED_PITCH - see that constant for why this is timed, not instant. */
    double stopped_for;

    double loop_start, loop_end; /* seconds, position-space, valid only while loop_active */

    double cue_point; /* position-space; defaults to `offset` until SET_CUE is ever sent - see PROTOCOL.md */

    bool key_lock; /* hold the track's ORIGINAL pitch while the platter changes tempo - see keylock.h */
    struct keylock keylock;
};

void player_set_key_lock(struct player *pl, bool on);

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
