# CS2 Ragdoll Pause Fix

In Counter-Strike 2 demos, ragdolls keep falling while the demo is paused.
As soon as you unpause, the body jumps to wherever the physics took it. 
That makes any form of Recording/Editing work (HLAE camera keyframes, screenshots, edits) painful.

https://github.com/user-attachments/assets/796e65c6-c8ac-4529-a0a7-250e2dea591f

*Video from [ValveSoftware/csgo-osx-linux#4405](https://github.com/ValveSoftware/csgo-osx-linux/issues/4405), the acknowledged bug report for
this behaviour.*

This is a small DLL for [HLAE](https://github.com/advancedfx/advancedfx). It loads next to `AfxHookSource2.dll`, adds the console
command `mirv_ragdollfix`, and makes ragdoll physics follow demo time: the demo is paused, so the physics is paused too.

Pause, unpause, tick stepping, demo speed and `cl_phys_timescale` all keep working as normal.

## How to use it

1. Open HLAE.
2. Menu: **Tools → Developer → Custom Loader**.
3. **DLLs to inject**, in this order:
   ```
   x64\AfxHookSource2.dll
   RagdollPauseFix.dll
   ```
4. Start CS2, play a demo, and type in the console:
   ```
   mirv_ragdollfix status
   ```
   It should say `ACTIVE`.

## Console commands

| Command | What it does |
| --- | --- |
| `mirv_ragdollfix` | status and the list of options |
| `mirv_ragdollfix status` | status only |
| `mirv_ragdollfix on` / `off` | on = ragdoll physics follow demo time (default), off = vanilla CS2 |
| `mirv_ragdollfix catchup <1-16>` | how many physics steps one engine tick may catch up with (default 4) |
| `mirv_ragdollfix resync <2-6400>` | demo time jumps bigger than this many ticks re-sync the clock instead of fast-forwarding (default 16) |
| `mirv_ragdollfix verbose <0\|1>` | print pause / resume / seek messages (default 0) |

## Comparison

![Ragdoll physics before and after the fix, while the demo is paused](media/pause-comparison.gif)

With `mirv_ragdollfix` active, the ragdoll holds still the whole time the demo is paused, instead of drifting and settling into a
different pose by the time you unpause.

## Disclaimer

Almost this entire project was created with AI assistance. I do not take credit in any way for creating it.
