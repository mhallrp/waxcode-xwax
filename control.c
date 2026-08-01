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

/*
 * Pi DVS control socket - Phase 2 fork proof-of-concept.
 *
 * Follows the same struct controller pattern as dicer.c (the
 * Novation Dicer MIDI controller): a controller owns file
 * descriptor(s), gets polled by the realtime thread alongside every
 * audio device, and acts on its assigned deck when data arrives.
 *
 * IMPORTANT #1: controller_handle() calls realtime() on EVERY
 * registered controller on EVERY poll() wakeup, regardless of which
 * fd actually became ready (see realtime.c's rt_main loop) - so both
 * the listening socket and any connected client socket MUST be
 * non-blocking. A blocking read here would stall the entire realtime
 * thread, freezing audio on every deck, not just this one.
 *
 * IMPORTANT #2, found by actually running this against real
 * hardware: deck_load() is NOT safe to call from the realtime thread
 * either. It leads into track.c's more_space(), which calls
 * rt_not_allowed() and aborts the whole process if invoked there
 * (xwax's own deliberate assertion against exactly this mistake -
 * see thread.c). In the stock interface, deck_load() is only ever
 * called from the SDL interface thread, a separate, non-realtime
 * thread - never from here. So the actual load is handed off to a
 * dedicated worker thread this file owns; realtime() only ever does
 * a strdup() (safe - not one of xwax's own guarded functions) and a
 * condvar signal, never the load itself.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "control.h"
#include "controller.h"
#include "deck.h"
#include "index.h"
#include "realtime.h"

#define MAX_LINE 1024
#define BACKLOG 1

struct control {
    struct deck *deck;
    char sockpath[108]; /* sizeof sockaddr_un.sun_path */
    int listen_fd;
    int client_fd; /* -1 if nothing connected */
    char buf[MAX_LINE];
    size_t fill;

    /* Hand-off to the worker thread - see IMPORTANT #2 above */
    pthread_t worker;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    char *pending_path; /* NULL if nothing pending */
    bool pending_unload;
    bool shutdown;
};

static int add_deck(struct controller *c, struct deck *d)
{
    struct control *ctrl = c->local;

    if (ctrl->deck != NULL)
        return -1; /* one deck per socket, by design */

    ctrl->deck = d;
    return 0;
}

/*
 * Runs on the dedicated worker thread ONLY - never the realtime
 * thread. Builds a record from a bare filepath, bypassing the
 * library/selector system entirely (this is the proof-of-concept
 * shortcut; a real LOAD would look up an existing library entry),
 * and loads it.
 *
 * record_clear() (library.c) only frees ->pathname and ->match, not
 * ->artist/->title - matching that here by using string literals for
 * artist/title rather than independent allocations, so nothing is
 * ever double-freed or leaked because of a mismatched assumption.
 */
static void do_load(struct control *ctrl, char *path)
{
    struct record *re;

    re = malloc(sizeof *re);
    if (re == NULL) {
        perror("malloc");
        return;
    }

    re->pathname = path; /* worker takes ownership of this allocation */
    re->artist = "";
    re->title = "";
    re->match = NULL;
    re->bpm = 0.0;

    fprintf(stderr, "control: LOAD %s\n", path);
    deck_load(ctrl->deck, re);
}

/*
 * Same worker-thread requirement as do_load() above - deck_unload()
 * ends up calling track_release() on whatever track was previously
 * loaded, which calls free() once its refcount reaches zero. free()
 * is just as unsafe on the realtime thread as the malloc() in
 * do_load() is (see IMPORTANT #2), so this can't be handled directly
 * from realtime() either, even though - unlike LOAD - it doesn't look
 * like it should need it at first glance.
 */
static void do_unload(struct control *ctrl)
{
    fprintf(stderr, "control: UNLOAD\n");
    deck_unload(ctrl->deck);
}

