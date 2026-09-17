"""Dump a vtable, disassemble its functions, and follow rdata vtables referenced by lea inside them (1 level).

usage: cmdre3.py <dll> <vtable_rva_hex> <count>
"""
import sys, struct
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

pe = pefile.PE(sys.argv[1], fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
data = pe.__data__
md = Cs(CS_ARCH_X86, CS_MODE_64)
text = next(s for s in pe.sections if s.Name.startswith(b".text"))
rdata = next(s for s in pe.sections if s.Name.startswith(b".rdata"))


def q(rva):
    return struct.unpack_from("<Q", data, pe.get_offset_from_rva(rva))[0]


def in_sec(rva, s):
    return s.VirtualAddress <= rva < s.VirtualAddress + s.Misc_VirtualSize


def dis_fn(rva, maxlen=0x120):
    off = pe.get_offset_from_rva(rva)
    refs = []
    for ins in md.disasm(data[off:off + maxlen], base + rva):
        print(f"      {ins.address - base:08x}: {ins.mnemonic} {ins.op_str}")
        if ins.mnemonic == "lea" and "rip +" in ins.op_str:
            tgt = ins.address + ins.size + int(ins.op_str.split("rip + ")[1].rstrip("]"), 16) - base
            if in_sec(tgt, rdata):
                refs.append(tgt)
        if ins.mnemonic in ("ret", "int3") or ins.mnemonic == "jmp" and "qword" in ins.op_str:
            break
    return refs


def dump_vt(rva, n, depth):
    print(f"=== vtable {rva:08x}")
    for i in range(n):
        f = q(rva + 8 * i)
        if not (base <= f < base + pe.OPTIONAL_HEADER.SizeOfImage) or not in_sec(f - base, text):
            print(f"  [{i}] {f:016x} (not code, stop)")
            break
        print(f"  [{i}] fn {f - base:08x}")
        refs = dis_fn(f - base)
        if depth > 0:
            for r in refs:
                if r != rva:
                    dump_vt(r, 4, depth - 1)


dump_vt(int(sys.argv[2], 16), int(sys.argv[3]), 1)
