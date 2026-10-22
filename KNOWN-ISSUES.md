# Known issues (arm64 / macOS 26+)

Note: for the upstream project, see the README. This file records problems
specific to the Apple-silicon build on current macOS, with enough analysis that a
future fix does not have to rediscover it.

## Guest keyboard input (fixed in 2.5.5)

**Was.** Typing in the guest was badly broken: key presses were dropped,
repeats and stuck keys appeared, Backspace/Delete behaved incorrectly, and
sometimes the wrong character came out. The keyboard was effectively
unusable.

**Fixed in 2.5.5** by taking the keyboard away from SDL and reading macOS
`NSEvent`s directly.

### Root cause

The emulator still links the SDL 1.2 API (`libSDL-1.2.0.dylib` →
`libSDL2-2.0.0.dylib` → SDL3 3.4.18), and the keyboard travelled the whole
way through that stack:

1. **sdl12-compat** has a fixed event pool of `SDL12_MAXEVENTS` (128).
   `SDL_PushEvent_locked()` returns −1 when the pool is full, i.e. events are
   **dropped silently**.
2. **sdl2-compat** translates SDL2 events to SDL3 ones, again through a
   queue.
3. **SDL3's** Cocoa backend does not send `SDL_KEYDOWN` the way one expects:
   it goes through the text-input path (`setPendingKey:` →
   `interpretKeyEvents:` → `insertText:` → `sendPendingKey`) while `SDL_KEYUP`
   is sent unconditionally. `SDL_SendKeyboardKeyInternal()` swallows a `KEYUP`
   when `keystate[scancode]` is already false, so a dropped `KEYDOWN`
   turns into a **stuck or repeating key**.

Measured on this machine: roughly **80 % of all key events were lost**, and
the ones that got through were delayed by up to 18 s. AppKit's own
`NSEvent` delivery, in contrast, was prompt and complete.

### The fix

* **`BasiliskII/src/SDL/video_keys.h`** (new) — shared declarations for the
  native keyboard path (`VK_*` modifier bits and the C-linkage entry points
  `VideoHandleEvents`, `VideoHandleKeyEvent`, `VideoSetNativeKeyboard`).
  `BasiliskII/src/include/video.h` cannot be used for this: it is C++-only
  and in this build the *copy* in `SheepShaver/src/include/video.h` is the
  one that is included.

* **`BasiliskII/src/SDL/SDLMain.m`** — installs an `NSEvent` local monitor for
  `KeyDown | KeyUp | FlagsChanged` and calls `VideoSetNativeKeyboard(1)`.
  The monitor is installed before `SDL_main` starts, so no key press is
  missed. The `NSTimer` that pumps SDL events is registered in
  `NSRunLoopCommonModes` so that it keeps firing during modal sessions.

* **`BasiliskII/src/SDL/video_sdl.cpp`** — `VideoHandleKeyEvent()` translates
  the macOS virtual key code to an ADB key code and calls `ADBKeyDown()` /
  `ADBKeyUp()`. Notable details:

  * `NSEvent.isARepeat` is ignored: the guest does its own key repeat.
  * The modifier keys are reported as ordinary ADB key codes (0x36–0x3a),
    which is what the SDL 1.2 path did and what the guest's ADB manager
    expects. `NSEvent.modifierFlags` drives them.
  * Caps Lock is a **toggle**: the guest flips its caps state when it sees
    the key *press* of 0x39, so every change of the modifier flag is
    reported as a complete press/release pair — again exactly what the
    SDL 1.2 path did.
  * The hotkeys (Ctrl-Tab suspend, Ctrl-Return fullscreen, Ctrl-Esc quit,
    Ctrl-F1 mount floppy, Ctrl-F5 grab mouse, Ctrl-Space resume) are
    recognised on the key *press* and the key is then **swallowed**, so it
    can never get stuck in the guest's key state. The matching key release
    is swallowed as well.
  * The generic SDL 1.2 key path is left intact for Linux/Windows. On
    macOS the `native_keyboard` flag makes `handle_events()` skip
    `SDL_KEYDOWN`/`SDL_KEYUP` so that nothing is delivered twice, and
    `SDL_EventState()` is used to keep the events out of the queue in the
    first place.

