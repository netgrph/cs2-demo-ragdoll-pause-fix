# Source

`src/RagdollPauseFix.cpp` is the whole fix — one file, plain Win32 and the C++17 standard library, no third-party dependencies.

## Build

Needs Visual Studio 2022 or newer with **Desktop development with C++** (that's where CMake and Ninja come from too).

```
build.cmd
```

It configures with CMake + Ninja into `build/`, links `bin/RagdollPauseFix.dll` and copies it to `../release/`.

The DLL is built with the static CRT (`/MT`), so the file you ship is the only file anyone needs.

## How it works, briefly

CS2 advances client physics through `VPhysics2_Interface_001` vtable slot 16, once per engine tick, whether or not demo time moves.
The DLL replaces that slot. Inside the hook it reads the demo clock — `demo tick + tick interpolation fraction` — and keeps its own
physics clock in demo ticks. The original step runs only when the physics clock is behind demo time. Paused, demo time stops, so
physics stops with it.

The rest of the file is the plumbing needed to do that safely:

- **Startup order.** Injected at process start, `DllMain` runs before `tier0`, `engine2`, `client` and `vphysics2` are loaded
  (`cs2.exe` imports only USER32 and KERNEL32). A background thread waits for each module, then verifies and hooks.
- **The console command.** `mirv_ragdollfix` is registered by hooking `ICvar::RegisterConCommand` (slot 42) and registering right
  after one of the game's own commands, on the game's own thread; the hook then removes itself. Fallbacks: a retry from the physics
  step, and direct registration when loaded into a running game by `mirv_loadlibrary`.
- **Self-checks.** Build stamps, interface vtables inside their own module's `.text`, and a signature scan for the client globals.
  Anything that fails disables the fix with a message rather than guessing. All game memory reads go through SEH.
- **One copy per process**, enforced with a named mutex, and a warning if another module already hooks the physics step.

Full background, measurements and the reasoning behind the tolerances: `../../research/PAPER.md`.
