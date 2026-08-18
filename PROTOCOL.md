# Control socket protocol

One Unix socket per deck (the socket path identifies the deck - no `<deck>` parameter in the protocol itself). Line-based, one command per line. See `control.c`/`control.h` for the implementation.

## Commands

**`LOAD <path>`** — load a track from a bare filepath. No reply; poll STATUS for `IMPORTING` → `PLAYING`/`STOPPED`.

**`UNLOAD`** — clear the deck back to empty, so a passthrough loop (`alsaloop`, managed by Node) can take over the DAC output. No reply; STATUS shows `EMPTY` once it takes effect. No `PASSTHRU` command exists here - passthrough lives entirely outside xwax.

**`STATUS`** — replies with one of:
- `STATUS EMPTY 0.0 0.000 <relative> 0.000 0 0.000 0.000\n`
- `STATUS IMPORTING 0.0 0.000 <relative> 0.000 0 0.000 0.000 <path>\n`
- `STATUS <PLAYING|STOPPED> <remain> <pitch> <relative> <cuePoint> <loopActive> <loopStart> <loopEnd> <path>\n`

Field meanings: `remain` = seconds left, clamped ≥ 0. `PLAYING`/`STOPPED` reflects whether the platter's spinning fast enough to be "on" (real needle or an active `PLAY_CUE`), not just whether a track is loaded. `pitch` = signed speed relative to normal (1.0 = real time, negative = reverse), read lock-free from `struct player`. `relative`/`cuePoint`/`loopActive`/`loopStart`/`loopEnd` are all "read it back" fields - the box is the source of truth, not the client, so state survives an app/Node reconnect. `relative` is live even in `EMPTY` (2026-08-19) - relative mode can be armed with nothing loaded (`player_set_relative_mode()` is a plain field write, no track needed), so a client selecting it pre-load needs to read that choice back before anything's loaded. `path` is always last, unquoted (may contain spaces, never a newline).

**`RELATIVE ON|OFF`** — toggle relative mode (see `player_set_relative_mode()`). While ON, the needle drives live pitch/scratch whenever the deck is playing, but never starts or stops playback itself and its absolute position is never consulted - lifting it leaves the track playing instead of stopping it, at whatever pitch was last read rather than snapping back to 1.0, and dropping the needle back down doesn't resume a paused deck (owner's call, 2026-08-06: the vinyl is a controller for pitch/mixing, not a play/pause switch - see `player.h`'s `relative_playing` field). No reply; read back via STATUS's `relative` field. Switching OFF snaps to wherever the needle currently reads, not an offset-preserving continuation.

**`SEEK <seconds>`** — jump to an elapsed-time offset and pause there, unconditionally in relative mode (same pause semantics as `GOTO_CUE`). Mainly for relative-mode tap/drag-to-position. No reply.

**`RELOCATE <seconds>`** — jump to an elapsed-time offset WITHOUT touching play/pause state (`player_relocate()`) - unlike `SEEK`, doesn't force a pause: whatever's currently holding (playing or paused) keeps holding. Used to keep a shrunk loop's own position inside its new bounds without interrupting playback (a `LOOP` command alone doesn't retroactively reposition - see `LOOP`'s own doc below). No reply.

**`SET_CUE <seconds>`** — store an explicit cue point (`player_set_cue_point()`), replacing any previous one. Takes the target directly - the app snaps to the nearest beat-grid tick before sending, since xwax has no notion of tempo/bars. No reply; read back via STATUS's `cuePoint`.

**`GOTO_CUE`** — jump to the stored cue point and pause, unconditionally in relative mode (`player_cue()`). Subsumes the old, removed `CUE` command (jump to track start) - the cue point defaults to track start until `SET_CUE` is ever sent. No reply.

**`PLAY_CUE`** — jump to the cue point and start playing immediately, needle up or down (`player_cue_play()`) - the one genuinely digital/software-driven playback path. No reply.

**`PLAY`** — resume digital playback from wherever the deck already is, no jump, needle up or down (`player_play()`). Unlike `PLAY_CUE`, not tied to the cue point at all - plain transport play. No reply.

**`PAUSE`** — pause at wherever the deck already is, no jump, needle up or down (`player_pause()`). The `PLAY`/`PAUSE` counterpart to `SEEK`/`GOTO_CUE`'s own pause semantics. No reply.

**`LOOP <start> <end>`** — loop the elapsed-time range `[start, end)` (`player_set_loop()`). Caller decides the range (e.g. one bar from the app's beat grid); only takes effect in relative mode. An armed loop only wraps the position while it's actually inside `[start, end)` - a `SEEK`/`GOTO_CUE`/`PLAY_CUE` landing outside the range plays on normally with the loop still armed, rather than fighting the jump every buffer or cancelling the loop outright.

**`LOOP OFF`** — disarm the loop (`player_clear_loop()`), no jump - the only thing that actually disarms one (`SEEK`/`GOTO_CUE`/`PLAY_CUE` do not). No reply; read back via STATUS's `loopActive`/`loopStart`/`loopEnd`.