static void *worker_main(void *arg)
{
    struct control *ctrl = arg;

    pthread_mutex_lock(&ctrl->lock);
    for (;;) {
        while (ctrl->pending_path == NULL && !ctrl->pending_unload && !ctrl->shutdown)
            pthread_cond_wait(&ctrl->cond, &ctrl->lock);

        if (ctrl->shutdown) {
            pthread_mutex_unlock(&ctrl->lock);
            return NULL;
        }

        if (ctrl->pending_path != NULL) {
            char *path = ctrl->pending_path;
            ctrl->pending_path = NULL;
            pthread_mutex_unlock(&ctrl->lock);

            do_load(ctrl, path); /* takes ownership of path, do not free here */
        } else {
            ctrl->pending_unload = false;
            pthread_mutex_unlock(&ctrl->lock);

            do_unload(ctrl);
        }

        pthread_mutex_lock(&ctrl->lock);
    }
}

/*
 * Runs on the realtime thread (called from realtime(), below). Must
 * stay non-blocking and must never touch xwax's guarded functions -
 * see IMPORTANT #2 at the top of this file. Only hands off a copy of
 * the path to the worker thread.
 */
static void handle_load(struct control *ctrl, const char *path)
{
    char *copy;

    if (ctrl->deck == NULL) {
        fprintf(stderr, "control: LOAD received before a deck was assigned\n");
        return;
    }

    copy = strdup(path);
    if (copy == NULL) {
        perror("strdup");
        return;
    }

    pthread_mutex_lock(&ctrl->lock);
    if (ctrl->pending_path != NULL) {
        fprintf(stderr, "control: dropping LOAD, previous request still pending\n");
        free(copy);
    } else {
        ctrl->pending_path = copy;
        pthread_cond_signal(&ctrl->cond);
    }
    pthread_mutex_unlock(&ctrl->lock);
}

/*
 * Runs on the realtime thread (called from realtime(), below) - same
 * constraints as handle_load() above, and for the same reason: see
 * do_unload()'s doc comment for why UNLOAD needs the worker thread
 * hand-off too, even though it has no path argument to copy.
 */
static void handle_unload(struct control *ctrl)
{
    if (ctrl->deck == NULL) {
        fprintf(stderr, "control: UNLOAD received before a deck was assigned\n");
        return;
    }

    pthread_mutex_lock(&ctrl->lock);
    if (ctrl->pending_path != NULL || ctrl->pending_unload) {
        fprintf(stderr, "control: dropping UNLOAD, previous request still pending\n");
    } else {
        ctrl->pending_unload = true;
        pthread_cond_signal(&ctrl->cond);
    }
    pthread_mutex_unlock(&ctrl->lock);
}

/*
 * Runs on the realtime thread (called from realtime(), via
 * handle_line()) - same constraints as handle_load() above. Unlike
 * LOAD, this reads state and replies synchronously rather than
 * handing off to the worker thread: player_get_remain()/
 * player_is_active() are plain field reads (see player.c), not one
 * of xwax's guarded, non-realtime-safe functions like deck_load() -
 * confirmed by xwax's own stock SDL interface (interface.c) calling
 * these same two functions directly from its UI thread with no lock,
 * which this follows.
 *
 * ctrl->deck->record is &no_record (deck.c, not exposed outside it)
 * until a real LOAD succeeds - checking ->pathname != NULL rather
 * than importing that static is enough to tell "nothing loaded yet"
 * apart from a real track, since no_record leaves pathname at its
 * zero-initialised NULL.
 */
