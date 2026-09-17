import sys, struct; sys.path.insert(0,'.')
from s2re import Img, fast_refs
C=r'C:/Program Files (x86)/Steam/steamapps/common/Counter-Strike Global Offensive/game/csgo/bin/win64/client.dll'
img=Img(C); d=img.data; B=img.base
def strs_in(f):
    out=[]
    for i in img.disasm(f[0], f[1]-f[0]):
        if 'rip +' in i.op_str and i.mnemonic=='lea':
            import re
            m=re.search(r'rip \+ (0x[0-9a-f]+)',i.op_str)
            t=i.address-B+i.size+int(m.group(1),16)
            s=bytes(d[t:t+60]).split(b'\0')[0]
            if len(s)>4 and all(32<=c<127 for c in s): out.append(s.decode())
    return out
vt=0x1ad8250
print('vtable',hex(vt))
for k in range(40):
    p=struct.unpack_from('<Q',d,vt+8*k)[0]
    if not (B<p<B+len(d)): print(k,'non-ptr',hex(p)); break
    r=p-B; f=img.func_of(r)
    sz=(f[1]-f[0]) if f else -1
    print(k,hex(r),'size',hex(sz), strs_in(f)[:4] if f and sz<0x2000 else '')
for name,t in (('convar',0x23a3358),('inst',0x23a3438)):
    rs=fast_refs(img,t)
    print(name,hex(t),[(hex(a), hex(img.func_of(a)[0]) if img.func_of(a) else None) for a in rs])
