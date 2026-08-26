# make_star_psf.py <output dir> - writes filter_star4/6/8.dds (256x256, uncompressed BGRA8 + mips):
# star-filter point-spread textures for the CinematicCamera Front Filter, the same asset type
# as the engine bokeh shape masks. Python 3, no dependencies.
# Uncompressed 32-bit BGRA DDS with a full mip chain - the same asset type as the engine's
# bokeh shape masks in textures/lights/lens_flares (no resource compiler involved).
import math, struct, sys

def star(size, points):
    lines = points // 2
    px = [[0.0, 0.0, 0.0] for _ in range(size * size)]
    for y in range(size):
        for x in range(size):
            dx = (x + 0.5) / size * 2 - 1
            dy = (y + 0.5) / size * 2 - 1
            r = [0.0, 0.0, 0.0]
            for j in range(lines):
                th = j * math.pi / lines
                c, s = math.cos(th), math.sin(th)
                along = dx * c + dy * s
                perp = -dx * s + dy * c
                a = min(abs(along), 1.0)
                line = math.exp(-abs(perp) * 40.0) * max(0.0, 1.0 - a) ** 2
                if a < 0.5:
                    t = a * 2
                    hue = [1.0 + (0.35 - 1.0) * t, 0.25 + (1.0 - 0.25) * t, 0.15 + (0.30 - 0.15) * t]
                else:
                    t = a * 2 - 1
                    hue = [0.35 + (0.20 - 0.35) * t, 1.0 + (0.35 - 1.0) * t, 0.30 + (1.0 - 0.30) * t]
                w = max(0.0, 1.0 - a)
                for k in range(3):
                    r[k] += line * (hue[k] * (1 - w) + 1.0 * w)
            core = math.exp(-(dx * dx + dy * dy) * 60.0)
            for k in range(3):
                r[k] = min(1.0, r[k] + core)
            px[y * size + x] = r
    return px

def to_bgra(px, size):
    out = bytearray()
    for p in px:
        out += bytes((int(p[2] * 255 + 0.5), int(p[1] * 255 + 0.5), int(p[0] * 255 + 0.5), 255))
    return bytes(out)

def downsample(px, size):
    h = size // 2
    o = []
    for y in range(h):
        for x in range(h):
            acc = [0.0, 0.0, 0.0]
            for dy in (0, 1):
                for dx in (0, 1):
                    p = px[(2 * y + dy) * size + (2 * x + dx)]
                    for k in range(3): acc[k] += p[k] * 0.25
            o.append(acc)
    return o

def write_dds(path, size, points):
    px = star(size, points)
    mips = []
    s = size
    while True:
        mips.append(to_bgra(px, s))
        if s == 1: break
        px = downsample(px, s); s //= 2
    DDSD = 0x1 | 0x2 | 0x4 | 0x1000 | 0x8 | 0x20000  # caps|height|width|pixelformat|pitch|mipmapcount
    hdr = struct.pack('<4sIIIIIII11I', b'DDS ', 124, DDSD, size, size, size * 4, 0, len(mips), *([0] * 11))
    pf = struct.pack('<IIIIIIII', 32, 0x41, 0, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000)  # RGB|ALPHA, BGRA8
    caps = struct.pack('<IIIII', 0x1000 | 0x400000 | 0x8, 0, 0, 0, 0)  # texture|mipmap|complex
    with open(path, 'wb') as f:
        f.write(hdr + pf + caps + b''.join(mips))

base = sys.argv[1]
for n in (4, 6, 8):
    write_dds(f'{base}/filter_star{n}.dds', 256, n)
print('ok')
