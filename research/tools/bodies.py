#!/usr/bin/env python3
"""Reads the physics-body-finder records ('Y' candidates, 'C' per-event changes, 'B' final state) of a RagdollDemoClock insight
recording and shows where the ragdoll's physics body transforms live and how they move across an unpause - so the unpause snap
(render bones catching up to physics bodies) can be measured instead of guessed.

  python bodies.py                         newest recording in ../bin/logs
  python bodies.py FILE                     summary of every body-finder run
  python bodies.py FILE --run N             run N: candidate table + the picked bodies' change timeline
  python bodies.py FILE --run N --idx 3,7   timeline for those candidate indices
  python bodies.py FILE --run N --top 8     timeline for the 8 most-changed candidates (default 6)
  python bodies.py FILE --run N --bone 5    also track this bone from the 'D' records, to time the body-vs-bone lag
  python bodies.py FILE --run N --csv       the timeline as CSV on stdout

Candidate 'idx' in the 'C' records indexes the run's candidate list (the order the 'Y'/'B' records emit).
"""
import argparse
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from insight import Insight, records, Rd, EFMT

EVSHORT = {1: 'A>', 2: 'A<', 3: 'S>', 4: 'S<', 5: 'F>', 6: 'F<', 7: 'I>', 8: 'I<'}  # anim/step/frame/inner-step, pre '>' / post '<'
EVNAME = {1: 'anim>', 2: 'anim<', 3: 'step>', 4: 'step<', 5: 'frame>', 6: 'frame<', 7: 'inner>', 8: 'inner<'}


def pat_str(pat):
    kinds = []
    if pat & 1:
        kinds.append('xyz')
    if pat & 2:
        kinds.append('m3x4')
    if pat & 0x10:
        kinds.append('meters')
    return '+'.join(kinds) or f'0x{pat:x}'


def mask_str(mask):
    return ''.join(EVSHORT[i] for i in range(1, 9) if mask & (1 << i)) or '-'


class Run:
    def __init__(self, rid):
        self.id = rid
        self.cand = {}       # idx -> dict(addr, pat, first=(x,y,z))
        self.final = {}      # idx -> dict from 'B': first, last, mask, changes, noisy, pat, addr
        self.timeline = {}   # idx -> list of (frame, ev, stepCalls, x, y, z)
        self.events = 0

    def n(self):
        return max(len(self.cand), len(self.final))


def cand_row(idx, R):
    """One display dict per candidate, preferring the richer 'B' record."""
    b = R.final.get(idx)
    y = R.cand.get(idx)
    if b:
        d = dict(b)
        d['first'] = b['first']
    elif y:
        d = dict(addr=y['addr'], pat=y['pat'], first=y['first'], last=y['first'], mask=0, changes=0, noisy=0)
    else:
        return None
    d['idx'] = idx
    dx = tuple(round(d['last'][k] - d['first'][k], 3) for k in range(3))
    d['delta'] = dx
    d['moved'] = max(abs(v) for v in dx)
    return d


def unpause_frame(markers, rid):
    for fr, text in markers:
        if text.startswith('BODIES finalized') and f'run {rid}' in text and 'unpause frame' in text:
            try:
                return int(text.split('unpause frame')[1].strip().split()[0])
            except (ValueError, IndexError):
                pass
    for fr, text in markers:
        if text.startswith('UNPAUSE'):
            return fr
    return None


def print_summary(ins, runs, markers, out=print):
    out(f'body finder: {os.path.basename(ins.path)} | addon {ins.version} | format {ins.format} | {len(runs)} run(s)')
    for fr, text in markers:
        if text.startswith('BODIES'):
            out(f'  {fr:7d}  {text}')
    for rid in sorted(runs):
        R = runs[rid]
        rows = [cand_row(i, R) for i in (R.final or R.cand)]
        rows = [r for r in rows if r]
        moved = [r for r in rows if r['changes']]
        up = unpause_frame(markers, rid)
        out(f'\n== run {rid}: {R.n()} candidates, {len(moved)} changed, {R.events} change-events'
            f'{f", unpause frame {up}" if up is not None else ""}')
        top = sorted(rows, key=lambda r: -r['changes'])[:12]
        if top and top[0]['changes']:
            out(f"   {'idx':>4} {'address':16} {'pat':11} {'changes':>7} {'moved':>8}  events        first xyz -> last xyz")
            for r in top:
                if not r['changes']:
                    continue
                fx = ' '.join(f'{v:.1f}' for v in r['first'])
                lx = ' '.join(f'{v:.1f}' for v in r['last'])
                out(f"   {r['idx']:>4} {r['addr']:016x} {pat_str(r['pat']):11} {r['changes']:>7} {r['moved']:>8.2f}  "
                    f"{mask_str(r['mask']):13} {fx} -> {lx}{'  NOISY' if r['noisy'] else ''}")