static void handle_status(struct control *ctrl)
{
    /* Sized for a real filesystem path, not just the fixed-format
     * state/remain fields - PATH_MAX on Linux is 4096, plus room for
     * "STATUS PLAYING 12345.6 " and the trailing newline. */
    char reply[4200];
    const char *state;
    double remain;
    int n;

    if (ctrl->deck == NULL) {
        fprintf(stderr, "control: STATUS received before a deck was assigned\n");
        return;
    }

    if (ctrl->deck->record->pathname == NULL) {
        /* No path field at all when nothing's loaded - there's nothing
         * meaningful to report, and it keeps this, the common idle
         * case, a fixed, simple shape. pitch is meaningless here too -
         * fixed 0.000, same reasoning as remain below. relative
         * (added 2026-08-01) is fixed 0 here too - relative mode can't
         * be meaningfully on for an empty deck. */
        n = snprintf(reply, sizeof reply, "STATUS EMPTY 0.0 0.000 0\n");
    } else if (track_is_importing(ctrl->deck->player.track)) {
        /* A LOAD was issued, but xwax's own import subprocess (see
         * track.c) is still decoding the file - track->length only
         * reflects however much has been decoded SO FAR, not the
         * eventual full duration, so player_get_remain() would report
         * a real but meaningless, steadily-growing number here.
         * Confirmed as a real, confusing thing to show on real
         * hardware: the app's countdown would start at some small,
         * wrong value and visibly "snap" to the correct one once
         * import finished, for every single load, with no needle
         * involved at all - purely an import-progress artifact, not
         * anything to do with timecode. Reporting a distinct state
         * instead lets a client show "loading" honestly rather than a
         * number it can't yet trust. remain/pitch are meaningless
         * here, sent as fixed 0.0/0.000 for a consistent reply shape;
         * path is still included so a client already knows which file
         * this is.
         */
        n = snprintf(reply, sizeof reply, "STATUS IMPORTING 0.0 0.000 0 %s\n", ctrl->deck->record->pathname);
    } else {
        remain = player_get_remain(&ctrl->deck->player);
        if (remain < 0.0)
            remain = 0.0;
        state = player_is_active(&ctrl->deck->player) ? "PLAYING" : "STOPPED";
        /* The path is what lets a client (Node, then the app) recover
         * "what's actually loaded on this deck" after ITS OWN restart
         * - xwax is the one component that keeps running (and keeps
         * the real answer) through a Node or app restart, so it's the
         * only place this can authoritatively come from. Last field,
         * unquoted - a path can contain spaces, but never a newline,
         * so "everything to end of line" is an unambiguous way for a
         * client to extract it without needing real escaping.
         *
         * pitch (added 2026-07-27): struct player's own field, read
         * directly with no lock - exactly how player_is_active() just
         * read it one line above to decide `state`. Lets a client
         * interpolate position accurately between polls (real speed
         * and direction, not an assumed steady 1x forward guess).
         *
         * remain now %.4f, not the original %.1f (same day) - the
         * underlying player_get_remain() was always a fully precise
         * double; %.1f only ever rounded it for display, but a client
         * re-anchoring its own pitch-based interpolation to this value
         * on every poll (250ms) was inheriting that rounding as a real,
         * visible position snap each time - up to ~50ms of error,
         * corrected abruptly every tick. Confirmed as a real
         * contributor to "not smooth" playback/scrub feel on real
         * hardware, even with pitch-based interpolation already in
         * place.
         *
         * relative (added 2026-08-01): struct player's own
         * relative_mode field, read the same direct/lock-free way
         * pitch is - lets a client show whether this deck's needle
         * currently drives live scratch/pitch only, or full absolute
         * position too (see player.h's own doc comment on the
         * feature). 0/1, not a word, to stay consistent with the
         * fixed-width numeric fields either side of it. */
        n = snprintf(reply, sizeof reply, "STATUS %s %.4f %.3f %d %s\n",
                     state, remain, ctrl->deck->player.pitch,
                     ctrl->deck->player.relative_mode ? 1 : 0, ctrl->deck->record->pathname);
    }

    if (n < 0 || (size_t)n >= sizeof reply) {
        fprintf(stderr, "control: STATUS reply truncated\n");
        return;
    }

    /* Non-blocking socket (see IMPORTANT #1), tiny fixed-size reply -
     * a short write() rather than a buffered/retrying send loop, same
     * complexity level as the rest of this proof-of-concept protocol.
     * Node reconnects and asks again on its own polling interval (see
     * deck-control.js), so a dropped reply here just costs one tick,
     * not a stuck client. */
    if (write(ctrl->client_fd, reply, (size_t)n) == -1)
        perror("control: write STATUS reply");
}

