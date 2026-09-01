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
 * Pi DVS per-deck control socket, mirrors dicer.c's controller pattern.
 * Sockets must stay non-blocking; LOAD/UNLOAD hand off to a worker thread since deck_load()/free() aren't realtime-safe (thread.c).
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

    /* Hand-off to the worker thread - see file header above */
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

/* Worker thread only. Builds a bare-filepath record, skipping the library/selector lookup a real LOAD would use. */
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

/* Worker thread only, same as do_load() - deck_unload() can call free() via track_release(). */
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

/* Realtime thread - must stay non-blocking, only hands the path off to the worker thread. */
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

/* Realtime thread, same worker hand-off as handle_load(). */
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

/* Realtime thread - safe to answer synchronously, these are all plain field reads (see player.c). */
static void handle_status(struct control *ctrl)
{
    /* PATH_MAX (4096) plus room for the fixed-format fields and newline. */
    char reply[4200];
    const char *state;
    double remain;
    int n;

    if (ctrl->deck == NULL) {
        fprintf(stderr, "control: STATUS received before a deck was assigned\n");
        return;
    }

    if (ctrl->deck->record->pathname == NULL) {
        /* Nothing loaded - pitch/cue/loop fixed at zero, keeps EMPTY a simple fixed shape. relative
         * stays LIVE, not fixed - relative mode can be armed before anything's loaded (a plain
         * field write, player_set_relative_mode() never touches the track), so a client selecting
         * it pre-load needs to be able to read that choice back. Same reasoning as IMPORTING's own
         * live relative field below, see DEVLOG.md 2026-08-01. */
        n = snprintf(reply, sizeof reply, "STATUS EMPTY 0.0 0.000 %d 0.000 0 0.000 0.000 0.000 0 0.000\n",
                     ctrl->deck->player.relative_mode ? 1 : 0);
    } else if (track_is_importing(ctrl->deck->player.track)) {
        /* timecodeValid is 0 here, and in EMPTY: nothing is locked to a needle in either state. A
         * client uses it to know whether anything OUTSIDE the app can move this deck at all.
         *
         * Import in progress - remain stays fixed because it is the one field that needs
         * track->length, which isn't final yet. `elapsed` is NOT fixed: it is position minus offset,
         * both straight off the timecode, so it is valid from the first revolution and has nothing
         * to do with how much of the track has decoded. A client that knows the duration (the app
         * reads it from the file's tags) can derive remaining from it itself, which is what stops
         * the readout sitting blank for the seconds an import takes. Audio is already playing by
         * then - build_pcm() reads track->length live and just plays whatever has decoded so far.
         *
         * relative stays LIVE too, not fixed - see DEVLOG.md 2026-08-01 for the real bug otherwise. */
        n = snprintf(reply, sizeof reply, "STATUS IMPORTING 0.0 0.000 %d 0.000 0 0.000 0.000 %.4f 0 %.3f %s\n",
                     ctrl->deck->player.relative_mode ? 1 : 0,
                     player_get_elapsed(&ctrl->deck->player),
                     player_get_offset_drift(&ctrl->deck->player),
                     ctrl->deck->record->pathname);
    } else {
        remain = player_get_remain(&ctrl->deck->player);
        if (remain < 0.0)
            remain = 0.0;
        state = player_is_active(&ctrl->deck->player) ? "PLAYING" : "STOPPED";
        /* Precision fields (pitch/relative/cuePoint/loop) are read back live, not client-tracked,
         * so scrub/loop/cue stay in sync across a reconnect - see DEVLOG.md for the full history. */
        n = snprintf(reply, sizeof reply, "STATUS %s %.4f %.3f %d %.3f %d %.3f %.3f %.4f %d %.3f %s\n",
                     state, remain, ctrl->deck->player.pitch,
                     ctrl->deck->player.relative_mode ? 1 : 0,
                     player_get_cue_point_elapsed(&ctrl->deck->player),
                     player_get_loop_active(&ctrl->deck->player) ? 1 : 0,
                     player_get_loop_start_elapsed(&ctrl->deck->player),
                     player_get_loop_end_elapsed(&ctrl->deck->player),
                     player_get_elapsed(&ctrl->deck->player),
                     ctrl->deck->player.timecode_valid ? 1 : 0,
                     player_get_offset_drift(&ctrl->deck->player),
                     ctrl->deck->record->pathname);
    }

    if (n < 0 || (size_t)n >= sizeof reply) {
        fprintf(stderr, "control: STATUS reply truncated\n");
        return;
    }

    /* Short, non-blocking write - a dropped reply just costs Node one poll tick, not a stuck client. */
    if (write(ctrl->client_fd, reply, (size_t)n) == -1)
        perror("control: write STATUS reply");
}

