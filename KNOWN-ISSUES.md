# Known issues (arm64 / macOS 26+)

Note: for the upstream project, see the README. This file records problems
specific to the Apple-silicon build on current macOS, with enough analysis that a
future fix does not have to rediscover it.

## Guest video-mode changes (fixed in 2.5.4)

**Was.** A guest application that changed video mode (e.g. SimCity 2000 at
startup) hung the emulator: the window went black and the app stopped
responding, with no crash report. On a stock build the same operation could
instead die with `SIGTRAP` (`EXC_BREAKPOINT`) and a crash report showing:

```
asi: 'Must only be used from the main thread'
```

Both symptoms were the same underlying problem reached by slightly different
orderings: `SDL_SetVideoMode` ran on the emulation thread. **Fixed in 2.5.4**;
verified by booting a guest and switching modes in SimCity 2000.

### Root cause

There were two independent problems, and both had to be fixed.

**1. The main thread was never free to service marshalled work.** `SDLMain.m`
called `SDL_main` *synchronously from inside* `applicationDidFinishLaunching:`:

```
main → -[NSApplication run] → nextEventMatchingMask:
  → AEProcessAppleEvent → _sendFinishLaunchingNotification
  → -[SDLMain applicationDidFinishLaunching:] → SDL_main
```

`-[NSApplication run]` had therefore already claimed the main thread's event
loop, and `main_unix.cpp`'s own pump loop was nested underneath it. A block
queued with `dispatch_sync(dispatch_get_main_queue(), ...)` is only drained when
*our* run loop is the active one, which it never was — so the block was
enqueued and never ran.

**2. `frame_buffer_lock` was a mutex used as a cross-thread flag.** This is the
part that produced the permanent hang, and it is worth recording precisely.

`video_open()` took `LOCK_FRAME_BUFFER`; `video_close()` released it. Those two
calls happen on **different threads**:

* at startup, `VideoInit()` → `video_open()` runs on the **main** thread,
  because it happens before the emulation thread exists;
* a guest mode change runs `video_close()`/`video_open()` on the **emulation**
  thread.

An `SDL_mutex` belongs to the thread that locked it, so unlocking it from
another thread left it corrupted: `SDL` then never satisfied the next lock and
`video_open()` blocked forever in `SDL_LockMutex_REAL`. A per-thread lock-depth
probe showed the failure exactly — the emulation thread ran `video_close`
(depth 0), unlocked, then entered `video_open` at depth **−1**, i.e. "already
held by a thread that does not hold it", and blocked there permanently while the
main thread sat idle.

The lock was never really expressing ownership. It expressed *"a video session
is open"* — the redraw thread is alive and the frame buffer must not be touched
elsewhere. That is a flag, not an owner.

### The fix

Three changes, all required:

* **`BasiliskII/src/SDL/SDLMain.m`** — `applicationDidFinishLaunching:` now runs
  `SDL_main`'s startup inline on the main thread and then **returns**, instead of
  `exit()`ing. Returning hands the main thread to `-[NSApplication run]`, whose
  run loop becomes the active one and can service AppKit work. SDL events are
  pumped by an `NSTimer` (0.01 s) added here, since the pump loop that used to
  live on the main thread is gone.

* **`SheepShaver/src/Unix/main_unix.cpp`** — `SDL_main` now starts the emulation
  on a worker thread and **returns** immediately (no pump loop, no
  `pthread_join`), leaving `NSApp run` owning the main thread. Adds
  `run_on_main_thread_blocking()`, which runs a block inline if already on the
  main thread (`pthread_main_np()`) and otherwise `dispatch_sync`s it to the main
  queue. Declared in `SheepShaver/src/include/main.h`.

* **`BasiliskII/src/SDL/video_sdl.cpp`** — replaces `frame_buffer_lock` with a
  `frame_buffer_held` flag guarded by its own mutex and signalled with a
  condition variable (`frame_buffer_acquire()` / `frame_buffer_release()`). Any
  thread may set or clear it, and the temporary give-up-and-retake in
  `VideoVBL()`/`VideoInterrupt()` (a release followed by an acquire) behaves as
  before. Also marshals **only** the `SDL_SetVideoMode()` call to the main
  thread via `run_on_main_thread_blocking()`, with a `set_video_mode_reentering`
  guard so a nested call runs directly rather than re-dispatching.

### Approaches already tried, and why they failed

All of these were tried and reverted. Recorded so they are not repeated.

1. **Marshalling the whole of `driver_base::init()` via a hand-rolled FIFO +
   `CFRunLoopSource`.** Deadlocked worse. `video_open()`/`video_close()` pair
   `LOCK_FRAME_BUFFER` with the lifetime of the redraw thread
   (`SDL_CreateThread(redraw_func)` in `video_open`, `SDL_WaitThread` +
   `UNLOCK_FRAME_BUFFER` in `video_close`). Moving the call moved the thread
   creation too, so the redraw thread was born on the main thread while the
   emulation thread blocked on a lock that thread then owned. This is why the
   fix marshals **only** `SDL_SetVideoMode`, never the surrounding `init()`.

2. **Hand-rolling `CFRunLoopSourceContext`.** Crashed with `SIGSEGV` in
   `objc_lookUpImpOrForward`. The struct has **five** `void *` fields
   (`version`, `info`, `retain`, `release`, `copyDescription`, `equal`, `hash`)
   before its four function pointers; omitting `hash` put `perform` at offset 56
   instead of 72, so CF called `cancel` as `release` when tearing the source
   down. Never hand-roll it — include `<CoreFoundation/CFRunLoop.h>`.

3. **Marshalling only `SDL_SetVideoMode` via `dispatch_sync`, with the main
   thread parked in `CFRunLoopRunInMode`.** Still deadlocked, now on
   `video_open` → `LOCK_FRAME_BUFFER`. This proved the `SDLMain` lifecycle was
   also broken: the block was enqueued on the main queue and simply never ran,
   because the main thread was inside AppKit's event loop (problem 1 above).

### Notes for future work

* `SDL_PumpEvents` must run on the main thread. Do **not** call
  `pump_cocoa_events()` — it calls `[NSApp nextEventMatchingMask:]`, which steals
  keyboard events from SDL2's queue (mouse arrives via `CGEventTap`, which is why
  mouse worked but keyboard did not).

* Startup must stay on the main thread: `prefs_init()` edits the main menu and
  sets `NSApp`'s delegate, and off the main thread macOS 26+ throws
  `NSInternalInconsistencyException: 'API misuse: modification of a menu's items
  on a non-main thread when the menu is part of the main menu.'`

* `ENABLE_VOSF` is **not** defined in this build, so `use_vosf` is `false` and
  the refresh path is the incremental `video_refresh_window_static` /
  `update_display_static_bbox` (memcmp against `the_buffer_copy`), not VOSF.

* Signing must stay ad-hoc **without** `--options runtime`; the hardened runtime
  makes dyld reject the vendored SDL dylibs (library validation / Team ID
  mismatch) and the app dies at launch. See `build.sh`.
