import pefile, capstone, struct, re
from capstone.x86 import *
p = pefile.PE(r"C:/Program Files (x86)/Steam/steamapps/common/Counter-Strike Global Offensive/game/csgo/bin/win64/client.dll", fast_load=True)
img = p.get_memory_mapped_image()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
def cstr(a):
    if a <= 0 or a >= len(img): return None
    m = re.match(rb'[\x20-\x7e]{4,80}\x00', img[a:a+81])
    return m.group(0)[:-1].decode() if m else None
def dis(a, n, stop_ret=True):
    out = []
    for ins in md.disasm(img[a:a+0x400], a):
        s = '  0x%08x  %-8s %s' % (ins.address, ins.mnemonic, ins.op_str)
        for op in ins.operands:
            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                t = ins.address + ins.size + op.mem.disp
                st = cstr(t)
                s += '   ; ->0x%x%s' % (t, (' "%s"' % st) if st else '')
        out.append(s)
        if len(out) >= n or (stop_ret and ins.mnemonic in ('ret', 'int3')): break
    return out
for a in (0x347530, 0x347698, 0x3476a4, 0x37afa0):
    print('#### 0x%x' % a); print('\n'.join(dis(a, 40)))
# find event name strings
text = img
names = [b'ClientPauseSimulate', b'ClientFrameSimulate', b'ClientAdvanceTick', b'ClientPostAdvanceTick', b'ClientPreEntityThink', b'ClientUpdate', b'ClientPostDataUpdate', b'ClientPreRender', b'ClientGamePostSimulate', b'ClientAdvanceNonRenderedFrame', b'GameFrameBoundary', b'ClientPostRender', b'ClientSimulate', b'ClientPostSimulate']
for nm in names:
    for m in re.finditer(rb'(?<=[\x00])' + nm + rb'(?=[\x00])', img):
        print('str %s at 0x%x' % (nm.decode(), m.start()))
