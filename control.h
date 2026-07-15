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

#ifndef CONTROL_H
#define CONTROL_H

struct controller;
struct rt;

/*
 * A minimal Unix-socket controller for remote track loading - see
 * CLAUDE.md's "xwax fork" section. One socket = one deck (no <deck>
 * parameter in the protocol - the socket path itself identifies
 * which deck this is), matching "one xwax process per deck".
 *
 * Proof-of-concept scope: LOAD <path> only, to validate real-time
 * tracking feel before building out PASSTHRU/STATUS.
 */
int control_init(struct controller *c, struct rt *rt, const char *path);

#endif
