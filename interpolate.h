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
