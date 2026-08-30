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

#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "debug.h"
#include "device.h"
#include "player.h"
#include "timecoder.h"

void device_init(struct device *dv, struct device_ops *ops)
{
    debug("%p", dv);
    dv->fault = false;
    dv->ops = ops;
}

/*
 * Clear (destruct) the device. The corresponding constructor is
 * specific to each particular audio system
 */

void device_clear(struct device *dv)
{
    if (dv->ops->clear != NULL)
        dv->ops->clear(dv);
}

void device_connect_timecoder(struct device *dv, struct timecoder *tc)
{
    dv->timecoder = tc;
}

void device_connect_player(struct device *dv, struct player *pl)
{
    dv->player = pl;
}

/*
 * Return: the sample rate of the device in Hz
 */

unsigned int device_sample_rate(struct device *dv)
{
    assert(dv->ops->sample_rate != NULL);
    return dv->ops->sample_rate(dv);
}

/*
 * Start the device inputting and outputting audio
 */

void device_start(struct device *dv)
{
    if (dv->ops->start != NULL)
        dv->ops->start(dv);
}

/*
 * Stop the device
 */

void device_stop(struct device *dv)
{
    if (dv->ops->stop != NULL)
        dv->ops->stop(dv);
}

/*
 * Get file descriptors which should be polled for this device
 *
 * Do not return anything for callback-based audio systems. If the
 * return value is > 0, there must be a handle() function available.
 *
 * Return: the number of pollfd filled, or -1 on error
 */

ssize_t device_pollfds(struct device *dv, struct pollfd *pe, size_t z)
{
    if (dv->ops->pollfds != NULL)
        return dv->ops->pollfds(dv, pe, z);
    else
        return 0;
}

/*
 * Handle any available input or output on the device
 *
 * This function can be called when there is activity on any file
 * descriptor, not specifically one returned by this device.
 */

void device_handle(struct device *dv)
{
    if (dv->fault)
        return;

    if (dv->ops->handle == NULL)
        return;

    if (dv->ops->handle(dv) != 0) {
        dv->fault = true;
        fputs("Error handling audio device; disabling it\n", stderr);
    }
}

/*
 * Send audio from a device for processing
 *
 * Pre: buffer pcm contains n stereo samples
 */

void device_submit(struct device *dv, signed short *pcm, size_t n)
{
    assert(dv->timecoder != NULL);
    timecoder_submit(dv->timecoder, pcm, n);
}

/* Frames per pass through the interleave below. Fixed and small so the two scratch buffers sit on
 * the stack - the realtime thread must not allocate - while staying large enough that the loop
 * overhead is nothing beside the filtering. */
#define COLLECT_CHUNK 256

/*
 * Collect audio from the processing to send to a device
 *
 * Post: buffer pcm is filled with n frames of DEVICE_PLAYBACK_CHANNELS samples - the track flat on
 * channels 0-1, and inverse-RIAA'd and attenuated on 2-3.
 *
 * Both are produced every time rather than one being selected, so a deck's line and phono outputs
 * are both always live and it is the cable that decides which is used, not a setting.
 */

void device_collect(struct device *dv, signed short *pcm, size_t n)
{
    signed short line[COLLECT_CHUNK * DEVICE_CHANNELS];
    signed short phono[COLLECT_CHUNK * DEVICE_CHANNELS];
    size_t done = 0;

    assert(dv->player != NULL);

    while (done < n) {
        size_t chunk, i;

        chunk = n - done;
        if (chunk > COLLECT_CHUNK)
            chunk = COLLECT_CHUNK;

        player_collect(dv->player, line, chunk);

        /* Filtered from a copy so the line half stays flat. riaa_apply is a stateful IIR, and
         * chunking preserves sample order, so its history stays continuous across passes. */
        memcpy(phono, line, chunk * DEVICE_CHANNELS * sizeof *phono);
        riaa_apply(&dv->riaa, phono, chunk);

        for (i = 0; i < chunk; i++) {
            signed short *frame = pcm + (done + i) * DEVICE_PLAYBACK_CHANNELS;

            frame[0] = line[i * DEVICE_CHANNELS];
            frame[1] = line[i * DEVICE_CHANNELS + 1];
            frame[2] = phono[i * DEVICE_CHANNELS];
            frame[3] = phono[i * DEVICE_CHANNELS + 1];
        }

        done += chunk;
    }
}
