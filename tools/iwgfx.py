#!/usr/bin/env python3
"""
iwgfx.py - reader/writer for Marmalade (IwResManager / IwGraphics / IwAnim) resources as
stored in COD Black Ops Zombies (mobile) *.group.bin files (serialise version 3.7.1).

Pure Python 3 (struct + PIL for PNG output).  See iwgfx_README.txt for the format notes.

    python iwgfx.py roundtrip GROUP.bin            # parse + re-serialise every list, assert byte-exact
    python iwgfx.py dump GROUP.bin [hash...]       # list resources (or details of the given hashes)
    python iwgfx.py export GROUP.bin MODELHASH OUTDIR  # OBJ + MTL + PNG textures + preview.png
    python iwgfx.py selftest GROUP.bin [OUTDIR]       # Speed Cola round trip + build/insert a new
                                                      # model in memory, re-parse, export preview

All integers little endian, byte aligned.  "hash" = djb2 of the lower-cased name
(h = 5381; h = h*33 + c), e.g. class names, resource names.
"""
import math
import os
import struct
import sys


def djb2(s):
    h = 5381
    for c in s.lower().encode():
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


CLASS_NAMES = [
    'CIwModel', 'CIwMaterial', 'CIwTexture', 'CIwAnim', 'CIwAnimSkel', 'CIwAnimSkin', 'CIwAnimBone',
    'CIwAnimSkinSet', 'CIwModelBlockVerts', 'CIwModelBlockNorms', 'CIwModelBlockCols',
    'CIwModelBlockGLUVs', 'CIwModelBlockGLUVs2', 'CIwModelBlockGLTriList', 'CIwModelBlockGLTriStrip',
    'CIwModelBlockVerts2', 'CIwModelBlockCols2', 'CIwModelBlockGLTriList2', 'CIwModel2',
    'CIwModelExtSphere', 'CIwModelExtPos', 'CIwModelExtSelSet', 'CIwModelExtSelSetFace',
    'CIwModelExtSelSetVert', 'CIwModelExtSelSetEdge', 'CIwModelBlockTangents', 'CIwModelBlockBiTangents',
    'CIsAnimStateMachine', 'CIsEntitySpec', 'CIwResGroup', 'CIwResList', 'ResGroupMembers',
    'ResGroupResources', 'ResGroupBuildData', 'ResGroupChildPaths', 'ResGroupHST', 'ResGroupDirectory',
    'ResGroupResourcesOptimised', 'CIwGxShaderTechnique', 'CIwSoundSpec', 'CIwSoundData']
CLS = {djb2(n): n for n in CLASS_NAMES}
H = {n: djb2(n) for n in CLASS_NAMES}

# Serialise version this game's groups are written with (header bytes 1..3 = 03 07 01).
VERSION = 0x030701


# ---------------------------------------------------------------- byte streams
class Reader:
    def __init__(self, d, p=0, end=None):
        self.d, self.p = d, p
        self.end = len(d) if end is None else end

    def take(self, n):
        if self.p + n > self.end:
            raise ValueError('read past end at %#x (+%d > %#x)' % (self.p, n, self.end))
        b = self.d[self.p:self.p + n]
        self.p += n
        return bytes(b)

    def _u(self, fmt, n):
        v = struct.unpack_from(fmt, self.d, self.p)[0]
        if self.p + n > self.end:
            raise ValueError('read past end')
        self.p += n
        return v

    def u8(self): return self._u('<B', 1)
    def i8(self): return self._u('<b', 1)
    def u16(self): return self._u('<H', 2)
    def i16(self): return self._u('<h', 2)
    def u32(self): return self._u('<I', 4)
    def i32(self): return self._u('<i', 4)

    def arr(self, fmt, n):
        v = list(struct.unpack_from('<%d%s' % (n, fmt), self.d, self.p))
        self.p += struct.calcsize('<%d%s' % (n, fmt))
        if self.p > self.end:
            raise ValueError('read past end')
        return v

    def cstr(self):
        e = self.d.index(b'\0', self.p)
        s = self.d[self.p:e].decode('latin-1')
        self.p = e + 1
        return s

    def left(self): return self.end - self.p


class Writer:
    def __init__(self):
        self.b = bytearray()

    def raw(self, b): self.b += b
    def u8(self, v): self.b += struct.pack('<B', v & 0xFF)
    def i8(self, v): self.b += struct.pack('<b', v)
    def u16(self, v): self.b += struct.pack('<H', v & 0xFFFF)
    def i16(self, v): self.b += struct.pack('<h', v)
    def u32(self, v): self.b += struct.pack('<I', v & 0xFFFFFFFF)
    def i32(self, v): self.b += struct.pack('<i', v)
    def arr(self, fmt, a): self.b += struct.pack('<%d%s' % (len(a), fmt), *a)
    def cstr(self, s): self.b += s.encode('latin-1') + b'\0'
    def bytes(self): return bytes(self.b)


# ---------------------------------------------------------------- IwSerialise "struct" streams
# A descriptor u16 per field: bits0-3 type, bit4 = no data, bits6+ = component count.
# Type 5 (int16, mean-centred): i16 mean, then component-planar arrays of i16 (value - mean).
# Type 4 (uint8 raw): component-planar arrays of u8.
def read_planar_i16(r, n, comps):
    mean = r.i16()
    planes = [r.arr('h', n) for _ in range(comps)]
    return mean, planes


def write_planar_i16(w, mean, planes):
    w.i16(mean)
    for p in planes:
        w.arr('h', p)


def planar_from_values(vals, comps):
    """vals: list of tuples (int16).  Returns (mean, planes) like the Marmalade tools:
    mean = (min+max)>>1 over all components, stored values = v - mean."""
    flat = [c for v in vals for c in v[:comps]]
    if flat:
        mean = (min(flat) + max(flat)) >> 1
    else:
        mean = 0
    mean = max(-32768, min(32767, mean))
    planes = []
    for k in range(comps):
        pl = []
        for v in vals:
            x = v[k] - mean
            if not -32768 <= x <= 32767:
                raise ValueError('component range too large for int16 after centring')
            pl.append(x)
        planes.append(pl)
    return mean, planes


def wrap16(x):
    x &= 0xFFFF
    return x - 0x10000 if x & 0x8000 else x


# ---------------------------------------------------------------- group container
class ResList:
    """One CIwResList: [class hash][count][all_same_class u8][has_sizes u8] records.
    Record = [size u32 incl. itself][class hash u32 if !all_same][resource hash u32][class data]."""

    def __init__(self, cls, all_same=1, has_sizes=1, records=None):
        self.cls, self.all_same, self.has_sizes = cls, all_same, has_sizes
        self.records = records or []   # list of [cls, hash, body bytes, file offset]

    def find(self, h):
        for rec in self.records:
            if rec[1] == h:
                return rec
        return None


