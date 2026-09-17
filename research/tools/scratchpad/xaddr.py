import pefile, sys, struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RIP
p = pefile.PE(sys.argv[1], fast_load=True)
img = bytes(p.get_memory_mapped_image())
text = [s for s in p.sections if s.Name.startswith(b'.text')][0]
t0, t1 = text.VirtualAddress, text.VirtualAddress + text.Misc_VirtualSize
tb = img[t0:t1]
md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True
targets = [int(a, 16) for a in sys.argv[2:]]
for tgt in targets:
    out = []
    lo, hi = tgt - t1 - 0x10, tgt - t0 + 0x10
    for k in range(0, len(tb) - 4):
        d = struct.unpack_from('<i', tb, k)[0]
        rel = tgt - (t0 + k)
        if not (0 <= rel - d <= 12): continue
        for back in range(1, 8):
            a = t0 + k - back
            ins = next(md.disasm(img[a:a+16], a), None)
            if not ins: continue
            ok = False
            for op in ins.operands:
                if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP and ins.address + ins.size + op.mem.disp == tgt:
                    ok = True
            if ok:
                out.append('%x: %s %s' % (a, ins.mnemonic, ins.op_str)); break
    print('== target', hex(tgt), len(out))
    for o in sorted(set(out)): print('  ', o)
