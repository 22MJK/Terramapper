#!/usr/bin/env python3
import argparse
import json


FONT_5X7 = {
    "a": ["00000", "01110", "00001", "01111", "10001", "10001", "01111"],
    "b": ["10000", "10000", "11110", "10001", "10001", "10001", "11110"],
    "c": ["00000", "01110", "10001", "10000", "10000", "10001", "01110"],
    "d": ["00001", "00001", "01111", "10001", "10001", "10001", "01111"],
    "e": ["00000", "01110", "10001", "11111", "10000", "10001", "01110"],
    "f": ["00110", "01001", "01000", "11100", "01000", "01000", "01000"],
    "g": ["00000", "01111", "10001", "10001", "01111", "00001", "01110"],
    "h": ["10000", "10000", "11110", "10001", "10001", "10001", "10001"],
    "i": ["00100", "00000", "01100", "00100", "00100", "00100", "01110"],
    "j": ["00010", "00000", "00110", "00010", "00010", "10010", "01100"],
    "k": ["10000", "10001", "10010", "11100", "10010", "10001", "10001"],
    "l": ["11000", "01000", "01000", "01000", "01000", "01001", "00110"],
    "m": ["00000", "11010", "10101", "10101", "10001", "10001", "10001"],
    "n": ["00000", "11110", "10001", "10001", "10001", "10001", "10001"],
    "o": ["00000", "01110", "10001", "10001", "10001", "10001", "01110"],
    "p": ["00000", "11110", "10001", "10001", "11110", "10000", "10000"],
    "q": ["00000", "01111", "10001", "10001", "01111", "00001", "00001"],
    "r": ["00000", "10110", "11001", "10000", "10000", "10000", "10000"],
    "s": ["00000", "01111", "10000", "01110", "00001", "10001", "01110"],
    "t": ["01000", "01000", "11100", "01000", "01000", "01001", "00110"],
    "u": ["00000", "10001", "10001", "10001", "10001", "10011", "01101"],
    "v": ["00000", "10001", "10001", "10001", "01010", "01010", "00100"],
    "w": ["00000", "10001", "10001", "10101", "10101", "10101", "01010"],
    "x": ["00000", "10001", "01010", "00100", "01010", "10001", "10001"],
    "y": ["00000", "10001", "10001", "01111", "00001", "00010", "01100"],
    "z": ["00000", "11111", "00010", "00100", "01000", "10000", "11111"],
    "0": ["01110", "10001", "10011", "10101", "11001", "10001", "01110"],
    "1": ["00100", "01100", "00100", "00100", "00100", "00100", "01110"],
    "2": ["01110", "10001", "00001", "00010", "00100", "01000", "11111"],
    "3": ["01110", "10001", "00001", "00110", "00001", "10001", "01110"],
    "4": ["00010", "00110", "01010", "10010", "11111", "00010", "00010"],
    "5": ["11111", "10000", "11110", "00001", "00001", "10001", "01110"],
    "6": ["00110", "01000", "10000", "11110", "10001", "10001", "01110"],
    "7": ["11111", "00001", "00010", "00100", "01000", "01000", "01000"],
    "8": ["01110", "10001", "10001", "01110", "10001", "10001", "01110"],
    "9": ["01110", "10001", "10001", "01111", "00001", "00010", "01100"],
    "_": ["00000", "00000", "00000", "00000", "00000", "00000", "11111"],
    ":": ["00000", "00100", "00100", "00000", "00100", "00100", "00000"],
    "-": ["00000", "00000", "00000", "11111", "00000", "00000", "00000"],
    ".": ["00000", "00000", "00000", "00000", "00000", "00100", "00100"],
    " ": ["00000", "00000", "00000", "00000", "00000", "00000", "00000"],
}


def draw_text(pixels, width, height, x, y, text, color, scale=2):
    cursor_x = x
    cursor_y = y
    for ch in text:
        if ch == "\n":
            cursor_x = x
            cursor_y += (7 + 2) * scale
            continue
        glyph = FONT_5X7.get(ch, FONT_5X7[" "])
        for row, bits in enumerate(glyph):
            for col, bit in enumerate(bits):
                if bit == "1":
                    for sy in range(scale):
                        for sx in range(scale):
                            px = cursor_x + col * scale + sx
                            py = cursor_y + row * scale + sy
                            if 0 <= px < width and 0 <= py < height:
                                idx = (py * width + px) * 3
                                pixels[idx:idx + 3] = color
        cursor_x += (5 + 1) * scale


def draw_rect(pixels, width, height, x, y, w, h, fill):
    for py in range(y, y + h):
        if 0 <= py < height:
            row_start = py * width * 3
            for px in range(x, x + w):
                if 0 <= px < width:
                    idx = row_start + px * 3
                    pixels[idx:idx + 3] = fill


def write_png(path, width, height, pixels):
    import struct
    import zlib

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    raw = bytearray()
    stride = width * 3
    for y in range(height):
        raw.append(0)
        start = y * stride
        raw.extend(pixels[start:start + stride])

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    idat = zlib.compress(bytes(raw), level=6)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def parse_args():
    p = argparse.ArgumentParser(description="Plot task distribution per device as a bar chart.")
    p.add_argument("--input", required=True)
    p.add_argument("--output", required=True)
    return p.parse_args()


def main():
    args = parse_args()
    with open(args.input) as f:
        data = json.load(f)

    counts = {}
    for t in data.get("tasks", []):
        dev = str(t.get("device", "?"))
        counts[dev] = counts.get(dev, 0) + 1

    devices = sorted(counts.keys(), key=lambda x: (len(x), x))
    values = [counts[d] for d in devices]
    max_val = max(values) if values else 1

    width, height = 1000, 500
    pixels = bytearray([255, 255, 255] * (width * height))

    title = "Task Distribution per Device"
    draw_text(pixels, width, height, 30, 20, title, (20, 20, 20), scale=3)

    chart_left = 120
    chart_right = 950
    chart_top = 100
    chart_bottom = 430
    chart_w = chart_right - chart_left
    chart_h = chart_bottom - chart_top

    bar_gap = 30
    bar_count = max(1, len(values))
    bar_w = max(20, (chart_w - bar_gap * (bar_count - 1)) // bar_count)

    colors = [(91, 143, 90), (92, 124, 153), (176, 137, 0), (173, 20, 87)]

    for i, (dev, val) in enumerate(zip(devices, values)):
        x = chart_left + i * (bar_w + bar_gap)
        h = int(chart_h * (val / max_val)) if max_val else 0
        y = chart_bottom - h
        draw_rect(pixels, width, height, x, y, bar_w, h, colors[i % len(colors)])
        draw_text(pixels, width, height, x, chart_bottom + 8, f"dev {dev}", (40, 40, 40), scale=2)
        draw_text(pixels, width, height, x + 6, y - 18, str(val), (40, 40, 40), scale=2)

    write_png(args.output, width, height, pixels)


if __name__ == "__main__":
    main()
