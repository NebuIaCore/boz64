# IMA ADPCM (mono), the layout of Marmalade's CIwSoundData ADPCM sounds (format 2 in its trailer): blocks of `align`
# bytes, each a 4-byte header (i16 first sample, u8 step index, u8 0) then 4-bit codes, low nibble first:
# (align - 4) * 2 + 1 samples a block. Black Ops' sounds use 256- and 512-byte blocks; World at War's also 1024, which
# Black Ops' loader does not take (it calls a null decoder), so tools/wawguns.py re-encodes those.
STEPS = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
         107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
         876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
         4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385,
         24623, 27086, 29794, 32767]
INDEX = [-1, -1, -1, -1, 2, 4, 6, 8]


def _step(pred, idx, code):
    step = STEPS[idx]
    diff = step >> 3
    if code & 1:
        diff += step >> 2
    if code & 2:
        diff += step >> 1
    if code & 4:
        diff += step
    pred = pred - diff if code & 8 else pred + diff
    pred = max(-32768, min(32767, pred))
    idx = max(0, min(88, idx + INDEX[code & 7]))
    return pred, idx


def decode(data, align):
    """ADPCM blocks -> list of int16 samples"""
    out = []
    for o in range(0, len(data) - align + 1, align):
        pred = int.from_bytes(data[o:o + 2], 'little', signed=True)
        idx = min(88, data[o + 2])
        out.append(pred)
        for b in data[o + 4:o + align]:
            for code in (b & 15, b >> 4):
                pred, idx = _step(pred, idx, code)
                out.append(pred)
    return out


def encode(samples, align):
    """int16 samples -> ADPCM blocks (the last one padded with its last sample)"""
    per = (align - 4) * 2 + 1
    out = bytearray()
    idx = 0
    for start in range(0, len(samples), per):
        blk = list(samples[start:start + per])
        blk += [blk[-1]] * (per - len(blk))
        pred = blk[0]
        out += int(pred).to_bytes(2, 'little', signed=True) + bytes((idx, 0))
        codes = []
        for s in blk[1:]:
            # the code whose decoded value is nearest (the decoder's own arithmetic, so it stays in step)
            step = STEPS[idx]
            diff = s - pred
            code = 0
            if diff < 0:
                code, diff = 8, -diff
            if diff >= step:
                code |= 4
                diff -= step
            if diff >= step >> 1:
                code |= 2
                diff -= step >> 1
            if diff >= step >> 2:
                code |= 1
            pred, idx = _step(pred, idx, code)
            codes.append(code)
        for i in range(0, len(codes), 2):
            out.append(codes[i] | (codes[i + 1] << 4 if i + 1 < len(codes) else 0))
    return bytes(out)
