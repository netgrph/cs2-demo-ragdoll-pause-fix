"""Streaming analysis of a huge RagdollProbe log. Writes a report to argv[2]."""
import math
import sys
from collections import defaultdict

path, out_path = sys.argv[1], sys.argv[2]
out = open(out_path, "w", encoding="utf-8")


def P(*a):
    print(*a, file=out)


def kvb(parts):
    d = {}
    for f in parts:
        k, _, v = f.partition(b"=")
        d[k] = v
    return d


def v3(b):
    try:
        x = [float(t) for t in b.split(b";")[:3]]
        return x if all(math.isfinite(t) and abs(t) < 1e5 for t in x) else None
    except ValueError:
        return None


frames = []          # (fc, t, paused, tick, ft08, physft34, curtime)
fc_index = {}
cur_fc = None
cur_paused = False
seg_w = defaultdict(lambda: [0, 0.0, set()])   # fc -> [calls, dtsum, substeps]
p_subdt = {}                                  # fc -> subdt (last)
rates = defaultdict(lambda: [0, 0])           # (set,slot) -> [playing, paused]
msgs = []                                     # (fc, text)
demo = []                                     # (fc, text)
tele = []                                     # (fc, tick, paused, idx, cls, jump, from, to)
tracks = defaultdict(list)                    # idx -> [(fc, paused, tick, x, y, z, extra)]
last_pos = {}
cls_of = {}

with open(path, "rb") as fh:
    for raw in fh:
        t = raw[:1]
        if t == b"E":
            p = raw.rstrip(b"\n").split(b",")
            if len(p) < 20:
                continue
            idx = int(p[4])
            cls = p[5]
            d = kvb(p[7:])
            key = b"b1" if d.get(b"bones") == b"1" and cls.find(b"PhysicsProp") < 0 else b"org"
            pos = v3(d.get(key, b""))
            if pos is None:
                continue
            fc = int(p[1])
            important = b"Pawn" in cls or b"Ragdoll" in cls
            lp = last_pos.get(idx)
            moved = lp is None or abs(lp[0] - pos[0]) + abs(lp[1] - pos[1]) + abs(lp[2] - pos[2]) > 0.01
            last_pos[idx] = pos
            cls_of[idx] = cls.decode(errors="replace")
            if important or moved:
                extra = None
                if important:
                    extra = (d.get(b"life"), d.get(b"org"), d.get(b"pose"), d.get(b"built"), d.get(b"enabled"), d.get(b"clientside"), d.get(b"csrag"), d.get(b"sim"), d.get(b"ragpos"), d.get(b"b1"))
                tracks[idx].append((fc, p[3] == b"1", int(p[2]), pos[0], pos[1], pos[2], extra))
        elif t == b"G":
            p = raw.rstrip(b"\n").split(b",")
            if len(p) < 4 or p[3] == b"noglobals":
                continue
            d = kvb(p[3:])
            cur_fc = int(p[2])
            cur_paused = d.get(b"paused") == b"1"
            fc_index[cur_fc] = len(frames)
            frames.append((cur_fc, float(p[1]), cur_paused, int(d[b"tick"]), float(d[b"ft08"]), float(d[b"physft34"]), float(d[b"curtime30"])))
        elif t == b"W":
            p = raw.rstrip(b"\n").split(b",")
            d = kvb(p[5:])
            s = seg_w[int(p[2])]
            s[0] += 1
            s[1] += float(d[b"dt"])
            s[2].add(int(d[b"substeps"]))
        elif t == b"P":
            p = raw.rstrip(b"\n").split(b",")
            d = kvb(p[5:])
            p_subdt[int(p[2])] = (float(d[b"subdt"]), int(d[b"substeps"]))
        elif t == b"C":
            p = raw.rstrip(b"\n").split(b",")
            for item in p[3:]:
                s, _, n = item.partition(b":")
                rates[(p[2].decode(), int(s))][1 if cur_paused else 0] += int(n)
        elif t == b"#":
            txt = raw.decode(errors="replace").rstrip()
            if "moved up to" in txt or "after resume (10" in txt:
                continue
            msgs.append((cur_fc, txt))
        elif t == b"D":
            txt = raw.decode(errors="replace").rstrip()
            if "SetTimeScale" in txt:
                if demo and demo[-1][1].split(",")[-1] == txt.split(",")[-1] and "SetTimeScale" in demo[-1][1]:
                    continue
            demo.append((cur_fc, txt))
        elif t == b"T":
            p = raw.decode(errors="replace").rstrip().split(",")
            tele.append((int(p[2]), int(p[3]), p[4] == "1", int(p[5]), p[6], float(p[7][5:]), p[8][5:], p[9][3:]))

