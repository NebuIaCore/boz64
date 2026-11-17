# CIsAnimStateMachine (Black Ops Zombies, group version 3.7.1), from the game's own Serialise
# (CIsAnimStateMachine__v5 0x02474ed0, CIsAnimState__v5 0x02474858). Records are packed little-endian streams.
#   machine: u32 nstates, nstates x state, then two arrays of 12-byte objects (each serialised by its own class),
#            then u16
#   state:   u32 hash (CIwManaged), u32 anim ref (+0xc), u32 anim ref (+0x10), i32 (+0x14, 0x1000 = 1.0),
#            i32 (+0x18), u16 (+0x1c), u32 (+0x20), u8 (+0x1e), u8 (+0x1f),
#            3 lists (+0x34, +0x44, +0x24) of {u16, i32, i32, i8},
#            list (+0x54) of {u16, u16, u16, i32, bool, bool}
import struct


class R:
    def __init__(self, d, p=0):
        self.d, self.p = d, p

    def take(self, fmt):
        v = struct.unpack_from('<' + fmt, self.d, self.p)
        self.p += struct.calcsize('<' + fmt)
        return v if len(v) > 1 else v[0]

    def left(self):
        return len(self.d) - self.p


def read_state(r):
    start = r.p
    s = {'hash': r.take('I'), 'anim': r.take('I'), 'anim2': r.take('I'), 'speed': r.take('i'), 'x18': r.take('i'),
         'x1c': r.take('H'), 'x20': r.take('I'), 'x1e': r.take('B'), 'x1f': r.take('B')}
    for key in ('l34', 'l44', 'l24'):
        n = r.take('I')
        assert n < 1000, (key, n, hex(r.p))
        s[key] = [r.take('HiiB') for _ in range(n)]
    n = r.take('I')
    assert n < 1000, ('l54', n, hex(r.p))
    s['l54_offset'] = r.p  # the transitions: 12 bytes each, (target state index, blend time (4096 = 1 s), event
    #                        index counted from the end of the machine's event table, i32 parameter, bool, bool)
    s['l54'] = [r.take('HHHi??') for _ in range(n)]
    s['offset'] = start  # where the state starts: anim ref at +4, anim2 +8, speed +12, length (ms) +16
    return s


def read_states(body):
    r = R(body)
    n = r.take('I')
    states = [read_state(r) for _ in range(n)]
    return states, r