/*
 * Runs on the realtime thread (called from realtime(), via
 * handle_line()) - unlike LOAD/UNLOAD, this is safe to do directly
 * here rather than handing off to the worker thread: player_set_
 * relative_mode() is a couple of plain field writes (see player.c),
 * not one of xwax's guarded, non-realtime-safe functions - same
 * reasoning as handle_status() below already relies on for its own
 * direct field reads.
 */
static void handle_relative(struct control *ctrl, bool on)
{
    if (ctrl->deck == NULL) {
        fprintf(stderr, "control: RELATIVE received before a deck was assigned\n");
        return;
    }

    fprintf(stderr, "control: RELATIVE %s\n", on ? "ON" : "OFF");
    player_set_relative_mode(&ctrl->deck->player, on);
}

/*
 * Runs on the realtime thread (called from realtime(), via
 * handle_line()) - same reasoning as handle_relative() above:
 * player_cue_to_start() is a lock-protected plain field write, not
 * one of xwax's guarded, non-realtime-safe functions.
 */
static void handle_cue(struct control *ctrl)
{
    if (ctrl->deck == NULL) {
        fprintf(stderr, "control: CUE received before a deck was assigned\n");
        return;
    }

    fprintf(stderr, "control: CUE\n");
    player_cue_to_start(&ctrl->deck->player);
}

static void handle_line(struct control *ctrl, char *line)
{
    if (!strncmp(line, "LOAD ", 5)) {
        handle_load(ctrl, line + 5);
    } else if (!strcmp(line, "UNLOAD")) {
        handle_unload(ctrl);
    } else if (!strcmp(line, "STATUS")) {
        handle_status(ctrl);
    } else if (!strcmp(line, "CUE")) {
        handle_cue(ctrl);
    } else if (!strcmp(line, "RELATIVE ON")) {
        handle_relative(ctrl, true);
    } else if (!strcmp(line, "RELATIVE OFF")) {
        handle_relative(ctrl, false);
    } else {
        fprintf(stderr, "control: unrecognised command '%s'\n", line);
    }
}

static void close_client(struct control *ctrl)
{
    close(ctrl->client_fd);
    ctrl->client_fd = -1;
    ctrl->fill = 0;
}

/*
 * Drain every pending connection on the listening socket. Non-blocking
 * (see IMPORTANT #1) - loops until accept() would block.
 */
static void accept_clients(struct control *ctrl)
{
    for (;;) {
        int fd;

        fd = accept(ctrl->listen_fd, NULL, NULL);
        if (fd == -1) {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                perror("accept");
            return;
        }

        if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1) {
            perror("fcntl");
            close(fd);
            continue;
        }

        if (ctrl->client_fd != -1) {
            fprintf(stderr, "control: replacing existing client connection\n");
            close_client(ctrl);
        }

        ctrl->client_fd = fd;
    }
}

/*
 * Drain everything currently available on the client socket.
 * Non-blocking (see IMPORTANT #1) - loops until read() would block.
 */
static void read_client(struct control *ctrl)
{
    for (;;) {
        ssize_t z;

        z = read(ctrl->client_fd, ctrl->buf + ctrl->fill,
                 sizeof(ctrl->buf) - ctrl->fill - 1);
        if (z == -1) {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                perror("read");
            return;
        }
        if (z == 0) { /* client closed the connection */
            close_client(ctrl);
            return;
        }

        ctrl->fill += z;
        ctrl->buf[ctrl->fill] = '\0';

        for (;;) {
            char *nl;
            size_t linelen;

            nl = memchr(ctrl->buf, '\n', ctrl->fill);
            if (nl == NULL)
                break;

            *nl = '\0';
            handle_line(ctrl, ctrl->buf);

            linelen = nl - ctrl->buf + 1;
            memmove(ctrl->buf, nl + 1, ctrl->fill - linelen);
            ctrl->fill -= linelen;
        }

        if (ctrl->fill == sizeof(ctrl->buf) - 1) {
            fprintf(stderr, "control: line too long, dropping connection\n");
            close_client(ctrl);
            return;
        }
    }
}

