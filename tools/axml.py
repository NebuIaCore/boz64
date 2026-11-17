# Small reader/writer for Android binary XML (AndroidManifest.xml inside an APK).
# Only what the packaging needs: decode the string pool, change attribute values, remove elements, encode again.
import struct

RES_STRING_POOL = 0x0001
RES_XML_START_ELEMENT = 0x0102
RES_XML_END_ELEMENT = 0x0103
UTF8_FLAG = 0x100

TYPE_REFERENCE = 0x01
TYPE_STRING = 0x03
TYPE_FLOAT = 0x04
TYPE_INT_DEC = 0x10


def _read_len16(d, o):
    n = struct.unpack_from('<H', d, o)[0]
    o += 2
    if n & 0x8000:
        n = ((n & 0x7fff) << 16) | struct.unpack_from('<H', d, o)[0]
        o += 2
    return n, o


def _read_len8(d, o):
    n = d[o]
    o += 1
    if n & 0x80:
        n = ((n & 0x7f) << 8) | d[o]
        o += 1
    return n, o


class Attr:
    def __init__(self, doc, off):
        self.doc, self.off = doc, off

    def _u32(self, k):
        return struct.unpack_from('<I', self.doc.data, self.off + k)[0]

    @property
    def ns(self):
        v = self._u32(0)
        return None if v == 0xffffffff else self.doc.strings[v]

    @property
    def name(self):
        return self.doc.strings[self._u32(4)]

    @property
    def type(self):
        return self.doc.data[self.off + 15]

    @property
    def data(self):
        return self._u32(16)

    @property
    def value(self):
        raw = self._u32(8)
        if raw != 0xffffffff:
            return self.doc.strings[raw]
        if self.type == TYPE_STRING:
            return self.doc.strings[self.data]
        return None

    def set_string(self, s):
        """Makes this attribute a plain string value (also turns a @string reference into a literal)."""
        idx = self.doc.add_string(s)
        struct.pack_into('<IHBBI', self.doc.data, self.off + 8, idx, 8, 0, TYPE_STRING, idx)


    def set_float(self, f):
        """Makes this attribute a float value."""
        struct.pack_into('<IHBBf', self.doc.data, self.off + 8, 0xffffffff, 8, 0, TYPE_FLOAT, f)

    def set_int(self, n):
        """Makes this attribute an integer value."""
        struct.pack_into('<IHBBi', self.doc.data, self.off + 8, 0xffffffff, 8, 0, TYPE_INT_DEC, n)

    def set_data(self, n):
        """Changes this attribute's value, keeping its type (flags stay hex flags)."""
        struct.pack_into('<I', self.doc.data, self.off + 16, n)


class Element:
    def __init__(self, doc, off):
        self.doc, self.off = doc, off
        self.name = doc.strings[struct.unpack_from('<I', doc.data, off + 20)[0]]
        a_start, a_size, a_count = struct.unpack_from('<HHH', doc.data, off + 24)
        self.attrs = [Attr(doc, off + 16 + a_start + i * a_size) for i in range(a_count)]

    def attr(self, name):
        for a in self.attrs:
            if a.name == name:
                return a
        return None


class AXml:
    def __init__(self, blob):
        d = bytes(blob)
        typ, hsize, size = struct.unpack_from('<HHI', d, 0)
        if typ != 0x0003:
            raise ValueError('not a binary XML file')
        self.file_header = d[:hsize]
        o = hsize
        typ, phsize, psize = struct.unpack_from('<HHI', d, o)
        if typ != RES_STRING_POOL:
            raise ValueError('string pool missing')
        count, styles, flags, s_start, st_start = struct.unpack_from('<IIIII', d, o + 8)
        if styles:
            raise ValueError('styled strings are not supported')
        self.utf8 = bool(flags & UTF8_FLAG)
        self.pool_flags = flags & ~0x1  # strings we append break the sorted order
        offs = struct.unpack_from(f'<{count}I', d, o + phsize)
        base = o + s_start
        self.strings = []
        for so in offs:
            p = base + so
            if self.utf8:
                _, p = _read_len8(d, p)
                n, p = _read_len8(d, p)
                self.strings.append(d[p:p + n].decode('utf-8'))
            else:
                n, p = _read_len16(d, p)
                self.strings.append(d[p:p + 2 * n].decode('utf-16-le'))
        self.data = bytearray(d[o + psize:size])  # everything after the pool
        self._index()

    def _index(self):
        self.elements = []
        p = 0
        while p < len(self.data):
            t, h, s = struct.unpack_from('<HHI', self.data, p)
            if t == RES_XML_START_ELEMENT:
                self.elements.append(Element(self, p))
            p += s

    def remove(self, element):
        """Takes an element and everything inside it out of the document. Elements found before this are
        stale afterwards (find them again)."""
        p, depth = element.off, 0
        while True:
            t, h, s = struct.unpack_from('<HHI', self.data, p)
            depth += {RES_XML_START_ELEMENT: 1, RES_XML_END_ELEMENT: -1}.get(t, 0)
            p += s
            if depth == 0:
                break
        del self.data[element.off:p]
        self._index()

    def add_string(self, s):
        if s in self.strings[self._resmap_len():]:
            return self.strings.index(s, self._resmap_len())
        self.strings.append(s)
        return len(self.strings) - 1

    def _resmap_len(self):
        t, h, s = struct.unpack_from('<HHI', self.data, 0)
        return (s - h) // 4 if t == 0x0180 else 0

    def find(self, name):
        return [e for e in self.elements if e.name == name]

    def _encode_pool(self):
        out, offs = bytearray(), []
        for s in self.strings:
            offs.append(len(out))
            if self.utf8:
                b = s.encode('utf-8')
                for n in (len(s), len(b)):
                    out += bytes([n]) if n < 0x80 else bytes([0x80 | (n >> 8), n & 0xff])
                out += b + b'\0'
            else:
                n = len(s.encode('utf-16-le')) // 2
                out += struct.pack('<H', n) if n < 0x8000 else struct.pack('<HH', 0x8000 | (n >> 16), n & 0xffff)
                out += s.encode('utf-16-le') + b'\0\0'
        while len(out) % 4:
            out += b'\0'
        head = 28 + 4 * len(offs)
        pool = struct.pack('<HHIIIIII', RES_STRING_POOL, 28, head + len(out), len(self.strings), 0,
                           self.pool_flags, head, 0)
        return pool + struct.pack(f'<{len(offs)}I', *offs) + out

    def to_bytes(self):
        body = self._encode_pool() + bytes(self.data)
        hsize = len(self.file_header)
        return self.file_header[:4] + struct.pack('<I', hsize + len(body)) + self.file_header[8:] + body
