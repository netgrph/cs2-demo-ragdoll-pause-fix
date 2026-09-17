"""Step-size changes over the whole probe log plus a per-frame step/tick cadence window.

usage: stepcadence.py <log> <out> <window_fc_start> <window_fc_count> [more window starts...]
"""
import sys

path, out_path = sys.argv[1], sys.argv[2]
count = int(sys.argv[4])
windows = [(int(s), int(s) + count) for s in [sys.argv[3]] + sys.argv[5:]]

out = open(out_path, "w", encoding="utf-8")
last_dt = None
last_subdt = None
last_ts = None
cur = {}           # fc -> dict(tick, frac, paused, curtime, steps[])
changes = []
with open(path, "rb") as fh:
    for raw in fh:
        t = raw[:2]
        if t == b"W,":
            p = raw.split(b",")
            fc = int(p[2])
            dt = float(p[6][3:])
            if dt != last_dt:
                changes.append(f"fc {fc} tick {int(p[3])} paused {int(p[4])}: step dt {last_dt} -> {dt} (1/{1 / dt if dt else 0:.1f})")
                last_dt = dt
            if any(a <= fc < b for a, b in windows):
                cur.setdefault(fc, {}).setdefault("steps", []).append(dt)
        elif t == b"P,":
            p = raw.split(b",")
            sub = p[9]
            if sub != last_subdt:
                changes.append(f"fc {int(p[2])} tick {int(p[3])}: CPhysicsGameSystem {p[8].decode()} {sub.decode()} (was {last_subdt})")
                last_subdt = sub
        elif t == b"D,":
            txt = raw.decode(errors="replace").rstrip()
            p = txt.split(",")
            if p[4] == "SetTimeScale":
                if p[5] != last_ts:
                    changes.append(f"fc {p[2]} tick {p[3]}: demo SetTimeScale {p[5]} (was {last_ts})")
                    last_ts = p[5]
            else:
                changes.append(f"fc {p[2]} tick {p[3]}: demo {p[4]} {','.join(p[5:])}")
        elif t == b"G,":
            p = raw.split(b",")
            if len(p) < 12:
                continue
            fc = int(p[2])
            if any(a <= fc < b for a, b in windows):
                d = dict(f.split(b"=", 1) for f in p[3:12] if b"=" in f)
                e = cur.setdefault(fc, {})
                e.update(tick=int(d[b"tick"]), paused=int(d[b"paused"]), frac=float(d[b"f38"]), cur=float(d[b"curtime30"]),
                         ft=float(d[b"absft0c"]), ps=float(d[b"physft34"]))

out.write("=== step size / timescale / demo player changes ===\n")
for c in changes:
    out.write(c + "\n")
for a, b in windows:
    out.write(f"\n=== cadence fc {a}..{b} (only frames with a step or tick change) ===\n")
    prev_tick = None
    for fc in range(a, b):
        e = cur.get(fc)
        if not e or "tick" not in e:
            continue
        steps = e.get("steps", [])
        if steps or e["tick"] != prev_tick:
            out.write(f"fc {fc} tick {e['tick']} paused {e['paused']} frac {e['frac']:.3f} curtime {e['cur']:.5f} absft {e['ft']:.5f} "
                      f"steps {len(steps)} dt {' '.join(f'{s:.6f}' for s in steps)}\n")
        prev_tick = e["tick"]
out.close()
