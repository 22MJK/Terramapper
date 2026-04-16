#!/usr/bin/env python3
import argparse
from collections import Counter
import json
from pathlib import Path
import shlex
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

def _parse_base_and_iter(name: str):
    base = name.split("@", 1)[0]
    parts = base.split("_")
    if parts and parts[-1].isdigit():
        return "_".join(parts[:-1]), int(parts[-1])
    return base, None

def abstract_taskflow(data, mode: str):
    if mode == "none":
        return data

    tasks = data.get("tasks", [])
    edges = data.get("edges", [])

    groups = {}
    for t in tasks:
        name = str(t.get("name", ""))
        base, it = _parse_base_and_iter(name)
        subtype = t.get("subtype", "") or base
        device = str(t.get("device", "?"))

        if mode == "op":
            key = (subtype,)
        elif mode == "iter":
            key = (it if it is not None else -1,)
        elif mode == "device":
            key = (device,)
        else:  # iter_op
            key = (it if it is not None else -1, subtype)

        if key not in groups:
            groups[key] = {
                "task_ids": [],
                "subtype": subtype,
                "iter": it,
                "devices": set(),
                "flops": 0,
                "name": base,
            }
        g = groups[key]
        g["task_ids"].append(t["id"])
        g["devices"].add(device)
        g["flops"] += t.get("flops", 0)

    group_ids = {k: idx for idx, k in enumerate(sorted(groups.keys()))}
    task_to_group = {}
    for k, g in groups.items():
        gid = group_ids[k]
        for tid in g["task_ids"]:
            task_to_group[tid] = gid

    abstract_tasks = []
    for k, g in groups.items():
        gid = group_ids[k]
        devs = sorted(g["devices"])
        dev_label = ",".join(devs)
        if len(devs) > 3:
            dev_label = f"{len(devs)} devices"

        if mode == "op":
            label = f"{g['subtype']}\\ncount={len(g['task_ids'])}\\n{dev_label}"
        elif mode == "iter":
            label = f"iter {g['iter']}\\ncount={len(g['task_ids'])}\\n{dev_label}"
        elif mode == "device":
            label = f"device {dev_label}\\ncount={len(g['task_ids'])}"
        else:
            label = f"iter {g['iter']} | {g['subtype']}\\ncount={len(g['task_ids'])}\\n{dev_label}"

        abstract_tasks.append({
            "id": gid,
            "kind": "compute",
            "name": label,
            "subtype": g["subtype"],
            "device": dev_label,
            "flops": g["flops"],
        })

    edge_agg = {}
    for e in edges:
        src = e.get("src")
        dst = e.get("dst")
        if src not in task_to_group or dst not in task_to_group:
            continue
        gs = task_to_group[src]
        gd = task_to_group[dst]
        if gs == gd:
            continue
        key = (gs, gd)
        if key not in edge_agg:
            edge_agg[key] = {
                "src": gs,
                "dst": gd,
                "bytes": 0,
                "kinds": set(),
            }
        edge_agg[key]["bytes"] += e.get("bytes", 0)
        kind = e.get("kind")
        if kind:
            edge_agg[key]["kinds"].add(kind)

    abstract_edges = []
    for idx, item in enumerate(edge_agg.values()):
        kinds = item["kinds"]
        if len(kinds) == 1:
            kind = next(iter(kinds))
        elif len(kinds) == 0:
            kind = ""
        else:
            kind = "mixed"
        abstract_edges.append({
            "id": idx,
            "src": item["src"],
            "dst": item["dst"],
            "bytes": item["bytes"],
            "kind": kind,
            "route": [],
        })

    return {
        "time_unit": data.get("time_unit", ""),
        "tasks": abstract_tasks,
        "edges": abstract_edges,
    }

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
        kind = e.get("kind", "")
        if kind:
            label_parts.append(str(kind))
        if include_bytes and bytes_val:
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
        kind = e.get("kind", "")
        if kind:
            label_parts.append(str(kind))
        if include_bytes and bytes_val:
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
    p = argparse.ArgumentParser(description="Visualize taskflow.json as Graphviz DOT/SVG/PNG or Mermaid.")
    p.add_argument("--input", required=True, help="Path to taskflow.json")
    p.add_argument("--output", help="Output file; defaults to taskflow.svg for DOT rendering")
    p.add_argument("--format", choices=["mermaid", "dot", "png"], default="dot")
    p.add_argument("--png", help="Write PNG to this path (requires dot for --format=dot or mmdc for --format=mermaid)")
    p.add_argument("--no-bytes", action="store_true", help="Do not label edges with bytes (zero bytes are hidden)")
    p.add_argument("--route", action="store_true", help="Include route list in edge labels")
    p.add_argument("--group-by-device", action="store_true", help="Group nodes into device subgraphs (mermaid only)")
    p.add_argument("--abstract", action="store_true", help="Abstract taskflow by grouping tasks")
    p.add_argument("--abstract-by", choices=["iter_op", "iter", "op", "device"], default="iter_op")
    p.add_argument("--max-nodes", type=int, default=2500, help="Skip rendering when task count exceeds this value (<=0 disables check)")
    p.add_argument("--max-edges", type=int, default=10000, help="Skip rendering when edge count exceeds this value (<=0 disables check)")
    p.add_argument("--force-render", action="store_true", help="Force rendering even if graph is larger than limits")
    p.add_argument("--summary", help="Write schedule summary text to this path")
    p.add_argument("--quiet-skip-summary", action="store_true", help="When rendering is skipped, avoid printing full summary text to stderr")
    return p.parse_args()

