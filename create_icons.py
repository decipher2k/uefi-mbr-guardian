#!/usr/bin/env python3
"""
Generate sample 64x64 BMP icons for MBR Guardian.
Creates simple OS-themed icons without external dependencies.
"""

import struct
import os

def create_bmp(filename, pixels, width=64, height=64):
    """Write a 24-bit uncompressed BMP file."""
    row_stride = (width * 3 + 3) & ~3  # 4-byte aligned rows
    pixel_size = row_stride * height
    file_size = 54 + pixel_size

    with open(filename, 'wb') as f:
        # BMP header
        f.write(b'BM')
        f.write(struct.pack('<I', file_size))
        f.write(struct.pack('<HH', 0, 0))
        f.write(struct.pack('<I', 54))  # Pixel data offset

        # DIB header (BITMAPINFOHEADER)
        f.write(struct.pack('<I', 40))
        f.write(struct.pack('<i', width))
        f.write(struct.pack('<i', height))  # Positive = bottom-up
        f.write(struct.pack('<HH', 1, 24))
        f.write(struct.pack('<I', 0))       # No compression
        f.write(struct.pack('<I', pixel_size))
        f.write(struct.pack('<ii', 2835, 2835))
        f.write(struct.pack('<II', 0, 0))

        # Pixel data (bottom-up)
        for y in range(height - 1, -1, -1):
            row = bytearray()
            for x in range(width):
                r, g, b = pixels[y * width + x]
                row.extend([b, g, r])  # BGR
            # Pad to 4-byte boundary
            while len(row) % 4 != 0:
                row.append(0)
            f.write(row)

def rounded_rect_mask(x, y, w, h, r, px, py):
    """Check if pixel (px, py) is inside a rounded rectangle."""
    if px < x or px >= x + w or py < y or py >= y + h:
        return False
    # Check corners
    corners = [(x + r, y + r), (x + w - r - 1, y + r),
               (x + r, y + h - r - 1), (x + w - r - 1, y + h - r - 1)]
    for cx, cy in corners:
        if ((px < x + r or px >= x + w - r) and
            (py < y + r or py >= y + h - r)):
            dx, dy = px - cx, py - cy
            if px < x + r: dx = (x + r) - px
            if px >= x + w - r: dx = px - (x + w - r - 1)
            if py < y + r: dy = (y + r) - py
            if py >= y + h - r: dy = py - (y + h - r - 1)
            if dx * dx + dy * dy > r * r:
                return False
    return True

def lerp(a, b, t):
    return int(a + (b - a) * t)

def make_gradient_bg(w, h, top, bottom, radius=10):
    """Create gradient background with rounded corners."""
    pixels = [(0, 0, 0)] * (w * h)
    for y in range(h):
        t = y / (h - 1) if h > 1 else 0
        r = lerp(top[0], bottom[0], t)
        g = lerp(top[1], bottom[1], t)
        b = lerp(top[2], bottom[2], t)
        for x in range(w):
            if rounded_rect_mask(0, 0, w, h, radius, x, y):
                pixels[y * w + x] = (r, g, b)
    return pixels

def draw_rect(pixels, w, x1, y1, x2, y2, color):
    for y in range(max(0, y1), min(64, y2)):
        for x in range(max(0, x1), min(64, x2)):
            pixels[y * w + x] = color

def draw_circle(pixels, w, cx, cy, r, color):
    for y in range(max(0, cy - r), min(64, cy + r + 1)):
        for x in range(max(0, cx - r), min(64, cx + r + 1)):
            if (x - cx)**2 + (y - cy)**2 <= r**2:
                pixels[y * w + x] = color

# === Icon Generators ===

def icon_windows():
    """Windows-style icon: blue gradient with 4 white squares."""
    px = make_gradient_bg(64, 64, (0, 100, 210), (0, 60, 160))
    white = (255, 255, 255)
    # Four panes
    draw_rect(px, 64, 16, 16, 30, 30, white)
    draw_rect(px, 64, 34, 16, 48, 30, white)
    draw_rect(px, 64, 16, 34, 30, 48, white)
    draw_rect(px, 64, 34, 34, 48, 48, white)
    return px

def icon_linux():
    """Linux-style icon: orange gradient with penguin silhouette."""
    px = make_gradient_bg(64, 64, (240, 140, 20), (180, 80, 0))
    white = (255, 255, 255)
    black = (40, 40, 40)
    # Penguin body (simplified)
    draw_circle(px, 64, 32, 22, 10, white)  # Head
    draw_rect(px, 64, 22, 28, 42, 50, white)  # Body
    # Eyes
    draw_circle(px, 64, 28, 20, 2, black)
    draw_circle(px, 64, 36, 20, 2, black)
    # Beak
    draw_rect(px, 64, 30, 24, 34, 27, (240, 180, 0))
    # Feet
    draw_rect(px, 64, 22, 48, 28, 52, (240, 180, 0))
    draw_rect(px, 64, 36, 48, 42, 52, (240, 180, 0))
    return px

