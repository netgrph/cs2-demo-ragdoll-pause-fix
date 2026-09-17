# Ragdoll physics during paused demo playback in Counter-Strike 2

**Analysis, a working fix, and a documented dead end**

Research period: 2026-09-14 – 2026-09-17
Game build: `client.dll` TimeDateStamp `0x6aa1ae5e`, `engine2.dll` `0x6aa1ae4f`, `vphysics2.dll` `0x6aa1add2`
Tooling: HLAE 2.192.2, custom instrumentation DLLs (MSVC, x64), Python analyzers
All in-game measurements were taken at `cl_phys_timescale 0.5`.

---

## Abstract

While a Counter-Strike 2 demo is paused, the rendered image freezes but the client physics simulation does not: the engine keeps
calling the physics world step once per engine tick. Player ragdolls therefore keep falling behind the frozen picture, and the
moment playback resumes the body appears at the position the hidden simulation reached. This breaks frame-accurate camera work.

We instrumented the client, measured the behaviour, and traced the cause to the physics step being driven by wall-clock engine
ticks instead of demo time. The fix presented here gates that step against a *demo clock*: physics may only advance when demo time
has actually advanced. It removes the artifact completely for the normal case, keeps vanilla behaviour during playback, and
survives tick stepping, speed changes and `cl_phys_timescale`.

A second artifact — a ragdoll rebuilt by a **seek across the moment of death** — was investigated over eleven builds and is *not*
solved. We report the negative result and the measurements that rule out each attempted approach, because the failing mechanism
is precisely located even though we could not act on it: the ragdoll's bones are only written from the physics bodies when demo
time really advances, so no amount of paused simulation reaches the screen.

---

## 1. The problem

A demo is paused. The camera is placed, the frame is composed. On unpause, the corpse in frame jumps — sometimes by tens of units.
The longer the pause, the bigger the jump.

The user's requirement was narrow and practical: *ragdolls must not move while the demo is paused, and must not teleport when it
resumes.*

## 2. Method

Three instruments were built, in this order.

1. **RagdollProbe** (`research-source/RagdollProbe`) — a logging DLL injected into CS2 through HLAE. It dumps entity lists, schema
   offsets, per-tick engine state and physics parameters into a text log. Its largest run produced a 2.33 GB log
   (`probe_20260914_195903`, not included here; see §10).
2. **DemoClock** (`research-source/DemoClock`) — the experimental fix DLL, versions 1.0.0 through 1.8.0. Every hypothesis in this
   paper was tested as a mode or sub-command of this DLL.
3. **Insight recorder** — a binary event recorder built into DemoClock (`src/Insight*.inc`). It snapshots selected heap objects and
   records the 4-byte words that change between engine events, so behaviour can be replayed offline instead of guessed at. The
   Python analyzers in `tools/` decode it.

The working method throughout: **record first, conclude second**. Where a conclusion is stated below, the recording that supports
it is named.

## 3. Reverse-engineering results

These are the structural facts the fix relies on. They come from static analysis of the three modules plus runtime confirmation.

**Physics step.** Client physics is advanced by `VPhysics2_Interface_001` vtable slot **16** ("step worlds"), called from
`client.dll+0x3d8050`, once per engine client tick (~64 Hz wall clock). Its `dt` is `cl_phys_timescale × per-substep dt`, where the
per-substep dt is `globals+0x34` (or exactly 1/64 when `cl_phys_assume_fixed_tick_interval` is set) divided by the substep count
(1–3). The call is gated by `cl_phys_enabled` and by nothing else — in particular, **not** by demo pause state.

**Demo state.** `Source2EngineToClient001` vtable slot **42** is `IsPlayingDemo`, slot **69** returns the demo file object. On that
object, slot **3** is `GetTick` and slot **12** is `IsPaused`. (`CDemoPlayer` also exposes slot 16 `SetTimeScale`, 18 `Pause(float)`,
20 `Resume`.)

**Demo clock.** Client globals hold `curtime` at `+0x30`, the tick interpolation fraction at `+0x38`, and the tick count at `+0x44`.
The demo tick increments exactly when the fraction wraps, so `tick + fraction` is a monotonic demo clock. `curtime` is not usable
for this: it jitters backwards.

**The ragdoll itself.** There is no `C_ClientRagdoll` or `C_RagdollProp` in demos. The ragdoll *is* the dead `C_CSPlayerPawn`
(`m_lifeState != 0`). `m_RagdollPose` is empty in demos — checked across the whole probe log, zero pawns with a non-zero pose — so
**the demo file contains no ragdoll pose data at all**. Whatever the viewer shows was simulated locally.

