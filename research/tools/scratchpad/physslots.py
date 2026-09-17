"""Find client.dll virtual calls through the VPhysics2 interface global (0x25f1318) and map slot -> call sites/functions.
Also locate the vphysics2 interface implementation vtable and print target RVAs for chosen slots.

usage: physslots.py [slot,slot,...]
"""
import sys, os, re, struct
from collections import defaultdict
sys.path.insert(0, os.path.dirname(__file__))
from s2re import Img, fast_refs

G = r"C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive\game"
want = [int(x) for x in sys.argv[1].split(",")] if len(sys.argv) > 1 else [16, 17, 44, 81, 82, 91, 92, 120, 125]
c = Img(G + r"\csgo\bin\win64\client.dll")
GLOBAL = 0x25f1318
refs = fast_refs(c, GLOBAL)
print(f"{len(refs)} rip refs to g_pPhysics2")
sites = defaultdict(list)
for r in refs:
    # walk from instruction start (disp is at +3 for mov reg,[rip+x] with REX)
    start = r - 3
    ins = c.disasm(start, 0x60)
    if not ins or not ins[0].mnemonic == "mov" or "rip" not in ins[0].op_str:
        continue
    reg = ins[0].op_str.split(",")[0].strip()
    vtreg = None
    for i in ins[1:14]:
        ops = i.op_str
        if vtreg is None and i.mnemonic == "mov" and re.match(rf"\w+, qword ptr \[{reg}\]$", ops):
            vtreg = ops.split(",")[0].strip()
            continue
        if i.mnemonic in ("call", "jmp"):
            m = re.match(rf"qword ptr \[{vtreg} \+ (0x[0-9a-f]+)\]$", ops) if vtreg else None
            m2 = re.match(rf"qword ptr \[{reg}\]$", ops)
            if m:
                slot = int(m.group(1), 16) // 8
                f = c.func_of(i.address - c.base)
                sites[slot].append((i.address - c.base, f[0] if f else 0))
                break
            if m2:
                sites[0].append((i.address - c.base, 0))
                break
for s in sorted(sites):
    fs = sorted(set(f for _, f in sites[s]))
    mark = " <==" if s in want else ""
    print(f"slot {s:3d}: {len(sites[s]):3d} sites in {len(fs)} funcs {[hex(f) for f in fs[:8]]}{mark}")

v = Img(G + r"\bin\win64\vphysics2.dll")
print("\nvphysics2 RTTI classes containing 'Physics':")
data = bytes(v.data)
for m in re.finditer(rb"\.\?AV[A-Za-z0-9_@?$]*Phys[A-Za-z0-9_@?$]*@@\x00", data):
    print("  ", m.group()[:-1].decode())
