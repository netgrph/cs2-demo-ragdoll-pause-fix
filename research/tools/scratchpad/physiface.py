import sys, os, re, struct
sys.path.insert(0, os.path.dirname(__file__))
from s2re import Img, fast_refs
g = r"C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive\game"
v = Img(g + r"\bin\win64\vphysics2.dll")
names = sorted(set(m.group().decode() for m in re.finditer(rb'[A-Za-z0-9_]{4,}_\d{3}\x00', bytes(v.data))))
print("vphysics2 iface-like strings:", names)
c = Img(g + r"\csgo\bin\win64\client.dll")
va = struct.pack('<Q', c.base + 0x25f1318)
for sec in ('.rdata', '.data'):
    for h in c.find_all(va, sec):
        print(f"abs ptr to 0x25f1318 at {sec} {h:#x}")
        for k in (-16, -8, 8, 16):
            p = struct.unpack_from('<Q', c.data, h + k)[0] - c.base
            if 0 < p < len(c.data):
                e = c.data.find(b'\0', p, p + 80)
                s_ = bytes(c.data[p:e])
                if len(s_) > 3 and all(32 <= b < 127 for b in s_): print(f"   [{k:+d}] -> '{s_.decode()}'")
for n in names:
    for h in c.find_all(n.encode()):
        refs = fast_refs(c, h)
        print(f"client has '{n}' @ {h:#x}, code refs {[hex(r) for r in refs[:6]]}")
        for r in refs[:6]:
            for ins in c.disasm(r - 3, 0x30)[:6]:
                print(f"     {ins.address - c.base:#x} {ins.mnemonic} {ins.op_str}")
