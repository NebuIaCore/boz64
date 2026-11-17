# Minimal PNG reading and writing in pure Python (8-bit RGB/RGBA, not interlaced), for the data tools.
import struct
import zlib


def read(path):
    """-> (width, height, RGBA bytes, rows top to bottom)"""
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n', path
    o, idat = 8, b''
    w = h = ct = None
    while o < len(d):
        n, t = struct.unpack('>I4s', d[o:o + 8])
        c = d[o + 8:o + 8 + n]
        o += 12 + n
        if t == b'IHDR':
            w, h, depth, ct, _comp, _filt, inter = struct.unpack('>IIBBBBB', c)
            assert depth == 8 and ct in (2, 6) and inter == 0, (path, depth, ct, inter)
        elif t == b'IDAT':
            idat += c
    bpp = 3 if ct == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * bpp
    out = bytearray()
    prev = bytearray(stride)
    o = 0
    for _ in range(h):
        f = raw[o]
        line = bytearray(raw[o + 1:o + 1 + stride])
        o += 1 + stride
        if f == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        if bpp == 3:
            px = bytearray()
            for i in range(0, stride, 3):
                px += line[i:i + 3] + b'\xff'
            out += px
        else:
            out += line
        prev = line
    return w, h, bytes(out)


def write(path, w, h, rgba):
    raw = b''.join(b'\0' + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))

    def chunk(t, c):
        return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def downscale(w, h, rgba, f):
    """box filter by an integer factor f"""
    nw, nh = w // f, h // f
    out = bytearray(nw * nh * 4)
    n = f * f
    for y in range(nh):
        for x in range(nw):
            acc = [0, 0, 0, 0]
            for dy in range(f):
                o = ((y * f + dy) * w + x * f) * 4
                for dx in range(f):
                    for k in range(4):
                        acc[k] += rgba[o + dx * 4 + k]
            q = (y * nw + x) * 4
            for k in range(4):
                out[q + k] = (acc[k] + n // 2) // n
    return nw, nh, bytes(out)
