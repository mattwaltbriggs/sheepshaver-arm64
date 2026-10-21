# Known issues (arm64 / macOS 26+)

Note: for the upstream project, see the README. This file records problems
specific to the Apple-silicon build on current macOS, with enough analysis that a
future fix does not have to rediscover it.

## Guest video-mode changes deadlock

**Symptom.** A guest application that changes video mode (e.g. SimCity 2000 at
startup) hangs the emulator. The window stays black and the app stops
responding. No crash report is produced.

On a stock build the same operation can instead die with `SIGTRAP`
(`EXC_BREAKPOINT`) and a crash report showing:

```
asi: 'Must only be used from the main thread'
```

The deadlock and the crash are the same underlying problem reached by slightly
different orderings: `SDL_SetVideoMode` runs on the emulation thread.

**Status.** Not fixed. Only affects guest apps that switch video mode; normal
booting and everything else is unaffected.

### Why it happens

The guest's `VideoControl` trap executes on the emulation thread, so a mode
change reaches SDL from there:

```
emul_func → powerpc_cpu::execute → execute_native_op → VideoDoDriverIO
  → video_mode_change → switch_to_current_mode → video_open
  → driver_base::init → driver_base::set_video_mode → SDL_SetVideoMode
```

`SDL_SetVideoMode` destroys the previous window before creating the new one
(`SDL_DestroyWindow → SDL_HideWindow → -[NSWindow _doOrderWindow:]`), and macOS
26+ `WindowManagement` hard-traps when an `NSWindow` is hidden off the main
thread. So the call has to happen on the main thread.

The trouble is that **the main thread is never free to service work queued for
it**, which is the part that makes this resist the obvious fix. `SDLMain.m` calls
`SDL_main` *synchronously from inside* `applicationDidFinishLaunching:`:

```
main → -[NSApplication run] → nextEventMatchingMask:
  → AEProcessAppleEvent → _sendFinishLaunchingNotification
  → -[SDLMain applicationDidFinishLaunching:] → SDL_main
```

`NSApp run` has therefore already claimed the main thread's event loop before
`main_unix.cpp`'s own pump loop starts, and that loop is nested underneath it.
Consequences:

* `dispatch_sync(dispatch_get_main_queue(), ...)` deadlocks — blocks on the main
  queue are only drained when our run loop is the *active* one, and it never is.
* `-nextEventMatchingMask:` is called by SDL's `Cocoa_PumpEventsUntilDate`, so
  the main thread spends most of its time parked in AppKit's nested dispatch.

### Approaches already tried, and why they failed

All of these were tried and reverted. Recorded so they are not repeated.

1. **Marshalling the whole of `driver_base::init()` via a hand-rolled FIFO +
   `CFRunLoopSource`.** Deadlocked worse. `video_open()`/`video_close()` pair
   `LOCK_FRAME_BUFFER` with the lifetime of the redraw thread
   (`SDL_CreateThread(redraw_func)` in `video_open`, `SDL_WaitThread` +
   `UNLOCK_FRAME_BUFFER` in `video_close`). Moving the call moved the thread
   creation too, so the redraw thread was born on the main thread while the
   emulation thread blocked on a lock that thread then owned.

2. **Hand-rolling `CFRunLoopSourceContext`.** Crashed with `SIGSEGV` in
   `objc_lookUpImpOrForward`. The struct has **five** `void *` fields
   (`version`, `info`, `retain`, `release`, `copyDescription`, `equal`, `hash`)
   before its four function pointers; omitting `hash` put `perform` at offset 56
   instead of 72, so CF called `cancel` as `release` when tearing the source
   down. Never hand-roll it — include `<CoreFoundation/CFRunLoop.h>`.

3. **Marshalling only the `SDL_SetVideoMode` call via `dispatch_sync`, with the
   main thread parked in `CFRunLoopRunInMode`.** Still deadlocked, now on
   `video_open` → `LOCK_FRAME_BUFFER`. This proved the redraw thread was not the
   whole story: the block was enqueued on the main queue and simply never ran,
   because the main thread is inside AppKit's event loop (see above).

### What a real fix would look like

Stop letting `SDL_main` run inside AppKit's launch notification. Rework
`BasiliskII/src/SDL/SDLMain.m` so `applicationDidFinishLaunching:` starts the
emulator and returns, leaving `main()`'s own run loop owning the main thread.
Then queued work can actually be serviced, and `SDL_SetVideoMode` can be called
from the main thread. This changes startup for every guest, so it needs a clean
boot test as well as a video-mode test.

### Related context

The main thread currently does not drain `dispatch_get_main_queue()` at all.
That matters independently of this bug: `objc_destructInstance()` queues to the
main queue from whichever thread does the release, so autoreleased AppKit
objects are affected too. Fixing the `SDLMain` lifecycle above would address
both.