def print_run(ins, runs, markers, state, bone_track, rid, sel_idx, top, csv, out=print):
    if rid not in runs:
        sys.exit(f'run {rid} not in this recording (have {sorted(runs)})')
    R = runs[rid]
    rows = [cand_row(i, R) for i in (R.final or R.cand)]
    rows = [r for r in rows if r]
    up = unpause_frame(markers, rid)

    if not csv:
        out(f'== run {rid}: {R.n()} candidates, {R.events} change-events'
            f'{f", unpause frame {up}" if up is not None else ""}')
        for fr, text in markers:
            if 'BODIES' in text and f'run {rid}' in text:
                out(f'  {fr:7d}  {text}')

        out('\n-- candidate table (most-changed first)')
        out(f"   {'idx':>4} {'address':16} {'pat':11} {'chg':>6} {'moved':>8} {'noisy':>5}  events        first xyz -> last xyz")
        for r in sorted(rows, key=lambda r: -r['changes'])[:60]:
            fx = ' '.join(f'{v:.2f}' for v in r['first'])
            lx = ' '.join(f'{v:.2f}' for v in r['last'])
            out(f"   {r['idx']:>4} {r['addr']:016x} {pat_str(r['pat']):11} {r['changes']:>6} {r['moved']:>8.2f} "
                f"{'yes' if r['noisy'] else '':>5}  {mask_str(r['mask']):13} {fx} -> {lx}")

    # which candidates to show a timeline for
    if sel_idx:
        chosen = [i for i in sel_idx if i in R.timeline or i in R.cand]
    else:
        chosen = [r['idx'] for r in sorted(rows, key=lambda r: -r['changes']) if r['changes']][:top]
    if not chosen:
        out('\n(no changing candidates to plot a timeline for)')
        return

    if csv:
        out('idx,frame,event,stepCalls,x,y,z,paused')
        for idx in chosen:
            for fr, ev, sc, x, y, z in R.timeline.get(idx, []):
                paused = state.get(fr, (0, 0, 0))[1]
                out(f'{idx},{fr},{EVNAME.get(ev, ev)},{sc},{x:.5f},{y:.5f},{z:.5f},{paused}')
        return

    out(f'\n-- change timeline for candidates {chosen} (each row is one event that moved it)')
    for idx in chosen:
        r = cand_row(idx, R)
        tl = R.timeline.get(idx, [])
        head = f"\n  candidate #{idx}  {r['addr']:016x}  {pat_str(r['pat'])}  {r['changes']} changes  events {mask_str(r['mask'])}"
        out(head)
        if up is not None:
            firsts = [row for row in tl if row[0] >= up]
            if firsts:
                fr, ev, sc, x, y, z = firsts[0]
                out(f"     first move at/after unpause: frame {fr} ({EVNAME.get(ev, ev)}, +{fr - up} frames), step calls {sc}")
        last = None
        for fr, ev, sc, x, y, z in tl:
            tick, paused, playing = state.get(fr, (-1, 0, 0))
            tag = 'PAUSE' if paused else 'play ' if playing else 'stop '
            up_tag = ''
            if up is not None and fr >= up:
                up_tag = f'  u+{fr - up}'
            d = ''
            if last is not None:
                dv = max(abs(x - last[0]), abs(y - last[1]), abs(z - last[2]))
                d = f'  d{dv:.3f}'
            out(f"     f{fr:<6} {EVNAME.get(ev, ev):6} {tag} sc{sc:<6}  {x:9.3f} {y:9.3f} {z:9.3f}{d}{up_tag}")
            last = (x, y, z)

    # bone reference, if requested
    if bone_track['bone'] is not None and bone_track['rows']:
        out(f"\n-- bone[{bone_track['bone']}] position from the 'D' records (for comparison with the bodies above)")
        if up is not None:
            after = [row for row in bone_track['rows'] if row[0] >= up]
            if after:
                fr, ev, x, y, z = after[0]
                out(f"     bone first moves at/after unpause: frame {fr} ({EVNAME.get(ev, ev)}, +{fr - up} frames)")
        last = None
        for fr, ev, x, y, z in bone_track['rows']:
            if up is not None and fr < up - 2:
                continue
            up_tag = f'  u+{fr - up}' if up is not None and fr >= up else ''
            d = ''
            if last is not None:
                d = f'  d{max(abs(x - last[0]), abs(y - last[1]), abs(z - last[2])):.3f}'
            out(f"     f{fr:<6} {EVNAME.get(ev, ev):6}       {x:9.3f} {y:9.3f} {z:9.3f}{d}{up_tag}")
            last = (x, y, z)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file', nargs='?')
    ap.add_argument('--run', type=int)
    ap.add_argument('--idx', default='')
    ap.add_argument('--top', type=int, default=6)
    ap.add_argument('--bone', type=int)
    ap.add_argument('--csv', action='store_true')
    a = ap.parse_args()

    path = a.file
    if not path:
        logs = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'bin', 'logs')
        files = sorted(glob.glob(os.path.join(logs, 'insight_*.bin')), key=os.path.getmtime)
        if not files:
            sys.exit(f'no insight_*.bin in {logs}')
        path = files[-1]

    ins, runs, markers, state, bone_track = load_with_bone(path, a.bone)

    if not runs:
        print(f'{os.path.basename(path)}: no body-finder records. Arm the finder while paused on a fresh ragdoll:')
        print('   ragdollfix insight rec 1 bodies   (then pause on a death and unpause; or: ragdollfix insight watch bodies start)')
        return

    if a.run is None:
        print_summary(ins, runs, markers)
        print('\nrun a single one for the timeline:  python bodies.py '
              f'{os.path.basename(path)} --run {sorted(runs)[0]}')
        return
    sel = [int(x, 0) for x in a.idx.split(',') if x.strip()] if a.idx else []
    print_run(ins, runs, markers, state, bone_track, a.run, sel, a.top, a.csv)


