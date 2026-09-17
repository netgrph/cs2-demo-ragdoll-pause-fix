import pefile, capstone, struct, sys
from capstone.x86 import *
p = pefile.PE(r"C:/Program Files (x86)/Steam/steamapps/common/Counter-Strike Global Offensive/game/csgo/bin/win64/client.dll", fast_load=True)
img = p.get_memory_mapped_image(); base = p.OPTIONAL_HEADER.ImageBase
# vtable slots of CAnimGraphGameSystem
vt = 0x19caac0
print("CAnimGraphGameSystem vtable")
for i in range(0, 60):
    v = struct.unpack_from('<Q', img, vt + 8*i)[0] - base
    if v <= 0 or v > len(img): break
    b = img[v:v+12].hex()
    print(" slot %2d -> 0x%x  %s" % (i, v, b))
text = [s for s in p.sections if s.Name.startswith(b'.text')][0]
lo, hi = 0x340000, 0x3a0000
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
code = img[lo:hi]
# linear sweep, tolerate garbage
off = 0
hits = []
while off < len(code):
    got = False
    for ins in md.disasm(code[off:off+64], lo + off):
        got = True
        for op in ins.operands:
            if op.type == X86_OP_MEM and op.size == 1 and op.mem.base != 0 and op.mem.index != 0 and op.mem.disp in (0x50, 0x51, 0x52, 0x53) and op.mem.scale == 1:
                hits.append("0x%x %s %s" % (ins.address, ins.mnemonic, ins.op_str))
        off += ins.size
        break
    if not got: off += 1
for h in hits: print(h)
