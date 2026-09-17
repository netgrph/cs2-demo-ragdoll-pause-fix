# Research

How CS2 handles ragdolls during paused demo playback, how it was measured, what worked, and what did not.

**Start here: [PAPER.md](PAPER.md)** — the full write-up, from the first probe to the final negative result.

## Folders

| Folder | What's in it |
| --- | --- |
| `tools/` | the Python analyzers (`insight.py`, `forensic.py`, `bodies.py`, `analyze_probe.py`) |
| `tools/scratchpad/` | the one-off analysis scripts written during the investigation |
| `research-source/RagdollProbe/` | the telemetry probe DLL |
| `research-source/DemoClock/` | the experimental DLL, v1.8.0 — every idea in the paper is a mode or sub-command of it |

## The short version

CS2 steps client physics once per engine tick whether or not demo time moves, so a paused demo keeps simulating ragdolls out of
sight. Gating that step against demo time fixes it, and a pinned-memory recording proves the bodies then hold perfectly still and
resume without a jump.

Seeking *across* a death is a different problem and is unsolved: the game rebuilds the ragdoll from nothing, and the bones only
take their pose from the physics bodies when demo time really advances. Eleven builds tried to force that while paused. None
worked, and §6 of the paper says exactly how each one failed.

## Measurement data

Not in this repository. The raw recordings are 23 MB – 2.33 GB binary memory dumps, and even the derived traces and reports run to
tens of megabytes. §10 of the paper lists every recording by name and says what each one shows; the tools here are what read them,
so the measurements can be reproduced rather than downloaded.