def _to_int(value):
    try:
        return int(value)
    except (TypeError, ValueError):
        return 0

def canonical_task_subtype(value):
    text = str(value or "unknown").strip().lower().replace("-", "_").replace(" ", "_")
    if text == "scalar":
        return "scalar_div"
    return text

def graph_stats(data):
    tasks = [t for t in data.get("tasks", []) if isinstance(t, dict)]
    edges = [e for e in data.get("edges", []) if isinstance(e, dict)]
    subtype_counter = Counter()
    device_counter = Counter()
    total_edge_bytes = 0
    kind_bytes = Counter()

    for task in tasks:
        subtype = canonical_task_subtype(task.get("subtype") or task.get("kind") or "unknown")
        device = str(task.get("device", "?"))
        subtype_counter[subtype] += 1
        device_counter[device] += 1

    for edge in edges:
        edge_bytes = _to_int(edge.get("bytes", 0))
        total_edge_bytes += edge_bytes
        if edge_bytes > 0:
            kind = str(edge.get("kind") or "unknown")
            kind_bytes[kind] += edge_bytes

    return {
        "task_count": len(tasks),
        "edge_count": len(edges),
        "device_count": len(device_counter),
        "subtype_counter": subtype_counter,
        "device_counter": device_counter,
        "kind_bytes": kind_bytes,
        "total_edge_bytes": total_edge_bytes,
    }

def build_schedule_summary(data, stats, args):
    top_subtypes = ", ".join(
        f"{name}:{count}" for name, count in stats["subtype_counter"].most_common(6)
    ) or "N/A"
    top_devices = ", ".join(
        f"{name}:{count}" for name, count in stats["device_counter"].most_common(6)
    ) or "N/A"
    top_kinds = ", ".join(
        f"{name}:{bytes_val}B" for name, bytes_val in stats["kind_bytes"].most_common(4) if bytes_val > 0
    ) or "N/A"
    in_path = shlex.quote(str(Path(args.input)))

    lines = [
        "Taskflow summary (render skipped due to graph size)",
        f"- tasks: {stats['task_count']}",
        f"- edges: {stats['edge_count']}",
        f"- devices: {stats['device_count']}",
        f"- total edge bytes: {stats['total_edge_bytes']}B",
        f"- top task subtypes: {top_subtypes}",
        f"- top communication kinds by bytes: {top_kinds}",
        f"- tasks per device: {top_devices}",
        "",
        "Suggested schedule-graph description plans:",
        f"1) Device-level abstract view: python3 visualize/taskflow_viz.py --input {in_path} --abstract --abstract-by device --output taskflow_device.svg",
        f"2) Iteration-level abstract view: python3 visualize/taskflow_viz.py --input {in_path} --abstract --abstract-by iter --output taskflow_iter.svg",
        f"3) Operator-level abstract view: python3 visualize/taskflow_viz.py --input {in_path} --abstract --abstract-by op --output taskflow_op.svg",
        f"4) Text-first Mermaid (group by device): python3 visualize/taskflow_viz.py --input {in_path} --format mermaid --group-by-device --output taskflow.mmd",
    ]
    return "\n".join(lines) + "\n"

