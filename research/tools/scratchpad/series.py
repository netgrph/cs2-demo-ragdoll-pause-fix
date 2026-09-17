"""Time series + focused change dump for insight recordings.
  series.py FILE --t bones:0x8:f --t scenenode:0x18:f [--frames a-b] [--every-event] [--dump a-b] [--match pawn,bones,scene,anim,Ragdoll]
Selectors: numeric id, or kind name (pawn/bones/scenenode/animctrl), or label substring; resolves to newest live object."""
import argparse, struct, sys, os
import os; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from insight import Insight, records, Rd, EFMT, EV, KIND, fmt_val

ap = argparse.ArgumentParser()
ap.add_argument('file')
ap.add_argument('--t', action='append', default=[])
ap.add_argument('--frames', default='0-99999999')
ap.add_argument('--every-event', action='store_true')
ap.add_argument('--dump', action='append', default=[])
ap.add_argument('--match', default='pawn #,bone transforms,scene node,animation controller,CPhysicsRagdoll,CBodyComponent,CSkeletonInstance,posectl,CPhysAggregateInstance,CRagdoll')
ap.add_argument('--skip-aggr', action='store_true')
ap.add_argument('--find', default='')  # lo-hi: int32 values to look for in globals/animctrl objects
ap.add_argument('--find-at', default='')  # comma list of frames
a = ap.parse_args()
a.find_at = {int(x) for x in a.find_at.split(',') if x}
lo, _, hi = a.frames.partition('-'); lo, hi = int(lo), int(hi)
dumps = []
for d in a.dump:
    x, _, y = d.partition('-'); dumps.append((int(x), int(y or x)))
match = [m for m in a.match.split(',') if m]
if a.skip_aggr: match = [m for m in match if m != 'CPhysAggregateInstance']

ins = Insight(a.file)
tracks = []
for t in a.t:
    sel, off, typ = (t.split(':') + ['f'])[:3]
    tracks.append([sel, int(off, 0) if off[:1].isdigit() else off, typ])

def resolve(sel):
    if sel.isdigit():
        o = ins.objs.get(int(sel)); return o if o and not o['dropped'] else None
    best = None
    for o in ins.objs.values():
        if o['dropped']: continue
        kn = KIND[o['kind']] if o['kind'] < len(KIND) else ''
        if kn == sel or sel in o['label'] or sel in ins.classes.get(o['cls'], ''):
            if best is None or o['id'] > best['id']: best = o
    return best

def val(o, off, typ):
    if o is None: return None
    if isinstance(off, str):
        ent = ins.fields.get(o['cls'])
        if not ent or off not in [n for n, _ in ent[1]]: return None
        off = ent[0][[n for n, _ in ent[1]].index(off)]
    if typ == 'f': return round(struct.unpack_from('<f', o['mem'], off)[0], 3)
    if typ == 'q': return hex(struct.unpack_from('<Q', o['mem'], off)[0])
    return struct.unpack_from('<I', o['mem'], off)[0]

print('frame,ev,tick,paused,mode,T,P,dt,steps,' + ','.join(t[0] + ':' + (hex(t[1]) if isinstance(t[1], int) else t[1]) for t in tracks))
last = None
ev = None
def emit():
    global last
    if ev is None or not (lo <= ev['frame'] <= hi): return
    vals = []
    for sel, off, typ in tracks:
        o = resolve(sel)
        vals.append((o['id'] if o else None, val(o, off, typ)))
    if a.every_event or vals != last:
        print(f"{ev['frame']},{EV.get(ev['ev'])},{ev['tick']},{ev['paused']},{ev['mode']},{ev['T']:.2f},{ev['P']:.2f},{ev['dt']:.4f},{ev['steps']},L{ev['last']},"
              + ','.join(f'#{i}={v}' for i, v in vals))
        last = vals

