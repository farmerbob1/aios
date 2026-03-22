#!/usr/bin/env python3
"""Generate PNG icons for AIOS apps and file types.
Creates RGBA PNGs with alpha transparency using only stdlib (zlib)."""

import struct, zlib, os, math

ICON_DIR = os.path.join(os.path.dirname(__file__), '..', 'harddrive', 'system', 'icons')

def make_png(width, height, pixels):
    """pixels = flat list of (R,G,B,A) tuples, row-major"""
    def chunk(ctype, data):
        c = ctype + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c) & 0xFFFFFFFF)
    header = b'\x89PNG\r\n\x1a\n'
    ihdr = chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0))
    raw = b''
    for y in range(height):
        raw += b'\x00'
        for x in range(width):
            r, g, b, a = pixels[y * width + x]
            raw += struct.pack('BBBB', r, g, b, a)
    idat = chunk(b'IDAT', zlib.compress(raw, 9))
    iend = chunk(b'IEND', b'')
    return header + ihdr + idat + iend

class Canvas:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = [(0,0,0,0)] * (w * h)

    def set(self, x, y, r, g, b, a=255):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y * self.w + x] = (r, g, b, a)

    def fill_rect(self, x0, y0, rw, rh, r, g, b, a=255):
        for y in range(max(0, y0), min(y0 + rh, self.h)):
            for x in range(max(0, x0), min(x0 + rw, self.w)):
                self.px[y * self.w + x] = (r, g, b, a)

    def fill_rounded(self, x0, y0, rw, rh, rad, r, g, b, a=255):
        for y in range(max(0, y0), min(y0 + rh, self.h)):
            for x in range(max(0, x0), min(x0 + rw, self.w)):
                lx, ly = x - x0, y - y0
                inside = True
                for cx, cy in [(rad, rad), (rw-rad-1, rad), (rad, rh-rad-1), (rw-rad-1, rh-rad-1)]:
                    if lx < rad and ly < rad and cx == rad and cy == rad:
                        if (lx - cx)**2 + (ly - cy)**2 > rad**2: inside = False
                    elif lx >= rw - rad and ly < rad and cx == rw-rad-1 and cy == rad:
                        if (lx - cx)**2 + (ly - cy)**2 > rad**2: inside = False
                    elif lx < rad and ly >= rh - rad and cx == rad and cy == rh-rad-1:
                        if (lx - cx)**2 + (ly - cy)**2 > rad**2: inside = False
                    elif lx >= rw - rad and ly >= rh - rad and cx == rw-rad-1 and cy == rh-rad-1:
                        if (lx - cx)**2 + (ly - cy)**2 > rad**2: inside = False
                if inside:
                    self.px[y * self.w + x] = (r, g, b, a)

    def fill_circle(self, cx, cy, rad, r, g, b, a=255):
        for y in range(max(0, cy - rad), min(cy + rad + 1, self.h)):
            for x in range(max(0, cx - rad), min(cx + rad + 1, self.w)):
                if (x - cx)**2 + (y - cy)**2 <= rad**2:
                    self.px[y * self.w + x] = (r, g, b, a)

    def draw_line(self, x0, y0, x1, y1, r, g, b, a=255):
        dx = abs(x1 - x0); dy = abs(y1 - y0)
        sx = 1 if x0 < x1 else -1; sy = 1 if y0 < y1 else -1
        err = dx - dy
        while True:
            self.set(x0, y0, r, g, b, a)
            if x0 == x1 and y0 == y1: break
            e2 = 2 * err
            if e2 > -dy: err -= dy; x0 += sx
            if e2 < dx: err += dx; y0 += sy

    def outline_rect(self, x0, y0, rw, rh, r, g, b, a=255):
        for x in range(x0, x0 + rw):
            self.set(x, y0, r, g, b, a)
            self.set(x, y0 + rh - 1, r, g, b, a)
        for y in range(y0, y0 + rh):
            self.set(x0, y, r, g, b, a)
            self.set(x0 + rw - 1, y, r, g, b, a)

    def save(self, path):
        data = make_png(self.w, self.h, self.px)
        with open(path, 'wb') as f:
            f.write(data)
        return len(data)

# ── Icon Drawing Functions ──────────────────────────