static ssize_t pollfds(struct controller *c, struct pollfd *pe, size_t z)
{
    struct control *ctrl = c->local;
    size_t n = 0;

    if (n >= z)
        return -1;
    pe[n].fd = ctrl->listen_fd;
    pe[n].events = POLLIN;
    n++;

    if (ctrl->client_fd != -1) {
        if (n >= z)
            return -1;
        pe[n].fd = ctrl->client_fd;
        pe[n].events = POLLIN;
        n++;
    }

    return n;
}

static int realtime(struct controller *c)
{
    struct control *ctrl = c->local;

    accept_clients(ctrl);

    if (ctrl->client_fd != -1)
        read_client(ctrl);

    return 0;
}

static void clear(struct controller *c)
{
    struct control *ctrl = c->local;

    pthread_mutex_lock(&ctrl->lock);
    ctrl->shutdown = true;
    pthread_cond_signal(&ctrl->cond);
    pthread_mutex_unlock(&ctrl->lock);
    pthread_join(ctrl->worker, NULL);

    if (ctrl->client_fd != -1)
        close(ctrl->client_fd);
    close(ctrl->listen_fd);
    unlink(ctrl->sockpath);
    pthread_mutex_destroy(&ctrl->lock);
    pthread_cond_destroy(&ctrl->cond);
    free(ctrl);
}

static struct controller_ops control_ops = {
    .add_deck = add_deck,
    .pollfds = pollfds,
    .realtime = realtime,
    .clear = clear,
};

int control_init(struct controller *c, struct rt *rt, const char *path)
{
    struct control *ctrl;
    struct sockaddr_un addr;

    if (strlen(path) >= sizeof addr.sun_path) {
        fprintf(stderr, "control: socket path too long: %s\n", path);
        return -1;
    }

    ctrl = malloc(sizeof *ctrl);
    if (ctrl == NULL) {
        perror("malloc");
        return -1;
    }

    ctrl->deck = NULL;
    ctrl->client_fd = -1;
    ctrl->fill = 0;
    ctrl->pending_path = NULL;
    ctrl->pending_unload = false;
    ctrl->shutdown = false;
    strcpy(ctrl->sockpath, path);
    pthread_mutex_init(&ctrl->lock, NULL);
    pthread_cond_init(&ctrl->cond, NULL);

    /* Remove a stale socket file from a previous run - bind() fails
     * with EADDRINUSE otherwise, even though nothing is listening */

    unlink(path);

    ctrl->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (ctrl->listen_fd == -1) {
        perror("socket");
        goto fail;
    }

    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, path);

    if (bind(ctrl->listen_fd, (struct sockaddr*)&addr, sizeof addr) == -1) {
        perror("bind");
        goto fail_socket;
    }

    if (listen(ctrl->listen_fd, BACKLOG) == -1) {
        perror("listen");
        goto fail_socket;
    }

    if (pthread_create(&ctrl->worker, NULL, worker_main, ctrl) != 0) {
        perror("pthread_create");
        goto fail_socket;
    }

    fprintf(stderr, "control: listening on %s\n", path);

    if (controller_init(c, &control_ops, ctrl, rt) == -1)
        goto fail_worker;

    return 0;

fail_worker:
    pthread_mutex_lock(&ctrl->lock);
    ctrl->shutdown = true;
    pthread_cond_signal(&ctrl->cond);
    pthread_mutex_unlock(&ctrl->lock);
    pthread_join(ctrl->worker, NULL);
fail_socket:
    close(ctrl->listen_fd);
    unlink(path);
fail:
    free(ctrl);
    return -1;
}
