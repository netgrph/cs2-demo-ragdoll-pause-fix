"""Replay the RagdollDemoClock mode-2 gate (same math as Hk_Step/DemoClockStep) against the real Step cadence of a probe log.

usage: clocksim.py <probe log> <out>
Per demo-player segment (between Pause/Resume/SetTimeScale events) it reports vanilla steps vs clock steps, gated unpaused calls,
steps executed while paused, re-syncs and the P-T error range, plus a list of every unpaused gate and every paused step.
"""
import sys

path, out_path = sys.argv[1], sys.argv[2]
out = open(out_path, "w", encoding="utf-8")

MAX_STEPS, RESYNC = 4, 16
st = dict(valid=False, P=0.0)
g = dict(fc=-1, playing=0, paused=0, tick=-1, frac=0.0)
timescale = 1.0
seg = None
segs = []
events = []


def new_seg(label, fc):
    global seg
    seg = dict(label=label, fc=fc, calls=0, vanilla=0, steps=0, gated_unpaused=0, paused_steps=0, paused_calls=0, resync=0,
               emin=1e9, emax=-1e9, extra=0)
    segs.append(seg)


new_seg("start", 0)

with open(path, "rb") as fh:
    for raw in fh:
        t = raw[:2]
        if t == b"G,":
            p = raw.decode(errors="replace").rstrip().split(",")
            if len(p) < 12:
                continue
            d = dict(f.split("=", 1) for f in p[3:12] if "=" in f)
            g.update(fc=int(p[2]), playing=int(d["playing"]), paused=int(d["paused"]), tick=int(d["tick"]), frac=float(d["f38"]))
        elif t == b"D,":
            p = raw.decode(errors="replace").rstrip().split(",")
            if p[4] == "SetTimeScale":
                ts = float(p[5])
                if ts != timescale:
                    timescale = ts
                    new_seg(f"SetTimeScale {ts}", int(p[2]))
            else:
                new_seg(f"{p[4]} at tick {p[3]}", int(p[2]))
        elif t == b"W,":
            p = raw.decode(errors="replace").rstrip().split(",")
            fc, dt = int(p[2]), float(p[6][3:])
            substeps = int(p[7].split("=")[1])
            seg["calls"] += 1
            seg["vanilla"] += 1
            if not g["playing"] or g["tick"] < 0:
                st["valid"] = False
                seg["steps"] += 1
                continue
            frac = min(max(g["frac"], 0.0), 0.999999)
            T = g["tick"] + frac
            # stepTicks = dt*substeps/cl_phys*64 = demo timescale when dt != 0 (dt = 1/64 * demo timescale * cl_phys_timescale)
            step = timescale if dt > 0 else 0.0
            if not (1e-4 < step <= 64):
                st["valid"] = False
                seg["steps"] += 1
                continue
            paused = g["paused"]
            lead = 0.5 * step if paused else max(1.0, 0.5 * step) + 0.5
            lag = max(1.0, step)
            rfwd = max(float(RESYNC), 2 * step + lag)
            rback = max(3.0, lead + step + 1.0)
            action = "gate"
            if not st["valid"]:
                st.update(valid=True, P=T - step)
                action = "start"
            else:
                e = st["P"] - T
                if e < -rfwd or e > rback:
                    st["P"] = T - step
                    seg["resync"] += 1
                    action = "resync"
                    events.append(f"fc {fc} tick {g['tick']} paused {paused}: resync, jump {-e:+.2f} ticks")
            e0 = st["P"] - T
            if paused:
                seg["paused_calls"] += 1
            else:
                seg["emin"], seg["emax"] = min(seg["emin"], e0), max(seg["emax"], e0)
            n = 0
            if st["P"] + step <= T + lead + 1e-9:
                lim = T + lead if paused else T - lag
                while True:
                    st["P"] += step
                    n += 1
                    if not (n < MAX_STEPS and st["P"] + step <= lim + 1e-9):
                        break
            seg["steps"] += n
            seg["extra"] += max(0, n - 1)
            if paused:
                seg["paused_steps"] += n
                if n:
                    events.append(f"fc {fc} tick {g['tick']} frac {frac:.3f} PAUSED step x{n}: P {e0:+.3f} -> {st['P'] - T:+.3f} vs T")
            else:
                if n == 0:
                    seg["gated_unpaused"] += 1
                    events.append(f"fc {fc} tick {g['tick']} frac {frac:.3f} unpaused GATED: P-T {e0:+.3f} step {step}")
                elif n > 1:
                    events.append(f"fc {fc} tick {g['tick']} frac {frac:.3f} unpaused catch-up x{n}: P-T {e0:+.3f}")

out.write("segment | engine ticks with Step | vanilla steps | clock steps | extra | unpaused gated | paused calls | paused steps | "
          "resyncs | unpaused P-T range\n")
for s in segs:
    if not s["calls"]:
        continue
    rng = f"{s['emin']:+.3f}..{s['emax']:+.3f}" if s["emin"] < 1e8 else "-"
    out.write(f"fc {s['fc']} {s['label']} | {s['calls']} | {s['vanilla']} | {s['steps']} | {s['extra']} | {s['gated_unpaused']} | "
              f"{s['paused_calls']} | {s['paused_steps']} | {s['resync']} | {rng}\n")
out.write("\n=== events ===\n")
for e in events:
    out.write(e + "\n")
out.close()
