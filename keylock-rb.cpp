/*
 * Copyright (C) 2026 Matt Hall <info@waxcode.co>
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

/** The only C++ in xwax: a boundary that stops Rubber Band's exceptions killing a deck. */

#include "keylock-rb.h"

extern "C" {

int kl_rb_set_time_ratio(RubberBandState rb, double ratio)
{
    try {
        rubberband_set_time_ratio(rb, ratio);
        return 0;
    } catch (...) {
        return -1;
    }
}

int kl_rb_process(RubberBandState rb, const float *const *input,
                  unsigned int samples, int final)
{
    try {
        rubberband_process(rb, input, samples, final);
        return 0;
    } catch (...) {
        return -1;
    }
}

int kl_rb_retrieve(RubberBandState rb, float *const *output,
                   unsigned int samples, unsigned int *produced)
{
    try {
        *produced = rubberband_retrieve(rb, output, samples);
        return 0;
    } catch (...) {
        *produced = 0;
        return -1;
    }
}

int kl_rb_available(RubberBandState rb, int *available)
{
    try {
        *available = rubberband_available(rb);
        return 0;
    } catch (...) {
        *available = 0;
        return -1;
    }
}

int kl_rb_required(RubberBandState rb, unsigned int *required)
{
    try {
        *required = rubberband_get_samples_required(rb);
        return 0;
    } catch (...) {
        *required = 0;
        return -1;
    }
}

int kl_rb_reset(RubberBandState rb)
{
    try {
        rubberband_reset(rb);
        return 0;
    } catch (...) {
        return -1;
    }
}

/** Walks `count` ratios so every window they need is allocated HERE, off the realtime thread. */
int kl_rb_prewarm(RubberBandState rb, const double *ratios, unsigned int count)
{
    try {
        for (unsigned int i = 0; i < count; i++)
            rubberband_set_time_ratio(rb, ratios[i]);
        rubberband_set_time_ratio(rb, 1.0);
        return 0;
    } catch (...) {
        return -1;
    }
}

} /* extern "C" */