**Console commands without an SDK.** Reverse-engineered from HLAE's `AfxHookSource2.dll`: `ICvar` is the tier0 interface
`VEngineCvar007`; slot 11 `FindConVar(this, &handle, name, 0)`, slot 41 `GetCvar(index)` → convar data (name at +0, float value at
+0x58), slot 42 `RegisterConCommand(this, &handle, descriptor, flags)`, slot 43 unregister. The descriptor is eight qwords
`{name, help, flags, ICommandCallback*, 0x101, 0, 1, 0xffff}`; the callback's vtable slot 0 is
`CommandCallback(this, ctx, CCommand*)`, with `argc` at `CCommand+0x438` and `argv` at `+0x440`. Registration failed if the returned
handle's low 16 bits are `0xffff`.

## 4. What actually happens while paused

Measured with RagdollProbe (`probe_20260914_195903`) and confirmed in-game:

| Quantity | While the demo is paused |
| --- | --- |
| demo tick | frozen |
| `curtime` | frozen |
| `frametime` | non-zero |
| physics step slot 16 | **still called every engine tick, with unchanged dt** |
| rendered pose | frozen |

So the displayed pose is a frozen *tick-interpolation* blend, while the physics bodies underneath keep moving. The unpause "snap"
is the display catching up with them.

Two control experiments pinned this down:

- Setting `cl_phys_timescale 0` did **not** remove the snap — the artifact is not about step size.
- Skipping the slot-16 call whenever `IsPaused()` is true froze the pose bit-exactly and resumed smoothly. That is the whole
  mechanism, in one line of code.

A determinism check: seeking back rebuilds the ragdoll at the death tick (6 of 6 replays). Replays with an identical step `dt`
(1/128) were identical at death+2 ticks; replays after changing cvars (dt 1/64, ~1/256) landed 1.7–2.5 units apart. **Physics is
repeatable given identical steps, but it is not tied to ticks** — which rules out "just replay it the same way" as a recovery
strategy.

## 5. The fix: a demo clock gate

Three designs were drafted (a plain freeze gate, a demo-time physics clock, a per-tick pose recorder). The demo clock was chosen
because it keeps normal playback bit-identical to vanilla while still being a hard gate when time stops.

**Idea.** Keep a physics clock `P`, measured in demo ticks: the demo time the physics simulation has reached. Read the demo clock
`T = tick + fraction` inside the hooked step itself (so pause state is never a frame late). Let the original step run only when
`P + stepTicks ≤ T + lead`. Then add `stepTicks` to `P`.

One physics step is worth

```
stepTicks = dt × substeps / cl_phys_timescale × 64
```

demo ticks — dividing by the timescale is what keeps `cl_phys_timescale` a *speed* control rather than something the gate fights.

**Tolerances.** Engine ticks do not arrive evenly, so the gate needs slack:

```
lead        = paused ? 0.5·stepTicks : max(1, 0.5·stepTicks) + 0.5
lag         = max(1, stepTicks)
resync fwd  = max(resyncTicks, 2·stepTicks + lag)
resync back = max(3, lead + stepTicks + 1)
```

While unpaused, physics may run up to one tick ahead: this absorbs the uneven engine cadence, so ordinary playback steps exactly
as often as vanilla CS2 does. While paused, the lead collapses to half a step, which is just enough to finish the tick you paused
on (and to serve single-tick stepping) and nothing more.

If the error `P − T` leaves the resync band — a seek, a demo restart — the clock is re-based instead of catching up by brute force.
Catch-up after tick stepping is capped at `catchup` steps per engine tick (default 4). A demo tick jump larger than 4096 ticks is
distrusted for one engine tick, because seeks briefly report nonsense tick values.

**Validation.** An offline replay of the probe log (`tools/scratchpad/clocksim.py`) reproduced vanilla step counts during playback
and 0–1 steps per pause. In-game (build 1, 2026-09-14): pause/resume with no snap at every demo speed, `+1` tick stepping correct,
on/off toggle correct.

This is the fix that ships. It is the whole of `RagdollPauseFix.dll`.

## 6. The unsolved case: seeking across a death

Build 1 failed exactly one test: seek *past* a death and the corpse stands there in a T-pose.

