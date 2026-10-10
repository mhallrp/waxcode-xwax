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
 * Shared 4-point cubic interpolation.
 *
 * Was static in player.c. Moved here when keylock.c arrived so the varispeed resampler and the
 * key-lock grain reader read source samples through the same kernel, rather than two copies kept
 * in step by hand.
 */

#ifndef INTERPOLATE_H
#define INTERPOLATE_H

static inline double cubic_interpolate(signed short y[4], double mu)
{
    signed long a0, a1, a2, a3;
    double mu2;

    mu2 = mu * mu;
    a0 = y[3] - y[2] - y[0] + y[1];
    a1 = y[0] - y[1] - a0;
    a2 = y[2] - y[0];
    a3 = y[1];

    return (mu * mu2 * a0) + (mu2 * a1) + (mu * a2) + a3;
}

#endif
