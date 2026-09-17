#!/usr/bin/env python3
"""Reads bin/logs/insight_*.bin (RagdollDemoClock 1.3 insight recorder) and writes <file>.report.txt next to it.

  python insight.py                      newest recording in ../bin/logs
  python insight.py FILE --after 40      detail window: frames after each PAUSE / UNPAUSE / JUMP marker (default 30)
  python insight.py FILE --frames 100-140   extra detail window
  python insight.py FILE --track 12:0x3c4   value of object #12 at offset 0x3c4 after every event (CSV to stdout)
"""
import argparse
import bisect
import glob
import mmap
import os
import struct
import sys
from collections import Counter, defaultdict

EV = {1: 'anim>', 2: 'anim<', 3: 'step>', 4: 'step<', 5: 'frame>', 6: 'frame<'}
KIND = ['globals', 'pawn', 'bones', 'object', 'blob', 'animctrl', 'scenenode', 'body']
DROP = {1: 'unreadable', 2: 'vtable changed (freed or reused)', 3: 'no longer selected', 4: 'globals moved', 5: 'target changed'}
ROOT = ['pawn', 'animctrl', 'world', 'physsys', 'animsys', 'posectl', 'ragsys', 'xref']
EFMT = struct.Struct('<BIdBBiff3fBQiBfiddi')
DETAIL_MARKERS = ('PAUSE', 'UNPAUSE', 'JUMP', 'TARGET #', 'USER')
BONES_SHOWN = 64


class Rd:
    def __init__(self, b, p):
        self.b, self.p = b, p

    def u(self, fmt):
        s = struct.Struct(fmt)
        v = s.unpack_from(self.b, self.p)
        self.p += s.size
        return v if len(v) > 1 else v[0]

    def s(self):
        n = self.u('<H')
        v = bytes(self.b[self.p:self.p + n]).decode('utf-8', 'replace')
        self.p += n
        return v


def records(mm, start):
    p, end = start, len(mm)
    while p + 5 <= end:
        tag = mm[p]
        n = struct.unpack_from('<I', mm, p + 1)[0]
        if p + 5 + n > end:
            break  # truncated tail (game closed while recording)
        yield chr(tag), p + 5, n
        p += 5 + n


def fmt_val(v):
    if v == 0:
        return '0'
    f = struct.unpack('<f', struct.pack('<I', v))[0]
    if 1e-4 <= abs(f) <= 1e6:
        return f'{f:.5g}'
    if v < 0x10000:
        return str(v)
    return f'0x{v:08x}'


