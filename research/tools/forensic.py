#!/usr/bin/env python3
"""Reads the 'W' (watchpoint) records of a RagdollDemoClock insight recording and says who wrote what, from where.

Each hit carries the faulting instruction, an unwound call stack, all 16 general registers, the 32 bytes at the watched
address and - for page-journal hits - the exact byte diff. Addresses are named as module+RVA and, where the census knows
the memory, as object #id+offset.

  python forensic.py                          newest recording in ../bin/logs
  python forensic.py FILE --summary           just the distinct writers, one line each (start here)
  python forensic.py FILE --frames 4300-4400  only hits in that frame range
  python forensic.py FILE --slot 0            only hardware slot 0 (the anim helper, say)
  python forensic.py FILE --kind exec         exec | write | page
  python forensic.py FILE --rip client.dll+0x1a2b3c   only hits from one writer
  python forensic.py FILE --max 200           detail hits printed (default 120)
"""
import argparse
import bisect
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from insight import Insight, records, Rd, EFMT, EV, KIND

GPR = ['rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi',
       'r8', 'r9', 'r10', 'r11', 'r12', 'r13', 'r14', 'r15']
KINDS = {0: 'EXEC', 1: 'WRITE', 2: 'PAGE', 3: 'ACCESS', 4: 'PGREAD'}
# Microsoft x64 call: the first four integer arguments.
ARGS = [(1, 'rcx'), (2, 'rdx'), (8, 'r8'), (9, 'r9')]
KWSTK = 256      # bytes of stack captured per hit (kWStk)
KWREGMEM = 64    # bytes captured behind each pointer register (kWRegMem)


