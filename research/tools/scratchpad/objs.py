import sys, struct
import os; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from insight import Insight, records, Rd, EFMT, KIND, ROOT
ins = Insight(sys.argv[1]); mm = ins.mm
lo, hi = int(sys.argv[2]), int(sys.argv[3])
frame = 0
live = {}
for tag, p, n in records(mm, ins.start):
    if tag == 'E':
        frame = EFMT.unpack_from(mm, p)[1]
        if frame > hi: break
    elif tag == 'O':
        r = Rd(mm, p)
        oid, kind, flags, addr, vt, size, cls, root, depth = r.u('<IBBQQIIBB')
        live[oid] = (frame, kind, addr, size, root, depth, r.s())
    elif tag == 'X':
        oid, why = struct.unpack_from('<IB', mm, p) if n >= 5 else (struct.unpack_from('<I', mm, p)[0], 0)
        if oid in live and live[oid][0] >= lo:
            print(f"f{frame} DROP #{oid} why{why} {live[oid][6]}")
        live.pop(oid, None)
for oid, (f, kind, addr, size, root, depth, label) in sorted(live.items()):
    tag = 'NEW ' if f >= lo else '    '
    print(f"{tag}f{f} #{oid} {KIND[kind] if kind < len(KIND) else kind} {addr:x} size {size:#x} root {ROOT[root] if root < len(ROOT) else root} d{depth} {label}")
