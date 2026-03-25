#!/usr/bin/env python3
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys


def load_workload(path: Path):
    with path.open() as f:
        return json.load(f)


def make_label(task):
    tid = task.get("id")
    name = task.get("name", "")
    ttype = task.get("type", "")
    subtype = task.get("subtype", "")
    if subtype:
        return f"{tid}: {name}\\n{ttype}/{subtype}"
    return f"{tid}: {name}\\n{ttype}"


def to_dot(data, include_bytes=True):
    tasks = data.get("tasks", [])
    tensors = data.get("tensors", [])
    tensor_map = {t.get("id"): t for t in tensors if isinstance(t, dict) and t.get("id") is not None}
    lines = ["digraph workload {", "  rankdir=LR;"]
    for t in tasks:
        label = make_label(t).replace('"', '')
        lines.append(f"  T{t['id']} [label=\"{label}\"]; ")

    for t in tasks:
        dst = t.get("id")
        inputs = t.get("inputs", [])
        for inp in inputs:
            if not isinstance(inp, dict):
                continue
            tensor_id = inp.get("tensor")
            if tensor_id is None:
                continue
            tensor = tensor_map.get(tensor_id)
            if not tensor:
                continue
            producer = tensor.get("producer")
            if producer is None:
                continue
            if include_bytes:
                bytes_val = tensor.get("bytes", 0)
                lines.append(f"  T{producer} -> T{dst} [label=\"{bytes_val}B\"]; ")
            else:
                lines.append(f"  T{producer} -> T{dst}; ")

    lines.append("}")
    return "\n".join(lines) + "\n"


def write_png_from_dot(dot_text: str, png_path: Path) -> int:
    dot = shutil.which("dot")
    if not dot:
        print("error: Graphviz 'dot' not found; install graphviz.", file=sys.stderr)
        return 2
    proc = subprocess.run([dot, "-Tpng", "-o", str(png_path)], input=dot_text.encode("utf-8"))
    return proc.returncode


def parse_args():
    p = argparse.ArgumentParser(description="Visualize workload.json as Graphviz DOT or PNG.")
    p.add_argument("--input", required=True, help="Path to workload.json")
    p.add_argument("--output", help="Output file; if omitted, prints to stdout")
    p.add_argument("--png", help="Write PNG to this path (requires dot)")
    p.add_argument("--no-bytes", action="store_true", help="Do not label edges with comm_bytes (if present)")
    return p.parse_args()


def main():
    args = parse_args()
    data = load_workload(Path(args.input))
    include_bytes = not args.no_bytes
    output = to_dot(data, include_bytes=include_bytes)

    if args.png:
        sys.exit(write_png_from_dot(output, Path(args.png)))

    if args.output:
        Path(args.output).write_text(output)
    else:
        print(output, end="")


if __name__ == "__main__":
    main()
