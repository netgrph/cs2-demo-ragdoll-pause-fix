import struct, sys

out = bytearray()
def s(t):
    b = t.encode(); return struct.pack('<H', len(b)) + b
def rec(tag, payload):
    out.extend(tag.encode() + struct.pack('<I', len(payload)) + payload)

out += b'INSIGHT\0' + struct.pack('<I', 1) + s('1.3.0') + s('synthetic')
rec('M', struct.pack('<dI', 1.0, 0) + s('START test'))
rec('K', struct.pack('<I', 0) + s('.?AVC_CSPlayerPawn@@') + s('C_CSPlayerPawn') + s('client.dll') + bytes([1, 1]))
rec('F', struct.pack('<II', 0, 2) + struct.pack('<I', 0x10) + s('m_iHealth') + s('int32') + s('C_BaseEntity')
    + struct.pack('<I', 0x20) + s('m_vecOrigin') + s('Vector') + s('C_BaseEntity'))
rec('O', struct.pack('<IBBQQIIBB', 1, 1, 3, 0x1000, 0x2000, 0x40, 0, 0, 0) + s('pawn #3'))
rec('S', struct.pack('<I', 1) + bytes(0x40))
rec('O', struct.pack('<IBBQQIIBB', 2, 2, 2, 0x5000, 0, 0x100, 0xffffffff, 0, 2) + s('bones'))
rec('S', struct.pack('<I', 2) + bytes(0x100))
tick = 100
for frame in range(12):
    paused = 1 if 4 <= frame < 9 else 0
    if frame == 4: rec('M', struct.pack('<dI', 1.0 + frame / 60, frame) + s(f'PAUSE tick {tick}'))
    if frame == 9: rec('M', struct.pack('<dI', 1.0 + frame / 60, frame) + s(f'UNPAUSE tick {tick}'))
    for ev in (1, 2, 3, 4, 5, 6):
        rec('E', struct.pack('<BIdBBiff3fBQiBfiddi', ev, frame, 1.0 + frame / 60, 1, paused, tick, 0.0, frame / 64, 1.0, 2.0, 30.0 - frame,
                             1, frame * 2, tick, 2, 0.015625, 1, tick, tick - 0.5, 3))
        if ev == 4:
            z = struct.unpack('<I', struct.pack('<f', 30.0 - frame))[0]
            rec('D', struct.pack('<II', 2, 2) + struct.pack('<IIII', 8, z, 0x28, z))
            rec('D', struct.pack('<II', 1, 1) + struct.pack('<II', 0x28, z))
    if not paused: tick += 1
rec('X', struct.pack('<IB', 2, 5))
rec('M', struct.pack('<dI', 2.0, 12) + s('STOP console'))
open(sys.argv[1], 'wb').write(out)
