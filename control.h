/*
 * Copyright (C) 2026 Pi DVS project
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

#ifndef CONTROL_H
#define CONTROL_H

struct controller;
struct rt;

/*
 * A minimal Unix-socket controller for remote track loading and
 * status queries - see CLAUDE.md's "xwax fork" section. One socket =
 * one deck (no <deck> parameter in the protocol - the socket path
 * itself identifies which deck this is), matching "one xwax process
 * per deck".
 *
 * Commands, one per line:
 *   LOAD <path>   - load a track from a bare filepath (see do_load())
 *   UNLOAD        - clear the deck back to empty (see deck_unload(),
 *                    do_unload()) - used to hand the DAC output over
 *                    to a passthrough loop (alsaloop, managed by
 *                    Node - see server/src/passthrough.js and
 *                    CLAUDE.md's "Passthrough" section), since xwax's
 *                    own playback and a real vinyl passthrough can't
 *                    both drive the same output at once. No reply;
 *                    a following STATUS will show EMPTY once it's
 *                    taken effect.
 *   STATUS        - reply with "STATUS EMPTY 0.0 0.000 0\n" if nothing's
 *                    loaded, "STATUS IMPORTING 0.0 0.000 0 <path>\n" if a
 *                    LOAD was issued but xwax's own import subprocess
 *                    is still decoding it (see track_is_importing() -
 *                    track->length only reflects however much has
 *                    decoded SO FAR during this window, so remain
 *                    would otherwise be a real but meaningless,
 *                    steadily-growing number, confirmed as a real,
 *                    confusing thing to show on real hardware), or
 *                    "STATUS <PLAYING|STOPPED> <remain> <pitch> <relative> <path>\n"
 *                    once import's done. <remain> is seconds left in
 *                    the loaded track, clamped to >= 0. PLAYING/STOPPED
 *                    reflects player_is_active() - whether the
 *                    platter's currently spinning fast enough to be
 *                    "on", not just whether a track is loaded. <pitch>
 *                    is struct player's own `pitch` field (from the
 *                    timecoder, updated every real-time audio buffer) -
 *                    positive for forward, negative for reverse,
 *                    magnitude is speed relative to normal (1.0 = real
 *                    time, 0 = stationary) - added 2026-07-27 so a
 *                    client can interpolate a scrub position accurately
 *                    BETWEEN polls instead of assuming steady 1x
 *                    forward motion; read the same lock-free way
 *                    player_is_active() already reads it one line away,
 *                    not a new access pattern. EMPTY/IMPORTING report a
 *                    fixed 0.000 - meaningless in those states, kept
 *                    only for a consistent field shape. <relative>
 *                    (added 2026-08-01) is 0/1, struct player's own
 *                    relative_mode - see RELATIVE below. <path> is the
 *                    loaded file's path, unquoted and always the last
 *                    field (may contain spaces, never a newline) - lets
 *                    a client recover "what's actually loaded on this
 *                    deck" after its OWN restart, since xwax is the one
 *                    thing that keeps running (and keeps the real
 *                    answer) through a Node or app restart.
 *
 * There's deliberately no PASSTHRU command here - passthrough is
 * implemented outside xwax entirely (UNLOAD plus an external alsaloop
 * process, see above), not as a mode xwax itself knows about.
 *
 *   RELATIVE ON|OFF - toggle relative mode (see player_set_relative_
 *                    mode()/struct player's own doc comment for the
 *                    full design). While ON, the needle still drives
 *                    live pitch/scratch when it's down, but its
 *                    absolute position is never consulted, so lifting
 *                    it simply leaves the track playing rather than
 *                    stopping it - unlike normal absolute mode. No
 *                    reply either way - a client learns the current
 *                    state from STATUS's own <relative> field above,
 *                    same as every other piece of deck state (Node/
 *                    the app don't need to track their own request
 *                    separately). Switching back OFF is a plain snap
 *                    to wherever the needle currently reads (once it
 *                    next provides a valid
 *                    reading, if it isn't already) - deliberately not
 *                    an offset-preserving continuation, owner's call.
 *
 *   CUE           - jump back to the track's own start point (see
 *                    player_cue_to_start()). Mainly for relative mode,
 *                    where there's no needle position to fall back on
 *                    to get back to the beginning, unlike absolute
 *                    mode. No reply - a client sees the jump reflected
 *                    in STATUS's own <remain> on the next poll, same
 *                    "read it back rather than track it locally" idiom
 *                    as RELATIVE above.
 */
int control_init(struct controller *c, struct rt *rt, const char *path);

#endif
