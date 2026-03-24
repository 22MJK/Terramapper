#!/usr/bin/env python3
import argparse
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import zlib

def load_taskflow(path: Path):
    with path.open() as f:
        return json.load(f)

def make_label(task):
    tid = task.get("id")
    name = task.get("name", "")
    kind = task.get("kind", "")
    subtype = task.get("subtype", "")
    device = task.get("device", "?")
    if subtype:
        return f"{tid}: {name}\\n{kind}/{subtype}\\n@dev {device}"
    return f"{tid}: {name}\\n{kind}\\n@dev {device}"

def to_mermaid(data, include_bytes=True, include_route=False, group_by_device=False):
    tasks = data.get("tasks", [])
    edges = data.get("edges", [])

    lines = ["flowchart LR"]

    if group_by_device:
        # Group nodes by device in subgraphs.
        device_to_tasks = {}
        for t in tasks:
            device_to_tasks.setdefault(str(t.get("device", "?")), []).append(t)
        for device, dev_tasks in device_to_tasks.items():
            lines.append(f"  subgraph dev_{device}[\"device {device}\"]")
            for t in dev_tasks:
                label = make_label(t).replace('"', '')
                lines.append(f"    T{t['id']}[\"{label}\"]")
            lines.append("  end")
    else:
        for t in tasks:
            label = make_label(t).replace('"', '')
            lines.append(f"  T{t['id']}[\"{label}\"]")

    for e in edges:
        src = e.get("src")
        dst = e.get("dst")
        bytes_val = e.get("bytes", 0)
        route = e.get("route", [])
        label_parts = []
        if include_bytes:
            label_parts.append(f"{bytes_val}B")
        if include_route and route:
            label_parts.append("route:" + ",".join(route))
        if label_parts:
            label = " ".join(label_parts)
            lines.append(f"  T{src} -->|\"{label}\"| T{dst}")
        else:
            lines.append(f"  T{src} --> T{dst}")

    return "\n".join(lines) + "\n"

def to_dot(data, include_bytes=True, include_route=False):
    tasks = data.get("tasks", [])
    edges = data.get("edges", [])
    lines = ["digraph taskflow {", "  rankdir=LR;"]
    for t in tasks:
        label = make_label(t).replace('"', '')
        lines.append(f"  T{t['id']} [label=\"{label}\"]; ")
    for e in edges:
        src = e.get("src")
        dst = e.get("dst")
        bytes_val = e.get("bytes", 0)
        route = e.get("route", [])
        label_parts = []
        if include_bytes:
            label_parts.append(f"{bytes_val}B")
        if include_route and route:
            label_parts.append("route:" + ",".join(route))
        if label_parts:
            label = " ".join(label_parts)
            lines.append(f"  T{src} -> T{dst} [label=\"{label}\"]; ")
        else:
            lines.append(f"  T{src} -> T{dst}; ")
    lines.append("}")
    return "\n".join(lines) + "\n"

def parse_args():
    p = argparse.ArgumentParser(description="Visualize taskflow.json as Mermaid, Graphviz DOT, or PNG.")
    p.add_argument("--input", required=True, help="Path to taskflow.json")
    p.add_argument("--output", help="Output file; if omitted, prints to stdout")
    p.add_argument("--format", choices=["mermaid", "dot", "png"], default="mermaid")
    p.add_argument("--png", help="Write PNG to this path (requires dot for --format=dot or mmdc for --format=mermaid)")
    p.add_argument("--no-bytes", action="store_true", help="Do not label edges with bytes")
    p.add_argument("--route", action="store_true", help="Include route list in edge labels")
    p.add_argument("--group-by-device", action="store_true", help="Group nodes into device subgraphs (mermaid only)")
    return p.parse_args()

def write_png_from_dot(dot_text: str, png_path: Path) -> int:
    dot = shutil.which("dot")
    if not dot:
        print("error: Graphviz 'dot' not found; install graphviz or use --format=mermaid with mmdc.", file=sys.stderr)
        return 2
    proc = subprocess.run([dot, "-Tpng", "-o", str(png_path)], input=dot_text.encode("utf-8"))
    return proc.returncode

def write_png_from_mermaid(mermaid_text: str, png_path: Path) -> int:
    mmdc = shutil.which("mmdc")
    if not mmdc:
        print("error: Mermaid CLI 'mmdc' not found; install @mermaid-js/mermaid-cli or use --format=dot.", file=sys.stderr)
        return 2
    proc = subprocess.run([mmdc, "-i", "-", "-o", str(png_path)], input=mermaid_text.encode("utf-8"))
    return proc.returncode

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
    " ": ["00000", "00000", "00000", "00000", "00000", "00000", "00000"],
}

def draw_text(pixels, width, height, x, y, text, color, scale=1):
    cursor_x = x
    cursor_y = y
    for ch in text:
        if ch == "\n":
            cursor_x = x
            cursor_y += (7 + 2) * scale
            continue
        glyph = FONT_5X7.get(ch, FONT_5X7["?"] if "?" in FONT_5X7 else FONT_5X7[" "])
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

def write_png(path: Path, width: int, height: int, pixels: bytearray):
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
    path.write_bytes(png)

