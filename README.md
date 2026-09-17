# CS2 Ragdoll Pause Fix

In Counter-Strike 2 demos, ragdolls keep falling while the demo is paused. The picture is frozen, so you don't see it, but the
physics keeps running in the background. As soon as you unpause, the body jumps to wherever the physics took it. That makes
frame-by-frame work (HLAE camera keyframes, screenshots, edits) painful.

This is a small DLL for [HLAE](https://github.com/advancedfx/advancedfx). It loads next to `AfxHookSource2.dll`, adds the console
command `mirv_ragdollfix`, and makes ragdoll physics follow demo time: the demo is paused, so the physics is paused too.

Pause, unpause, tick stepping, demo speed and `cl_phys_timescale` all keep working as normal.

## Download

Get `RagdollPauseFix.dll` from the **Releases** page. It is one file with no dependencies. (You can also build it yourself —
see [`addon/source/`](addon/source/).)

## How to use it

1. Open HLAE.
2. Menu: **Tools → Developer → Custom Loader**.
3. **ProgramPath**: your `cs2.exe`, normally
   `C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive\game\bin\win64\cs2.exe`
4. **CommandLine**: `-steam -insecure`
5. **Environment variables**:
   ```
   SteamPath=C:\Program Files (x86)\Steam
   SteamClientLaunch=1
   SteamGameId=730
   SteamAppId=730
   SteamOverlayGameId=730
   ```
6. **DLLs to inject**, in this order:
   ```
   <your HLAE folder>\x64\AfxHookSource2.dll
   <where you put it>\RagdollPauseFix.dll
   ```
7. Start CS2, play a demo, and type in the console:
   ```
   mirv_ragdollfix status
   ```
   It should say `ACTIVE`. That's it — pausing now freezes ragdolls.

Already running CS2 through HLAE? Then you can also load it live:

```
mirv_loadlibrary "<where you put it>\RagdollPauseFix.dll"
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

## What it does not fix

If you **seek across a death** (the player is alive at the old tick and dead at the new one), the game rebuilds the ragdoll from
scratch and it still moves once you unpause. We measured why: the bones only take their pose from the physics bodies when demo time
really advances, and nothing we could call while paused makes that happen. The whole investigation is written up in
[`research/PAPER.md`](research/PAPER.md).

Everything else — pausing on a body that is already lying there, stepping ticks, changing speed — is stable.

## What's in this repository

```
addon/release/   where the DLL lands (the binary itself ships as a release, not in git)
addon/source/    its source code and a build script
research/        the paper, the research code and the analysis tools
```

## Safety

- The DLL does nothing unless it is inside `cs2.exe`, and only acts during demo playback.
- It checks the game's structures before hooking anything. If a CS2 update moves them, it switches itself off and tells you, instead
  of crashing.
- It changes nothing on disk and nothing in your game files.
- Use it on demos, with `-insecure`, like the rest of HLAE. It is not meant for live matchmaking.

## License

None. No license file was added on purpose — the author has not picked one yet.

## Credits

Built as a research project against CS2 build `client.dll 0x6aa1ae5e` with HLAE 2.192.2.
