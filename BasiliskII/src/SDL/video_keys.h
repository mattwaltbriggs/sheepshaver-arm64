/*
 *  video_keys.h - Keyboard interface between the platform layer and the
 *                 video driver
 *
 *  Basilisk II (C) 1997-2008 Christian Bauer
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

#ifndef VIDEO_KEYS_H
#define VIDEO_KEYS_H

/*
 *  On macOS the keyboard is handled by the Cocoa front end (SDLMain.m)
 *  instead of going through SDL 1.2: the SDL 1.2 keyboard path drops most
 *  of the key events on macOS and leaves the guest with stuck keys. The
 *  Cocoa layer therefore reports the key events directly.
 *
 *  "keycode" is the macOS virtual key code, "modifiers" is a mask of the
 *  VK_* bits below. See KNOWN-ISSUES.md for the details.
 */

#define VK_SHIFT    0x01		// Shift key held down
#define VK_CONTROL  0x02		// Control key held down
#define VK_OPTION   0x04		// Option (Alt) key held down
#define VK_COMMAND  0x08		// Command key held down
#define VK_CAPSLOCK 0x10		// Caps Lock is on

// Key code used when only the modifier state changed (no key was pressed or
// released).
#define VK_NO_KEY   0xffffffffu

#ifdef __cplusplus
extern "C" {
#endif

// Pump and process SDL events on the main thread
extern void VideoHandleEvents(void);

// Report a key event to the video driver. "down" is non-zero for a key press
// and zero for a key release, "is_repeat" is non-zero for an auto-repeat
// event (which the guest handles itself).
extern void VideoHandleKeyEvent(unsigned int keycode, int down, int is_repeat, unsigned int modifiers);

// Tell the video driver that the keyboard is handled by the platform layer,
// so that the SDL 1.2 keyboard events are discarded instead of queued.
extern void VideoSetNativeKeyboard(int enable);

#ifdef __cplusplus
}
#endif

#endif
