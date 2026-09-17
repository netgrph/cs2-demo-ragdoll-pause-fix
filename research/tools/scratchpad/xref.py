import pefile, sys, re, struct
p = pefile.PE(sys.argv[1], fast_load=True)
img = bytes(p.get_memory_mapped_image())
text = [s for s in p.sections if s.Name.startswith(b'.text')][0]
t0, t1 = text.VirtualAddress, text.VirtualAddress + text.Misc_VirtualSize
for name in sys.argv[2:]:
    for m in re.finditer(re.escape(name.encode()) + b'\x00', img):
        rva = m.start()
        if img[rva-1] != 0: continue
        refs = []
        # rip-relative disp32: any 4 bytes at i with i+4+disp == rva, checked at plausible instruction ends
        tgt = rva
        tb = img[t0:t1]
        arr = memoryview(tb)
        for k in range(0, len(tb) - 4):
            d = struct.unpack_from('<i', tb, k)[0]
            if t0 + k + 4 + d == tgt:
                op = tb[k-3:k]
                if op[1] in (0x8d, 0x8b) and op[0] in (0x48, 0x4c):
                    refs.append(hex(t0 + k - 3))
        # also absolute pointers (64-bit) in data
        ab = struct.pack('<Q', p.OPTIONAL_HEADER.ImageBase + rva)
        ptrs = [hex(x.start()) for x in re.finditer(re.escape(ab), img)]
        print(name, 'rva', hex(rva), 'code refs', refs[:20], 'ptrs', ptrs[:10])