/* Realtime thread - safe directly, player_set_relative_mode() is a plain field write. */
static void handle_relative(struct control *ctrl, bool on)
{
    if (ctrl->deck == NULL) {
        fprintf(stderr, "control: RELATIVE received before a deck was assigned\n");
        return;
    }

    fprintf(stderr, "control: RELATIVE %s\n", on ? "ON" : "OFF");
    player_set_relative_mode(&ctrl->deck->player, on);
}

/* Realtime thread - safe directly, player_seek_to_elapsed() is a plain field write. */
static void handle_seek(struct control *ctrl, const char *args)
{
    double seconds;

    if (ctrl->deck == NULL) {
        fprintf(stderr, "control: SEEK received before a deck was assigned\n");
        return;
    }

    if (sscanf(args, "%lf", &seconds) != 1) {
        fprintf(stderr, "control: malformed SEEK command '%s'\n", args);
        return;
    }

    fprintf(stderr, "control: SEEK %.3f\n", seconds);
    player_seek_to_elapsed(&ctrl->deck->player, seconds);
}

/* Realtime thread - safe directly, player_relocate() is a plain field write. */
static void handle_relocate(struct control *ctrl, const char *args)
{
    double seconds;

    if (ctrl->deck == NULL) {
        fprintf(stderr, "control: RELOCATE received before a deck was assigned\n");
        return;
    }

    if (sscanf(args, "%lf", &seconds) != 1) {
        fprintf(stderr, "control: malformed RELOCATE command '%s'\n", args);
        return;
    }

    fprintf(stderr, "control: RELOCATE %.3f\n", seconds);
    player_relocate(&ctrl->deck->player, seconds);
}

/* Realtime thread, safe directly. Explicit target, not "current position" - beat-grid snapping happens app-side. */
static void handle_set_cue(struct control *ctrl, const char *args)
{
    double seconds;

    if (ctrl->deck == NULL) {
        fprintf(stderr, "control: SET_CUE received before a deck was assigned\n");
        return;
    }

    if (sscanf(args, "%lf", &seconds) != 1) {
        fprintf(stderr, "control: malformed SET_CUE command '%s'\n", args);
        return;
    }

    fprintf(stderr, "control: SET_CUE %.3f\n", seconds);
    player_set_cue_point(&ctrl->deck->player, seconds);
}

/* Realtime thread - safe directly, player_cue() is a plain field write. */
static void handle_goto_cue(struct control *ctrl)
{
    if (ctrl->deck == NULL) {
        fprintf(stderr, "control: GOTO_CUE received before a deck was assigned\n");
        return;
    }

    fprintf(stderr, "control: GOTO_CUE\n");
    player_cue(&ctrl->deck->player);
}

/* Realtime thread - safe directly, player_cue_play() is a plain field write. */
static void handle_play_cue(struct control *ctrl)
{
    if (ctrl->deck == NULL) {
        fprintf(stderr, "control: PLAY_CUE received before a deck was assigned\n");
        return;
    }

    fprintf(stderr, "control: PLAY_CUE\n");
    player_cue_play(&ctrl->deck->player);
}

/* Realtime thread - safe directly, player_play() is a plain field write. */
static void handle_play(struct control *ctrl)
{
    if (ctrl->deck == NULL) {
        fprintf(stderr, "control: PLAY received before a deck was assigned\n");
        return;
    }

    fprintf(stderr, "control: PLAY\n");
    player_play(&ctrl->deck->player);
}

/* Realtime thread - safe directly, player_pause() is a plain field write. */
static void handle_pause(struct control *ctrl)
{
    if (ctrl->deck == NULL) {
        fprintf(stderr, "control: PAUSE received before a deck was assigned\n");
        return;
    }

    fprintf(stderr, "control: PAUSE\n");
    player_pause(&ctrl->deck->player);
}

/* Realtime thread - safe directly, player_set_loop()/player_clear_loop() are plain field writes. */
static void handle_loop(struct control *ctrl, const char *args)
{
    double start, end;

    if (ctrl->deck == NULL) {
        fprintf(stderr, "control: LOOP received before a deck was assigned\n");
        return;
    }

    if (!strcmp(args, "OFF")) {
        fprintf(stderr, "control: LOOP OFF\n");
        player_clear_loop(&ctrl->deck->player);
        return;
    }

    if (sscanf(args, "%lf %lf", &start, &end) != 2) {
        fprintf(stderr, "control: malformed LOOP command '%s'\n", args);
        return;
    }

    fprintf(stderr, "control: LOOP %.3f %.3f\n", start, end);
    player_set_loop(&ctrl->deck->player, start, end);
}