class Group:
    """.group.bin = 4-byte header [?][03][07][01] (version 3.7.1 = bytes 1..3), u16 (only if
    version > 3.1.1), then chunks [chunk-name hash u32][size u32 (incl. size field)][payload],
    terminated by a u32 0.  Chunk ResGroupMembers = cstr group name + u32 flags;
    chunk ResGroupResources = u32 list count + CIwResList[]."""

    def __init__(self, data):
        self.data = data
        r = Reader(data)
        self.head = r.take(4)
        self.version = (self.head[1] << 16) | (self.head[2] << 8) | self.head[3]
        self.head2 = r.u16() if self.version > 0x30101 else None
        self.chunks = []   # [hash, payload bytes or list-of-ResList]
        while True:
            h = r.u32()
            if h == 0:
                break
            p0 = r.p
            size = r.u32()
            payload_end = p0 + size
            if h == H['ResGroupResources']:
                rr = Reader(data, r.p, payload_end)
                lists = []
                for _ in range(rr.u32()):
                    cls = rr.u32()
                    cnt = rr.u32()
                    all_same, has_sizes = rr.u8(), rr.u8()
                    rl = ResList(cls, all_same, has_sizes)
                    if not has_sizes:
                        raise NotImplementedError('list without record sizes')
                    for _ in range(cnt):
                        q = rr.p
                        sz = rr.u32()
                        c = rr.u32() if not all_same else cls
                        hh = rr.u32()
                        body = data[rr.p:q + sz]
                        rl.records.append([c, hh, body, q])
                        rr.p = q + sz
                    lists.append(rl)
                if rr.p != payload_end:
                    raise ValueError('ResGroupResources size mismatch')
                self.chunks.append([h, lists])
            else:
                self.chunks.append([h, data[r.p:payload_end]])
            r.p = payload_end
        self.tail = data[r.p:]

    def lists(self):
        for h, pl in self.chunks:
            if h == H['ResGroupResources']:
                return pl
        return []

    def list_of(self, cls):
        for rl in self.lists():
            if rl.cls == cls:
                return rl
        return None

    def find(self, cls, h):
        rl = self.list_of(cls)
        return rl.find(h) if rl else None

    def add(self, cls, h, body, replace=False):
        """Append (or replace) a record in the list of class `cls`.  Counts and all sizes are
        recomputed by write().  The list must already exist in this group."""
        rl = self.list_of(cls)
        if rl is None:
            raise KeyError('group has no %s list' % CLS.get(cls, '%08x' % cls))
        old = rl.find(h)
        if old is not None:
            if not replace:
                raise KeyError('%08x already in the %s list' % (h, CLS.get(cls)))
            old[2] = body
            return
        rl.records.append([cls, h, body, None])

    def all_hashes(self):
        return {rec[1] for rl in self.lists() for rec in rl.records}

    def name(self):
        for h, pl in self.chunks:
            if h == H['ResGroupMembers']:
                return Reader(pl).cstr()

    def write(self):
        w = Writer()
        w.raw(self.head)
        if self.head2 is not None:
            w.u16(self.head2)
        for h, pl in self.chunks:
            w.u32(h)
            if isinstance(pl, list):
                pw = Writer()
                pw.u32(len(pl))
                for rl in pl:
                    pw.u32(rl.cls)
                    pw.u32(len(rl.records))
                    pw.u8(rl.all_same)
                    pw.u8(rl.has_sizes)
                    for c, hh, body, _ in rl.records:
                        extra = 0 if rl.all_same else 4
                        pw.u32(4 + extra + 4 + len(body))
                        if not rl.all_same:
                            pw.u32(c)
                        pw.u32(hh)
                        pw.raw(body)
                pl = pw.bytes()
            w.u32(4 + len(pl))
            w.raw(pl)
        w.u32(0)
        return w.bytes() + self.tail   # tail = anything after the 0 terminator (empty here)


# ---------------------------------------------------------------- CIwModel
class Block:
    """Common CIwModelBlock header: m_Hash u32 (= class hash), u16 size field,
    u16 element count, u16 (always 0 here)."""
    elem_size = 0

    def __init__(self, cls):
        self.cls = cls
        self.hash = cls
        self.size_field = 0
        self.n = 0
        self.x10 = 0

    def read_head(self, r):
        self.hash = r.u32()
        self.size_field, self.n, self.x10 = r.u16(), r.u16(), r.u16()

    def write_head(self, w):
        w.u32(self.hash)
        w.u16(self.size_field)
        w.u16(self.n)
        w.u16(self.x10)

    def calc_size_field(self):
        # observed: align4(0x1c + n * element size)
        return (0x1C + self.n * self.elem_size + 3) & ~3


class BlockVerts(Block):
    """CIwModelBlockVerts: u16 nStored; type-5 stream (i16 mean, X[nStored], Y[], Z[]) of
    CIwSVec3 positions; then (n - nStored) u16: each extra vertex is a duplicate of unique
    vertex [idx] (model flag 0x20000 set when present)."""
    elem_size = 6

    def read(self, r):
        self.read_head(r)
        ns = r.u16()
        self.mean, self.planes = read_planar_i16(r, ns, 3)
        self.dups = r.arr('H', self.n - ns)

    def write(self, w):
        self.write_head(w)
        w.u16(len(self.planes[0]))
        write_planar_i16(w, self.mean, self.planes)
        w.arr('H', self.dups)

    def unique(self):
        return [tuple(wrap16(self.planes[k][i] + self.mean) for k in range(3))
                for i in range(len(self.planes[0]))]

    def positions(self):
        u = self.unique()
        return u + [u[i] for i in self.dups]

    @classmethod
    def build(cls, verts):
        b = cls(H['CIwModelBlockVerts'])
        b.n = len(verts)
        b.mean, b.planes = planar_from_values(verts, 3)
        b.dups = []
        b.size_field = b.calc_size_field()
        return b


class BlockNorms(Block):
    """CIwModelBlockNorms: type-5 stream (i16 mean + 3 planes) of n CIwSVec3 normals (|n| = 4096)."""
    elem_size = 6

    def read(self, r):
        self.read_head(r)
        self.mean, self.planes = read_planar_i16(r, self.n, 3)

    def write(self, w):
        self.write_head(w)
        write_planar_i16(w, self.mean, self.planes)

    def normals(self):
        return [tuple(wrap16(self.planes[k][i] + self.mean) for k in range(3)) for i in range(self.n)]


class BlockCols(Block):
    """CIwModelBlockCols: u8 mode then
       mode&1 : n x u8 grey (R=G=B=v, A=255)
       mode&4 : (version > 3.5.5) n x [u16 RGB565 (+ u8 A if mode&2)]
       else   : type-4 stream: 4 planes of n x u8 (R[], G[], B[], A[])."""
    elem_size = 4

    def read(self, r):
        self.read_head(r)
        self.mode = r.u8()
        if self.mode & 1:
            self.data = r.arr('B', self.n)
        elif self.mode & 4:
            self.data = []
            for _ in range(self.n):
                c = r.u16()
                a = r.u8() if self.mode & 2 else None
                self.data.append((c, a))
        else:
            self.data = [r.arr('B', self.n) for _ in range(4)]

    def write(self, w):
        self.write_head(w)
        w.u8(self.mode)
        if self.mode & 1:
            w.arr('B', self.data)
        elif self.mode & 4:
            for c, a in self.data:
                w.u16(c)
                if self.mode & 2:
                    w.u8(a)
        else:
            for p in self.data:
                w.arr('B', p)

    def rgba(self):
        if self.mode & 1:
            return [(g, g, g, 255) for g in self.data]
        if self.mode & 4:
            out = []
            for c, a in self.data:
                out.append(((c & 31) << 3, ((c >> 5) & 63) << 2, ((c >> 11) & 31) << 3,
                            255 if a is None else a))
            return out
        return list(zip(*self.data))

    @classmethod
    def build(cls, rgba):
        b = cls(H['CIwModelBlockCols'])
        b.n = len(rgba)
        # planar RGBA; the exporter sets bit 1 when some alpha < 255 (mode 2), else mode 0
        b.mode = 2 if any(c[3] < 255 for c in rgba) else 0
        b.data = [[c[k] for c in rgba] for k in range(4)]
        b.size_field = b.calc_size_field()
        return b


