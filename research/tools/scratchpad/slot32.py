import pefile, capstone, struct, re
p = pefile.PE(r"C:/Program Files (x86)/Steam/steamapps/common/Counter-Strike Global Offensive/game/csgo/bin/win64/client.dll", fast_load=True)
img = p.get_memory_mapped_image()
t = [s for s in p.sections if s.Name.startswith(b'.text')][0]
lo, hi = t.VirtualAddress, t.VirtualAddress + t.Misc_VirtualSize
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
code = bytes(img[lo:hi])
for slot in (27, 31, 32):
    pat = b'\x48\x8b\x01\xff\xa0' + struct.pack('<I', slot * 8)
    for m in re.finditer(re.escape(pat), code):
        lam = lo + m.start()
        # find lea r?, [rip+X] referencing lam (48 8D 15 / 4C 8D 05 etc.)
        refs = []
        for mm in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', code):
            a = lo + mm.start()
            disp = struct.unpack_from('<i', code, mm.start() + 3)[0]
            if a + 7 + disp == lam: refs.append(a)
        print('slot %d lambda 0x%x refs %s' % (slot, lam, [hex(r) for r in refs]))
        for r in refs[:4]:
            print('  --- context 0x%x' % r)
            for ins in md.disasm(code[r - lo - 0x60: r - lo + 0x30], r - 0x60):
                print('    0x%08x  %-8s %s' % (ins.address, ins.mnemonic, ins.op_str))
