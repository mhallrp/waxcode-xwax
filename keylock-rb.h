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

/*
 * Rubber Band, wrapped so an exception cannot kill the deck. See keylock-rb.cpp for why this exists.
 *
 * Every call returns 0 on success and -1 if something escaped. A -1 means the stretcher cannot be
 * trusted for this block, not that the deck is in trouble: keylock.c bypasses and carries on.
 */

#ifndef KEYLOCK_RB_H
#define KEYLOCK_RB_H

#include <rubberband/rubberband-c.h>

#ifdef __cplusplus
extern "C" {
#endif

int kl_rb_set_time_ratio(RubberBandState rb, double ratio);
int kl_rb_process(RubberBandState rb, const float *const *input,
                  unsigned int samples, int final);
int kl_rb_retrieve(RubberBandState rb, float *const *output,
                   unsigned int samples, unsigned int *produced);
int kl_rb_available(RubberBandState rb, int *available);
int kl_rb_required(RubberBandState rb, unsigned int *required);
int kl_rb_reset(RubberBandState rb);

/** Allocates the windows for every ratio given, then returns the stretcher to 1.0. */
int kl_rb_prewarm(RubberBandState rb, const double *ratios, unsigned int count);

#ifdef __cplusplus
}
#endif

#endif