class BlockUVs(Block):
    """CIwModelBlockGLUVs / GLUVs2: n x (i16 u, i16 v), 1.0 = 4096 (v down, GL convention:
    v=0 is the first texel row of the image)."""
    elem_size = 4

    def read(self, r):
        self.read_head(r)
        self.uv = r.arr('h', self.n * 2)

    def write(self, w):
        self.write_head(w)
        w.arr('h', self.uv)

    def uvs(self):
        return [(self.uv[2 * i], self.uv[2 * i + 1]) for i in range(self.n)]

    @classmethod
    def build(cls, uvs, second=False):
        b = cls(H['CIwModelBlockGLUVs2' if second else 'CIwModelBlockGLUVs'])
        b.n = len(uvs)
        b.uv = [c for uv in uvs for c in uv]
        b.size_field = b.calc_size_field()
        return b


class BlockPrim(Block):
    """CIwModelBlockGLTriList (and GLTriStrip, same serialiser = CIwModelBlockGLPrimBase):
       u32 material index into the model's material list (version > 3.4.2; older = material hash)
       u16 indices[n]  (n = 3 * triangles for a list)
       u16 first vertex, u16 end vertex (min index, max index + 1) (version > 3.0.255)."""
    elem_size = 2

    def read(self, r):
        self.read_head(r)
        self.material = r.u32()
        self.indices = r.arr('H', self.n)
        self.vmin, self.vend = r.u16(), r.u16()

    def write(self, w):
        self.write_head(w)
        w.u32(self.material)
        w.arr('H', self.indices)
        w.u16(self.vmin)
        w.u16(self.vend)

    @classmethod
    def build(cls, material_index, indices):
        b = cls(H['CIwModelBlockGLTriList'])
        b.n = len(indices)
        b.material = material_index
        b.indices = list(indices)
        b.vmin, b.vend = min(indices), max(indices) + 1
        b.size_field = b.calc_size_field()
        return b


class ExtSelSetFace:
    """CIwModelExtSelSetFace (model ext list): u32 hash (selection-set name), u32 (+0x10),
    u8 x3, i8, u32 count, count x u16 face index."""

    def __init__(self, cls):
        self.cls = cls

    def read(self, r):
        self.hash, self.x10 = r.u32(), r.u32()
        self.b = r.take(4)
        self.faces = r.arr('H', r.u32())

    def write(self, w):
        w.u32(self.hash)
        w.u32(self.x10)
        w.raw(self.b)
        w.u32(len(self.faces))
        w.arr('H', self.faces)


class ExtPos:
    """CIwModelExtPos (model ext list): u32 hash, u32 (+0x10) (CIwModelExt's part), then a position: 3 x i16 (+0x14)
    (Black Ops' CIwModelExtPos Serialise, vtable 0x02863750 entry 5; World at War's models use it)."""

    def __init__(self, cls):
        self.cls = cls

    def read(self, r):
        self.hash, self.x10 = r.u32(), r.u32()
        self.pos = r.arr('h', 3)

    def write(self, w):
        w.u32(self.hash)
        w.u32(self.x10)
        w.arr('h', self.pos)


EXT_TYPES = {H['CIwModelExtSelSetFace']: ExtSelSetFace, H['CIwModelExtPos']: ExtPos}

BLOCK_TYPES = {
    H['CIwModelBlockVerts']: BlockVerts,
    H['CIwModelBlockNorms']: BlockNorms,
    H['CIwModelBlockCols']: BlockCols,
    H['CIwModelBlockGLUVs']: BlockUVs,
    H['CIwModelBlockGLUVs2']: BlockUVs,
    H['CIwModelBlockGLTriList']: BlockPrim,
    H['CIwModelBlockGLTriStrip']: BlockPrim,
}