def icon_freebsd():
    """FreeBSD-style icon: red gradient with devil horns."""
    px = make_gradient_bg(64, 64, (200, 30, 30), (140, 10, 10))
    white = (255, 255, 255)
    # Devil face
    draw_circle(px, 64, 32, 34, 14, white)
    # Horns
    for i in range(8):
        draw_rect(px, 64, 18 - i//2, 14 + i, 22 - i//2, 16 + i, (255, 50, 50))
        draw_rect(px, 64, 42 + i//2, 14 + i, 46 + i//2, 16 + i, (255, 50, 50))
    # Eyes
    draw_circle(px, 64, 27, 32, 3, (200, 30, 30))
    draw_circle(px, 64, 37, 32, 3, (200, 30, 30))
    # Mouth
    draw_rect(px, 64, 26, 40, 38, 42, (200, 30, 30))
    return px

def icon_openbsd():
    """OpenBSD-style icon: yellow with pufferfish."""
    px = make_gradient_bg(64, 64, (220, 200, 40), (180, 160, 0))
    # Body
    draw_circle(px, 64, 32, 32, 16, (240, 230, 180))
    # Spikes
    for angle_i in range(12):
        import math
        a = angle_i * math.pi * 2 / 12
        sx = int(32 + 18 * math.cos(a))
        sy = int(32 + 18 * math.sin(a))
        draw_circle(px, 64, sx, sy, 2, (200, 180, 60))
    # Eyes
    draw_circle(px, 64, 27, 28, 3, (20, 20, 20))
    draw_circle(px, 64, 37, 28, 3, (20, 20, 20))
    draw_circle(px, 64, 28, 27, 1, (255, 255, 255))
    draw_circle(px, 64, 38, 27, 1, (255, 255, 255))
    return px

def icon_haiku():
    """Haiku/BeOS-style icon: green leaf."""
    px = make_gradient_bg(64, 64, (60, 200, 100), (20, 140, 60))
    white = (255, 255, 255)
    # Leaf shape (ellipse)
    for y in range(64):
        for x in range(64):
            dx, dy = (x - 32) / 18, (y - 32) / 24
            if dx * dx + dy * dy <= 1:
                px[y * 64 + x] = (80, 220, 120)
    # Stem
    for y in range(36, 56):
        draw_rect(px, 64, 31, y, 33, y + 1, (40, 120, 40))
    # Vein
    for y in range(14, 36):
        px[y * 64 + 32] = (60, 180, 80)
    return px

def icon_generic():
    """Generic disk icon: gray with HDD symbol."""
    px = make_gradient_bg(64, 64, (100, 100, 120), (60, 60, 80))
    silver = (200, 200, 210)
    # HDD body
    draw_rect(px, 64, 12, 20, 52, 44, silver)
    draw_rect(px, 64, 12, 32, 52, 34, (80, 80, 100))
    # LED
    draw_rect(px, 64, 44, 36, 48, 40, (0, 255, 80))
    # Label area
    draw_rect(px, 64, 16, 22, 36, 30, (160, 160, 180))
    return px

def icon_macos():
    """macOS-style icon: gray with apple silhouette."""
    px = make_gradient_bg(64, 64, (180, 180, 185), (120, 120, 130))
    dark = (60, 60, 65)
    # Apple shape (circle + bite)
    for y in range(64):
        for x in range(64):
            dx, dy = x - 32, y - 36
            if dx * dx + dy * dy <= 196:  # r=14
                # Bite
                bx, by = x - 44, y - 30
                if bx * bx + by * by > 64:
                    px[y * 64 + x] = dark
    # Stem
    draw_rect(px, 64, 32, 16, 34, 24, dark)
    # Leaf
    for y in range(16, 22):
        for x in range(34, 40):
            dx = (x - 37) / 3
            dy = (y - 19) / 3
            if dx * dx + dy * dy <= 1:
                px[y * 64 + x] = (80, 180, 80)
    return px

# === Main ===

def main():
    os.makedirs('icons', exist_ok=True)

    icons = {
        'windows.bmp': icon_windows(),
        'linux.bmp': icon_linux(),
        'freebsd.bmp': icon_freebsd(),
        'openbsd.bmp': icon_openbsd(),
        'haiku.bmp': icon_haiku(),
        'macos.bmp': icon_macos(),
        'generic.bmp': icon_generic(),
    }

    for name, pixels in icons.items():
        path = os.path.join('icons', name)
        create_bmp(path, pixels)
        print(f"  Created: {path}")

    print(f"\n  {len(icons)} icons generated in icons/")
    print(f"  Copy to ESP: cp icons/*.bmp /boot/efi/EFI/mbr-guardian/icons/")

if __name__ == '__main__':
    main()
