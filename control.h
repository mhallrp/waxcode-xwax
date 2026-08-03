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
 *   STATUS        - reply with "STATUS EMPTY 0.0 0.000 0 0.000 0 0.000
 *                    0.000\n" if nothing's loaded, "STATUS IMPORTING 0.0
 *                    0.000 0 0.000 0 0.000 0.000 <path>\n" if a LOAD was
 *                    issued but xwax's own import subprocess is still
 *                    decoding it (see track_is_importing() - track->
 *                    length only reflects however much has decoded SO
 *                    FAR during this window, so remain would otherwise
 *                    be a real but meaningless, steadily-growing number,
 *                    confirmed as a real, confusing thing to show on
 *                    real hardware), or "STATUS <PLAYING|STOPPED>
 *                    <remain> <pitch> <relative> <cuePoint> <loopActive>
 *                    <loopStart> <loopEnd> <path>\n" once import's done.
 *                    <remain> is seconds left in the loaded track,
 *                    clamped to >= 0. PLAYING/STOPPED reflects player_is_
 *                    active() - whether the platter's currently spinning
 *                    fast enough to be "on" (true for either a real
 *                    needle or an active PLAY_CUE - see below), not just
 *                    whether a track is loaded. <pitch> is struct
 *                    player's own `pitch` field (from the timecoder,
 *                    updated every real-time audio buffer) - positive
 *                    for forward, negative for reverse, magnitude is
 *                    speed relative to normal (1.0 = real time, 0 =
 *                    stationary) - added 2026-07-27 so a client can
 *                    interpolate a scrub position accurately BETWEEN
 *                    polls instead of assuming steady 1x forward motion;
 *                    read the same lock-free way player_is_active()
 *                    already reads it one line away, not a new access
 *                    pattern. EMPTY/IMPORTING report a fixed 0.000 -
 *                    meaningless in those states, kept only for a
 *                    consistent field shape. <relative> (added
 *                    2026-08-01) is 0/1, struct player's own
 *                    relative_mode - see RELATIVE below. <cuePoint>
 *                    (added 2026-08-03) is player_get_cue_point_elapsed(),
 *                    same elapsed-time convention as <remain> - lets the
 *                    app draw a cue marker on the waveform that's always
 *                    in sync with the box's own real cue point, rather
 *                    than tracking what it last sent client-side (which
 *                    would go stale across a reconnect - see SET_CUE
 *                    below). Fixed 0.000 for EMPTY/IMPORTING, same
 *                    reasoning as pitch. <loopActive>/<loopStart>/
 *                    <loopEnd> (added 2026-08-04) are player_get_loop_
 *                    active()/player_get_loop_start_elapsed()/player_
 *                    get_loop_end_elapsed() - same "read it back" idiom
 *                    as cuePoint just above (see LOOP below for why a
 *                    client can no longer just track this itself).
 *                    <loopStart>/<loopEnd> are 0.000 whenever
 *                    <loopActive> is 0. <path> is the loaded file's
 *                    path, unquoted and always the last field (may
 *                    contain spaces, never a newline) - lets a client
 *                    recover "what's actually loaded on this deck" after
 *                    its OWN restart, since xwax is the one thing that
 *                    keeps running (and keeps the real answer) through a
 *                    Node or app restart.
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
 *   SEEK <seconds> - jump the playhead to an arbitrary elapsed-time
 *                    offset within the track (same convention as
 *                    STATUS's <remain>/LOOP's own start/end) and pause
 *                    there - see player_seek_to_elapsed(). Mainly for
 *                    relative mode: tap-to-seek and drag-to-position on
 *                    the app's waveform. "Pause" here means the same
 *                    thing it does for GOTO_CUE below - if the needle
 *                    is down and providing a real reading, the jump
 *                    just becomes the new position and playback
 *                    continues following the live pitch as normal; only
 *                    with the needle up does this actually hold still.
 *                    No reply - a client sees the jump reflected in
 *                    STATUS's own <remain> on the next poll, same "read
 *                    it back rather than track it locally" idiom as
 *                    RELATIVE above.
 *
 *   SET_CUE <seconds> - store an explicit elapsed-time offset as this
 *                    deck's single cue point (same convention as
 *                    STATUS's <remain>/SEEK's own argument) - see
 *                    player_set_cue_point(). Takes the target directly
 *                    rather than always using the deck's CURRENT
 *                    position - the app is expected to snap this to the
 *                    nearest beat-grid tick before sending it, and xwax
 *                    itself has no notion of tempo/bars to do that
 *                    snapping here (same "caller decides the range"
 *                    reasoning as LOOP below). Replaces whatever cue
 *                    point was set before, if any. No reply - the app
 *                    reads it back from STATUS's own <cuePoint> field
 *                    above rather than tracking what it sent, same "read
 *                    it back" idiom as SEEK above.
 *
 *   GOTO_CUE      - jump back to the stored cue point and pause there
 *                    (see player_cue(), same pause semantics as SEEK
 *                    above) - replaces the earlier fixed CUE command
 *                    (jump to track start), which this now subsumes:
 *                    the cue point defaults to the track's own start
 *                    point until SET_CUE is ever sent (see struct
 *                    player's own doc comment on `cue_point`), so this
 *                    does exactly what the old CUE did before that.
 *                    Mainly for relative mode, where there's no needle
 *                    position to fall back on to get back to a cue
 *                    point, unlike absolute mode. No reply - same idiom
 *                    as SEEK above.
 *
 *   PLAY_CUE      - jump to the stored cue point and start playing FROM
 *                    there immediately, even with the needle up and no
 *                    real timecode signal present (see player_cue_
 *                    play()) - a genuine digital/software-driven
 *                    playback, unlike every other position command
 *                    here, which pauses without a real needle signal.
 *                    Reuses relative mode's own existing "lift the
 *                    needle, keep playing" mechanism rather than a
 *                    separate synthetic playback path - if the needle
 *                    IS down and valid, its real reading takes over
 *                    immediately as normal, this never fights a real,
 *                    present signal. No reply - same idiom as SEEK
 *                    above.
 *
 *   LOOP <start> <end> - loop the track's own elapsed-time range
 *                    [start, end) (seconds, same convention as
 *                    STATUS's <remain>/player_get_elapsed()) - see
 *                    player_set_loop(). The caller decides the range
 *                    (eg. the app computing one bar from its own beat
 *                    grid); xwax has no notion of tempo/bars itself.
 *                    Only takes effect while relative mode is on (see
 *                    player_collect()'s own gate) - absolute mode's
 *                    position is dictated by the physical needle,
 *                    there's nothing here to loop against. An ARMED
 *                    loop only actually wraps the position while
 *                    that position is inside [start, end) (see
 *                    player_collect()'s own was_in_loop gate, added
 *                    2026-08-04, owner's spec: "the loop stays active
 *                    but is only acted upon if the position marker is
 *                    within the loop area... it just activates again
 *                    if the position goes inside the loop area
 *                    again") - a SEEK/GOTO_CUE/PLAY_CUE landing
 *                    outside the range just plays on normally with the
 *                    loop still armed, rather than either fighting the
 *                    jump every buffer (the original, buggy behaviour)
 *                    or cancelling the loop outright (an earlier,
 *                    since-reverted fix for that same bug).
 *   LOOP OFF      - stop looping outright (see player_clear_loop()) -
 *                    playback continues from wherever the loop
 *                    currently is, no jump. No reply either way - same
 *                    "read it back from STATUS" idiom as RELATIVE/CUE
 *                    above; STATUS's own <loopActive>/<loopStart>/
 *                    <loopEnd> fields (added 2026-08-04, see STATUS
 *                    above) are what a client reads this back from - a
 *                    client-tracked flag with nothing to read it back
 *                    from went stale across a reconnect (a real,
 *                    confirmed bug: the app restarted mid-loop, showed
 *                    no loop indication, while xwax kept faithfully
 *                    looping underneath it). This is the only thing
 *                    that actually disarms a loop now - SEEK/GOTO_CUE/
 *                    PLAY_CUE do NOT, see LOOP above.
 */
int control_init(struct controller *c, struct rt *rt, const char *path);

#endif