def indump(f): return any(x <= f <= y for x, y in dumps)
mm = ins.mm
for tag, p, n in records(mm, ins.start):
    if tag == 'E':
        if ev is not None and tracks: emit()
        if a.find and ev is not None and ev['frame'] in a.find_at:
            flo, fhi = map(int, a.find.split('-'))
            for o in ins.objs.values():
                if o['dropped'] or o['kind'] not in (0, 5): continue
                hits = [hex(off) for off in range(0, len(o['mem']) - 3, 4) if flo <= struct.unpack_from('<i', o['mem'], off)[0] <= fhi]
                if hits: print(f"?? f{ev['frame']} {EV.get(ev['ev'])} #{o['id']} kind {o['kind']} {o['label'][:30]}: " + ' '.join(f"{h}={struct.unpack_from('<i', o['mem'], int(h, 16))[0]}" for h in hits[:12]))
        v = EFMT.unpack_from(mm, p)
        ev = dict(ev=v[0], frame=v[1], playing=v[3], paused=v[4], tick=v[5], curtime=v[7], c=v[8:11], haveC=v[11], steps=v[12], last=v[13], mode=v[14], dt=v[15], T=v[17], P=v[18])
        if indump(ev['frame']):
            c = ' '.join(f'{x:.1f}' for x in ev['c']) if ev['haveC'] else '-'
            print(f"## f{ev['frame']} {EV.get(ev['ev'])} tick {ev['tick']} {'PAUSED' if ev['paused'] else 'play'} curtime {ev['curtime']:.3f} T {ev['T']:.2f} P {ev['P']:.2f} dt {ev['dt']:.4f} steps {ev['steps']} centroid {c}")
    elif tag == 'D':
        oid, cnt = struct.unpack_from('<II', mm, p)
        o = ins.objs.get(oid)
        if not o: continue
        pairs = struct.unpack_from(f'<{2 * cnt}I', mm, p + 8)
        show = ev is not None and indump(ev['frame']) and ('*' in match or any(m in o['label'] or m in ins.classes.get(o['cls'], '') for m in match))
        lines = []; bz = []
        for k in range(0, 2 * cnt, 2):
            off, new = pairs[k], pairs[k + 1]
            old = struct.unpack_from('<I', o['mem'], off)[0]
            struct.pack_into('<I', o['mem'], off, new)
            if show:
                if o['kind'] == 2:
                    bi, sub = divmod(off, 32)
                    if sub == 8:
                        fo, fn = struct.unpack('<ff', struct.pack('<II', old, new)); bz.append((bi, fo, fn))
                elif len(lines) < 24:
                    lines.append(f'{ins.field(o, off)} {fmt_val(old)} -> {fmt_val(new)}')
        if show:
            if o['kind'] == 2:
                s = ' '.join(f'b{bi}:{fo:.1f}>{fn:.1f}' for bi, fo, fn in bz[:12])
                print(f"   #{oid} {o['label'][:40]}: {cnt} words, z: {s}")
            else:
                print(f"   #{oid} {o['label'][:50]}: {cnt} words | " + ' | '.join(lines))
    elif tag == 'O':
        r = Rd(mm, p)
        oid, kind, flags, addr, vt, size, cls, root, depth = r.u('<IBBQQIIBB')
        cls = struct.unpack('<i', struct.pack('<I', cls))[0]
        ins.objs[oid] = dict(id=oid, kind=kind, addr=addr, size=size, cls=cls, label=r.s(), mem=bytearray(size), dropped=None)
    elif tag == 'S':
        oid = struct.unpack_from('<I', mm, p)[0]
        if oid in ins.objs: ins.objs[oid]['mem'][:] = mm[p + 4:p + n]
    elif tag == 'X':
        oid, why = struct.unpack_from('<IB', mm, p)
        if oid in ins.objs: ins.objs[oid]['dropped'] = True
    elif tag == 'K':
        r = Rd(mm, p); cls = r.u('<I'); raw, pretty, module = r.s(), r.s(), r.s()
        ins.classes[cls] = f'{module}!{pretty}'
    elif tag == 'F':
        r = Rd(mm, p); cls, cnt = r.u('<II'); ent = []
        for _ in range(cnt):
            off = r.u('<I'); ent.append((off, r.s(), r.s(), r.s()))
        ent.sort(); ins.fields[cls] = ([e[0] for e in ent], [(e[1], e[2]) for e in ent])
    elif tag == 'M':
        r = Rd(mm, p); wall, frame = r.u('<dI'); text = r.s()
        if lo <= frame <= hi and not text.startswith('CENSUS'):
            print(f'!! f{frame} {text[:160]}')
