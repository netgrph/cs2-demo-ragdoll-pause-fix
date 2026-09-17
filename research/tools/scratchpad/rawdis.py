import sys, struct, pefile, capstone
# usage: rawdis.py <dll> <addr>:<nbytes> ...   linear disasm, stops after nbytes
# ptr mode: rawdis.py <dll> ptr <rva> ...      find qword pointers (vtable slots) + RTTI class name
dll = sys.argv[1]
pe = pefile.PE(dll, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

def rtti_name(vt_rva):
    try:
        col = struct.unpack_from('<Q', img, vt_rva - 8)[0] - base
        sig, off, cdoff, td, chd = struct.unpack_from('<IIIII', img, col)
        if sig != 1:
            return None
        name = img[td + 0x10: td + 0x10 + 200].split(b'\0')[0].decode(errors='replace')
        return name
    except Exception:
        return None

if len(sys.argv) > 2 and sys.argv[2] == 'ptr':
    for a in sys.argv[3:]:
        rva = int(a, 16)
        needle = struct.pack('<Q', base + rva)
        print('##### ptrs to', hex(rva))
        for s in pe.sections:
            nm = s.Name.rstrip(b'\0').decode()
            if nm == '.text':
                continue
            lo = s.VirtualAddress; hi = lo + s.Misc_VirtualSize
            i = img.find(needle, lo, hi)
            while i != -1:
                # walk back to vtable start: previous qword that is the COL pointer
                j = i
                name = None
                for k in range(0, 2000):
                    cand = i - 8 * k
                    q = struct.unpack_from('<Q', img, cand - 8)[0]
                    n = rtti_name(cand)
                    if n:
                        name = n; j = cand; break
                slot = (i - j) // 8 if name else None
                print('  %s @%s vt=%s slot=%s %s' % (nm, hex(i), hex(j), slot, name))
                i = img.find(needle, i + 8, hi)
    sys.exit(0)

for a in sys.argv[2:]:
    addr, n = a.split(':')
    addr = int(addr, 16); n = int(n, 16) if n.startswith('0x') else int(n)
    print('#####', hex(addr))
    for ins in md.disasm(bytes(img[addr:addr + n]), base + addr):
        print('  0x%08x  %-8s %s' % (ins.address - base, ins.mnemonic, ins.op_str))
