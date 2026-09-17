import sys, re
from s2re import Img
img = Img(sys.argv[1])
known = {0xb85e30:'ADD',0xba13b0:'PURGE',0xb9b710:'NoteChangedWrap',0xb99d80:'NoteChanged'}
for a in sys.argv[2:]:
    rva=int(a,16); f=img.func_of(rva)
    size = (f[1]-f[0]) if f and f[0]==rva else 0x200
    ins = img.disasm(rva, min(size,0x1000))
    calls=[]; vs=[]
    for x in ins:
        if x.mnemonic in('call','jmp'):
            if x.op_str.startswith('0x'):
                t=int(x.op_str,16)-img.base
                if not (rva<=t<rva+size): calls.append(known.get(t,hex(t)))
            else:
                m=re.search(r'\[r\w+ \+ (0x[0-9a-f]+)\]',x.op_str)
                if m: vs.append(int(m.group(1),16)//8)
    print(hex(rva), 'size', hex(size), 'calls', calls[:16], 'vslots', vs[:10])
