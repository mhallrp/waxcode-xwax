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

static void *worker_main(void *arg)
{
    struct control *ctrl = arg;

    pthread_mutex_lock(&ctrl->lock);
    for (;;) {
        while (ctrl->pending_path == NULL && !ctrl->shutdown)
            pthread_cond_wait(&ctrl->cond, &ctrl->lock);

        if (ctrl->shutdown) {
            pthread_mutex_unlock(&ctrl->lock);
            return NULL;
        }

        char *path = ctrl->pending_path;
        ctrl->pending_path = NULL;
        pthread_mutex_unlock(&ctrl->lock);

        do_load(ctrl, path); /* takes ownership of path, do not free here */

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

static void handle_line(struct control *ctrl, char *line)
{
    if (!strncmp(line, "LOAD ", 5)) {
        handle_load(ctrl, line + 5);
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
