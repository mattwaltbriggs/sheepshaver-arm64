/*
 *	utils_macosx.mm - Mac OS X utility functions.
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

#include "utils_macosx.h"
#include "sysdeps.h"

// define _UINT64 to avoid a conflict with a Security framework header
#define _UINT64
#include <Cocoa/Cocoa.h>
#include "sdl_compat.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// This is used from video_sdl.cpp.
void NSAutoReleasePool_wrap(void (*fn)(void))
{
	NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
	fn();
	[pool release];
}

bool macosx_display_alert(const char *title, const char *text, const char *first_button, const char *second_button)
{
	NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
	NSAlert *alert = [[[NSAlert alloc] init] autorelease];
	NSString *message = title ? [NSString stringWithUTF8String:title] : @"SheepShaver";
	NSString *informativeText = text ? [NSString stringWithUTF8String:text] : @"";

	[alert setMessageText:message];
	[alert setInformativeText:informativeText];
	[alert addButtonWithTitle:first_button ? [NSString stringWithUTF8String:first_button] : @"OK"];
	if (second_button)
		[alert addButtonWithTitle:[NSString stringWithUTF8String:second_button]];

	NSInteger result = [alert runModal];
	[pool release];
	return result == NSAlertFirstButtonReturn;
}

// Show the alert in a way that never stops the run loop.
//
// -[NSAlert show] and -[NSAlert runModal] both run a modal session: the
// main thread stays inside the modal loop and, worse, the main dispatch
// queue is not serviced any more. The emulator runs its initialization on
// the main thread and marshals SDL_SetVideoMode() to it, so a blocking
// alert freezes the whole emulator (see KNOWN-ISSUES.md).
//
// A sheet attached to an existing window does not have that problem, and if
// there is no window yet (the alert is shown while the emulator is still
// starting up) the message goes into a window of its own.
static void show_alert_panel(const char *title, const char *text)
{
	NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

	@try {
		NSString *message = title ? [NSString stringWithUTF8String:title] : @"SheepShaver";
		NSString *informative = text ? [NSString stringWithUTF8String:text] : @"";

		// Prefer a sheet on the emulator window: it stays clearly attached
		// to the window it belongs to and does not stop the run loop.
		NSWindow *parent = [NSApp mainWindow];
		if (!parent) {
			for (NSWindow *w in [NSApp windows]) {
				if ([w isVisible]) {
					parent = w;
					break;
				}
			}
		}

		if (parent) {
			NSAlert *alert = [[[NSAlert alloc] init] autorelease];
			[alert setAlertStyle:NSInformationalAlertStyle];
			[alert setMessageText:message];
			[alert setInformativeText:informative];
			[alert addButtonWithTitle:@"OK"];
			[alert beginSheetModalForWindow:parent completionHandler:nil];
		} else {
			// No window to attach the sheet to: use a window of our own
			// instead of -[NSAlert show], which would block the run loop.
			NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 420, 130)
				styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable)
				backing:NSBackingStoreBuffered defer:NO];
			[w setTitle:message];
			[w setReleasedWhenClosed:NO];

			NSTextField *info = [[[NSTextField alloc] initWithFrame:NSMakeRect(16, 16, 388, 92)] autorelease];
			[info setStringValue:informative];
			[info setEditable:NO];
			[info setSelectable:YES];
			[info setBordered:NO];
			[info setDrawsBackground:NO];
			[w setContentView:info];

			[w center];
			[w makeKeyAndOrderFront:nil];
		}
	} @catch (NSException *e) {
		fprintf(stderr, "Exception while showing alert: %s\n", [[e reason] UTF8String]);
	}

	[pool release];
}

// Displays a non-modal informational alert.
//
// This never blocks the caller: the work is always handed to the main
// queue, so that the caller (which may be the emulator's initialization
// code running on the main thread) can continue. See KNOWN-ISSUES.md.
void macosx_post_alert(const char *title, const char *text)
{
	// Copy the strings: the block may run much later.
	char *title_copy = title ? strdup(title) : NULL;
	char *text_copy = text ? strdup(text) : NULL;

	dispatch_async(dispatch_get_main_queue(), ^{
		show_alert_panel(title_copy, text_copy);
		free(title_copy);
		free(text_copy);
	});
}

#if SDL_VERSION_ATLEAST(2,0,0)
void disable_SDL2_macosx_menu_bar_keyboard_shortcuts() {
	for (NSMenuItem * menu_item in [NSApp mainMenu].itemArray) {
		if (menu_item.hasSubmenu) {
			for (NSMenuItem * sub_item in menu_item.submenu.itemArray) {
				sub_item.keyEquivalent = @"";
				sub_item.keyEquivalentModifierMask = 0;
			}
		}
		if ([menu_item.title isEqualToString:@"View"]) {
			[[NSApp mainMenu] removeItem:menu_item];
			break;
		}
	}
}

bool is_fullscreen_osx(SDL_Window * window)
{
	if (!window) {
		return false;
	}
	
	SDL_SysWMinfo wmInfo;
	SDL_VERSION(&wmInfo.version);
	if (!SDL_GetWindowWMInfo(window, &wmInfo)) {
		return false;
	}

	const NSWindowStyleMask styleMask = [wmInfo.info.cocoa.window styleMask];
	return (styleMask & NSWindowStyleMaskFullScreen) != 0;
}
#endif

void set_menu_bar_visible_osx(bool visible)
{
	[NSMenu setMenuBarVisible:(visible ? YES : NO)];
}

void set_current_directory()
{
	NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
	chdir([[[[NSBundle mainBundle] bundlePath] stringByDeletingLastPathComponent] UTF8String]);
	[pool release];
}

static void *load_sdl_library()
{
	static void *handle = NULL;
	static bool attempted = false;

	if (attempted)
		return handle;

	attempted = true;

	const char *paths[] = {
		"@executable_path/../Frameworks/libSDL.dylib",
		"/opt/homebrew/opt/sdl12-compat/lib/libSDL.dylib",
		"/opt/homebrew/opt/sdl12-compat/lib/libSDL-1.2.0.dylib",
		"/opt/homebrew/lib/libSDL.dylib",
		"/opt/homebrew/lib/libSDL-1.2.0.dylib",
		"libSDL.dylib",
		NULL
	};

	for (const char **path = paths; *path; ++path) {
		handle = dlopen(*path, RTLD_LAZY | RTLD_LOCAL);
		if (handle)
			return handle;
	}

	fprintf(stderr, "Unable to load SDL 1.2 compatibility library: %s\n", dlerror());
	abort();
}

template<typename Function>
static Function load_sdl_symbol(const char *name)
{
	void *symbol = dlsym(load_sdl_library(), name);
	if (!symbol) {
		fprintf(stderr, "Unable to load SDL symbol %s: %s\n", name, dlerror());
		abort();
	}
	return reinterpret_cast<Function>(symbol);
}

#ifdef SDL_malloc
#undef SDL_malloc
#endif
#ifdef SDL_free
#undef SDL_free
#endif
#ifdef SDL_strlen
#undef SDL_strlen
#endif
#ifdef SDL_strlcpy
#undef SDL_strlcpy
#endif

#define FORWARD_SDL(return_type, name, arguments, call_arguments) \
extern "C" return_type SDLCALL name arguments \
{ \
	typedef return_type (SDLCALL *Function) arguments; \
	static Function function = load_sdl_symbol<Function>(#name); \
	return function call_arguments; \
}

#define FORWARD_SDL_VOID(name, arguments, call_arguments) \
extern "C" void SDLCALL name arguments \
{ \
	typedef void (SDLCALL *Function) arguments; \
	static Function function = load_sdl_symbol<Function>(#name); \
	function call_arguments; \
}

FORWARD_SDL(int, SDL_Init, (Uint32 flags), (flags))
FORWARD_SDL_VOID(SDL_Quit, (void), ())
FORWARD_SDL(char *, SDL_GetError, (void), ())
FORWARD_SDL(const SDL_version *, SDL_Linked_Version, (void), ())

FORWARD_SDL(char *, SDL_AudioDriverName, (char *namebuf, int maxlen), (namebuf, maxlen))
FORWARD_SDL(int, SDL_OpenAudio, (SDL_AudioSpec *desired, SDL_AudioSpec *obtained), (desired, obtained))
FORWARD_SDL_VOID(SDL_PauseAudio, (int pause_on), (pause_on))
FORWARD_SDL_VOID(SDL_CloseAudio, (void), ())
FORWARD_SDL_VOID(SDL_LockAudio, (void), ())
FORWARD_SDL_VOID(SDL_UnlockAudio, (void), ())
FORWARD_SDL_VOID(SDL_MixAudio, (Uint8 *dst, const Uint8 *src, Uint32 len, int volume), (dst, src, len, volume))

FORWARD_SDL(char *, SDL_VideoDriverName, (char *namebuf, int maxlen), (namebuf, maxlen))
FORWARD_SDL(const SDL_VideoInfo *, SDL_GetVideoInfo, (void), ())
FORWARD_SDL(int, SDL_VideoModeOK, (int width, int height, int bpp, Uint32 flags), (width, height, bpp, flags))
FORWARD_SDL(SDL_Rect **, SDL_ListModes, (SDL_PixelFormat *format, Uint32 flags), (format, flags))
FORWARD_SDL(SDL_Surface *, SDL_SetVideoMode, (int width, int height, int bpp, Uint32 flags), (width, height, bpp, flags))
FORWARD_SDL_VOID(SDL_UpdateRects, (SDL_Surface *screen, int numrects, SDL_Rect *rects), (screen, numrects, rects))
FORWARD_SDL_VOID(SDL_UpdateRect, (SDL_Surface *screen, Sint32 x, Sint32 y, Uint32 w, Uint32 h), (screen, x, y, w, h))
FORWARD_SDL(int, SDL_SetPalette, (SDL_Surface *surface, int flags, SDL_Color *colors, int firstcolor, int ncolors), (surface, flags, colors, firstcolor, ncolors))
FORWARD_SDL(Uint32, SDL_MapRGB, (const SDL_PixelFormat * const format, const Uint8 r, const Uint8 g, const Uint8 b), (format, r, g, b))
FORWARD_SDL_VOID(SDL_FreeSurface, (SDL_Surface *surface), (surface))
FORWARD_SDL(int, SDL_LockSurface, (SDL_Surface *surface), (surface))
FORWARD_SDL_VOID(SDL_UnlockSurface, (SDL_Surface *surface), (surface))
FORWARD_SDL(SDL_Surface *, SDL_ConvertSurface, (SDL_Surface *src, SDL_PixelFormat *fmt, Uint32 flags), (src, fmt, flags))
FORWARD_SDL(int, SDL_UpperBlit, (SDL_Surface *src, SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect), (src, srcrect, dst, dstrect))
FORWARD_SDL_VOID(SDL_WM_SetCaption, (const char *title, const char *icon), (title, icon))
FORWARD_SDL(SDL_GrabMode, SDL_WM_GrabInput, (SDL_GrabMode mode), (mode))

FORWARD_SDL(Uint8, SDL_GetMouseState, (int *x, int *y), (x, y))
FORWARD_SDL_VOID(SDL_WarpMouse, (Uint16 x, Uint16 y), (x, y))
FORWARD_SDL(SDL_Cursor *, SDL_CreateCursor, (Uint8 *data, Uint8 *mask, int w, int h, int hot_x, int hot_y), (data, mask, w, h, hot_x, hot_y))
FORWARD_SDL_VOID(SDL_SetCursor, (SDL_Cursor *cursor), (cursor))
FORWARD_SDL_VOID(SDL_FreeCursor, (SDL_Cursor *cursor), (cursor))
FORWARD_SDL(int, SDL_ShowCursor, (int toggle), (toggle))

FORWARD_SDL_VOID(SDL_PumpEvents, (void), ())
FORWARD_SDL(int, SDL_PeepEvents, (SDL_Event *events, int numevents, SDL_eventaction action, Uint32 mask), (events, numevents, action, mask))
FORWARD_SDL(int, SDL_PushEvent, (SDL_Event *event), (event))

FORWARD_SDL(SDL_mutex *, SDL_CreateMutex, (void), ())
FORWARD_SDL(int, SDL_mutexP, (SDL_mutex *mutex), (mutex))
FORWARD_SDL(int, SDL_mutexV, (SDL_mutex *mutex), (mutex))
FORWARD_SDL_VOID(SDL_DestroyMutex, (SDL_mutex *mutex), (mutex))
FORWARD_SDL(SDL_sem *, SDL_CreateSemaphore, (Uint32 initial_value), (initial_value))
FORWARD_SDL(int, SDL_SemPost, (SDL_sem *sem), (sem))
FORWARD_SDL(int, SDL_SemWait, (SDL_sem *sem), (sem))
FORWARD_SDL_VOID(SDL_DestroySemaphore, (SDL_sem *sem), (sem))

FORWARD_SDL(SDL_Thread *, SDL_CreateThread, (int (SDLCALL *fn)(void *), void *data), (fn, data))
FORWARD_SDL_VOID(SDL_WaitThread, (SDL_Thread *thread, int *status), (thread, status))

FORWARD_SDL(void *, SDL_malloc, (size_t size), (size))
FORWARD_SDL_VOID(SDL_free, (void *mem), (mem))
FORWARD_SDL(size_t, SDL_strlen, (const char *string), (string))
FORWARD_SDL(size_t, SDL_strlcpy, (char *dst, const char *src, size_t maxlen), (dst, src, maxlen))

#undef FORWARD_SDL
#undef FORWARD_SDL_VOID
