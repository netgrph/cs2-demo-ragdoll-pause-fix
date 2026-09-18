# CS2 Ragdoll Pause Fix

In Counter-Strike 2 demos, ragdolls keep falling while the demo is paused.
As soon as you unpause, the body jumps to wherever the physics took it. 
That makes any form of Recording/Editing work (HLAE camera keyframes, screenshots, edits) painful.

https://github.com/user-attachments/assets/796e65c6-c8ac-4529-a0a7-250e2dea591f

*Video from [ValveSoftware/csgo-osx-linux#4405](https://github.com/ValveSoftware/csgo-osx-linux/issues/4405), the acknowledged bug report for
this behaviour.*

This is a small DLL for [HLAE](https://github.com/advancedfx/advancedfx). It loads next to `AfxHookSource2.dll`, adds the console
command `mirv_ragdollfix`, and makes ragdoll physics follow demo time: the demo is paused, so the physics is paused too.

> ⚠️ **Ragdolls falling too fast?**
>
> There is a separate CS2 bug that causes ragdolls to fall significantly faster than they should.  
> **[See the workaround →](#how-to-fix-fast-falling-ragdolls)**

## How to use it

1. Open HLAE.
2. Menu: **Tools → Developer → Custom Loader**.
3. **DLLs to inject**, in this order:
   ```
   x64\AfxHookSource2.dll
   RagdollPauseFix.dll
   ```
4. Start CS2 and play a demo. The fix is enabled automatically.  
To check whether the DLL loaded correctly, enter:  
   ```
   mirv_ragdollfix status
   ```
   
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

<img src="media/pause-comparison.gif" alt="Ragdoll physics before and after the fix, while the demo is paused" width="50%">

With `mirv_ragdollfix` active, the ragdoll holds still the whole time the demo is paused, instead of drifting and settling into a
different pose by the time you unpause.

## HOW TO FIX FAST FALLING RAGDOLLS

There is currently an additional bug that causes ragdolls to fall significantly faster than they should.  

Since this project, in its current state, does not address that issue, here is a workaround to make them behave normally again:

```
mirv_cvar_unhide_all;
mirv_cvar_unlock_sv_cheats;
cl_phys_timescale 0.5
```

<details>
<summary>Video Showcasing the Issue</summary>

https://github.com/user-attachments/assets/e6a549bc-0e41-4090-bfdb-08b40d7cb338

*Video from [ValveSoftware/csgo-osx-linux#4403](https://github.com/ValveSoftware/csgo-osx-linux/issues/4403)*

</details>

## Compatibility

Confirmed to work on the current **September 10, 2026** build of the game.

I have not tested this on older game versions, so I cannot confirm that everything works correctly on them.

## Disclaimer

Almost this entire project was created with AI assistance. I do not take credit in any way for creating it.
