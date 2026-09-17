"""Find how AfxHookSource2.dll builds + registers a CS2 ConCommand (CCmd layout, ICvar vtable index).

usage: cmdre.py <dll> <string> [string...]
For each string: find RIP-relative xrefs in .text and disassemble around them.
"""
import sys, struct
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

path = sys.argv[1]
pe = pefile.PE(path, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
data = pe.__data__
text = next(s for s in pe.sections if s.Name.startswith(b".text"))
tva, traw, tsize = text.VirtualAddress, text.PointerToRawData, text.SizeOfRawData
tbytes = data[traw:traw + tsize]
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = False


def off2rva(off):
    for s in pe.sections:
        if s.PointerToRawData <= off < s.PointerToRawData + s.SizeOfRawData:
            return s.VirtualAddress + off - s.PointerToRawData
    return None


def rva2off(rva):
    return pe.get_offset_from_rva(rva)


def xrefs(target_rva):
    out = []
    for i in range(len(tbytes) - 7):
        # lea/mov with rip disp32: disp at i+3 for REX-prefixed 7-byte insns
        d = struct.unpack_from("<i", tbytes, i)[0]
        insn_end = tva + i + 4
        if insn_end + d == target_rva:
            out.append(tva + i)
    return out


def dis(rva, before=0x60, after=0x80):
    start = rva - before
    off = rva2off(start)
    code = data[off:off + before + after]
    for ins in md.disasm(code, base + start):
        mark = ">>" if ins.address <= base + rva < ins.address + ins.size else "  "
        print(f"{mark} {ins.address - base:08x}: {ins.mnemonic} {ins.op_str}")


for s in sys.argv[2:]:
    needle = s.encode() + b"\0"
    pos = data.find(b"\0" + needle)
    while pos != -1:
        srva = off2rva(pos + 1)
        print(f"=== string {s!r} at rva {srva:08x}")
        for x in xrefs(srva):
            print(f"--- xref disp at {x:08x}")
            dis(x, 0x40, 0x70)
        pos = data.find(b"\0" + needle, pos + 1)