**Why.** A seek loads the nearest full packet (every 3840 ticks: "non-incremental update", "entity slot re-use") and recreates the
entities. A player who is already dead at the landing tick therefore gets a **brand-new, never-simulated ragdoll** — and mode 2, by
design, refuses to simulate it while paused.

`m_flDeathTime` (`C_BasePlayerPawn`, networked) gives its age, so the amount of missing simulation is knowable. That produced the
first recovery attempt and, over the following three days, ten more. All of them are listed here with the reason they failed,
because each one eliminates a hypothesis.

### 6.1 Seek settle (v1.1.0)

Track dead `C_CSPlayerPawn`s through the entity list (RTTI + schema offsets), and give a rebuilt ragdoll a burst of extra physics
steps: `min(death age, 384 ticks)`.

*Result:* the settled pose is correct — **but it only appears after unpausing, and then snaps.**

### 6.2 Paused pose refresh (v1.2.0 – v1.2.1)

*Cause found:* entities render from `CInterpolatedVar` history. `NoteChanged` (var vtable slot 29) rejects any sample whose tick is
`≤` the newest one when var flag `0x2` is set — and the tick does not move while paused. So the settled pose never enters the
history the renderer reads.

Structure (RE): var slot 5 = `ClearHistory`; history pointer at `var+0x20`; a packed dword at `+8` holds head (bits 0–5), count
(13–18), capacity (19–24); interpolation amount as a float at `+0x18`. An entity's interpolated vars live in three lists at
`+0x210/+0x218`, `+0x258/+0x260`, `+0x2a0/+0x2a8` (16-byte entries).

v1.2.0 cleared and re-noted the pose history at the existing newest ticks for 8 engine ticks after each settle.
*Result:* worked about half the time.

*Cause of the flakiness:* a demo skip reports "unpaused" for 1–2 engine ticks and then pauses again at the same or next tick. Short
settles finished inside that window, so the refresh was never armed. v1.2.1 replaced the trigger with a **stale-pose mark** (a
settle, a catch-up of more than one step, or a tick jump that ran steps) that arms the refresh on any paused call within 16 engine
ticks and ±2 demo ticks. An offline replay of trace `democlock_20260915_073455` armed every failed seek and no ordinary pause.

*Result (v1.2.1):* forward seeks of 10–15 ticks past a death now work. Two cases still needed an unpause: a **big skip across the
death** (15200 → 15250, death at 15206) and a **backward skip** (15250 → 15220).

### 6.3 Forcing the animation system (v1.2.2 – v1.2.3)

*Cause found:* only the per-tick `CAnimGraphGameSystem` client tick pulls the ragdoll pose out of the physics bodies, and it does
not run while the demo tick is frozen. Rebuilding interpolation history cannot help if the pose being copied is itself stale.

RE: `CAnimGraphGameSystem` vtable `0x19caac0`, slot 28 = thunk `0x37afa0` → `0x34c6b0(this, 1)`, with no tick or pause gate and no
singleton (so the hook has to capture `this`). `CPhysicsGameSystem` vtable `0x19d1b48`, slot 48 = `FrameBoundary` `0x3a2770`, which
runs every frame including while paused.

v1.2.2 forced one animation tick before the history rebuild.
*Result:* the big skip was still stuck on the first try, and the paused pose differed on every retry.

*Cause:* the per-entity job computes `dt = tick − [controller+0xd4]` via helper `0x902370`. A `dt` of 0 skips the graph update, the
pose tasks and entity vtable `0x878` — so a forced tick while paused does **nothing**.

v1.2.3 therefore wrote `last − 1` into the controller so the game would run one real update.
*Result (trace `democlock_20260915_190425`):* the corpse still froze almost standing and snapped to the ground on unpause. Bones
*did* follow physics during the settle, but 44 settle steps lowered the body centroid by only ~9 units. The animation time was
consumed — and the pose did not move.

*Hypothesis at that point:* the death animation's ragdoll pose control (`CRagdollPoseControlSystem::OnPreSolve` ≈ `0x92b660`,
released by animation event `AE_CL_STOP_RAGDOLL_CONTROL`, id 13) holds the rebuilt body up, because animation time stayed at the
death start while physics ran ahead.

### 6.4 Lockstep settle (v1.2.4)

Run the settle in chunks: give N ticks of animation time in the frame's animation update, then N ticks of physics in the step after
it, so the two never diverge.

*Result:* the body falls back a little, then freezes too early and too high, and still snaps to the ground on unpause. The backward
skip was also wrong.

At this point the user stopped the guess-and-test cycle and asked for full instrumentation instead. That was the right call.