P(f"frames: {len(frames)}, paused frames: {sum(1 for f in frames if f[2])}")
P("\n=== messages / experiment toggles / marks ===")
for fc, m in msgs:
    if any(k in m for k in ("MARK", "EXPERIMENT", "PAUSED", "RESUMED", "entity sampling", "TELEPORT")):
        P(f"  fc {fc}: {m}")
P("\n=== demo player calls (dedup SetTimeScale) ===")
for fc, m in demo:
    P(f"  fc {fc}: {m}")

# segments of equal paused state
segs = []
for i, f in enumerate(frames):
    if not segs or segs[-1][2] != f[2]:
        segs.append([i, i, f[2]])
    else:
        segs[-1][1] = i

P("\n=== segments ===")
for a, b, paused in segs:
    fa, fb = frames[a], frames[b]
    ws = [seg_w[frames[i][0]] for i in range(a, b + 1)]
    calls = sum(w[0] for w in ws)
    dts = sum(w[1] for w in ws)
    subs = set().union(*[w[2] for w in ws]) if ws else set()
    n = b - a + 1
    ft = [frames[i][4] for i in range(a, b + 1)]
    pft = [frames[i][5] for i in range(a, b + 1)]
    P(f"{'PAUSED ' if paused else 'playing'} fc {fa[0]}..{fb[0]} ({n} fr, {fb[1] - fa[1]:.2f}s wall) tick {fa[3]}->{fb[3]} "
      f"curtime {fa[6]:.3f}->{fb[6]:.3f} | ft08 mean {sum(ft) / n:.5f} min {min(ft):.5f} max {max(ft):.5f} | phys34 mean {sum(pft) / n:.5f} "
      f"min {min(pft):.5f} | world steps {calls} ({calls / n:.2f}/fr) dtsum {dts:.3f}s substeps {sorted(subs)}")

