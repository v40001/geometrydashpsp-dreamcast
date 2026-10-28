#!/usr/bin/env python3
"""
mkatlas.py - build Dreamcast sprite sheets (.dcs) from the game's tex3ds lists

The game picks its sprites by their position in the .t3s list, so the order is
kept. Images are packed into power-of-two pages of at most 512x512 (the PVR's
limit for a twiddled texture), each page in ARGB4444 and already twiddled the
way the PVR reads it: pvr_txr_load() copies the file's page straight to VRAM.

File layout, little endian:
  "DCS1" u32 images u32 pages
  pages:  u16 w, u16 h, u8 fmt (0 ARGB4444), u8 pad[3], u32 size, then the
          twiddled texels
  images: u16 page, u16 x, u16 y, u16 w, u16 h, u16 pad

Usage: mkatlas.py <file.t3s> <out.dcs>
"""
import struct
import sys
import os
from PIL import Image

PAGE_MAX = 512


def dc_override(path):
    """gfx/x/y.png -> dc/gfx/x/y.png when there is one: the Dreamcast's own
    art, without touching the other builds' files."""
    rel = os.path.relpath(path, 'gfx')
    alt = os.path.join('dc', 'gfx', rel)
    return alt if not rel.startswith('..') and os.path.exists(alt) else path


def parse_t3s(path):
    lines = [l.strip() for l in open(path) if l.strip()]
    opts = lines[0].split()
    border = opts[opts.index('--border') + 1] if '--border' in opts else 'none'
    base = os.path.dirname(path)
    return border, [dc_override(os.path.join(base, l)) for l in lines[1:]]


def pow2(n):
    p = 16
    while p < n:
        p *= 2
    return p


def pack(images, border):
    """Shelf packer: tallest first. Returns per image (page, x, y) and page sizes."""
    pad = 1 if border != 'none' else 0
    order = sorted(range(len(images)), key=lambda i: (-images[i].size[1], -images[i].size[0]))
    pages = []
    place = [None] * len(images)

    for i in order:
        w, h = images[i].size
        p = pad if w + 2 * pad <= PAGE_MAX and h + 2 * pad <= PAGE_MAX else 0
        bw, bh = w + 2 * p, h + 2 * p
        done = False
        for pi, page in enumerate(pages):
            shelves = page[0]
            for shelf in shelves:
                if bh <= shelf[1] and shelf[2] + bw <= PAGE_MAX:
                    place[i] = (pi, shelf[2] + p, shelf[0] + p, p)
                    shelf[2] += bw
                    done = True
                    break
            if done:
                break
            if page[1] + bh <= PAGE_MAX:
                shelves.append([page[1], bh, bw])
                place[i] = (pi, p, page[1] + p, p)
                page[1] += bh
                done = True
                break
        if not done:
            pages.append([[[0, bh, bw]], bh])
            place[i] = (len(pages) - 1, p, p, p)

    sizes = []
    for page in pages:
        mw = max(s[2] for s in page[0])
        sizes.append((pow2(mw), pow2(page[1])))
    return place, sizes


def blit(canvas, img, x, y, p, border):
    canvas.paste(img, (x, y))
    if p == 0 or border != 'edge':
        return
    w, h = img.size
    canvas.paste(img.crop((0, 0, w, 1)), (x, y - 1))
    canvas.paste(img.crop((0, h - 1, w, h)), (x, y + h))
    canvas.paste(img.crop((0, 0, 1, h)), (x - 1, y))
    canvas.paste(img.crop((w - 1, 0, w, h)), (x + w, y))
    for cx, cy, sx, sy in ((x - 1, y - 1, 0, 0), (x + w, y - 1, w - 1, 0),
                           (x - 1, y + h, 0, h - 1), (x + w, y + h, w - 1, h - 1)):
        canvas.putpixel((cx, cy), img.getpixel((sx, sy)))


def twidtab(x):
    return ((x & 1) | ((x & 2) << 1) | ((x & 4) << 2) | ((x & 8) << 3) |
            ((x & 16) << 4) | ((x & 32) << 5) | ((x & 64) << 6) | ((x & 128) << 7) |
            ((x & 256) << 8) | ((x & 512) << 9))


def twidout(x, y):
    return twidtab(y) | (twidtab(x) << 1)


def twiddle16(canvas):
    """ARGB4444 texels in the PVR's twiddled order (pvr_txr_load_ex's 16bpp
    case), little endian."""
    w, h = canvas.size
    src = canvas.tobytes()
    out = bytearray(w * h * 2)
    mn = min(w, h)
    mask = mn - 1
    for y in range(h):
        for x in range(w):
            i = (y * w + x) * 4
            r, g, b, a = src[i], src[i + 1], src[i + 2], src[i + 3]
            v = ((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)
            o = twidout(x & mask, y & mask) + (x // mn + y // mn) * mn * mn
            out[o * 2] = v & 0xff
            out[o * 2 + 1] = v >> 8
    return bytes(out)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    t3s, out = sys.argv[1], sys.argv[2]
    border, files = parse_t3s(t3s)
    images = [Image.open(f).convert('RGBA') for f in files]
    place, sizes = pack(images, border)

    canvases = [Image.new('RGBA', s, (0, 0, 0, 0)) for s in sizes]
    for i, img in enumerate(images):
        pi, x, y, p = place[i]
        blit(canvases[pi], img, x, y, p, border)

    with open(out, 'wb') as f:
        f.write(b'DCS1' + struct.pack('<II', len(images), len(canvases)))
        for c in canvases:
            # VRAM is 8 MB: these big sheets go at half size, the rest sharp
            half = os.path.basename(out).split('.')[0] in ('ui_2', 'glow') or out.endswith(('bg_one_01.dcs','bg_one_02.dcs','bg_one_03.dcs','bg_one_04.dcs','bg_one_05.dcs','bg_one_06.dcs','bg_one_07.dcs'))
            img = c.resize((c.size[0] // 2, c.size[1] // 2), Image.LANCZOS) if half else c
            data = twiddle16(img)
            f.write(struct.pack('<HHB3xI', c.size[0], c.size[1], 1 if half else 0, len(data)))
            f.write(data)
        for i, img in enumerate(images):
            pi, x, y, p = place[i]
            f.write(struct.pack('<HHHHHH', pi, x, y, img.size[0], img.size[1], 0))

    total = sum(w * h for w, h in sizes)
    print(f"{os.path.basename(out)}: {len(images)} images, {len(sizes)} pages "
          f"{sizes[:4]}{'...' if len(sizes) > 4 else ''}, {total * 2 // 1024} KB")
    return 0


if __name__ == '__main__':
    sys.exit(main())
