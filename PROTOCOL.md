# Control socket protocol

One Unix socket per deck (the socket path identifies the deck - no `<deck>` parameter in the protocol itself). Line-based, one command per line. See `control.c`/`control.h` for the implementation.

## Commands

**`LOAD <path>`** — load a track from a bare filepath. No reply; poll STATUS for `IMPORTING` → `PLAYING`/`STOPPED`.

**`UNLOAD`** — clear the deck back to empty, so a passthrough loop (`alsaloop`, managed by Node) can take over the DAC output. No reply; STATUS shows `EMPTY` once it takes effect. No `PASSTHRU` command exists here - passthrough lives entirely outside xwax.

**`STATUS`** — replies with one of:
- `STATUS EMPTY 0.0 0.000 <relative> 0.000 0 0.000 0.000 0.000 0 0.000\n`
- `STATUS IMPORTING 0.0 0.000 <relative> 0.000 0 0.000 0.000 <elapsed> 0 <drift> <path>\n`
- `STATUS <PLAYING|STOPPED> <remain> <pitch> <relative> <cuePoint> <loopActive> <loopStart> <loopEnd> <elapsed> <timecodeValid> <drift> <path>\n`

`timecodeValid` (added 2026-08-29) is `player.timecode_valid`: the needle is down and reading a
locked position. Deliberately not the same as `pitch != 0`, which is also false when merely paused.

It tells a client whether anything OUTSIDE the app can move this deck. With the needle up in
relative mode nothing can - the deck runs at a fixed rate under the app's own commands - so the app
can extrapolate the playhead from its own clock and ignore position corrections entirely, which is
the only way to guarantee the playhead never twitches. Once the needle is down the deck's position
and pitch are driven by the record and the app must follow what is reported.

Always 0 for EMPTY and IMPORTING: nothing is locked to a needle in either state.

`elapsed` (added 2026-08-28) is `player_get_elapsed()` - position minus the cue offset, straight off
the timecode. It owes nothing to the loaded track, so unlike `remain` it is **live during
IMPORTING**: `remain` is the one field that needs `track->length`, which is still growing while the
import runs, which is why it alone stays fixed at 0.0 there.

A client that knows the track's duration (the app reads it from the file's tags) derives remaining
from `elapsed` itself, so its readout is live from the moment the needle drops rather than blank for
the seconds a decode takes. Audio is already playing by then regardless - `build_pcm()` reads
`track->length` atomically each buffer and plays whatever has decoded so far, filling silence beyond
it.

`drift` (added 2026-09-01) is `player_get_offset_drift()` - how far `offset` has been slid from the
`--cue-offset` calibration, in seconds, signed. Zero means the needle's position and the track's
agree. Positive means the track has slid FORWARD along the timecode record, so track 0:00 now sits
that far into the vinyl.

It becomes non-zero when a loop wraps in tracking mode (each wrap adds one loop length - see
`LOOP`), and from any seek in non-tracking mode, where moving `offset` is the whole mechanism. It
returns to zero on a load, and on `RELATIVE OFF`.

Clients should surface it: the DJ has no other way to know the record has been re-labelled under
them, and it is the number that warns them before the track's tail runs past the end of the usable
timecode. Fixed at 0.000 for `EMPTY` - a load resets it, so it has no meaning with nothing loaded.

Note for parsers: `elapsed` and `drift` sit BEFORE `<path>`, because a path may contain spaces and
so must stay last. Node's own parser treats both as optional and requires the path to be absolute, so it reads this
format and an older xwax that omits either field - the two deploy separately.

Field meanings: `remain` = seconds left, clamped ≥ 0. `PLAYING`/`STOPPED` reflects whether the platter's spinning fast enough to be "on" (real needle or an active `PLAY_CUE`), not just whether a track is loaded. `pitch` = signed speed relative to normal (1.0 = real time, negative = reverse), read lock-free from `struct player`. `relative`/`cuePoint`/`loopActive`/`loopStart`/`loopEnd` are all "read it back" fields - the box is the source of truth, not the client, so state survives an app/Node reconnect. `relative` is live even in `EMPTY` (2026-08-19) - relative mode can be armed with nothing loaded (`player_set_relative_mode()` is a plain field write, no track needed), so a client selecting it pre-load needs to read that choice back before anything's loaded. `path` is always last, unquoted (may contain spaces, never a newline).