# per pause: tracked movers
P("\n=== per-pause entity behaviour (entities that moved within +-40 frames of the pause) ===")
for a, b, paused in segs:
    if not paused or b - a < 3:
        continue
    fa, fb = frames[a], frames[b]
    lo_fc = frames[max(0, a - 40)][0]
    hi_fc = frames[min(len(frames) - 1, b + 40)][0]
    P(f"\n--- pause fc {fa[0]}..{fb[0]} tick {fa[3]}->{fb[3]} ({fb[1] - fa[1]:.2f}s) ---")
    for idx, tr in tracks.items():
        win = [r for r in tr if lo_fc <= r[0] <= hi_fc]
        if len(win) < 2:
            continue
        # movement check
        tot = sum(math.dist(win[i][3:6], win[i - 1][3:6]) for i in range(1, len(win)))
        if tot < 1.0:
            continue
        before = [r for r in win if r[0] < fa[0]]
        during = [r for r in win if fa[0] <= r[0] <= fb[0]]
        after = [r for r in win if r[0] > fb[0]]
        s = f"  #{idx} {cls_of.get(idx)}:"
        if before:
            s += f" 40fr-before path {sum(math.dist(before[i][3:6], before[i-1][3:6]) for i in range(1, len(before))):.1f}u"
        if during:
            p0 = before[-1][3:6] if before else during[0][3:6]
            dist_series = [math.dist(r[3:6], p0) for r in during]
            steps = [math.dist(during[i][3:6], during[i - 1][3:6]) for i in range(1, len(during))]
            last_move = max([i for i, st in enumerate(steps) if st > 0.05], default=-1)
            last_fc = during[last_move + 1][0] if last_move >= 0 else None
            last_t = (frames[fc_index[last_fc]][1] - fa[1]) if last_fc in fc_index else float("nan")
            s += (f" | DURING pause moved {dist_series[-1]:.1f}u net (max {max(dist_series):.1f}), kept moving until {last_t:.2f}s into pause"
                  f" (z {during[0][5]:.1f}->{during[-1][5]:.1f})")
        if after and during:
            j = [math.dist(after[0][3:6], during[-1][3:6])] + [math.dist(after[i][3:6], after[i - 1][3:6]) for i in range(1, min(5, len(after)))]
            s += f" | first frames after resume steps {', '.join(f'{x:.1f}' for x in j)} (tick {after[0][2]})"
        P(s)
        if during and during[0][6] is not None:
            # compact path: position every ~0.25s wall during the pause, plus 10 frames around resume
            t0 = fa[1]
            nxt = 0.0
            pts = []
            for r in during:
                ti = fc_index.get(r[0])
                tw = frames[ti][1] - t0 if ti is not None else 0.0
                if tw >= nxt:
                    pts.append(f"{tw:.2f}s:({r[3]:.0f},{r[4]:.0f},{r[5]:.1f})")
                    nxt = tw + 0.25
            P(f"      path during pause: {' '.join(pts[:60])}")
            if before:
                P(f"      last 6 before : {' '.join(f'fc{r[0]}:z{r[5]:.1f}' for r in before[-6:])}")
            if after:
                P(f"      first 10 after: {' '.join(f'fc{r[0]}/t{r[2]}:({r[3]:.0f},{r[4]:.0f},{r[5]:.1f})' for r in after[:10])}")
        if during and during[0][6]:
            e0, e1 = during[0][6], during[-1][6]
            P(f"      extra at pause start: life={e0[0]} org={e0[1]} pose={e0[2]} built={e0[3]} enabled={e0[4]} clientside={e0[5]} csrag={e0[6]} sim={e0[7]}")
            P(f"      extra at pause end  : life={e1[0]} org={e1[1]} pose={e1[2]} built={e1[3]} enabled={e1[4]} clientside={e1[5]} csrag={e1[6]} sim={e1[7]}")
            if after and after[0][6]:
                e2 = after[min(3, len(after) - 1)][6]
                P(f"      extra 3fr after     : life={e2[0]} org={e2[1]} pose={e2[2]} built={e2[3]} enabled={e2[4]} clientside={e2[5]} csrag={e2[6]} sim={e2[7]}")

P("\n=== teleports summarised (jump>24u) ===")
by = defaultdict(list)
for tl in tele:
    by[(tl[1], tl[2])].append(tl)
for (tick, paused), lst in sorted(by.items(), key=lambda kv: kv[1][0][0]):
    cls = defaultdict(int)
    for tl in lst:
        cls[tl[4]] += 1
    P(f"  fc {lst[0][0]} tick {tick} paused {int(paused)}: {len(lst)} jumps {dict(cls)} max {max(tl[5] for tl in lst):.0f}u"
      f" e.g. #{lst[0][3]} {lst[0][6]} -> {lst[0][7]}")

P("\n=== vtable call rates playing vs paused (per frame) ===")
n_p = sum(1 for f in frames if f[2])
n_r = len(frames) - n_p
for k in sorted(rates):
    r, pz = rates[k][0] / max(1, n_r), rates[k][1] / max(1, n_p)
    tag = "STOPS when paused" if r > 0.3 and pz < 0.02 else ("ONLY when paused" if pz > 0.3 and r < 0.02 else "")
    P(f"  {k[0]:>40} [{k[1]:3}] playing {r:9.3f} paused {pz:9.3f} {tag}")
out.close()
