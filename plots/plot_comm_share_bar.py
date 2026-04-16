#!/usr/bin/env python3
import argparse
import json
import math
from collections import defaultdict


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
    "@": ["01110", "10001", "10111", "10101", "10111", "10000", "01110"],
    "-": ["00000", "00000", "00000", "11111", "00000", "00000", "00000"],
    ".": ["00000", "00000", "00000", "00000", "00000", "00100", "00100"],
    "%": ["10001", "00010", "00100", "01000", "10001", "00000", "00000"],
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


def load_hardware(path):
    with open(path) as f:
        data = json.load(f)
    devices = {d["id"]: d for d in data.get("devices", [])}
    links = {(l["src"], l["dst"]): l for l in data.get("links", [])}
    return devices, links


def average_link_stats(links):
    if not links:
        return 1.0, 0.0
    bws = [l["bw_gbps"] for l in links.values() if l.get("bw_gbps")]
    lats = [l["latency_ms"] for l in links.values() if l.get("latency_ms") is not None]
    avg_bw = sum(bws) / len(bws) if bws else 1.0
    avg_lat = sum(lats) / len(lats) if lats else 0.0
    return avg_bw, avg_lat


def comm_time_seconds(edge, tasks_by_id, devices, links, avg_bw, avg_lat):
    bytes_ = edge.get("bytes", 0)
    if bytes_ <= 0:
        return 0.0
    src = tasks_by_id[edge["src"]]
    dst = tasks_by_id[edge["dst"]]
    if src.get("device") == dst.get("device"):
        return 0.0
    kind = edge.get("kind", "p2p")
    if kind == "allreduce":
        p = max(2, len(devices))
        alpha = avg_lat / 1000.0
        beta = bytes_ / (avg_bw * 1e9)
        return alpha * math.log2(p) + beta * (p - 1) / p
    link = links.get((src.get("device"), dst.get("device")))
    bw = link["bw_gbps"] if link else avg_bw
    lat = link["latency_ms"] if link else avg_lat
    return bytes_ / (bw * 1e9) + (lat / 1000.0)


def compute_time_seconds(task, devices, avg_gflops):
    flops = task.get("flops", 0)
    dev = devices.get(task.get("device"))
    gflops = dev.get("peak_gflops") if dev else avg_gflops
    gflops = gflops if gflops else avg_gflops
    return flops / (gflops * 1e9) if gflops else 0.0


def compute_totals(taskflow_path, devices, links):
    with open(taskflow_path) as f:
        data = json.load(f)
    tasks = data.get("tasks", [])
    edges = data.get("edges", [])
    tasks_by_id = {t["id"]: t for t in tasks}

    avg_bw, avg_lat = average_link_stats(links)
    gflops_list = [d.get("peak_gflops", 0) for d in devices.values() if d.get("peak_gflops")]
    avg_gflops = sum(gflops_list) / len(gflops_list) if gflops_list else 1.0

    compute_time = 0.0
    comm_time = 0.0

    for t in tasks:
        compute_time += compute_time_seconds(t, devices, avg_gflops)

    for e in edges:
        comm_time += comm_time_seconds(e, tasks_by_id, devices, links, avg_bw, avg_lat)

    return compute_time, comm_time


def parse_args():
    p = argparse.ArgumentParser(description="Plot compute vs comm share (bar).")
    p.add_argument("--hardware", required=True)
    p.add_argument("--inputs", nargs="+", required=True)
    p.add_argument("--labels", nargs="*")
    p.add_argument("--output", required=True)
    p.add_argument("--title-offset", type=int, default=240)
    return p.parse_args()


def main():
    args = parse_args()
    devices, links = load_hardware(args.hardware)
    labels = args.labels or [p.split("/")[-1].replace(".json", "") for p in args.inputs]
    if len(labels) != len(args.inputs):
        raise SystemExit("labels count must match inputs")

    rows = []
    for path, label in zip(args.inputs, labels):
        comp, comm = compute_totals(path, devices, links)
        total = comp + comm
        comm_share = (comm / total) if total > 0 else 0.0
        rows.append((label, comm_share))

    width, height = 2800, 604
    pixels = bytearray([255, 255, 255] * (width * height))

    title = "compute vs comm share per taskflow"
    draw_text(pixels, width, height, args.title_offset, 22, title, (20, 20, 20), scale=3)

    bar_left = 520
    bar_right = 2700
    bar_width = bar_right - bar_left
    bar_height = 36
    row_gap = 62
    start_y = 150

    green = (139, 195, 74)
    orange = (255, 152, 0)
    text_color = (30, 30, 30)

    for i, (label, comm_share) in enumerate(rows):
        y = start_y + i * row_gap
        draw_text(pixels, width, height, 40, y + 6, label, text_color, scale=2)

        comm_w = int(bar_width * comm_share)
        comp_w = bar_width - comm_w
        draw_rect(pixels, width, height, bar_left, y, comp_w, bar_height, green)
        draw_rect(pixels, width, height, bar_left + comp_w, y, comm_w, bar_height, orange)

        percent = f"{comm_share * 100:.1f}%"
        draw_text(pixels, width, height, bar_right + 12, y + 6, percent, text_color, scale=2)

    write_png(args.output, width, height, pixels)


if __name__ == "__main__":
    main()