def vec_floats(b, count):
    """Interpret the first `count` little-endian floats of a byte string, blanking implausible ones."""
    out = []
    for i in range(min(count, len(b) // 4)):
        v = struct.unpack_from('<f', b, i * 4)[0]
        out.append(f'{v:.3f}' if (v == 0 or 1e-6 < abs(v) < 1e7) else '-')
    return ' '.join(out)


class Syms:
    """Module table from the recording's 'L' records: address -> module+RVA."""

    def __init__(self):
        self.mods = []  # (base, size, name), kept sorted
        self.bases = []

    def add(self, base, size, name):
        for b, s, n in self.mods:
            if b == base:
                return
        self.mods.append((base, size, name))
        self.mods.sort()
        self.bases = [m[0] for m in self.mods]

    def name(self, a):
        if not a:
            return '0'
        i = bisect.bisect_right(self.bases, a) - 1
        if i >= 0:
            b, s, n = self.mods[i]
            if a < b + s:
                return f'{n}+0x{a - b:x}'
        return f'{a:016x}'


class Objs:
    """Live census objects, so a raw address can be reported as object #id + offset."""

    def __init__(self):
        self.by_id = {}

    def add(self, oid, addr, size, kind, label):
        self.by_id[oid] = dict(id=oid, addr=addr, size=size, kind=kind, label=label, dropped=False)

    def drop(self, oid):
        if oid in self.by_id:
            self.by_id[oid]['dropped'] = True

    def where(self, a):
        if not a:
            return ''
        best = None
        for o in self.by_id.values():
            if o['addr'] <= a < o['addr'] + max(o['size'], 1):
                if best is None or o['size'] < best['size']:
                    best = o
        if not best:
            return ''
        d = a - best['addr']
        k = KIND[best['kind']] if best['kind'] < len(KIND) else '?'
        if best['kind'] == 2:  # bone transforms
            bi, sub = divmod(d, 32)
            sub = {0: '.x', 4: '.y', 8: '.z', 12: '.scale'}.get(sub, f'.q+{sub - 16:x}')
            return f"#{best['id']} bone[{bi}]{sub}"
        tail = '' if best['dropped'] else ''
        return f"#{best['id']} {k} {best['label'][:34]}+0x{d:x}{tail}"


def f32(u32):
    return struct.unpack('<f', struct.pack('<I', u32 & 0xffffffff))[0]


def as_float(u64):
    """Two floats packed in a 64-bit word, shown when they look like plausible world coordinates."""
    a, b = f32(u64 & 0xffffffff), f32(u64 >> 32)
    out = []
    for v in (a, b):
        out.append(f'{v:.3f}' if (v == 0 or 1e-6 < abs(v) < 1e7) else '-')
    return ' '.join(out)


def read_w(mm, p, n, fmt):
    r = Rd(mm, p)
    h = {}
    h['seq'] = r.u('<Q')
    h['when'] = r.u('<d')
    h['frame'], h['tid'] = r.u('<II')
    h['slot'], h['kind'] = r.u('<BB')
    h['addr'] = r.u('<Q')
    h['rip'] = r.u('<Q')
    h['gpr'] = list(r.u('<16Q'))
    ns = r.u('<B')
    h['stack'] = list(r.u(f'<{ns}Q')) if ns else []
    nd = r.u('<B')
    h['diff'] = [r.u('<IQQ') for _ in range(nd)]
    h['mem'] = bytes(mm[r.p:r.p + 32])
    r.p += 32
    # format 2 tail: instruction-level nature, the XMM file, raw stack bytes, memory behind each pointer register, total diff words
    h['access'] = None
    h['xmm'] = None
    h['stk'] = b''
    h['regmem'] = {}
    h['diffTotal'] = len(h['diff'])
    if fmt >= 2:
        h['access'] = r.u('<B')
        xmm_ok = r.u('<B')
        if xmm_ok:
            h['xmm'] = bytes(mm[r.p:r.p + 256]); r.p += 256
        sn = r.u('<H')
        h['stk'] = bytes(mm[r.p:r.p + sn]); r.p += sn
        reg_mask = r.u('<H')
        for i in range(16):
            if (reg_mask >> i) & 1:
                rl = r.u('<B')
                h['regmem'][i] = bytes(mm[r.p:r.p + rl]); r.p += rl
        h['diffTotal'] = r.u('<H')
    return h


def read_p(mm, p, n):
    """A per-frame page diff: writes a page took that the live watchpoints did not individually record."""
    r = Rd(mm, p)
    h = {}
    h['frame'] = r.u('<I')
    h['page'] = r.u('<Q')
    h['objId'] = r.u('<I')
    h['why'] = r.u('<B')      # bit0 saturated, bit1 dirty passthrough, bit2 access page
    h['exc'] = r.u('<I')
    h['total'] = r.u('<I')
    nd = r.u('<I')
    h['diff'] = [r.u('<IQQ') for _ in range(nd)]
    return h


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file', nargs='?')
    ap.add_argument('--frames', default='')
    ap.add_argument('--slot', type=int, default=-1)
    ap.add_argument('--kind', default='')
    ap.add_argument('--rip', default='')
    ap.add_argument('--max', type=int, default=120)
    ap.add_argument('--stack', type=int, default=12)
    ap.add_argument('--summary', action='store_true')
    ap.add_argument('--xmm', action='store_true', help='show the XMM register file per hit')
    ap.add_argument('--wide', action='store_true', help='also show raw stack bytes per hit')
    a = ap.parse_args()

    path = a.file
    if not path:
        logs = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'bin', 'logs')
        files = sorted(glob.glob(os.path.join(logs, 'insight_*.bin')), key=os.path.getmtime)
        if not files:
            sys.exit(f'no insight_*.bin in {logs}')
        path = files[-1]
    lo, hi = 0, 1 << 30
    if a.frames:
        x, _, y = a.frames.partition('-')
        lo, hi = int(x), int(y or x)
    want_kind = {'exec': 0, 'write': 1, 'page': 2, 'access': 3, 'pgread': 4, 'pageread': 4}.get(a.kind.lower(), -1)

    ins = Insight(path)
    mm = ins.mm
    syms, objs = Syms(), Objs()
    marks = {}
    state = {}  # frame -> (tick, paused)
    hits = []
    pages = []  # 'P' per-frame page diffs
    labels = {}  # (kind, slot) -> label from the WATCH arm / page marker
    for tag, p, n in records(mm, ins.start):
        if tag == 'L':
            r = Rd(mm, p)
            base, size = r.u('<QI')
            syms.add(base, size, r.s())
        elif tag == 'O':
            r = Rd(mm, p)
            oid, kind, flags, addr, vt, size, cls, root, depth = r.u('<IBBQQIIBB')
            objs.add(oid, addr, size, kind, r.s())
        elif tag == 'X':
            objs.drop(struct.unpack_from('<I', mm, p)[0])
        elif tag == 'E':
            v = EFMT.unpack_from(mm, p)
            state[v[1]] = (v[5], v[4], v[3])
        elif tag == 'M':
            r = Rd(mm, p)
            wall, frame = r.u('<dI')
            text = r.s()
            if not text.startswith('CENSUS'):
                marks.setdefault(frame, []).append(text)
        elif tag == 'W':
            h = read_w(mm, p, n, ins.format)
            if lo <= h['frame'] <= hi and (a.slot < 0 or h['slot'] == a.slot) and (want_kind < 0 or h['kind'] == want_kind):
                hits.append(h)
        elif tag == 'P':
            h = read_p(mm, p, n)
            if lo <= h['frame'] <= hi:
                pages.append(h)

    for frame in sorted(marks):
        for t in marks[frame]:
            if t.startswith(('WATCH', 'FORENSIC', 'BODIES')):
                print(f'{frame:7d}  {t}')

    # ---- page-diff summary: per-frame writes caught by the frame diff rather than a live watchpoint
    if pages:
        by_page = {}
        for h in pages:
            e = by_page.setdefault(h['page'], dict(n=0, words=0, frames=set(), obj=h['objId'], acc=False, read=False))
            e['n'] += 1
            e['words'] += h['total']
            e['frames'].add(h['frame'])
            e['acc'] = e['acc'] or bool(h['why'] & 4)
        print(f"\n== {len(pages)} page-diff records over {len(by_page)} pages "
              f"(frames {min(h['frame'] for h in pages)}-{max(h['frame'] for h in pages)})")
        print(f"{'words':>8} {'recs':>6}  {'page':16} {'frames':13}  where")
        for page, e in sorted(by_page.items(), key=lambda kv: -kv[1]['words'])[:40]:
            fr = sorted(e['frames'])
            span = f'{fr[0]}' if len(fr) == 1 else f'{fr[0]}..{fr[-1]} ({len(fr)})'
            print(f"{e['words']:8d} {e['n']:6d}  {page:016x} {span:13}  {objs.where(page)}{'  [access]' if e['acc'] else ''}")

    if not hits:
        if not pages:
            print(f'\n{os.path.basename(path)}: no watchpoint hits recorded'
                  f'{"" if not a.frames else " in frames " + a.frames}.'
                  '\nArm them first:  ragdollfix insight rec 1 <label>  then  ragdollfix forensic')
        return

    # ---- summary: distinct writers
    by_rip = {}
    for h in hits:
        key = (h['kind'], h['slot'], h['rip'])
        e = by_rip.setdefault(key, dict(n=0, first=h, frames=set()))
        e['n'] += 1
        e['frames'].add(h['frame'])
    print(f'\n== {len(hits)} hits from {len(by_rip)} distinct instructions '
          f"(frames {min(h['frame'] for h in hits)}-{max(h['frame'] for h in hits)})")
    print(f"{'hits':>7}  {'kind':5} slot  {'instruction':44}  frames        watched address")
    for (kind, slot, rip), e in sorted(by_rip.items(), key=lambda kv: -kv[1]['n']):
        fr = sorted(e['frames'])
        span = f'{fr[0]}' if len(fr) == 1 else f'{fr[0]}..{fr[-1]} ({len(fr)})'
        print(f"{e['n']:7d}  {KINDS.get(kind, kind):5} {slot:^4}  {syms.name(rip):44}  {span:13} "
              f"{e['first']['addr']:016x} {objs.where(e['first']['addr'])}")
    if a.summary:
        return

    # ---- detail
    print(f'\n== hit detail (first {a.max})')
    shown = 0
    last_frame = None
    rip_filter = a.rip.lower()
    for h in hits:
        if rip_filter and rip_filter not in syms.name(h['rip']).lower():
            continue
        if shown >= a.max:
            print(f'   ... {len(hits) - shown} more hits, raise --max')
            break
        shown += 1
        if h['frame'] != last_frame:
            last_frame = h['frame']
            for t in marks.get(h['frame'], []):
                print(f"\n  ---- f{h['frame']} {t}")
            tick, paused, playing = state.get(h['frame'], (-1, 0, 0))
            print(f"\n-- frame {h['frame']}  demo tick {tick} {'PAUSED' if paused else 'play' if playing else 'stopped'}")
        kind = KINDS.get(h['kind'], h['kind'])
        after = '  (rip is the NEXT instruction: data breakpoints are traps)' if h['kind'] == 1 else ''
        nat = ''
        if h['access'] is not None:
            base = h['access'] & 0x7f
            nat = ' ' + {0: 'read', 1: 'write', 3: 'read/write', 8: 'execute'}.get(base, f'acc0x{base:x}')
            if h['access'] & 0x80:
                nat += '+earlier-writes'
        print(f"  #{h['seq']} {kind}{nat} slot{h['slot']} tid {h['tid']}  at {h['addr']:016x} {objs.where(h['addr'])}")
        print(f"     rip  {syms.name(h['rip'])}{after}")
        if h['kind'] == 0:
            print('     args ' + '  '.join(f"{nm}={h['gpr'][i]:x} ({h['gpr'][i] & 0xffffffff}){(' ' + objs.where(h['gpr'][i])) if objs.where(h['gpr'][i]) else ''}"
                                           for i, nm in ARGS))
        else:
            print('     regs ' + '  '.join(f"{GPR[i]}={h['gpr'][i]:x}" for i in (0, 1, 2, 8, 9, 10) if h['gpr'][i]))
        # memory behind each pointer register that pointed at readable memory (the vector being moved shows up here)
        for i in sorted(h['regmem']):
            b = h['regmem'][i]
            where = objs.where(h['gpr'][i])
            print(f"     [{GPR[i]}] {vec_floats(b, 8)}{('  ' + where) if where else ''}")
        if h['diff']:
            for off, old, new in h['diff']:
                print(f"     page +0x{off:03x}  {old:016x} -> {new:016x}   [{as_float(old)}] -> [{as_float(new)}]")
            if h['diffTotal'] > len(h['diff']):
                print(f"     ... {h['diffTotal']} words changed on this page this frame ({len(h['diff'])} shown)")
        mem = h['mem']
        words = struct.unpack('<8I', mem)
        print('     mem  ' + ' '.join(f'{w:08x}' for w in words[:4]) + '   floats ' + ' '.join(f'{f32(w):.3f}' for w in words[:4]))
        if a.xmm and h['xmm']:
            for i in range(16):
                lane = vec_floats(h['xmm'][i * 16:i * 16 + 16], 4)
                if any(c not in '0. -' for c in lane):  # skip all-zero registers
                    print(f'     xmm{i:<2} {lane}')
        if a.wide and h['stk']:
            sw = struct.unpack_from(f'<{len(h["stk"]) // 4}I', h['stk'])
            print('     stk  ' + ' '.join(f'{w:08x}' for w in sw[:16]))
        for fr in h['stack'][:a.stack]:
            print(f'       {syms.name(fr)}')


if __name__ == '__main__':
    main()