def load_with_bone(path, bone):
    """load() but with the bone tracker primed before the linear pass."""
    # load() reads bone_track['bone'] lazily; prime it by monkeypatching the default via a wrapper.
    ins = Insight(path)
    mm = ins.mm
    runs, markers, state = {}, [], {}
    bones_mem = {}
    bt = {'bone': bone, 'rows': [], 'cur': (0, 0, 0, 0)}

    def run(rid):
        return runs.setdefault(rid, Run(rid))

    for tag, p, n in records(mm, ins.start):
        if tag == 'E':
            v = EFMT.unpack_from(mm, p)
            state[v[1]] = (v[5], v[4], v[3])
            bt['cur'] = (v[1], v[0], v[4], v[3])
        elif tag == 'M':
            r = Rd(mm, p)
            wall, frame = r.u('<dI')
            text = r.s()
            if text.startswith(('BODIES', 'PAUSE', 'UNPAUSE', 'JUMP')):
                markers.append((frame, text))
        elif tag == 'O':
            r = Rd(mm, p)
            oid, kind, flags, addr, vt, size, cls, root, depth = r.u('<IBBQQIIBB')
            if kind == 2:
                bones_mem[oid] = bytearray(size)
        elif tag == 'S':
            oid = struct.unpack_from('<I', mm, p)[0]
            if oid in bones_mem:
                seg = mm[p + 4:p + n]
                bones_mem[oid][:len(seg)] = seg
        elif tag == 'D' and bone is not None:
            oid, cnt = struct.unpack_from('<II', mm, p)
            if oid not in bones_mem:
                continue
            mem = bones_mem[oid]
            pairs = struct.unpack_from(f'<{2 * cnt}I', mm, p + 8)
            lo, hi = bone * 32, bone * 32 + 12
            touched = False
            for k in range(0, 2 * cnt, 2):
                off, new = pairs[k], pairs[k + 1]
                if off + 4 <= len(mem):
                    struct.pack_into('<I', mem, off, new)
                if lo <= off < hi:
                    touched = True
            if touched and len(mem) >= hi:
                x, y, z = struct.unpack_from('<3f', mem, lo)
                fr, ev, paused, playing = bt['cur']
                bt['rows'].append((fr, ev, x, y, z))
        elif tag == 'Y':
            r = Rd(mm, p)
            rid, start, count = r.u('<III')
            R = run(rid)
            for i in range(count):
                addr = r.u('<Q'); pat = r.u('<B'); x, y, z = r.u('<3f')
                R.cand[start + i] = dict(addr=addr, pat=pat, first=(x, y, z))
        elif tag == 'C':
            r = Rd(mm, p)
            rid = r.u('<I'); ev = r.u('<B'); frame = r.u('<I'); stepCalls = r.u('<Q'); start = r.u('<I'); count = r.u('<I')
            R = run(rid); R.events += 1
            for _ in range(count):
                idx = r.u('<I'); x, y, z = r.u('<3f')
                R.timeline.setdefault(idx, []).append((frame, ev, stepCalls, x, y, z))
        elif tag == 'B':
            r = Rd(mm, p)
            rid, start, count = r.u('<III')
            R = run(rid)
            for i in range(count):
                addr = r.u('<Q'); pat = r.u('<B')
                fx, fy, fz = r.u('<3f'); lx, ly, lz = r.u('<3f')
                mask = r.u('<H'); changes = r.u('<I'); noisy = r.u('<B')
                R.final[start + i] = dict(addr=addr, pat=pat, first=(fx, fy, fz), last=(lx, ly, lz),
                                          mask=mask, changes=changes, noisy=noisy)
    return ins, runs, markers, state, bt


if __name__ == '__main__':
    main()
