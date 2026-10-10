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
 * Inverse RIAA equalisation and attenuation for the playback output.
 *
 * Lets a digital track share a mixer's phono channel with a real record: the box emits a
 * cartridge-level, pre-emphasised signal, and the mixer's own phono stage undoes both, exactly as
 * it would for vinyl. Without it the owner has to flip the mixer between phono and line whenever
 * they swap between a real record and a digital track, mid-set.
 */

#ifndef RIAA_H
#define RIAA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RIAA_CHANNELS 2

struct riaa {
    bool active;
    double b[3], a[3];              /* biquad, a[0] normalised to 1 */
    double x[RIAA_CHANNELS][2];     /* per-channel input history */
    double y[RIAA_CHANNELS][2];     /* per-channel output history */
    uint32_t dither[RIAA_CHANNELS]; /* xorshift state, one per channel */
};

void riaa_init(struct riaa *ri, unsigned int rate, double attenuate_db);
void riaa_apply(struct riaa *ri, signed short *pcm, size_t frames);

#endif
