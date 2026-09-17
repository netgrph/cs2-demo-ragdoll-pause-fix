"""Compare the dying pawn's bone positions at the same demo tick across replays (runs split at backwards seeks)."""
import math
import sys
from collections import defaultdict

path, out_path, idx = sys.argv[1], sys.argv[2], sys.argv[3].encode()
needle = b"," + idx + b",C_CSPlayerPawn,"
runs = []            # each: dict tick -> first sample (fc, paused, life, b1, b2, b3)
pauses = []          # per run: list of (tick, frames)
last_tick = None
cur_pause_frames = 0
with open(path, "rb") as fh:
    for raw in fh:
        if raw[:2] != b"E," or needle not in raw:
            continue
        p = raw.rstrip(b"\n").split(b",")
        fc, tick, paused = int(p[1]), int(p[2]), p[3] == b"1"
        d = {}
        for f in p[7:]:
            k, _, v = f.partition(b"=")
            d[k] = v
        try:
            b = [tuple(float(t) for t in d[k].split(b";")) for k in (b"b1", b"b2", b"b3")]
        except (KeyError, ValueError):
            continue
        if last_tick is None or tick < last_tick - 5:
            runs.append({})
            pauses.append([])
        last_tick = tick
        r = runs[-1]
        if paused:
            if not pauses[-1] or pauses[-1][-1][0] != tick or pauses[-1][-1][2] != True:
                pauses[-1].append([tick, 0, True])
            pauses[-1][-1][1] += 1
        elif pauses[-1] and pauses[-1][-1][2]:
            pauses[-1][-1][2] = False
        if tick not in r and not paused:
            r[tick] = (fc, int(d.get(b"life", b"-1")), b)

with open(out_path, "w", encoding="utf-8") as out:
    for i, r in enumerate(runs):
        ticks = sorted(r)
        death = next((t for t in ticks if r[t][1] != 0), None)
        out.write(f"run {i}: ticks {ticks[0] if ticks else None}..{ticks[-1] if ticks else None}, first life!=0 tick {death}, "
                  f"pauses {[(t, n) for t, n, _ in pauses[i]]}\n")
    all_ticks = sorted(set().union(*[set(r) for r in runs]))
    ref_run = max(range(len(runs)), key=lambda i: len(runs[i]))
    out.write(f"\nreference run {ref_run}; per tick: b1 xyz per run and max deviation from reference (units)\n")
    for t in all_ticks:
        if t % 4 and t not in (15142, 15143):
            continue
        row = []
        dev = 0.0
        for i, r in enumerate(runs):
            if t in r:
                b1 = r[t][2][0]
                row.append(f"r{i}:({b1[0]:.1f},{b1[1]:.1f},{b1[2]:.1f})L{r[t][1]}")
                if t in runs[ref_run]:
                    ref = runs[ref_run][t][2]
                    dev = max(dev, max(math.dist(a, c) for a, c in zip(r[t][2], ref)))
        out.write(f"tick {t}: maxdev {dev:7.2f} | {' '.join(row)}\n")
