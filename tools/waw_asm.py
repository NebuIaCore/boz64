# World at War Zombies (iOS) CIsAnimStateMachine records, from the game's own code (kiwi.app/kiwi):
#   CIsAnimStateMachine vtable 0x2ad2e8, entry 5 (Serialise) 0x1a7368; CIsAnimState vtable 0x2a4194, entry 5 0x12a4b0.
# machine: u32 nstates, each state written with its class hash ('CIsAnimState'), then the event names (u32 count +
#          hashes), the callback names (u32 count + hashes), u16.
# state:   u32 class hash, u32 hash, u32 anim ref (+0xc), i32 (+0x10, 0x1000 = 1.0), i32 (+0x14), u16 (+0x18, index),
#          u32 (+0x1c), u8 (+0x50), u8 (+0x51),
#          list +0x20 {u16, i32, u16}, list +0x30 {u16, i32, u16, u8}, list +0x40 {u16, u16, u16, i32, u8, u8} (transitions)
# (Black Ops' layout, tools in asm.py: no class hash per state, a second anim ref, i32/u16/u32 the same, 3 lists of
#  {u16, i32, i32, i8} and one of {u16, u16, u16, i32, bool, bool}, no u8 after the states.)
import struct

STATE_CLASS = 0x0b3d362a  # djb2('CIsAnimState')


def parse(body):
    p = 0

    def take(fmt):
        nonlocal p
        v = struct.unpack_from('<' + fmt, body, p)
        p += struct.calcsize('<' + fmt)
        return v

    states = []
    for _ in range(take('I')[0]):
        cls, h, anim, speed, x14, index, x1c, x50, x51 = take('IIIiiHIBB')
        assert cls == STATE_CLASS, (hex(cls), p)
        s = {'hash': h, 'anim': anim, 'speed': speed, 'x14': x14, 'index': index, 'x1c': x1c, 'x50': x50, 'x51': x51}
        s['events'] = [take('HiH') for _ in range(take('I')[0])]
        s['timed'] = [take('HiHB') for _ in range(take('I')[0])]
        s['transitions'] = [take('HHHiBB') for _ in range(take('I')[0])]
        states.append(s)
    event_names = take('%dI' % take('I')[0])
    callback_names = take('%dI' % take('I')[0])
    tail = take('H')[0]
    assert p == len(body), (p, len(body))
    return {'states': states, 'events': list(event_names), 'callbacks': list(callback_names), 'tail': tail}
