# RagdollPauseFix.dll — release

One file, no dependencies, nothing to install. Put `RagdollPauseFix.dll` anywhere you like and load it with HLAE.

The DLL is not committed to git: download it from the **Releases** page, or build it with `../source/build.cmd`, which drops it
right here.

## Load it

**HLAE → Tools → Developer → Custom Loader**, and under **DLLs to inject** list both files in this order:

```
<your HLAE folder>\x64\AfxHookSource2.dll
<this folder>\RagdollPauseFix.dll
```

Start CS2, play a demo, open the console and type:

```
mirv_ragdollfix status
```

`ACTIVE` means it works. Pausing now freezes ragdolls.

If CS2 is already running through HLAE, you can load it live instead:

```
mirv_loadlibrary "<this folder>\RagdollPauseFix.dll"
```

## Commands

```
mirv_ragdollfix                  status and help
mirv_ragdollfix status           status only
mirv_ragdollfix on | off         on = physics follow demo time (default), off = vanilla CS2
mirv_ragdollfix catchup <1-16>   physics steps one engine tick may catch up with (default 4)
mirv_ragdollfix resync <2-6400>  demo time jumps over this many ticks re-sync instead of fast-forwarding (default 16)
mirv_ragdollfix verbose <0|1>    print pause / resume / seek messages (default 0)
```

## Notes

- Built and tested against CS2 `client.dll 0x6aa1ae5e` with HLAE 2.192.2. On another build the DLL checks the game's structures
  first; if they moved it switches itself off and prints why, instead of crashing.
- Load only one ragdoll fix at a time. If something else already hooks the physics step, it warns you and names the module.
- It only acts during demo playback, and does nothing at all outside `cs2.exe`.
- Known limit: seeking across a death still leaves a ragdoll that moves on unpause. See `../../research/PAPER.md`.
