import sys,re; sys.path.insert(0,'.')
from s2re import Img, fast_refs
C=r'C:/Program Files (x86)/Steam/steamapps/common/Counter-Strike Global Offensive/game/csgo/bin/win64/client.dll'
img=Img(C); d=bytes(img.data); B=img.base
names=[b"EventClientSimulate_t",b"EventClientAdvanceNonRenderedFrame_t",b'EventClientAdvanceTick_t',b'EventClientPostAdvanceTick_t',b'EventClientPreSimulate_t',b'EventClientPostSimulate_t',b'EventFrameBoundary_t',b'EventPostDataUpdate_t',b'EventServerPostAdvanceTick_t',b'EventServerAdvanceTick_t',b'EventAdvanceTick_t',b'EventPostAdvanceTick_t']
for n in names:
    for m in re.finditer(re.escape(n)+b'\x00', d):
        s=m.start()
        if d[s-1]!=0 and d[s-1:s]!=b' ': pass
        rs=fast_refs(img,s)
        print(n.decode(),hex(s),'refs',len(rs))
        for r in rs[:6]:
            f=img.func_of(r)
            if not f: print('   ',hex(r),'nofunc'); continue
            calls=[]
            for i in img.disasm(f[0],f[1]-f[0]):
                mm=re.match(r'qword ptr \[r\w+ \+ (0x[0-9a-f]+)\]',i.op_str)
                if i.mnemonic in('call','jmp') and mm: calls.append(int(mm.group(1),16)//8)
            print('   ref',hex(r),'func',hex(f[0]),'vslots',calls[:12])