def draw_folder(c):
    s = c.w
    # Body
    c.fill_rounded(1, s//4, s-2, s*3//4-2, 3, 70, 150, 245)
    # Tab
    c.fill_rounded(1, s//4-3, s//3, 5, 2, 90, 170, 255)
    # Highlight line
    c.fill_rect(3, s//4+2, s-6, 1, 110, 190, 255, 160)
    # Shadow
    c.fill_rect(2, s-4, s-4, 1, 40, 100, 180, 100)

def draw_file_text(c):
    s = c.w
    # Page body
    c.fill_rounded(s//6, 1, s*2//3, s-2, 2, 220, 225, 230)
    # Dog-ear fold
    fold = s//4
    for i in range(fold):
        c.fill_rect(s//6 + s*2//3 - fold + i, 1, 1, fold - i, 180, 185, 195)
    # Text lines
    for i in range(4):
        lw = s//2 - (i % 2) * s//6
        c.fill_rect(s//4, s//3 + i * (s//8), lw, max(1, s//16), 140, 145, 155)

def draw_terminal(c):
    s = c.w
    # Terminal body
    c.fill_rounded(1, 1, s-2, s-2, 4, 35, 35, 45)
    # Border
    c.outline_rect(1, 1, s-2, s-2, 60, 60, 75)
    # Titlebar
    c.fill_rect(2, 2, s-4, s//6, 50, 50, 65)
    # Prompt >_
    c.fill_rect(s//5, s*2//5, 2, s//5, 80, 255, 80)  # >
    c.draw_line(s//5, s*2//5, s//5+s//8, s*2//5+s//10, 80, 255, 80)
    c.draw_line(s//5, s*2//5+s//5, s//5+s//8, s*2//5+s//10, 80, 255, 80)
    # Cursor block
    c.fill_rect(s//5 + s//5, s*2//5, s//8, s//5, 80, 255, 80, 180)

def draw_settings(c):
    s = c.w
    cx, cy = s//2, s//2
    r_outer = s//3
    r_inner = s//5
    # Gear teeth (8 teeth)
    for i in range(8):
        angle = i * math.pi / 4
        tx = int(cx + (r_outer + 1) * math.cos(angle))
        ty = int(cy + (r_outer + 1) * math.sin(angle))
        c.fill_circle(tx, ty, max(2, s//10), 160, 165, 180)
    # Outer ring
    c.fill_circle(cx, cy, r_outer, 160, 165, 180)
    # Inner circle (cutout)
    c.fill_circle(cx, cy, r_inner, 100, 105, 120)
    # Center dot
    c.fill_circle(cx, cy, max(1, s//10), 160, 165, 180)

def draw_music(c):
    s = c.w
    # Note head 1
    c.fill_circle(s//4, s*3//4, s//6, 140, 100, 255)
    # Note head 2
    c.fill_circle(s*3//4-1, s*2//3, s//6, 140, 100, 255)
    # Stems
    c.fill_rect(s//4 + s//6-1, s//4, 2, s//2, 180, 140, 255)
    c.fill_rect(s*3//4-1 + s//6-1, s//6, 2, s//2, 180, 140, 255)
    # Beam
    c.fill_rect(s//4 + s//6-1, s//6, s//2, max(2, s//10), 180, 140, 255)

def draw_image(c):
    s = c.w
    # Frame
    c.fill_rounded(1, 1, s-2, s-2, 3, 55, 55, 70)
    c.fill_rounded(3, 3, s-6, s-6, 2, 40, 120, 180)
    # Sun
    c.fill_circle(s//3, s//3, s//8, 255, 220, 80)
    # Mountain
    for i in range(s//2):
        lx = s//4 + i
        if lx < s - 3:
            peak_h = max(0, s//3 - abs(i - s//4))
            c.fill_rect(lx, s - 4 - peak_h, 1, peak_h, 60, 160, 80)

def draw_cube3d(c):
    s = c.w
    cx, cy = s//2, s//2
    hs = s//3  # half-size
    # Front face
    c.fill_rect(cx - hs//2, cy - hs//4, hs, hs, 100, 140, 220)
    # Top face (parallelogram)
    for i in range(hs//2):
        c.fill_rect(cx - hs//2 + i, cy - hs//4 - hs//2 + i, hs, 1, 130, 170, 240)
    # Right face
    for i in range(hs):
        c.fill_rect(cx + hs//2 + hs//2 - i//2, cy - hs//4 + i, max(1, hs//2), 1, 70, 110, 190)

def draw_globe(c):
    s = c.w
    cx, cy = s//2, s//2
    r = s//3
    # Globe body
    c.fill_circle(cx, cy, r, 60, 140, 200)
    # Latitude lines
    for lat in [-r//2, 0, r//2]:
        w = int(math.sqrt(max(0, r*r - lat*lat)))
        c.fill_rect(cx - w, cy + lat, w*2, 1, 80, 170, 230, 180)
    # Meridian
    c.fill_rect(cx, cy - r, 1, r*2, 80, 170, 230, 180)
    # Ellipse meridian
    for y in range(-r, r+1):
        x = int(r//3 * math.sqrt(max(0, 1 - (y/r)**2)))
        c.set(cx + x, cy + y, 80, 170, 230, 180)
        c.set(cx - x, cy + y, 80, 170, 230, 180)

def draw_chart(c):
    s = c.w
    # Background
    c.fill_rounded(1, 1, s-2, s-2, 3, 45, 45, 60)
    # Bars
    bars = [0.4, 0.7, 0.5, 0.9, 0.6]
    bw = max(2, (s - 8) // len(bars) - 1)
    for i, h in enumerate(bars):
        bx = 4 + i * (bw + 1)
        bh = int((s - 12) * h)
        by = s - 4 - bh
        colors = [(80,200,120), (100,180,255), (255,180,80), (220,100,100), (180,120,255)]
        r, g, b = colors[i % len(colors)]
        c.fill_rect(bx, by, bw, bh, r, g, b)

def draw_pencil(c):
    s = c.w
    # Pencil body (diagonal)
    for i in range(s*2//3):
        x = s//6 + i
        y = s*5//6 - i
        c.fill_rect(x, y-2, 3, 5, 255, 200, 80)
    # Tip
    for i in range(s//6):
        x = s//6 - s//6 + i
        y = s*5//6 + s//6 - i
        c.fill_rect(x + s//8, y - 1, 2, 2, 60, 60, 60)
    # Eraser end
    c.fill_rect(s*5//6-1, s//6-1, 4, 4, 255, 130, 130)

def draw_sysmon(c):
    s = c.w
    # Screen frame
    c.fill_rounded(1, 2, s-2, s*2//3, 3, 45, 45, 60)
    # Screen
    c.fill_rect(3, 4, s-6, s*2//3-4, 30, 30, 40)
    # Heartbeat line
    pts = [s//2, s//2-2, s//2+3, s//2-s//5, s//2+1, s//2+s//6, s//2-1, s//2]
    mid_y = 4 + (s*2//3-4)//2
    for i in range(len(pts)-1):
        x0 = 4 + i * (s-8) // (len(pts)-1)
        x1 = 4 + (i+1) * (s-8) // (len(pts)-1)
        c.draw_line(x0, pts[i] + 2, x1, pts[i+1] + 2, 80, 255, 80)
    # Stand
    c.fill_rect(s//3, s*2//3+2, s//3, 3, 120, 120, 135)
    c.fill_rect(s//4, s*2//3+5, s//2, 2, 120, 120, 135)

# ── Generate all icons ──────────────────────────────

icons = [
    ("files",    [32, 48], draw_folder),
    ("terminal", [32, 48], draw_terminal),
    ("settings", [32, 48], draw_settings),
    ("text",     [32, 48], draw_file_text),
    ("music",    [32, 48], draw_music),
    ("image",    [32, 48], draw_image),
    ("cobj",     [32, 48], draw_cube3d),
    ("globe",    [32, 48], draw_globe),
    ("sysmon",   [32, 48], draw_chart),
    ("edit",     [32, 48], draw_pencil),
]

os.makedirs(ICON_DIR, exist_ok=True)
total = 0
for name, sizes, draw_fn in icons:
    for sz in sizes:
        c = Canvas(sz, sz)
        draw_fn(c)
        path = os.path.join(ICON_DIR, f'{name}_{sz}.png')
        nbytes = c.save(path)
        total += 1
        print(f'  {name}_{sz}.png ({nbytes} bytes)')

print(f'Generated {total} PNG icons')