def draw_line(pixels, width, height, x0, y0, x1, y1, color):
    dx = abs(x1 - x0)
    dy = -abs(y1 - y0)
    sx = 1 if x0 < x1 else -1
    sy = 1 if y0 < y1 else -1
    err = dx + dy
    while True:
        if 0 <= x0 < width and 0 <= y0 < height:
            idx = (y0 * width + x0) * 3
            pixels[idx:idx + 3] = color
        if x0 == x1 and y0 == y1:
            break
        e2 = 2 * err
        if e2 >= dy:
            err += dy
            x0 += sx
        if e2 <= dx:
            err += dx
            y0 += sy

def draw_rect(pixels, width, height, x, y, w, h, fill, outline):
    for py in range(y, y + h):
        if 0 <= py < height:
            row_start = py * width * 3
            for px in range(x, x + w):
                if 0 <= px < width:
                    idx = row_start + px * 3
                    pixels[idx:idx + 3] = fill
    # Outline
    for px in range(x, x + w):
        if 0 <= px < width and 0 <= y < height:
            idx = (y * width + px) * 3
            pixels[idx:idx + 3] = outline
        if 0 <= px < width and 0 <= y + h - 1 < height:
            idx = ((y + h - 1) * width + px) * 3
            pixels[idx:idx + 3] = outline
    for py in range(y, y + h):
        if 0 <= x < width and 0 <= py < height:
            idx = (py * width + x) * 3
            pixels[idx:idx + 3] = outline
        if 0 <= x + w - 1 < width and 0 <= py < height:
            idx = (py * width + x + w - 1) * 3
            pixels[idx:idx + 3] = outline

def layout_levels(data):
    tasks = {t["id"]: t for t in data.get("tasks", [])}
    edges = data.get("edges", [])
    preds = {tid: [] for tid in tasks}
    succs = {tid: [] for tid in tasks}
    indeg = {tid: 0 for tid in tasks}
    for e in edges:
        src = e.get("src")
        dst = e.get("dst")
        if src in tasks and dst in tasks:
            preds[dst].append(src)
            succs[src].append(dst)
            indeg[dst] += 1
    queue = [tid for tid, d in indeg.items() if d == 0]
    queue.sort()
    topo = []
    while queue:
        node = queue.pop(0)
        topo.append(node)
        for nxt in succs.get(node, []):
            indeg[nxt] -= 1
            if indeg[nxt] == 0:
                queue.append(nxt)
                queue.sort()
    if len(topo) != len(tasks):
        topo = sorted(tasks.keys())

    level = {}
    for node in topo:
        if not preds[node]:
            level[node] = 0
        else:
            level[node] = max(level[p] for p in preds[node]) + 1

    levels = {}
    for node, lv in level.items():
        levels.setdefault(lv, []).append(node)
    for lv in levels:
        levels[lv].sort()
    return levels

def to_png(data, output_path: Path):
    tasks = {t["id"]: t for t in data.get("tasks", [])}
    edges = data.get("edges", [])
    levels = layout_levels(data)
    max_level = max(levels.keys()) if levels else 0
    node_w, node_h = 220, 80
    gap_x, gap_y = 70, 50
    margin = 40

    max_nodes = max((len(v) for v in levels.values()), default=1)
    width = margin * 2 + (max_level + 1) * node_w + max_level * gap_x
    height = margin * 2 + max_nodes * node_h + (max_nodes - 1) * gap_y

    pixels = bytearray([255, 255, 255] * (width * height))

    positions = {}
    for lv in range(max_level + 1):
        nodes = levels.get(lv, [])
        for idx, node in enumerate(nodes):
            x = margin + lv * (node_w + gap_x)
            y = margin + idx * (node_h + gap_y)
            positions[node] = (x, y)

    # Draw edges first.
    edge_color = (80, 80, 80)
    for e in edges:
        src = e.get("src")
        dst = e.get("dst")
        if src not in positions or dst not in positions:
            continue
        sx, sy = positions[src]
        dx, dy = positions[dst]
        x0 = sx + node_w
        y0 = sy + node_h // 2
        x1 = dx
        y1 = dy + node_h // 2
        draw_line(pixels, width, height, x0, y0, x1, y1, edge_color)

    # Draw nodes.
    for tid, task in tasks.items():
        if tid not in positions:
            continue
        x, y = positions[tid]
        kind = task.get("kind", "")
        if kind == "communication":
            fill = (255, 236, 217)
        else:
            fill = (220, 235, 255)
        outline = (60, 60, 60)
        draw_rect(pixels, width, height, x, y, node_w, node_h, fill, outline)

        name = str(task.get("name", "")).lower()
        device = str(task.get("device", "?"))
        label = f"{tid}:{name}\n@{device}"
        draw_text(pixels, width, height, x + 8, y + 10, label, (20, 20, 20), scale=1)

    write_png(output_path, width, height, pixels)

def main():
    args = parse_args()
    data = load_taskflow(Path(args.input))
    include_bytes = not args.no_bytes

    if args.format == "mermaid":
        output = to_mermaid(data, include_bytes=include_bytes, include_route=args.route, group_by_device=args.group_by_device)
        if args.png:
            sys.exit(write_png_from_mermaid(output, Path(args.png)))
        if args.output:
            Path(args.output).write_text(output)
        else:
            print(output, end="")
        return

    if args.format == "png":
        output_path = Path(args.output) if args.output else Path("taskflow.png")
        to_png(data, output_path)
        return

    output = to_dot(data, include_bytes=include_bytes, include_route=args.route)
    if args.png:
        sys.exit(write_png_from_dot(output, Path(args.png)))
    if args.output:
        Path(args.output).write_text(output)
    else:
        print(output, end="")

if __name__ == "__main__":
    main()