**`SIGNAL`** — replies with `SIGNAL <peakLeft> <peakRight> <refLevel> <validCounter> <ticker> <forwards> <safe> <threshold> <sensitivity>\n`.

Diagnostics for a calibration screen. Deliberately NOT part of STATUS: that is polled ~20 times a
second for every deck, and this is only wanted while someone is looking at a calibration display.
None of it is used for decoding.

- `peakLeft` / `peakRight` — decaying peak per INPUT channel, same scale as the samples (a 16-bit
  sample shifted left 16), so divide by INT_MAX for 0..1. Both low is a weak cartridge; one near
  zero is a dead channel or an unplugged lead. This is the pair that tells a user *why* a setup is
  failing rather than just that it is.
- `refLevel` — what bit decisions actually compare against. It self-calibrates to whatever amplitude
  arrives (a running average of observed peaks), which is why a software gain control would not help
  decoding: scaling the input scales this identically.
- `validCounter` — consecutive successful error checks. The honest measure of lock quality.
- `ticker` — samples since a valid timecode was read; climbs the moment the needle leaves the record.
- `forwards` — the direction the timecode is being read in. With the platter running forwards, a 0
  here means the channels are swapped, which otherwise reads as perfectly clean timecode at a steady
  negative pitch.
- `safe` — whether the decoded position is currently trustworthy.
- `threshold` — the amplitude below which a signal cannot be decoded at all (`ZERO_THRESHOLD`,
  shifted down ~36dB in phono mode). Sent rather than left for the client to derive, so a level
  meter can mark the real floor without duplicating the phono shift and drifting out of step.
- `sensitivity` — the current noise-sensitivity step, 0 to 3. See `SENSITIVITY` below.

**`SENSITIVITY <level>`** — sets how much noise the decoder tolerates before reading a wave at all.
No reply.

`level` is 0 (the calibrated default) to 3, each step doubling the zero-crossing threshold. Raising
it stops a booth with heavy low-end triggering the decoder on rumble, at the cost of no longer
reading the quietest part of the signal — the compromise a DJ makes knowingly, which is why it is
exposed rather than fixed. Capped at 3 because past that a healthy cartridge's own signal starts
being rejected along with the rumble.

Not persisted by xwax: it resets to 0 on restart, and the server reapplies it (see
`deck-sensitivity.js`).

**`RELATIVE ON|OFF`** — toggle relative mode (see `player_set_relative_mode()`). While ON, the needle drives live pitch/scratch whenever the deck is playing, but never starts or stops playback itself and its absolute position is never consulted - lifting it leaves the track playing instead of stopping it, at whatever pitch was last read rather than snapping back to 1.0, and dropping the needle back down doesn't resume a paused deck (owner's call, 2026-08-06: the vinyl is a controller for pitch/mixing, not a play/pause switch - see `player.h`'s `relative_playing` field). No reply; read back via STATUS's `relative` field. Switching OFF snaps to wherever the needle currently reads, not an offset-preserving continuation.

**The app presents this inverted, as a "position tracking" toggle: tracking ON is `RELATIVE OFF`.** The two are one control with one question behind it - *is the needle the authority on this deck?* Only four behaviours actually differ: a needle drop (snaps the track, vs does nothing), a small skip (pitch-corrected, vs ignored), a large skip past `SKIP_THRESHOLD` (jumps, vs ignored), and losing the signal - needle lifted, record run out, past the safe zone - which stops a tracking deck and lets a non-tracking one play on at its held pitch. Everything else, loops and cues included, is identical.

Switching OFF resets `offset` to `cue_offset`, which since 2026-09-01 is also the DJ's way to **discard accumulated drift** (see STATUS's `drift`) and get the record's own labelling back. It no longer clears an active loop: the loop survives the switch in both directions.

### How a jump lands, in each mode

`SEEK`, `RELOCATE`, `GOTO_CUE` and `PLAY_CUE` all move the deck to an elapsed time. **Since
2026-09-01 they work in both modes**, but by different means, for the same reason `LOOP` does:

- **Non-tracking (`RELATIVE ON`)** - the jump writes `position` directly. Unchanged.
- **Tracking (`RELATIVE OFF`)** - `retarget()` would drag `position` straight back to the needle
  within a buffer or two, which is why every cue and seek used to be relative-only. The jump moves
  the `position`↔`elapsed` mapping instead (`player_rebase_offset()`), which sticks because nothing
  else writes `offset`. The cue point and any armed loop shift with it, so they keep the ELAPSED
  meaning they had rather than sliding through the track.

The cost in tracking mode is `drift` (see STATUS). That is inherent rather than a defect: with the
needle authoritative, the only way the track can sit somewhere the needle does not say is to move
the mapping.

**`SEEK <seconds>`** — jump to an elapsed-time offset and pause there, unconditionally in relative mode (same pause semantics as `GOTO_CUE`). Mainly for relative-mode tap/drag-to-position. No reply.

**`RELOCATE <seconds>`** — jump to an elapsed-time offset WITHOUT touching play/pause state (`player_relocate()`) - unlike `SEEK`, doesn't force a pause: whatever's currently holding (playing or paused) keeps holding. Used to keep a shrunk loop's own position inside its new bounds without interrupting playback (a `LOOP` command alone doesn't retroactively reposition - see `LOOP`'s own doc below). No reply.

**`SET_CUE <seconds>`** — store an explicit cue point (`player_set_cue_point()`), replacing any previous one. Takes the target directly - the app snaps to the nearest beat-grid tick before sending, since xwax has no notion of tempo/bars. No reply; read back via STATUS's `cuePoint`.

**`GOTO_CUE`** — jump to the stored cue point and pause, unconditionally in relative mode (`player_cue()`). Subsumes the old, removed `CUE` command (jump to track start) - the cue point defaults to track start until `SET_CUE` is ever sent. No reply.

**`PLAY_CUE`** — jump to the cue point and start playing immediately, needle up or down (`player_cue_play()`) - the one genuinely digital/software-driven playback path. No reply.

**`PLAY`** — resume digital playback from wherever the deck already is, no jump, needle up or down (`player_play()`). Unlike `PLAY_CUE`, not tied to the cue point at all - plain transport play. No reply.

**`PAUSE`** — pause at wherever the deck already is, no jump, needle up or down (`player_pause()`). The `PLAY`/`PAUSE` counterpart to `SEEK`/`GOTO_CUE`'s own pause semantics. No reply.

**`LOOP <start> <end>`** — loop the elapsed-time range `[start, end)` (`player_set_loop()`). Caller decides the range (e.g. one bar from the app's beat grid). **Works in both modes since 2026-09-01** (it was relative-only before), but wraps differently in each, because the two disagree about who owns `position`:

- **Non-tracking (`RELATIVE ON`)** - `position` free-runs from pitch and nothing else writes it, so the wrap rewrites it directly. Unchanged.
- **Tracking (`RELATIVE OFF`)** - `retarget()` drags `position` toward the needle every cycle, so rewriting it would just be undone; that is why a loop was impossible here before. Instead `offset` and the loop bounds slide forward by one loop length per wrap and `position` is never touched. Since `elapsed` is `position - offset`, elapsed drops back by exactly the loop length while the needle keeps driving position. The loop window travels through timecode space at the needle's own rate and stands still in track time.

The consequence, which a client must surface: in tracking mode the track slides along the record by the total time looped, so `drift` grows. Loop long enough and the track's tail runs past `timecoder_get_safe()` and becomes unreachable by any needle position. `RELATIVE OFF` (re-asserting tracking) is the recovery. An armed loop only wraps the position while it's actually inside `[start, end)` - a `SEEK`/`GOTO_CUE`/`PLAY_CUE` landing outside the range plays on normally with the loop still armed, rather than fighting the jump every buffer or cancelling the loop outright.

**`LOOP OFF`** — disarm the loop (`player_clear_loop()`), no jump - the only thing that actually disarms one (`SEEK`/`GOTO_CUE`/`PLAY_CUE` do not). No reply; read back via STATUS's `loopActive`/`loopStart`/`loopEnd`.
