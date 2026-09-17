import struct, sys
d = open(sys.argv[1], 'rb').read()
n, rva = struct.unpack_from('<II', d, 8)
streams = {}
for i in range(n):
    t, sz, r = struct.unpack_from('<III', d, rva + 12*i)
    streams.setdefault(t, (sz, r))
def mstr(r):
    l = struct.unpack_from('<I', d, r)[0]
    return d[r+4:r+4+l].decode('utf-16le')
mods = []
sz, r = streams[4]
cnt = struct.unpack_from('<I', d, r)[0]
for i in range(cnt):
    o = r + 4 + 108*i
    base, size, ck, ts, nr = struct.unpack_from('<QIIII', d, o)
    mods.append((base, size, ts, mstr(nr).split(chr(92))[-1]))
def sym(a):
    for b, s, ts, nm in mods:
        if b <= a < b + s: return '%s+0x%x' % (nm, a - b)
    return None
sz, r = streams[6]
tid = struct.unpack_from('<I', d, r)[0]
code, flags, rec, addr, npar = struct.unpack_from('<IIQQI', d, r + 8)
pars = struct.unpack_from('<15Q', d, r + 8 + 32)[:npar]
csz, crva = struct.unpack_from('<II', d, r + 8 + 32 + 120)
print('thread %d code %08x addr %x %s params %s' % (tid, code, addr, sym(addr), [hex(p) for p in pars]))
ctx = d[crva:crva+csz]
reg = ['rax','rcx','rdx','rbx','rsp','rbp','rsi','rdi','r8','r9','r10','r11','r12','r13','r14','r15','rip']
vals = struct.unpack_from('<17Q', ctx, 0x78)
for k, v in zip(reg, vals): print('  %s=%x %s' % (k, v, sym(v) or ''))
for b, s, ts, nm in mods:
    if nm.lower() in ('client.dll','engine2.dll','ragdolldemoclock.dll','vphysics2.dll','animationsystem.dll','tier0.dll'):
        print('mod %s base %x stamp %x' % (nm, b, ts))
rsp = vals[4]
sz, r = streams[3]
cnt = struct.unpack_from('<I', d, r)[0]
for i in range(cnt):
    o = r + 4 + 48*i
    t = struct.unpack_from('<I', d, o)[0]
    if t != tid: continue
    start, dsz, drva = struct.unpack_from('<QII', d, o + 24)
    off = rsp - start
    hits = 0
    for p in range(max(0, off), dsz - 8, 8):
        v = struct.unpack_from('<Q', d, drva + p)[0]
        s = sym(v)
        if s and ('.dll' in s) and not s.startswith('ntdll'):
            print('  [rsp+%x] %s' % (p - off, s)); hits += 1
            if hits > 60: break
