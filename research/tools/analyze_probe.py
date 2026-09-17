r"""Summarises a RagdollProbe log (stdlib only).

usage: python analyze_probe.py [path\to\probe_xxx.log]   (default: newest log in ..\bin\logs)
       python analyze_probe.py <log> --traj <entityIndex>  (per-frame positions of one entity)
"""
import glob
import math
import os
import sys
from collections import defaultdict


def kv(fields):
    d = {}
    for f in fields:
        k, sep, v = f.partition("=")
        if sep:
            d[k] = v
    return d


def vec(s):
    try:
        return tuple(float(x) for x in s.split(";"))
    except (ValueError, AttributeError):
        return None


def dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a[:3], b[:3])))


def fnum(d, k, default=float("nan")):
    try:
        return float(d[k])
    except (KeyError, ValueError):
        return default


def stats(xs):
    xs = [x for x in xs if not math.isnan(x)]
    if not xs:
        return "n/a"
    return f"min {min(xs):.6f} / mean {sum(xs) / len(xs):.6f} / max {max(xs):.6f} (n={len(xs)})"


def main():
    args = sys.argv[1:]
    traj = None
    if "--traj" in args:
        i = args.index("--traj")
        traj = int(args[i + 1])
        del args[i:i + 2]
    if args:
        path = args[0]
    else:
        logs = sorted(glob.glob(os.path.join(os.path.dirname(__file__), "..", "bin", "logs", "probe_*.log")), key=os.path.getmtime)
        if not logs:
            sys.exit("no logs found")
        path = logs[-1]
    print("log:", os.path.abspath(path))

    frames = {}           # fc -> dict(t, paused, tick, g)
    frame_order = []
    msgs = []
    worlds = defaultdict(list)    # fc -> [W dict]
    pgs = defaultdict(list)       # fc -> [P dict]
    counters = defaultdict(dict)  # fc -> {(set, slot): n}
    ents = defaultdict(dict)      # idx -> {fc: E dict}
    teleports = []
    demo_calls = []

    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\n")
            p = line.split(",")
            typ = p[0]
            try:
                if typ == "#":
                    msgs.append(",".join(p[1:]))
                elif typ == "G" and len(p) > 3 and p[3] != "noglobals":
                    d = kv(p[3:])
                    fc = int(p[2])
                    if fc not in frames:
                        frame_order.append(fc)
                    frames[fc] = dict(t=float(p[1]), paused=d.get("paused") == "1", tick=int(d.get("tick", -1)), g=d)
                elif typ == "W":
                    d = kv(p[5:]); d["t"] = float(p[1]); d["paused"] = p[4] == "1"
                    worlds[int(p[2])].append(d)
                elif typ == "P":
                    d = kv(p[5:]); d["t"] = float(p[1]); d["paused"] = p[4] == "1"
                    pgs[int(p[2])].append(d)
                elif typ == "C":
                    fc = int(p[1])
                    for item in p[3:]:
                        s, _, n = item.partition(":")
                        counters[fc][(p[2], int(s))] = int(n)
                elif typ == "E":
                    d = kv(p[7:]); d["cls"] = p[5]; d["designer"] = p[6]; d["tick"] = int(p[2]); d["paused"] = p[3] == "1"
                    ents[int(p[4])][int(p[1])] = d
                elif typ == "T":
                    teleports.append(line)
                elif typ == "D":
                    demo_calls.append(line)
            except (ValueError, IndexError):
                pass

    print("\n=== probe messages ===")
    for m in msgs:
        print("  " + m.strip())

    if traj is not None:
        print(f"\n=== trajectory of entity #{traj} ===")
        prev = None
        for fc in sorted(ents.get(traj, {})):
            e = ents[traj][fc]
            pos = vec(e.get("b0")) or vec(e.get("org"))
            step = dist(pos, prev) if pos and prev else 0.0
            prev = pos
            w = worlds.get(fc, [])
            print(f"fc {fc} tick {e['tick']} paused {int(e['paused'])} b0 {e.get('b0')} step {step:7.2f} org {e.get('org')} "
                  f"pose {e.get('pose')} ragpos {e.get('ragpos')} steps {len(w)} dt {sum(fnum(x, 'dt', 0) for x in w):.5f}")
        return

    frame_order.sort()
    n_paused = sum(1 for fc in frame_order if frames[fc]["paused"])
    print(f"\n=== frames: {len(frame_order)} demo frames logged, {n_paused} paused ===")

    # pause intervals
    intervals = []
    cur = None
    for fc in frame_order:
        if frames[fc]["paused"] and cur is None:
            cur = [fc, fc]
        elif frames[fc]["paused"]:
            cur[1] = fc
        elif cur is not None:
            intervals.append(cur); cur = None
    if cur:
        intervals.append(cur)

    for n, (a, b) in enumerate(intervals, 1):
        fcs = [fc for fc in frame_order if a <= fc <= b]
        fa, fb = frames[a], frames[b]
        g = [frames[fc]["g"] for fc in fcs]
        print(f"\n--- pause #{n}: frames {a}..{b} ({len(fcs)} frames, {fb['t'] - fa['t']:.2f}s), demo tick {fa['tick']} -> {fb['tick']}")
        print("  globals frametime  @0x08 :", stats([fnum(x, "ft08") for x in g]))
        print("  globals absframe   @0x0c :", stats([fnum(x, "absft0c") for x in g]))
        print("  globals phys ft    @0x34 :", stats([fnum(x, "physft34") for x in g]))
        print(f"  globals curtime    @0x30 : {g[0].get('curtime30')} -> {g[-1].get('curtime30')}")
        w = [x for fc in fcs for x in worlds.get(fc, [])]
        dts = [fnum(x, "dt") for x in w]
        print(f"  physics world steps: {len(w)} calls ({len(w) / max(1, len(fcs)):.2f}/frame), summed dt {sum(d for d in dts if not math.isnan(d)):.4f}s, "
              f"skipped {sum(1 for x in w if x.get('skip') == '1')}")
        print("    step dt:", stats(dts), " substeps:", sorted({x.get("substeps") for x in w}))
        pp = [x for fc in fcs for x in pgs.get(fc, [])]
        print(f"  CPhysicsGameSystem events: {len(pp)}; slots {sorted({x.get('slot') for x in pp})}; subdt:", stats([fnum(x, "subdt") for x in pp]))

        # frames after resume
        after = [fc for fc in frame_order if fc > b][:15]
        wa = [x for fc in after for x in worlds.get(fc, [])]
        if after:
            print(f"  first {len(after)} frames after resume: {len(wa)} world steps, summed dt {sum(fnum(x, 'dt', 0) for x in wa):.4f}s; "
                  f"phys ft @0x34:", stats([fnum(frames[fc]['g'], 'physft34') for fc in after]))

        print("  entities (bone0 / origin movement):")
        shown = 0
        for idx in sorted(ents):
            e = ents[idx]
            inside = [fc for fc in fcs if fc in e]
            if not inside:
                continue
            p0 = vec(e[inside[0]].get("b0")) if e[inside[0]].get("bones") == "1" else vec(e[inside[0]].get("org"))
            key = "b0" if e[inside[0]].get("bones") == "1" else "org"
            if not p0:
                continue
            maxmove = max(dist(vec(e[fc].get(key)) or p0, p0) for fc in inside)
            # movement in the last 15 frames before the pause (was it moving at all?)
            before = [fc for fc in frame_order if fc < a][-15:]
            bpos = [vec(e[fc].get(key)) for fc in before if fc in e]
            premove = dist(bpos[0], bpos[-1]) if len(bpos) > 1 and bpos[0] and bpos[-1] else 0.0
            apos = [vec(e[fc].get(key)) for fc in after if fc in e]
            last = vec(e[inside[-1]].get(key)) or p0
            steps = []
            prev = last
            for q in apos:
                if q:
                    steps.append(dist(q, prev)); prev = q
            if maxmove < 0.5 and premove < 0.5 and (not steps or max(steps) < 0.5):
                continue
            pose_first, pose_last = e[inside[0]].get("pose"), e[inside[-1]].get("pose")
            rag_first, rag_last = e[inside[0]].get("ragpos"), e[inside[-1]].get("ragpos")
            print(f"    #{idx} {e[inside[0]]['cls']} ({e[inside[0]]['designer']}): moved {premove:.1f}u in 15 frames before pause, "
                  f"{maxmove:.1f}u max DURING pause, after resume max 1-frame step {max(steps) if steps else 0:.1f}u")
            if pose_first != pose_last:
                print(f"        networked m_RagdollPose changed during pause: {pose_first} -> {pose_last}")
            if rag_first != rag_last:
                print(f"        m_ragPos changed during pause: {rag_first} -> {rag_last}")
            shown += 1
            if shown >= 25:
                print("    ...")
                break

    # which vtable slots keep running while paused
    paused_fcs = [fc for fc in frame_order if frames[fc]["paused"]]
    play_fcs = [fc for fc in frame_order if not frames[fc]["paused"]]
    if counters and paused_fcs and play_fcs:
        tot_p, tot_r = defaultdict(int), defaultdict(int)
        for fc in paused_fcs:
            for k, v in counters.get(fc, {}).items():
                tot_p[k] += v
        for fc in play_fcs:
            for k, v in counters.get(fc, {}).items():
                tot_r[k] += v
        print("\n=== vtable call rates (calls per frame): playing vs paused ===")
        for k in sorted(set(tot_p) | set(tot_r)):
            rp, rr = tot_p[k] / len(paused_fcs), tot_r[k] / len(play_fcs)
            tag = "STOPS when paused" if rr > 0.5 and rp < 0.05 else ("still runs paused" if rp > 0.05 else "")
            print(f"  {k[0]:>40} [{k[1]:3}]  playing {rr:8.2f}  paused {rp:8.2f}  {tag}")

    print(f"\n=== demo player calls ({len(demo_calls)}) ===")
    last = None
    for line in demo_calls:
        key = line.split(",", 4)[-1]
        if "SetTimeScale" in line and key == last:
            continue
        last = key
        print("  " + line)
    print(f"\n=== teleports ({len(teleports)}) ===")
    for line in teleports[:60]:
        print("  " + line)


if __name__ == "__main__":
    main()