class Model:
    """CIwModel (record body after the resource hash):
       u32 flags (0x10000 always set by the exporter; 0x20000 = Verts block has duplicate verts;
                  0x80000 = skip GPU-skin preprocessing)
       u16 numVerts (incl. duplicates), u16 numUniqueVerts
       CIwSphere bounding sphere: i32 x, y, z, i32 radius (model units)
       CIwManagedList blocks: u32 count, count x [u32 class hash][block data]
       CIwManagedList exts:   u32 count, count x [u32 class hash][ext data]
       materials: u32 count, count x u32 CIwMaterial hash"""

    def __init__(self):
        self.hash = 0
        self.flags = 0x10000
        self.nverts = self.nunique = 0
        self.sphere = (0, 0, 0, 0)
        self.blocks = []
        self.exts = []
        self.materials = []

    @classmethod
    def read(cls, h, body):
        m = cls()
        m.hash = h
        r = Reader(body)
        m.flags = r.u32()
        m.nverts, m.nunique = r.u16(), r.u16()
        m.sphere = tuple(r.arr('i', 4))
        for _ in range(r.u32()):
            c = r.u32()
            if c not in BLOCK_TYPES:
                raise NotImplementedError('model %08x: block class %08x %s' % (h, c, CLS.get(c)))
            b = BLOCK_TYPES[c](c)
            b.read(r)
            m.blocks.append(b)
        for _ in range(r.u32()):
            c = r.u32()
            if c not in EXT_TYPES:
                raise NotImplementedError('model %08x: ext class %08x %s' % (h, c, CLS.get(c)))
            e = EXT_TYPES[c](c)
            e.read(r)
            m.exts.append(e)
        m.materials = r.arr('I', r.u32())
        if r.left():
            raise ValueError('model %08x: %d bytes left' % (h, r.left()))
        return m

    def write(self):
        w = Writer()
        w.u32(self.flags)
        w.u16(self.nverts)
        w.u16(self.nunique)
        w.arr('i', self.sphere)
        w.u32(len(self.blocks))
        for b in self.blocks:
            w.u32(b.cls)
            b.write(w)
        w.u32(len(self.exts))
        for e in self.exts:
            w.u32(e.cls)
            e.write(w)
        w.u32(len(self.materials))
        w.arr('I', self.materials)
        return w.bytes()

    def block(self, cls_name, nth=0):
        k = 0
        for b in self.blocks:
            if b.cls == H[cls_name]:
                if k == nth:
                    return b
                k += 1
        return None

    def prims(self):
        return [b for b in self.blocks if isinstance(b, BlockPrim)]

    @classmethod
    def build(cls, h, verts, uvs, tris_by_material, material_hashes, cols=None, normals=None,
              skinned=True):
        """verts: [(x,y,z) int16 model units]; uvs: [(u,v) int16, 4096 = 1.0, v down];
        tris_by_material: [[(i0,i1,i2), ...] per material index]; cols: [(r,g,b,a)] or None
        (defaults to opaque white, like baked-lighting-free); normals: optional [(x,y,z)] |n|=4096.
        Block order follows the exporter: Verts, [Norms], GLUVs, Cols, GLTriList per material."""
        n = len(verts)
        if n > 0x3FFF:
            raise ValueError('skinned models are limited to 16383 vertices (skin index is 14 bits)')
        if len(uvs) != n:
            raise ValueError('need one UV per vertex')
        m = cls()
        m.hash = h
        m.flags = 0x30000 if skinned else 0x20000
        m.nverts = m.nunique = n
        m.sphere = cls.bounding_sphere(verts, margin=1)
        m.blocks.append(BlockVerts.build(verts))
        if normals:
            nb = BlockNorms(H['CIwModelBlockNorms'])
            nb.n = n
            nb.mean, nb.planes = planar_from_values(normals, 3)
            nb.size_field = nb.calc_size_field()
            m.blocks.append(nb)
        m.blocks.append(BlockUVs.build(uvs))
        m.blocks.append(BlockCols.build(cols or [(255, 255, 255, 255)] * n))
        for mi, tris in enumerate(tris_by_material):
            if not tris:
                continue
            idx = [i for t in tris for i in t]
            # keep each block's size field < 64K: split very large lists
            for k in range(0, len(idx), 3 * 10000):
                m.blocks.append(BlockPrim.build(mi, idx[k:k + 3 * 10000]))
        m.materials = list(material_hashes)
        return m

    @staticmethod
    def bounding_sphere(verts, margin=0):
        """What the exporter stored: centre = AABB centre (rounded toward -inf),
        radius = ceil(max distance) from it.  (Checked against the perk machines.)"""
        xs, ys, zs = zip(*verts)
        c = [(min(a) + max(a)) // 2 for a in (xs, ys, zs)]
        r = max(math.sqrt((x - c[0]) ** 2 + (y - c[1]) ** 2 + (z - c[2]) ** 2) for x, y, z in verts)
        return (c[0], c[1], c[2], int(math.ceil(r)) + margin)


# ---------------------------------------------------------------- CIwMaterial
class Material:
    """CIwMaterial (record body after hash):
       u8 compact (1 = everything else default)
       compact: u32 flags, u32 texture0 hash
       full:    u32 flags, i16 zDepthOfs, i16 zDepthOfsHW, u8x4 colEmissive, u8x4 colAmbient,
                u8x4 colDiffuse, u8x4 colSpecular (A = specular power), u32 nTex (4),
                nTex x u32 texture hash (0 = none), u8 hasAnim [+6 bytes anim: u8 at +0,+2,+3,+5,+6,+7],
                u8 (field +0x17), u32 shader technique hash (0 = default)."""

    @classmethod
    def read(cls, h, body):
        m = cls()
        m.hash = h
        r = Reader(body)
        m.compact = r.u8()
        m.flags = r.u32()
        if m.compact:
            m.textures = [r.u32()]
        else:
            m.zofs, m.zofs_hw = r.i16(), r.i16()
            m.cols = [r.take(4) for _ in range(4)]
            m.textures = r.arr('I', r.u32())
            m.has_anim = r.u8()
            m.anim = r.take(6) if m.has_anim else b''
            m.b17 = r.u8()
            m.shader = r.u32()
        if r.left():
            raise ValueError('material %08x: %d bytes left' % (h, r.left()))
        return m

    def write(self):
        w = Writer()
        w.u8(self.compact)
        w.u32(self.flags)
        if self.compact:
            w.u32(self.textures[0])
        else:
            w.i16(self.zofs)
            w.i16(self.zofs_hw)
            for c in self.cols:
                w.raw(c)
            w.u32(len(self.textures))
            w.arr('I', self.textures)
            w.u8(self.has_anim)
            w.raw(self.anim)
            w.u8(self.b17)
            w.u32(self.shader)
        return w.bytes()

    @classmethod
    def build(cls, h, texture_hash, flags=0x00050000):
        m = cls()
        m.hash, m.compact, m.flags, m.textures = h, 1, flags, [texture_hash]
        return m


# ---------------------------------------------------------------- CIwTexture / CIwImage
FMT_NAMES = {1: 'RGB332', 2: 'BGR332', 3: 'RGB565', 4: 'BGR565', 5: 'RGBA4444', 6: 'ABGR4444',
             7: 'RGBA5551', 8: 'ABGR1555', 9: 'RGB888', 10: 'BGR888', 11: 'RGBA6666', 12: 'ABGR6666',
             13: 'RGBA8888', 14: 'ABGR8888', 15: 'RGBA_AAA2', 16: 'ABGR_2AAA', 0x23: 'COMPRESSED(GL)'}
COMPRESSED_FMTS = set(range(0x20, 0x24)) | set(range(0x34, 0x3B)) | {0x27, 0x2B}


class Image:
    """CIwImage: u8 format, u16 flags (bit2 = no texels, bit1 = has palette), u16 width,
    u16 height, u16 pitch (bytes per row), u32 dataSize (version >= 3.4.1; only meaningful for
    compressed formats), texels (compressed: dataSize bytes, else height*pitch bytes),
    palette (if flags&2)."""

    @classmethod
    def read(cls, r):
        im = cls()
        im.fmt = r.u8()
        im.flags = r.u16()
        im.w, im.h, im.pitch = r.u16(), r.u16(), r.u16()
        im.size = r.u32()
        if im.flags & 4:
            im.texels = b''
        else:
            n = im.size if im.fmt in COMPRESSED_FMTS else im.h * im.pitch
            im.texels = r.take(n)
        if im.flags & 2:
            raise NotImplementedError('palettised image')
        return im

    def write(self, w):
        w.u8(self.fmt)
        w.u16(self.flags)
        w.u16(self.w)
        w.u16(self.h)
        w.u16(self.pitch)
        w.u32(self.size)
        w.raw(self.texels)

    def to_rgba(self):
        """Returns (w, h, bytes RGBA) of the top mip level."""
        if self.fmt == 14:     # ABGR8888 = bytes R,G,B,A (GL_RGBA/GL_UNSIGNED_BYTE)
            rows = [self.texels[y * self.pitch:y * self.pitch + self.w * 4] for y in range(self.h)]
            return self.w, self.h, b''.join(rows)
        if self.fmt == 13:     # RGBA8888 = bytes A,B,G,R
            out = bytearray()
            for y in range(self.h):
                row = self.texels[y * self.pitch:y * self.pitch + self.w * 4]
                for x in range(self.w):
                    a, b, g, r = row[4 * x:4 * x + 4]
                    out += bytes((r, g, b, a))
            return self.w, self.h, bytes(out)
        if self.fmt == 0x23:
            hdr = gl_blob_header(self.texels)
            if hdr['glfmt'] != 0x8D64:
                raise NotImplementedError('GL format %#x' % hdr['glfmt'])
            return hdr['w'], hdr['h'], etc1_decode(self.texels[72:72 + hdr['levels'][0]], hdr['w'], hdr['h'])
        raise NotImplementedError('image format %#x' % self.fmt)

    @classmethod
    def build_abgr8888(cls, w, h, rgba):
        im = cls()
        im.fmt, im.flags, im.w, im.h, im.pitch, im.size = 14, 0x1000, w, h, w * 4, 0
        im.texels = bytes(rgba)
        assert len(im.texels) == w * h * 4
        return im


def gl_blob_header(b):
    """Format 0x23 texel blob = 72-byte header + mip levels:
       u32 tag (0x00020027), u32 total blob size, u32 GL internal format (0x8D64 = ETC1_RGB8),
       u32 numLevels, u32 width, u32 height, u32 levelSize[12] (unused slots = garbage)."""
    v = struct.unpack_from('<6I12I', b, 0)
    n = v[3]
    return {'tag': v[0], 'size': v[1], 'glfmt': v[2], 'nlevels': n, 'w': v[4], 'h': v[5],
            'levels': list(v[6:6 + n])}


class Texture:
    """CIwTexture (record body after hash):
       u32 flags, u8 (+0x40), u8 (+0x41, 0x27 on ETC textures, 0 on ABGR8888 ones),
       i16 x2 (version > 3.5.0; UV scale u,v, 4096 = 1.0; one padded texture has 4096,2088),
       if !(flags & 0x2000000): if flags & 0x1000: u32 + image header only, else CIwImage
       u8 hasMipArray [+ i32 count + count x (u8 present [+ CIwImage])]
       if flags & 0x400 (version > 3.5.1): 32 x u32."""

    @classmethod
    def read(cls, h, body):
        t = cls()
        t.hash = h
        r = Reader(body)
        t.flags = r.u32()
        t.b40, t.b41 = r.u8(), r.u8()
        t.uvscale = (r.i16(), r.i16())
        if t.flags & 0x2000000 or t.flags & 0x1000:
            raise NotImplementedError('texture flags %08x' % t.flags)
        t.image = Image.read(r)
        t.has_mips = r.u8()
        if t.has_mips:
            raise NotImplementedError('mip array')
        t.extra = r.take(128) if t.flags & 0x400 else b''
        if r.left():
            raise ValueError('texture %08x: %d bytes left' % (h, r.left()))
        return t

    def write(self):
        w = Writer()
        w.u32(self.flags)
        w.u8(self.b40)
        w.u8(self.b41)
        w.i16(self.uvscale[0])
        w.i16(self.uvscale[1])
        self.image.write(w)
        w.u8(self.has_mips)
        w.raw(self.extra)
        return w.bytes()

    @classmethod
    def build_abgr8888(cls, h, w, hgt, rgba, mipmaps=False):
        """Uncompressed 32-bit texture exactly like the game's own ABGR8888 textures
        (flags 0x0024430c = no mipmaps, 0x0020430c = GL generates mipmaps)."""
        t = cls()
        t.hash = h
        t.flags = 0x0020430C if mipmaps else 0x0024430C
        t.b40 = t.b41 = 0
        t.uvscale = (4096, 4096)
        t.image = Image.build_abgr8888(w, hgt, rgba)
        t.has_mips = 0
        t.extra = b''
        return t


# ---------------------------------------------------------------- ETC1 decoder
_ETC_MOD = [(2, 8), (5, 17), (9, 29), (13, 42), (18, 60), (24, 80), (33, 106), (47, 183)]


def _clamp(x):
    return 0 if x < 0 else 255 if x > 255 else x


def etc1_decode(data, w, h):
    out = bytearray(w * h * 4)
    bw, bh = (w + 3) // 4, (h + 3) // 4
    p = 0
    for by in range(bh):
        for bx in range(bw):
            hi, lo = struct.unpack_from('>II', data, p)
            p += 8
            flip = hi & 1
            diff = (hi >> 1) & 1
            cw = ((hi >> 5) & 7, (hi >> 2) & 7)
            if diff:
                r1, g1, b1 = (hi >> 27) & 31, (hi >> 19) & 31, (hi >> 11) & 31
                dr, dg, db = (hi >> 24) & 7, (hi >> 16) & 7, (hi >> 8) & 7
                dr, dg, db = [d - 8 if d & 4 else d for d in (dr, dg, db)]
                r2, g2, b2 = r1 + dr, g1 + dg, b1 + db
                base = [tuple((c << 3) | (c >> 2) for c in (r1, g1, b1)),
                        tuple((c << 3) | (c >> 2) for c in (r2 & 31, g2 & 31, b2 & 31))]
            else:
                base = [tuple(((hi >> s) & 15) * 17 for s in (28, 20, 12)),
                        tuple(((hi >> s) & 15) * 17 for s in (24, 16, 8))]
            for i in range(16):
                x, y = i >> 2, i & 3   # pixel index is column-major
                sub = (y >= 2) if flip else (x >= 2)
                msb = (lo >> (16 + i)) & 1
                lsb = (lo >> i) & 1
                a, b = _ETC_MOD[cw[sub]]
                d = [a, b, -a, -b][(msb << 1) | lsb]
                px, py = bx * 4 + x, by * 4 + y
                if px < w and py < h:
                    o = (py * w + px) * 4
                    c = base[sub]
                    out[o] = _clamp(c[0] + d)
                    out[o + 1] = _clamp(c[1] + d)
                    out[o + 2] = _clamp(c[2] + d)
                    out[o + 3] = 255
    return bytes(out)


# ---------------------------------------------------------------- CIwAnimSkel / CIwAnimBone
def quat_unpack(v):
    """CIwAnimBone rotation: 32-bit 'smallest three'.  bits30-31 = index of the dropped
    (largest) component k, bits 0-9 / 10-19 / 20-29 = the other three, each
    q = (raw * 0x16A2 >> 10) - 0xB51 (i.e. +-4096/sqrt2).  Result (w, x, y, z), 4096 = 1.0.
    0xE0080200 = identity."""
    k = v >> 30
    a = ((v & 0x3FF) * 0x16A2 >> 10) - 0xB51
    b = (((v >> 10) & 0x3FF) * 0x16A2 >> 10) - 0xB51
    c = (((v >> 20) & 0x3FF) * 0x16A2 >> 10) - 0xB51
    m = math.sqrt(max(0, 4096 * 4096 - a * a - b * b - c * c))
    if k == 3:
        return (m, a, b, c)
    if k == 2:
        return (c, a, b, m)
    if k == 1:
        return (c, a, m, b)   # CIwSQuat_SerialisePacked case 1: [0]=c,[1]=a,[2]=m,[3]=b
    return (c, m, a, b)       # case 0: [0]=c,[1]=m,[2]=a,[3]=b


QUAT_IDENTITY = 0xE0080200


class Bone:
    """CIwAnimBone (inside the skel's managed list, after class hash 0xe1c36f11):
       u32 bone hash, u32 parent bone hash (0 = root), u32 packed rotation (quat_unpack),
       i16 x3 bind position (model units, relative to parent), u16 bone index,
       u16 flags (3: bit0 = has bind pose), i32 (+0x18, 0x400 here; default 0x1000)."""

    @classmethod
    def read(cls, r):
        b = cls()
        b.hash, b.parent, b.rot = r.u32(), r.u32(), r.u32()
        b.pos = tuple(r.arr('h', 3))
        b.index, b.flags = r.u16(), r.u16()
        b.x18 = r.i32()
        return b

    def write(self, w):
        w.u32(self.hash)
        w.u32(self.parent)
        w.u32(self.rot)
        w.arr('h', self.pos)
        w.u16(self.index)
        w.u16(self.flags)
        w.i32(self.x18)


class Skel:
    """CIwAnimSkel: CIwManagedList of bones (u32 count, count x [u32 0xe1c36f11][Bone]),
       i32 (+0x20, version > 3.4.x: 0x4000 here; default 0x1000)."""

    @classmethod
    def read(cls, h, body):
        s = cls()
        s.hash = h
        r = Reader(body)
        s.bones = []
        for _ in range(r.u32()):
            c = r.u32()
            if c != H['CIwAnimBone']:
                raise ValueError('bone class %08x' % c)
            s.bones.append(Bone.read(r))
        s.x20 = r.i32()
        if r.left():
            raise ValueError('skel: %d bytes left' % r.left())
        return s

    def write(self):
        w = Writer()
        w.u32(len(self.bones))
        for b in self.bones:
            w.u32(H['CIwAnimBone'])
            b.write(w)
        w.i32(self.x20)
        return w.bytes()

    @classmethod
    def build_one_bone(cls, h):
        """Single root bone at the origin, identity rotation; bone hash = skel hash like the
        perk machine skeletons."""
        s = cls()
        s.hash = h
        b = Bone()
        b.hash, b.parent, b.rot, b.pos, b.index, b.flags, b.x18 = h, 0, QUAT_IDENTITY, (0, 0, 0), 0, 3, 0x400
        s.bones = [b]
        s.x20 = 0x4000
        return s

    def world(self):
        """Bind-pose world (rot 3x3 float, pos) per bone index, model units."""
        out = {}
        byhash = {b.hash: b for b in self.bones}

        def mat(q):
            w, x, y, z = [c / 4096.0 for c in q]
            return [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                    [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                    [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]

        def get(b):
            if b.hash in out:
                return out[b.hash]
            R = mat(quat_unpack(b.rot))
            if b.parent and b.parent in byhash:
                PR, PP = get(byhash[b.parent])
                R2 = [[sum(PR[i][k] * R[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
                P2 = [PP[i] + sum(PR[i][k] * b.pos[k] for k in range(3)) for i in range(3)]
            else:
                R2, P2 = R, list(b.pos)
            out[b.hash] = (R2, P2)
            return out[b.hash]

        for b in self.bones:
            get(b)
        return out


# ---------------------------------------------------------------- CIwAnimSkin
class SkinSet:
    """CIwAnimSkinSet (after class hash 0x7ec0334e): u32 hash (0), u32 numVerts,
       u32 dataSize, dataSize bytes:
         bone ids: 1 bone -> [id, 0xFF] (2 bytes); 2..4 bones -> 4 bytes padded with 0xFF
         per vertex: u16 vertex index (low 14 bits), nb x CIwSVec3 i16 bone-space position,
                     nb x u8 weight (0x80 = 1.0), pad to 2 bytes (nb<3) or 4 bytes (nb>=3).
       Bone ids index the skeleton's bone list (Bone.index)."""

    @classmethod
    def read(cls, r):
        s = cls()
        s.hash, s.nverts, s.size = r.u32(), r.u32(), r.u32()
        s.data = r.take(s.size & ~1)
        return s

    def write(self, w):
        w.u32(self.hash)
        w.u32(self.nverts)
        w.u32(self.size)
        w.raw(self.data)

    def decode(self):
        d = self.data
        if d[1] == 0xFF:
            nb, p = 1, 2
        else:
            nb = 2 if d[2] == 0xFF else 3 if d[3] == 0xFF else 4
            p = 4
        bones = list(d[:nb])
        stride = 2 + nb * 6 + (2 if nb < 3 else 4)
        verts = []
        for _ in range(self.nverts):
            vid = struct.unpack_from('<H', d, p)[0] & 0x3FFF
            pos = [struct.unpack_from('<3h', d, p + 2 + 6 * k) for k in range(nb)]
            wts = list(d[p + 2 + 6 * nb:p + 2 + 7 * nb])
            pad = d[p + 2 + 7 * nb:p + stride]
            verts.append((vid, pos, wts, pad))
            p += stride
        return bones, verts

    @classmethod
    def build(cls, bones, verts):
        """bones: list of bone indices (1..4); verts: [(vid, [pos per bone], [weight per bone])].
        Weight bytes: 0x80 = 1.0; pad bytes 0xFF (as the exporter wrote)."""
        nb = len(bones)
        w = Writer()
        if nb == 1:
            w.raw(bytes([bones[0], 0xFF]))
        else:
            w.raw(bytes(bones + [0xFF] * (4 - nb)))
        padn = (2 if nb < 3 else 4) - nb
        for vid, pos, wts in verts:
            w.u16(vid)
            for pp in pos:
                w.arr('h', pp)
            w.raw(bytes(wts))
            w.raw(b'\xff' * padn)
        s = cls()
        s.hash, s.nverts, s.data = 0, len(verts), w.bytes()
        s.size = len(s.data)
        return s


class Skin:
    """CIwAnimSkin: u32 flags (1), u32 CIwModel hash, u32 CIwAnimSkel hash,
       CIwManagedList of CIwAnimSkinSet: u32 count, count x [u32 0x7ec0334e][SkinSet].
       Every UNIQUE model vertex (index < numUniqueVerts) must appear in exactly one set;
       duplicates copy their source vertex's binding at load."""

    @classmethod
    def read(cls, h, body):
        s = cls()
        s.hash = h
        r = Reader(body)
        s.flags, s.model, s.skel = r.u32(), r.u32(), r.u32()
        s.sets = []
        for _ in range(r.u32()):
            c = r.u32()
            if c != H['CIwAnimSkinSet']:
                raise ValueError('skin set class %08x' % c)
            s.sets.append(SkinSet.read(r))
        if r.left():
            raise ValueError('skin: %d bytes left' % r.left())
        return s

    def write(self):
        w = Writer()
        w.u32(self.flags)
        w.u32(self.model)
        w.u32(self.skel)
        w.u32(len(self.sets))
        for st in self.sets:
            w.u32(H['CIwAnimSkinSet'])
            st.write(w)
        return w.bytes()

    @classmethod
    def build_rigid(cls, h, model_hash, skel, bone_index, verts):
        """Bind every vertex in `verts` (the model's unique positions, model units) 100% to the
        bone with Bone.index == bone_index of Skel object `skel`.
        Skin/skeleton space = model units * (skel.x20 / 4096) (= 4 for all of this game's skels);
        bone-space position = R_world^T * (4 * v - P_world)  (verified against all skins, +-few units)."""
        W = skel.world()
        bone = [b for b in skel.bones if b.index == bone_index][0]
        R, P = W[bone.hash]
        sc = skel.x20 / 4096.0
        vv = []
        for i, v in enumerate(verts):
            d = [sc * v[k] - P[k] for k in range(3)]
            lp = tuple(max(-32768, min(32767, int(round(sum(R[k][j] * d[k] for k in range(3))))))
                       for j in range(3))
            vv.append((i, [lp], [0x80]))
        s = cls()
        s.hash, s.flags, s.model, s.skel = h, 1, model_hash, skel.hash
        s.sets = [SkinSet.build([bone_index], vv)]
        return s


CODECS = {H['CIwModel']: Model, H['CIwMaterial']: Material, H['CIwTexture']: Texture,
          H['CIwAnimSkel']: Skel, H['CIwAnimSkin']: Skin}


def decode(cls, rec):
    return CODECS[cls].read(rec[1], rec[2])


def record_bytes(h, body):
    return struct.pack('<II', 8 + len(body), h) + body


# ---------------------------------------------------------------- export / preview
def model_triangles(m):
    tris = []
    for pb in m.prims():
        ix = pb.indices
        if pb.cls == H['CIwModelBlockGLTriStrip']:
            for i in range(len(ix) - 2):
                t = (ix[i], ix[i + 1], ix[i + 2]) if i % 2 == 0 else (ix[i + 1], ix[i], ix[i + 2])
                if len(set(t)) == 3:
                    tris.append((pb.material, t))
        else:
            for i in range(0, len(ix) - 2, 3):
                tris.append((pb.material, (ix[i], ix[i + 1], ix[i + 2])))
    return tris


def export_obj(g, mh, outdir, skinh=None, skelh=None):
    from PIL import Image as PImage
    os.makedirs(outdir, exist_ok=True)
    m = decode(H['CIwModel'], g.find(H['CIwModel'], mh))
    pos = m.block('CIwModelBlockVerts').positions()
    uvb = m.block('CIwModelBlockGLUVs')
    uvs = uvb.uvs() if uvb else [(0, 0)] * len(pos)
    colb = m.block('CIwModelBlockCols')
    cols = colb.rgba() if colb else None
    name = '%08x' % mh
    texfiles = {}
    mats = []
    for i, mhash in enumerate(m.materials):
        mrec = g.find(H['CIwMaterial'], mhash)
        mat = decode(H['CIwMaterial'], mrec) if mrec else None
        th = mat.textures[0] if mat else 0
        mats.append((mhash, mat, th))
        if th and th not in texfiles:
            trec = g.find(H['CIwTexture'], th)
            if trec:
                t = decode(H['CIwTexture'], trec)
                w, hh, rgba = t.image.to_rgba()
                fn = 'tex_%08x.png' % th
                PImage.frombytes('RGBA', (w, hh), rgba).save(os.path.join(outdir, fn))
                texfiles[th] = fn
    with open(os.path.join(outdir, name + '.mtl'), 'w') as f:
        for i, (mhash, mat, th) in enumerate(mats):
            f.write('newmtl mat%d_%08x\nKd 1 1 1\n' % (i, mhash))
            if th in texfiles:
                f.write('map_Kd %s\n' % texfiles[th])
    tris = model_triangles(m)
    with open(os.path.join(outdir, name + '.obj'), 'w') as f:
        f.write('# CIwModel %08x, %d verts (%d unique), %d tris, units = model units\n'
                % (mh, len(pos), m.nunique, len(tris)))
        f.write('mtllib %s.mtl\n' % name)
        for i, p in enumerate(pos):
            if cols:
                c = cols[i]
                f.write('v %d %d %d %.3f %.3f %.3f\n' % (p[0], p[1], p[2], c[0] / 255, c[1] / 255, c[2] / 255))
            else:
                f.write('v %d %d %d\n' % p)
        for u, v in uvs:
            f.write('vt %.5f %.5f\n' % (u / 4096.0, 1 - v / 4096.0))
        cur = None
        for mi, t in tris:
            if mi != cur:
                f.write('usemtl mat%d_%08x\n' % (mi, m.materials[mi]))
                cur = mi
            f.write('f %d/%d %d/%d %d/%d\n' % tuple(x for k in t for x in (k + 1, k + 1)))
    render_preview(m, pos, uvs, cols, mats, g, os.path.join(outdir, 'preview.png'))
    return m


def render_preview(m, pos, uvs, cols, mats, g, path, size=360):
    """Tiny software rasteriser: 3 views (front, 3/4, side), textured * vertex colour, z-buffer."""
    from PIL import Image as PImage
    texs = {}
    for i, (mhash, mat, th) in enumerate(mats):
        trec = g.find(H['CIwTexture'], th) if th else None
        if trec:
            t = decode(H['CIwTexture'], trec)
            w, hh, rgba = t.image.to_rgba()
            texs[i] = (w, hh, rgba)
    tris = model_triangles(m)
    views = []
    for yaw in (0, 40, 90, 180):
        a = math.radians(yaw)
        ca, sa = math.cos(a), math.sin(a)
        views.append([((p[0] * ca + p[2] * sa), p[1], (-p[0] * sa + p[2] * ca)) for p in pos])
    allp = [q for v in views for q in v]
    minx, maxx = min(q[0] for q in allp), max(q[0] for q in allp)
    miny, maxy = min(q[1] for q in allp), max(q[1] for q in allp)
    sc = (size - 20) / max(maxx - minx, maxy - miny, 1)
    out = PImage.new('RGB', (size * len(views), size), (40, 40, 48))
    px = out.load()
    for vi, vp in enumerate(views):
        zb = [[-1e30] * size for _ in range(size)]
        cx = vi * size + size / 2
        scr = [(cx + q[0] * sc, size - 10 - (q[1] - miny) * sc, q[2]) for q in vp]
        for mi, (i0, i1, i2) in tris:
            (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = scr[i0], scr[i1], scr[i2]
            den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
            if abs(den) < 1e-9:
                continue
            tex = texs.get(mi)
            xa, xb = int(max(vi * size, math.floor(min(x0, x1, x2)))), int(min(vi * size + size - 1, math.ceil(max(x0, x1, x2))))
            ya, yb = int(max(0, math.floor(min(y0, y1, y2)))), int(min(size - 1, math.ceil(max(y0, y1, y2))))
            for y in range(ya, yb + 1):
                for x in range(xa, xb + 1):
                    l0 = ((y1 - y2) * (x - x2) + (x2 - x1) * (y - y2)) / den
                    l1 = ((y2 - y0) * (x - x2) + (x0 - x2) * (y - y2)) / den
                    l2 = 1 - l0 - l1
                    if l0 < 0 or l1 < 0 or l2 < 0:
                        continue
                    z = l0 * z0 + l1 * z1 + l2 * z2
                    lx = x - vi * size
                    if z <= zb[y][lx]:
                        continue
                    zb[y][lx] = z
                    if tex:
                        u = (l0 * uvs[i0][0] + l1 * uvs[i1][0] + l2 * uvs[i2][0]) / 4096.0
                        v = (l0 * uvs[i0][1] + l1 * uvs[i1][1] + l2 * uvs[i2][1]) / 4096.0
                        tw, th, rgba = tex
                        tx, ty = int(u * tw) % tw, int(v * th) % th
                        o = (ty * tw + tx) * 4
                        c = [rgba[o], rgba[o + 1], rgba[o + 2]]
                    else:
                        c = [200, 200, 200]
                    if cols:
                        vc = [l0 * cols[i0][k] + l1 * cols[i1][k] + l2 * cols[i2][k] for k in range(3)]
                        c = [int(c[k] * vc[k] / 255) for k in range(3)]
                    px[x, y] = tuple(c)
    out.save(path)


# ---------------------------------------------------------------- CLI
def roundtrip(path, verbose=True):
    data = open(path, 'rb').read()
    g = Group(data)
    assert g.write() == data, 'group container round trip failed'
    stats = {}
    for rl in g.lists():
        if rl.cls not in CODECS:
            continue
        ok = fail = 0
        for rec in rl.records:
            try:
                obj = CODECS[rl.cls].read(rec[1], rec[2])
            except NotImplementedError as e:
                fail += 1
                if verbose:
                    print('  skip %s %08x: %s' % (CLS.get(rl.cls), rec[1], e))
                continue
            out = obj.write()
            assert out == rec[2], '%s %08x round trip mismatch' % (CLS.get(rl.cls), rec[1])
            ok += 1
        stats[CLS.get(rl.cls)] = (ok, fail)
    return g, stats


SPEED_COLA = {'model': 0x9B750EB5, 'skin': 0x9B750EB5, 'skel': 0x5857CB59}


def selftest(path, outdir):
    """1) byte-exact round trip of the Speed Cola model/skin/skel/materials/textures;
    2) build a brand-new model (Speed Cola geometry re-encoded with no duplicate verts, new
    ABGR8888 textures, new compact materials, new one-bone skel + rigid skin), insert it into an
    in-memory copy of the group, serialise, re-parse, check, export preview.  Nothing is written
    back to the game files."""
    data = open(path, 'rb').read()
    g = Group(data)
    sc = SPEED_COLA
    recs = [(H['CIwModel'], sc['model']), (H['CIwAnimSkin'], sc['skin']), (H['CIwAnimSkel'], sc['skel'])]
    m = decode(H['CIwModel'], g.find(H['CIwModel'], sc['model']))
    for mh in m.materials:
        recs.append((H['CIwMaterial'], mh))
        mat = decode(H['CIwMaterial'], g.find(H['CIwMaterial'], mh))
        for th in mat.textures:
            if th and (H['CIwTexture'], th) not in recs:
                recs.append((H['CIwTexture'], th))
    for cls, h in recs:
        rec = g.find(cls, h)
        obj = decode(cls, rec)
        assert obj.write() == rec[2]
        print('round trip OK  %-12s %08x  %6d bytes at %08x' % (CLS[cls], h, len(rec[2]) + 8, rec[3]))

    # ---- new resources
    pos = m.block('CIwModelBlockVerts').positions()
    uvs = m.block('CIwModelBlockGLUVs').uvs()
    cols = m.block('CIwModelBlockCols').rgba()
    tris = [[] for _ in m.materials]
    for mi, t in model_triangles(m):
        tris[mi].append(t)
    nm = 'iwgfx_test'
    hm = djb2(nm + '_model')
    hskel = djb2(nm + '_skel')
    used = g.all_hashes()
    mat_hashes, new_tex = [], []
    for i, mh in enumerate(m.materials):
        mat = decode(H['CIwMaterial'], g.find(H['CIwMaterial'], mh))
        th = mat.textures[0]
        nth = djb2('%s_tex_%08x' % (nm, th))      # one new texture per original texture
        if not any(x[0] == nth for x in new_tex):
            t = decode(H['CIwTexture'], g.find(H['CIwTexture'], th))
            w, hh, rgba = t.image.to_rgba()       # ETC1 -> RGBA, re-stored as ABGR8888
            new_tex.append((nth, Texture.build_abgr8888(nth, w, hh, rgba)))
        nmh = djb2('%s_mat%d' % (nm, i))
        mat_hashes.append(nmh)
        new_mat = Material.build(nmh, nth, flags=mat.flags)
        g.add(H['CIwMaterial'], nmh, new_mat.write())
    for nth, t in new_tex:
        g.add(H['CIwTexture'], nth, t.write())
    newm = Model.build(hm, pos, uvs, tris, mat_hashes, cols=cols)
    skel = Skel.build_one_bone(hskel)
    skin = Skin.build_rigid(hm, hm, skel, 0, pos)
    for h in [hm, hskel] + mat_hashes + [x[0] for x in new_tex]:
        assert h not in used, 'hash collision %08x' % h
    g.add(H['CIwModel'], hm, newm.write())
    g.add(H['CIwAnimSkel'], hskel, skel.write())
    g.add(H['CIwAnimSkin'], hm, skin.write())
    out = g.write()
    g2 = Group(out)
    assert g2.write() == out
    m2 = decode(H['CIwModel'], g2.find(H['CIwModel'], hm))
    p2 = m2.block('CIwModelBlockVerts').positions()
    assert p2 == pos and m2.block('CIwModelBlockGLUVs').uvs() == uvs
    assert [t for _, t in model_triangles(m2)] == [t for _, t in model_triangles(m)]
    s2 = decode(H['CIwAnimSkin'], g2.find(H['CIwAnimSkin'], hm))
    b, v = s2.sets[0].decode()
    assert b == [0] and len(v) == len(pos) and v[5][1][0] == tuple(4 * c for c in pos[5])
    for cls, h in [(H['CIwModel'], hm), (H['CIwAnimSkin'], hm), (H['CIwAnimSkel'], hskel)] +             [(H['CIwMaterial'], x) for x in mat_hashes] + [(H['CIwTexture'], x[0]) for x in new_tex]:
        rec = g2.find(cls, h)
        assert decode(cls, rec).write() == rec[2]
        print('new %-12s %08x  %7d bytes' % (CLS[cls], h, len(rec[2]) + 8))
    print('group with new records: %d -> %d bytes, re-parses and round-trips' % (len(data), len(out)))
    print('new model sphere', m2.sphere, 'flags %#x' % m2.flags, 'verts', m2.nverts)
    export_obj(g2, hm, outdir)
    print('exported new model to', outdir)


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return
    cmd, path = argv[1], argv[2]
    if cmd == 'roundtrip':
        g, stats = roundtrip(path)
        print('%s: container byte-exact; per-class (round-tripped, unsupported): %s' % (path, stats))
    elif cmd == 'dump':
        g = Group(open(path, 'rb').read())
        want = {int(x, 16) for x in argv[3:]}
        for rl in g.lists():
            print('%-20s %4d records' % (CLS.get(rl.cls, '%08x' % rl.cls), len(rl.records)))
            for rec in rl.records:
                if rec[1] in want:
                    print('   %08x at %08x size %d' % (rec[1], rec[3], len(rec[2]) + 8))
    elif cmd == 'selftest':
        selftest(path, argv[3] if len(argv) > 3 else 'iwgfx_selftest')
    elif cmd == 'export':
        g = Group(open(path, 'rb').read())
        export_obj(g, int(argv[3], 16), argv[4])
        print('wrote', argv[4])


if __name__ == '__main__':
    main(sys.argv)