def write_png_from_dot(dot_text: str, png_path: Path) -> int:
    dot = shutil.which("dot")
    if not dot:
        print("error: Graphviz 'dot' not found; install graphviz or use --format=mermaid with mmdc.", file=sys.stderr)
        return 2
    proc = subprocess.run([dot, "-Tpng", "-o", str(png_path)], input=dot_text.encode("utf-8"))
    return proc.returncode

def write_svg_from_dot(dot_text: str, svg_path: Path) -> int:
    dot = shutil.which("dot")
    if not dot:
        print("error: Graphviz 'dot' not found; install graphviz or use --format=mermaid with mmdc.", file=sys.stderr)
        return 2
    proc = subprocess.run([dot, "-Tsvg", "-o", str(svg_path)], input=dot_text.encode("utf-8"))
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

def to_png(data, output_path: Path, include_bytes=True):
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

        label_parts = []
        kind = e.get("kind", "")
        if kind:
            label_parts.append(str(kind))
        if include_bytes and e.get("bytes", 0):
            label_parts.append(f"{e.get('bytes', 0)}B")
        if label_parts:
            label = " ".join(label_parts)
            lx = (x0 + x1) // 2 + 6
            ly = (y0 + y1) // 2 - 6
            draw_text(pixels, width, height, lx, ly, label, (30, 30, 30), scale=1)

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
    if args.abstract:
        data = abstract_taskflow(data, args.abstract_by)
    include_bytes = not args.no_bytes
    stats = graph_stats(data)

    too_many_nodes = args.max_nodes > 0 and stats["task_count"] > args.max_nodes
    too_many_edges = args.max_edges > 0 and stats["edge_count"] > args.max_edges
    if not args.force_render and (too_many_nodes or too_many_edges):
        reasons = []
        if too_many_nodes:
            reasons.append(f"tasks={stats['task_count']} > max_nodes={args.max_nodes}")
        if too_many_edges:
            reasons.append(f"edges={stats['edge_count']} > max_edges={args.max_edges}")
        print("warning: rendering skipped for large graph (" + ", ".join(reasons) + ")", file=sys.stderr)
        summary = build_schedule_summary(data, stats, args)
        if args.summary:
            Path(args.summary).write_text(summary)
            print(f"summary written to {args.summary}", file=sys.stderr)
        if not args.quiet_skip_summary:
            print(summary, file=sys.stderr, end="")
        sys.exit(3)

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
        to_png(data, output_path, include_bytes=include_bytes)
        return

    output = to_dot(data, include_bytes=include_bytes, include_route=args.route)
    if args.png:
        sys.exit(write_png_from_dot(output, Path(args.png)))
    if args.output:
        out_path = Path(args.output)
        if out_path.suffix == ".dot":
            out_path.write_text(output)
            return
        if out_path.suffix == ".png":
            sys.exit(write_png_from_dot(output, out_path))
        sys.exit(write_svg_from_dot(output, out_path))
    sys.exit(write_svg_from_dot(output, Path("taskflow.svg")))

if __name__ == "__main__":
    main()
