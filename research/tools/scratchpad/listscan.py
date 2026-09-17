import sys, re
sys.argv=[sys.argv[0]]+sys.argv[1:]
from s2re import Img
img = Img(sys.argv[1])
lo, hi = img.sec_range('.text')
d = bytes(img.data)
def hits(v):
    pat = v.to_bytes(4,'little')
    out=set(); i=d.find(pat, lo, hi)
    while i!=-1:
        m = d[i-1]
        if (m>>6)==2: 
            f=img.func_of(i)
            if f: out.add(f)
        i=d.find(pat,i+1,hi)
    return out
a=hits(0x218); b=hits(0x260); c=hits(0x2a8)
fs = sorted((a&b&c) | (a&b) )
print(len(fs),'funcs')
for f in fs:
    ins = img.disasm(f[0], min(f[1]-f[0], 0x3000))
    slots=[]; calls=[]
    for x in ins:
        if x.mnemonic=='call' or x.mnemonic=='jmp':
            m=re.match(r'qword ptr \[r\w+ \+ (0x[0-9a-f]+)\]', x.op_str)
            if m: slots.append(int(m.group(1),16)//8)
            elif x.op_str.startswith('0x'): calls.append(hex(int(x.op_str,16)-img.base))
    print(hex(f[0]), hex(f[1]-f[0]), 'in3' if f in c else '   ', 'vslots', slots[:20], 'calls', calls[:12])