class Insight:
    def __init__(self, path):
        self.path = path
        self.f = open(path, 'rb')
        self.mm = mmap.mmap(self.f.fileno(), 0, access=mmap.ACCESS_READ)
        if self.mm[:8] != b'INSIGHT\0':
            sys.exit(f'{path}: not an insight recording')
        r = Rd(self.mm, 8)
        self.format = r.u('<I')
        self.version, self.label = r.s(), r.s()
        self.start = r.p
        self.classes, self.fields, self.objs = {}, {}, {}
        self.markers = []

    def scan_markers(self):
        for tag, p, n in records(self.mm, self.start):
            if tag == 'M':
                r = Rd(self.mm, p)
                wall, frame = r.u('<dI')
                self.markers.append((frame, wall, r.s()))

    def field(self, o, off):
        if o['kind'] == 2:
            i, sub = divmod(off, 32)
            return f'bone[{i}]' + {0: '.x', 4: '.y', 8: '.z', 12: '.scale'}.get(sub, f'.q+{sub - 16:x}')
        fl = self.fields.get(o['cls'])
        if fl:
            i = bisect.bisect_right(fl[0], off) - 1
            if i >= 0 and off - fl[0][i] < 0x40:
                name = fl[1][i][0]
                d = off - fl[0][i]
                return f'{name}+{d:x}' if d else name
        return f'+0x{off:x}'

    def run(self, windows, track):
        mm = self.mm
        ev = None
        evs_in_frame = []
        frame_rows = {}  # frame -> list of event rows (for detail windows)
        detail = defaultdict(list)  # frame -> lines
        paused_chg = Counter()  # (id, field) -> events changed while paused
        paused_first = {}
        play_chg = Counter()
        track_id, track_off = track if track else (None, None)
        for tag, p, n in records(mm, self.start):
            if tag == 'E':
                v = EFMT.unpack_from(mm, p)
                ev = dict(ev=v[0], frame=v[1], wall=v[2], playing=v[3], paused=v[4], tick=v[5], frac=v[6], curtime=v[7],
                          c=v[8:11], haveC=v[11], steps=v[12], lastAnim=v[13], mode=v[14], dt=v[15], sub=v[16], T=v[17], P=v[18],
                          idx=v[19])
                if ev['frame'] in windows:
                    frame_rows.setdefault(ev['frame'], []).append(ev)
            elif tag == 'D':
                oid, cnt = struct.unpack_from('<II', mm, p)
                o = self.objs.get(oid)
                if not o or ev is None:
                    continue
                pairs = struct.unpack_from(f'<{2 * cnt}I', mm, p + 8)
                mem = o['mem']
                in_win = ev['frame'] in windows
                paused = ev['playing'] and ev['paused']
                o['chg'][(EV.get(ev['ev'], '?'), 'paused' if paused else 'playing' if ev['playing'] else 'stopped')] += 1
                lines, bones_moved, bone_dz = [], set(), 0.0
                for k in range(0, 2 * cnt, 2):
                    off, new = pairs[k], pairs[k + 1]
                    old = struct.unpack_from('<I', mem, off)[0]
                    struct.pack_into('<I', mem, off, new)
                    name = self.field(o, off)
                    key = (oid, name)
                    if paused:
                        paused_chg[key] += 1
                        paused_first.setdefault(key, fmt_val(old))
                    elif ev['playing']:
                        play_chg[key] += 1
                    if o['kind'] == 2:
                        bi, sub = divmod(off, 32)
                        bones_moved.add(bi)
                        if sub == 8:
                            a, b = struct.unpack('<ff', struct.pack('<II', old, new))
                            bone_dz = max(bone_dz, abs(b - a), key=abs)
                        if bi >= BONES_SHOWN:
                            continue
                    if in_win and len(lines) < 14:
                        lines.append(f'{name} {fmt_val(old)} -> {fmt_val(new)}')
                if oid == track_id:
                    val = struct.unpack_from('<I', mem, track_off)[0]
                    print(f"{ev['frame']},{EV.get(ev['ev'])},{ev['tick']},{ev['paused']},{fmt_val(val)}")
                if in_win:
                    extra = f' | {len(bones_moved)} bones moved, largest z change {bone_dz:.2f}' if bones_moved else ''
                    detail[(ev['frame'], id(ev))].append(f"      #{oid} {o['label'][:60]}: {cnt} words{extra}\n"
                                                         + ''.join(f'          {s}\n' for s in lines))
            elif tag == 'O':
                r = Rd(mm, p)
                oid, kind, flags, addr, vt, size, cls, root, depth = r.u('<IBBQQIIBB')
                cls = struct.unpack('<i', struct.pack('<I', cls))[0]
                self.objs[oid] = dict(id=oid, kind=kind, flags=flags, addr=addr, vt=vt, size=size, cls=cls, root=root, depth=depth,
                                      label=r.s(), mem=bytearray(size), chg=Counter(), born=ev['frame'] if ev else 0, dropped=None)
            elif tag == 'S':
                oid = struct.unpack_from('<I', mm, p)[0]
                if oid in self.objs:
                    self.objs[oid]['mem'][:] = mm[p + 4:p + n]
            elif tag == 'X':
                oid, why = struct.unpack_from('<IB', mm, p)
                if oid in self.objs:
                    self.objs[oid]['dropped'] = (ev['frame'] if ev else 0, DROP.get(why, why))
            elif tag == 'K':
                r = Rd(mm, p)
                cls = r.u('<I')
                raw, pretty, module = r.s(), r.s(), r.s()
                self.classes[cls] = f'{module}!{pretty}'
            elif tag == 'F':
                r = Rd(mm, p)
                cls, cnt = r.u('<II')
                ent = []
                for _ in range(cnt):
                    off = r.u('<I')
                    ent.append((off, r.s(), r.s(), r.s()))
                ent.sort()
                self.fields[cls] = ([e[0] for e in ent], [(e[1], e[2]) for e in ent])
        return frame_rows, detail, paused_chg, paused_first, play_chg


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file', nargs='?')
    ap.add_argument('--after', type=int, default=30)
    ap.add_argument('--before', type=int, default=3)
    ap.add_argument('--frames', action='append', default=[])
    ap.add_argument('--track')
    a = ap.parse_args()
    path = a.file
    if not path:
        logs = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'bin', 'logs')
        files = sorted(glob.glob(os.path.join(logs, 'insight_*.bin')), key=os.path.getmtime)
        if not files:
            sys.exit(f'no insight_*.bin in {logs}')
        path = files[-1]
    ins = Insight(path)
    ins.scan_markers()
    windows = set()
    for frame, _, text in ins.markers:
        if text.startswith(DETAIL_MARKERS):
            windows.update(range(max(0, frame - a.before), frame + a.after + 1))
    for fr in a.frames:
        lo, _, hi = fr.partition('-')
        windows.update(range(int(lo), int(hi or lo) + 1))
    track = None
    if a.track:
        oid, _, off = a.track.partition(':')
        track = (int(oid), int(off, 0))
        print('frame,event,tick,paused,value')
    frame_rows, detail, paused_chg, paused_first, play_chg = ins.run(windows, track)
    if track:
        return

    out = path + '.report.txt'
    with open(out, 'w', encoding='utf-8') as w:
        w.write(f'insight recording {os.path.basename(path)} | addon {ins.version} | label {ins.label!r} | '
                f'{os.path.getsize(path) / 2**20:.1f} MB\n\n== markers (frame numbers count frame boundaries)\n')
        t0 = ins.markers[0][1] if ins.markers else 0
        for frame, wall, text in ins.markers:
            w.write(f'{frame:6d} {wall - t0:8.3f}s  {text}\n')

        w.write('\n== tracked objects: changes per hooked call (event > before / < after) and demo state\n')
        for o in ins.objs.values():
            cls = ins.classes.get(o['cls'], '')
            drop = f" | dropped at frame {o['dropped'][0]}: {o['dropped'][1]}" if o['dropped'] else ''
            w.write(f"#{o['id']:<4} {KIND[o['kind']] if o['kind'] < len(KIND) else o['kind']:9} {o['addr']:016x} 0x{o['size']:<5x} "
                    f"root {ROOT[o['root']] if o['root'] < len(ROOT) else '-'} depth {o['depth']} | {o['label']} {cls}{drop}\n")
            if o['chg']:
                w.write('        ' + ', '.join(f'{e}/{s} {c}' for (e, s), c in sorted(o['chg'].items())) + '\n')

        w.write('\n== changed while the demo was paused (number of hooked calls with a change; value before the first change)\n')
        for (oid, name), c in paused_chg.most_common(300):
            o = ins.objs[oid]
            w.write(f"{c:7d}  #{oid} {o['label'][:50]:50} {name:40} first old {paused_first[(oid, name)]} | while playing {play_chg[(oid, name)]}\n")

        w.write(f'\n== detail windows ({a.before} frames before to {a.after} after PAUSE / UNPAUSE / JUMP / TARGET / USER markers)\n')
        marks = defaultdict(list)
        for frame, _, text in ins.markers:
            marks[frame].append(text)
        last_steps = None
        for frame in sorted(frame_rows):
            for text in marks.get(frame, []):
                w.write(f'\n  ---- {text}\n')
            for ev in frame_rows[frame]:
                steps = ev['steps'] - last_steps if last_steps is not None else 0
                last_steps = ev['steps']
                c = ' '.join(f'{x:.1f}' for x in ev['c']) if ev['haveC'] else '-'
                w.write(f"  f{frame} {EV.get(ev['ev'], '?'):6} tick {ev['tick']} {'PAUSED' if ev['paused'] else 'play  '} "
                        f"steps +{steps} dt {ev['dt']:.4f} sub {ev['sub']} lastAnimTick {ev['lastAnim']} curtime {ev['curtime']:.3f} "
                        f"T {ev['T']:.2f} P {ev['P']:.2f} mode {ev['mode']} target #{ev['idx']} centroid {c}\n")
                for block in detail.get((frame, id(ev)), []):
                    w.write(block)
    print(f'report -> {out}')
    for frame, _, text in ins.markers:
        if text.startswith(DETAIL_MARKERS + ('START', 'STOP', 'XREF', 'CENSUS')):
            print(f'{frame:6d} {text[:150]}')


if __name__ == '__main__':
    main()
