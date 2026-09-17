"""(1) absolute pointers to a string (static CCmd objects), dump surrounding qwords;
(2) disassemble every use of the g_pCVar globals to recover ICvar vtable indices.

usage: cmdre2.py <dll> <string_rva_hex> <global_rva_hex> [global_rva_hex...]
"""
import sys, struct
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

path = sys.argv[1]
str_rva = int(sys.argv[2], 16)
globs = [int(g, 16) for g in sys.argv[3:]]
pe = pefile.PE(path, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
data = pe.__data__
md = Cs(CS_ARCH_X86, CS_MODE_64)


def off2rva(off):
    for s in pe.sections:
        if s.PointerToRawData <= off < s.PointerToRawData + s.SizeOfRawData:
            return s.VirtualAddress + off - s.PointerToRawData, s.Name.rstrip(b"\0").decode()
    return None, None


def cstr(rva):
    try:
        off = pe.get_offset_from_rva(rva)
    except Exception:
        return None
    end = data.find(b"\0", off, off + 200)
    if end <= off:
        return None
    s = data[off:end]
    return s.decode() if all(32 <= c < 127 for c in s) else None


print("=== absolute pointers to string")
needle = struct.pack("<Q", base + str_rva)
pos = data.find(needle)
while pos != -1:
    rva, sec = off2rva(pos)
    print(f"--- at rva {rva:08x} ({sec})")
    for k in range(-6, 10):
        o = pos + k * 8
        q = struct.unpack_from("<Q", data, o)[0]
        extra = ""
        if base <= q < base + pe.OPTIONAL_HEADER.SizeOfImage:
            s = cstr(q - base)
            extra = f"  -> rva {q - base:08x}" + (f" {s!r}" if s else "")
        print(f"  {k * 8:+4d}: {q:016x}{extra}")
    pos = data.find(needle, pos + 1)

text = next(s for s in pe.sections if s.Name.startswith(b".text"))
tva, traw, tsize = text.VirtualAddress, text.PointerToRawData, text.SizeOfRawData
tb = data[traw:traw + tsize]
print("=== uses of globals")
for i in range(len(tb) - 4):
    d = struct.unpack_from("<i", tb, i)[0]
    end = tva + i + 4
    if end + d in globs:
        # try instruction starts 3 bytes before disp (REX + op + modrm)
        st = tva + i - 3
        code = data[pe.get_offset_from_rva(st):pe.get_offset_from_rva(st) + 0x60]
        insns = list(md.disasm(code, base + st))
        if not insns or "rip" not in insns[0].op_str:
            continue
        print(f"--- {st:08x}")
        for ins in insns[:14]:
            print(f"   {ins.address - base:08x}: {ins.mnemonic} {ins.op_str}")
