# Edits the entity list in a level's <map>_statics.group.bin (Marmalade resource group).
#
# The level's entities are a list of CIsEntitySpec records:
#   [class hash 0x78b2e3ec][count u32][01 01] then <count> records, each
#   [size u32 (whole record)][name hash u32 (djb2 of the lowercase name)][component count u32]
#   components: [instance hash u32][parent hash u32][class hash u32][size u32] then <size> bytes:
#       (some transforms have a scale field that the LFER size leaves out; the outer size counts it)
#       "LFER" [class hash][size][field count] fields: [class][type hash][1][field hash][1][len][value]
#   [child count u32] then the child records.
# Names, classes and fields are djb2 hashes of lowercase names.
#
# As a library: load(), find(), clone + set_field(), append(), save().
import struct

LIST_CLASS = 0x78b2e3ec  # CIsEntitySpec


def djb2(s):
    h = 5381
    for c in s.lower().encode():
        h = (h * 33 + c) & 0xffffffff
    return h


def u32(d, o):
    return struct.unpack_from('<I', d, o)[0]


class Field:
    def __init__(self, cls, typ, a, fh, b, data):
        self.cls, self.typ, self.a, self.fh, self.b, self.data = cls, typ, a, fh, b, data

    def pack(self):
        return struct.pack('<6I', self.cls, self.typ, self.a, self.fh, self.b, len(self.data)) + self.data


class Comp:
    def __init__(self, pre1, pre2, cls, fields, short=0):  # short: how much the LFER size undercounts (see top)
        self.pre1, self.pre2, self.cls, self.fields, self.short = pre1, pre2, cls, fields, short

    def pack(self):
        body = b''.join(f.pack() for f in self.fields)
        size = 16 + len(body)
        return (struct.pack('<4I', self.pre1, self.pre2, self.cls, size) + b'LFER' +
                struct.pack('<3I', self.cls, size - self.short, len(self.fields)) + body)


class Record:
    def __init__(self, hash_, comps, kids_raw, nkids):
        self.hash, self.comps, self.kids_raw, self.nkids = hash_, comps, kids_raw, nkids

    def pack(self):
        body = b''.join(c.pack() for c in self.comps) + struct.pack('<I', self.nkids) + self.kids_raw
        return struct.pack('<3I', 12 + len(body), self.hash, len(self.comps)) + body

    def comp(self, cls):
        return next(c for c in self.comps if c.cls == cls)

    def field(self, cls, fh):
        return next(f for f in self.comp(cls).fields if f.fh == fh)

    def set(self, cls, fh, data):
        self.field(cls, fh).data = data


def parse_record(d, o):
    size, h, n = u32(d, o), u32(d, o + 4), u32(d, o + 8)
    p = o + 12
    comps = []
    for _ in range(n):
        pre1, pre2, cls, sz = struct.unpack_from('<4I', d, p)
        q = p + 16
        lsz = u32(d, q + 8)
        assert d[q:q + 4] == b'LFER' and u32(d, q + 4) == cls and lsz <= sz, hex(p)
        nf = u32(d, q + 12)
        r = q + 16
        fields = []
        for _ in range(nf):
            fc, t, a, fh, b, ln = struct.unpack_from('<6I', d, r)
            fields.append(Field(fc, t, a, fh, b, d[r + 24:r + 24 + ln]))
            r += 24 + ln
        assert r == q + sz, (hex(p), hex(r), hex(q + sz))
        comps.append(Comp(pre1, pre2, cls, fields, sz - lsz))
        p = q + sz
    nkids = u32(d, p)
    rec = Record(h, comps, d[p + 4:o + size], nkids)
    assert rec.pack() == d[o:o + size], hex(o)  # round trip check
    return rec


class Level:
    def __init__(self, path, data=None):  # from a file, or from data (path is then only a label)
        self.path = path
        self.d = bytearray(open(path, 'rb').read() if data is None else data)
        self.list_off, self.count = self._find_list()
        self.records = []  # (offset, size, name hash)
        o = self.list_off + 10
        for _ in range(self.count):
            self.records.append((o, u32(self.d, o), u32(self.d, o + 4)))
            o += u32(self.d, o)
        self.end = o

    def _find_list(self):
        key = struct.pack('<I', LIST_CLASS)
        i = self.d.find(key)
        while i >= 0:
            cnt = u32(self.d, i + 4)
            if self.d[i + 8:i + 10] == b'\x01\x01' and 0 < cnt < 100000:
                # the records must chain to the end without running off the file
                o = i + 10
                ok = True
                for _ in range(cnt):
                    sz = u32(self.d, o)
                    if sz < 12 or o + sz > len(self.d):
                        ok = False
                        break
                    o += sz
                if ok:
                    return i, cnt
            i = self.d.find(key, i + 1)
        raise ValueError('entity list not found')

    def find(self, name):
        h = djb2(name)
        for o, sz, rh in self.records:
            if rh == h:
                return parse_record(self.d, o)
        raise KeyError(name)

    def has(self, name):
        return any(rh == djb2(name) for _, _, rh in self.records)

    def replace(self, name, rec):
        i = next(k for k, (_, _, rh) in enumerate(self.records) if rh == djb2(name))
        o, sz, _ = self.records[i]
        blob = rec.pack()
        self.d[o:o + sz] = blob
        delta = len(blob) - sz
        self.records[i] = (o, len(blob), rec.hash)
        for k in range(i + 1, len(self.records)):
            ok, szk, hk = self.records[k]
            self.records[k] = (ok + delta, szk, hk)
        self.end += delta

    def replace_index(self, i, rec):
        """replace the i-th record (for entities that share a name)"""
        o, sz, _ = self.records[i]
        blob = rec.pack()
        self.d[o:o + sz] = blob
        delta = len(blob) - sz
        self.records[i] = (o, len(blob), rec.hash)
        for k in range(i + 1, len(self.records)):
            ok, szk, hk = self.records[k]
            self.records[k] = (ok + delta, szk, hk)
        self.end += delta

    def append(self, rec):
        blob = rec.pack()
        self.d[self.end:self.end] = blob
        self.records.append((self.end, len(blob), rec.hash))
        self.end += len(blob)
        self.count += 1
        struct.pack_into('<I', self.d, self.list_off + 4, self.count)

    def save(self, path):
        open(path, 'wb').write(self.d)


# field helpers
def f32s(*v):
    return struct.pack('<%df' % len(v), *v)


def i32(v):
    return struct.pack('<I', v & 0xffffffff)


def cstr(s):
    return s.encode() + b'\0'