/*
 * Signal diagnostics, for the app's calibration screen.
 *
 * Separate from STATUS deliberately: STATUS is polled ~20 times a second for every deck and this is
 * only wanted while someone is actually looking at a calibration screen. None of it is used for
 * decoding.
 *
 * Levels are raw, in the same scale as the samples themselves (a 16-bit sample shifted left 16), so
 * a client divides by INT_MAX to get 0..1 rather than this having to pick a unit.
 */
static void handle_sensitivity(struct control *ctrl, const char *arg)
{
    unsigned int level;

    if (ctrl->deck == NULL) {
        fprintf(stderr, "control: SENSITIVITY received before a deck was assigned\n");
        return;
    }

    if (sscanf(arg, "%u", &level) != 1) {
        fprintf(stderr, "control: SENSITIVITY expects a level\n");
        return;
    }

    timecoder_set_sensitivity(&ctrl->deck->timecoder, level);
}

static void handle_signal(struct control *ctrl)
{
    char reply[256];
    struct timecoder *tc;
    int n;

    if (ctrl->deck == NULL) {
        fprintf(stderr, "control: SIGNAL received before a deck was assigned\n");
        return;
    }

    tc = &ctrl->deck->timecoder;

    /* peakLeft/peakRight diagnose the input itself - both low is a weak cartridge, one near zero is
     * a dead channel or an unplugged lead. refLevel is what bit decisions actually compare against
     * and self-calibrates, so it says how strong the timecode is once decoding. validCounter is how
     * many consecutive error checks have passed: the honest measure of lock quality. ticker is
     * samples since a valid timecode was read - it climbs the moment the needle leaves the record. */

    /* threshold travels with the reading rather than being reproduced by the client: it is
     * ZERO_THRESHOLD shifted down in phono mode, so a client deriving it would have to duplicate
     * that and stay in step with it. Sent last so an older client's parser is unaffected. */
    n = snprintf(reply, sizeof reply, "SIGNAL %d %d %d %u %u %d %d %d %u\n",
                 tc->peak_left, tc->peak_right, tc->ref_level,
                 tc->valid_counter, tc->timecode_ticker,
                 tc->forwards ? 1 : 0,
                 timecoder_get_safe(tc) ? 1 : 0,
                 tc->threshold,
                 tc->sensitivity);

    if (n < 0 || (size_t)n >= sizeof reply) {
        fprintf(stderr, "control: SIGNAL reply truncated\n");
        return;
    }

    /* Same short, non-blocking write as STATUS - a dropped reply costs one poll tick. */
    if (write(ctrl->client_fd, reply, (size_t)n) == -1)
        perror("control: write SIGNAL reply");
}

static void handle_line(struct control *ctrl, char *line)
{
    if (!strncmp(line, "LOAD ", 5)) {
        handle_load(ctrl, line + 5);
    } else if (!strcmp(line, "UNLOAD")) {
        handle_unload(ctrl);
    } else if (!strcmp(line, "STATUS")) {
        handle_status(ctrl);
    } else if (!strcmp(line, "SIGNAL")) {
        handle_signal(ctrl);
    } else if (!strncmp(line, "SENSITIVITY ", 12)) {
        handle_sensitivity(ctrl, line + 12);
    } else if (!strncmp(line, "SEEK ", 5)) {
        handle_seek(ctrl, line + 5);
    } else if (!strncmp(line, "RELOCATE ", 9)) {
        handle_relocate(ctrl, line + 9);
    } else if (!strncmp(line, "SET_CUE ", 8)) {
        handle_set_cue(ctrl, line + 8);
    } else if (!strcmp(line, "GOTO_CUE")) {
        handle_goto_cue(ctrl);
    } else if (!strcmp(line, "PLAY_CUE")) {
        handle_play_cue(ctrl);
    } else if (!strcmp(line, "PLAY")) {
        handle_play(ctrl);
    } else if (!strcmp(line, "PAUSE")) {
        handle_pause(ctrl);
    } else if (!strcmp(line, "RELATIVE ON")) {
        handle_relative(ctrl, true);
    } else if (!strcmp(line, "RELATIVE OFF")) {
        handle_relative(ctrl, false);
    } else if (!strncmp(line, "LOOP ", 5)) {
        handle_loop(ctrl, line + 5);
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

/* Drains every pending connection; loops until accept() would block (non-blocking socket). */
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

/* Drains everything available; loops until read() would block (non-blocking socket). */
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
