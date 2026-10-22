/*
 *	utils_macosx.h - Mac OS X utility functions.
 *
 *  Copyright (C) 2011 Alexei Svitkine
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef UTILS_MACOSX_H
#define UTILS_MACOSX_H

// Invokes the specified function with an NSAutoReleasePool in place.
void NSAutoReleasePool_wrap(void (*fn)(void));

// Displays a native modal alert. Returns true when the first button is pressed.
bool macosx_display_alert(const char *title, const char *text, const char *first_button, const char *second_button);

// Displays a non-modal informational alert. The run loop keeps running, so
// the emulator stays usable while the alert is on screen.
void macosx_post_alert(const char *title, const char *text);

#endif