### 6.5 Instrumentation (v1.3.0 – v1.7.0)

- **v1.3.0 insight recorder** — records events (frame, anim tick, physics step) with per-word diffs of up to 256 heap objects
  chosen by a pointer census, plus the dead pawn, its node, bones and animation controller. A background `ReadProcessMemory` scan
  finds who references the pawn.
- **v1.4.x–v1.5.0 watch/forensic layer** — hardware breakpoints plus `PAGE_NOACCESS` "access windows" catch the game's own reads and
  writes of watched pages, with call stacks.
- **v1.6.0 body finder** — a background scan of committed private read/write memory looks for transform-shaped floats near the
  ragdoll centroid and tracks them across the unpause.
- **v1.7.0 physics pin** — data-driven pinning: any census node whose class name matches
  `Ragdoll|Aggregate|PhysicsBody|RnBody|*Shape`, plus near-nodes whose parent is such a class, is kept forever (cap 64). No
  hard-coded offsets.

**Forensic verdict** (recording `insight_20260916_181726_F_forensic.bin`): the physics bodies live in `CPhysAggregateInstance`
objects reachable from the world's physics list at `world+0x50`; the ragdoll is a `CPhysicsRagdoll` at `pawn+0x10c0`. Hot transform
bytes sit around `+0x140..+0x1c8` and `+0x460..+0x4e8`, read by `scenesystem.dll` (render) and `animationsystem.dll+0x493cae`. After
unpause the render pipeline reads bones through `client.dll+0xa62e0c/0xa63c92` (bone[0]..bone[112]), about one frame behind physics.

**Negative result — the heuristic body finder was the wrong instrument** (recording `insight_20260917_005251_bodies2.bin`). Its
change-count ranking selects the highest-churn float memory near the centroid, which is render and particle scratch, not rigid
bodies. Proven with hardware breakpoints on its own two top picks: one was touched only by `rendersystemdx11.dll` and the NVIDIA
driver (a GPU bone matrix), the other almost only by `particles.dll`. Scan statistics: 11.2 GB scanned, 8955 regions, 32754
candidates kept, of which ~15172 were static holders and most "movers" were freed heap going to zero/NaN. Cross-referencing later
showed **none** of the 32754 candidates fell inside the real physics objects' 316 pointer targets — independent proof that the scan
missed the bodies entirely.

The structural replacement worked. Mining the same recording located, by exact pointer path:
`#638 CPhysicsRagdoll @ pawn+0x10c0` (0x800 bytes, a 123-pointer table — transforms are in children, not inline),
`#639 CPhysAggregateInstance @ pawn>+30>+2a0`,
`#648 CPhysAggregateInstance @ world>+50>+328>+260>+220` (inline `CTransform` at `+0x110`, and a contiguous 32-byte-stride array of
per-body world transforms at `+0x1e0`), and `#649 CRnCapsuleShape` (a limb capsule).

### 6.6 What the pinned recording proved

Recording `insight_20260917_013655_pinbodies.bin`, mode 2 active, paused at tick 15215 for ~2400 frames, unpause at frame 2407:

- **The freeze is real.** For the entire pause, `CPhysicsRagdoll` and every `CPhysicsBody` and hull shape showed **zero word
  changes**. The only paused churn was in render scene objects.
- **The unpause is smooth.** Every body's first post-unpause value equalled its last paused value to within 1 unit (e.g. an
  aggregate position −2068.462 → −2068.431), then settled monotonically over about a second. No teleport.

So for a corpse that is already lying there, the shipped fix is not merely "good enough" — it is measurably exact.

### 6.7 The recovery case, and why it stays broken

Recording `insight_20260917_180257_recovery.bin` (seek 15200 → 15250, death age ≈ 45 ticks):

- Bodies **did** settle during the pause (the burst is visible at frames 1306–1307).
- Bones got **no** real write while paused (120 words, Δpos 0.001 = noise).
- At unpause, the animation job ran with `lastAnim 16696 → 16746` (elapsed 50), and *that* frame wrote 616 words across 86 bones
  with Δpos 113. **That is the snap.** Everything after it is smooth.

**The bone-write gate** (`tools/scratchpad/rec_bonediff.py`): real bone pose writes happen only in frames where the animation job
ran with `elapsed > 0`. Physics steps alone never write bones.

Two last attempts followed from that:

- **Animation catch-up** (`animcatchup`): force one active animation update with `elapsed = death age`, written as
  `controller+last = last − age` (it must come off the controller's last tick; taking it off globals gives a negative elapsed and
  flings the ragdoll across the map). Ordering matters: physics settle first, animation update after.
- **v1.8.0 pose copy**: when a settle burst ends, queue one forced update on the next paused step, after physics, so the controller
  returns to the job tick and the natural unpause elapsed is unchanged.

**v1.8.0 result** (`insight_20260917_185757_posecopy.bin`): it ran exactly as designed — 1 copy, 1 ragdoll, 0 misses, no crash. The
forced paused update (elapsed 46) reached the controller and wrote its tick. It produced **no bone write**, and the pose before and
after was byte-identical. The unpause still snapped: 637 words across 97 bones, Δpos 63, centroid ~21 units.

**Conclusion.** Neither physics steps nor a forced animation update puts a settled pose onto the bones while the demo is paused.
The bone write is gated by real demo advancement, and that gate was not identified. The only non-guessing path left would be a
hardware watchpoint on a bone word at the unpause frame, to capture the writer's call stack and work backwards to its condition.

The research was stopped here by decision, and the pause fix was shipped on its own.

## 7. The shipped implementation

`RagdollPauseFix.dll` is the demo clock of §5 and nothing else. All recovery machinery was removed deliberately.

- Hooks exactly one function: `VPhysics2_Interface_001` vtable slot 16.
- Registers `mirv_ragdollfix` by hooking `ICvar::RegisterConCommand` (slot 42) during startup, registering right after one of the
  game's own commands on the game's own thread, then removing the hook. Fallbacks: a retry from inside the physics step, and a
  direct registration path when the DLL is loaded into a running game by `mirv_loadlibrary`.
- Loading at process start means `DllMain` runs before `tier0`, `engine2`, `client` and `vphysics2` exist (`cs2.exe` imports only
  `USER32` and `KERNEL32`), so initialisation happens on a background thread that waits for each module.
- Self-checks before hooking: build stamps, interface vtables inside the owning module's `.text`, the client globals signature. A
  failure disables the fix and says so; it never falls back to guessing. Every read of game memory goes through SEH.
- A per-process mutex prevents a second copy from loading, and a warning fires if another module has already hooked the physics
  step.

## 8. Limitations

1. Seeking across a death leaves a rebuilt ragdoll that still moves on unpause (§6).
2. Ragdolls fall about twice as fast in demos as they should. This is a separate CS2 bug, unrelated to this fix; the user works
   around it with `cl_phys_timescale 0.5`, and all measurements here were taken that way.
3. Offsets and signatures are tied to the analysed build. Other builds run only if the structural self-check passes.

## 9. Reproducing this

```
research/research-source/RagdollProbe    the telemetry probe
research/research-source/DemoClock       the experimental DLL (v1.8.0, all modes and the insight recorder)
research/tools/insight.py                decodes an insight recording
research/tools/forensic.py               W/P access records (who read or wrote a watched page)
research/tools/bodies.py                 body-finder candidates (kept for the negative result)
research/tools/analyze_probe.py          the RagdollProbe text log
research/tools/scratchpad/               the one-off analyzers named throughout this paper
```

The scratchpad scripts import the main analyzers relative to their own location. The recording paths at the top of each script are
the original absolute ones; point them at your own file.

## 10. Data availability

The measurement data is **not** in this repository. The recordings are 23 MB – 2.33 GB binary memory dumps — the largest single
file, the probe log, is more than twenty times GitHub's per-file limit — and the derived traces, reports and census maps run to
tens of megabytes of machine-generated text. What is kept here instead is the code that produced and read them (§9), so the
measurements can be reproduced.

The recordings referenced by name in this paper are, for the record:

| File | Size | What it shows |
| --- | --- | --- |
| `probe_20260914_195903.log` | 2.33 GB | full probe telemetry, phase 1 |
| `insight_20260916_181726_F_forensic.bin` | 535 MB | page-access forensics, where the bodies live |
| `insight_20260917_005251_bodies2.bin` | 147 MB | body-finder run (negative result) |
| `insight_20260916_075618_D_fwd.bin` | 99 MB | forward seek with animation catch-up |
| `insight_20260917_013655_pinbodies.bin` | 28 MB | pinned bodies: freeze proven, unpause smooth |
| `insight_20260917_180257_recovery.bin` | 29 MB | the across-death case and the bone-write gate |
| `insight_20260917_185757_posecopy.bin` | 23 MB | v1.8.0, the final negative result |