* **`SheepShaver/src/adb.cpp`** — the ADB key ring was made thread-safe
  (`key_lock`; producer = main thread, consumer = emulation thread in
  `ADBInterrupt()`) and enlarged from 16 to 256 entries with oldest-first
  overflow, so that a burst of typing can no longer overflow it between
  two 60 Hz interrupts.

### Note

`pump_cocoa_events()` must **not** be called: it calls
`[NSApp nextEventMatchingMask:]`, which steals keyboard events (see the
notes at the end of this file).

## Audio device failure freezes the emulator (fixed in 2.5.5)

**Was.** On a machine where the CoreAudio output device cannot be opened
(see below), the emulator did not start at all: it showed an alert and then
sat there, dead, with no window and no keyboard.

### Root cause

`open_sdl_audio()` fails, `WarningAlert()` is called, and `WarningAlert()`
ends up in `macosx_display_alert()`, which runs `[alert runModal]`.

The emulator's whole initialization runs on the **main thread** inside
`SDL_main` (`InitAll()` → `AudioInit()` → `WarningAlert()`), so a modal
alert there stops the initialization: the SDL window is never created, the
emulation thread is never started, and the app looks hung.

`macosx_post_alert()` was introduced to avoid this, but it still used
`-[NSAlert show]` for the "no parent window yet" case — and `-[NSAlert show]`
behaves like `runModal`: it runs a modal session. Verified with a 60-line
AppKit test program: after `[alert show]`, the main thread never returns
from the call **and the main dispatch queue is not serviced any more**
(a `dispatch_async` block queued afterwards never ran, and a
`dispatch_sync` from another thread hung). The emulator marshals
`SDL_SetVideoMode()` to the main queue with `dispatch_sync`, so this is
fatal even when the alert itself is dismissed.

### The fix

* **`BasiliskII/src/MacOSX/utils_macosx.mm`** — `macosx_post_alert()` now
  *always* hands the work to the main queue with `dispatch_async()`, so the
  caller (which may be the main thread in the middle of the initialization)
  continues immediately, and the alert is shown as a **sheet** on an
  existing window. A sheet does not stop the run loop. If there is no
  window yet, the message is put into a window of its own instead of
  using `-[NSAlert show]`.
* **`SheepShaver/src/Unix/main_unix.cpp`** — `WarningAlert()` uses
  `macosx_post_alert()` and additionally writes the message to stderr, so
  the warning is visible in `~/Library/Logs/SheepShaver.log` even if the
  window is missed.

Result: with the audio device unavailable the emulator now boots and runs
(silently), and the warning is only informational.

### The CoreAudio failure itself (not fixed — it is an OS problem)

The error seen on this machine is

```
WARNING: Cannot open audio: CoreAudio error (AudioQueueStart): 2003329396
```

`'what'` 2003329396 is `HALC_ShellDriverPlugIn::Open: opening the plug-in
failed`: macOS' own `AppleHDAEngineOutput` factory
(`F8BB1C28-BAE8-11D6-9C31-00039315CD46`) cannot be opened. The failure is
**intermittent** — the same binary boots with working sound minutes later —
and it is not caused by the emulator: the released 2.5.4 build fails
identically. Third-party audio drivers installed on this machine
(`BlackHole2ch.driver`, `MSTeamsAudioDevice.driver`, `ZoomAudioDevice.driver`)
are a likely contributor; `afplay` (which uses AUHAL instead of AudioQueue)
keeps working.

Things worth trying on the affected machine: `sudo killall coreaudiod`,
remove any Multi-Output Device / BlackHole Multi-Output from *Audio MIDI
Setup*, or reboot. Until then, the emulator runs without sound.

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
