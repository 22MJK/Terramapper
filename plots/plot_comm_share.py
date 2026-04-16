#!/usr/bin/env python3
import argparse
import json
import math
import os
import subprocess
from collections import defaultdict


def load_hardware(path):
    with open(path) as f:
        data = json.load(f)
    devices = {d['id']: d for d in data.get('devices', [])}
    links = {(l['src'], l['dst']): l for l in data.get('links', [])}
    return devices, links


def average_link_stats(links):
    if not links:
        return 1.0, 0.0
    bws = [l['bw_gbps'] for l in links.values() if l.get('bw_gbps')]
    lats = [l['latency_ms'] for l in links.values() if l.get('latency_ms') is not None]
    avg_bw = sum(bws) / len(bws) if bws else 1.0
    avg_lat = sum(lats) / len(lats) if lats else 0.0
    return avg_bw, avg_lat


def iter_of_task_name(name):
    base = name.split('@', 1)[0]
    parts = base.split('_')
    try:
        return int(parts[-1])
    except Exception:
        return 0


def comm_time_seconds(edge, tasks_by_id, devices, links, avg_bw, avg_lat):
    bytes_ = edge.get('bytes', 0)
    if bytes_ <= 0:
        return 0.0
    src = tasks_by_id[edge['src']]
    dst = tasks_by_id[edge['dst']]
    if src.get('device') == dst.get('device'):
        return 0.0
    kind = edge.get('kind', 'p2p')
    if kind == 'allreduce':
        # Simple ring model: alpha*log2(P) + beta*(P-1)/P
        p = max(2, len(devices))
        alpha = avg_lat / 1000.0
        beta = bytes_ / (avg_bw * 1e9)
        return alpha * math.log2(p) + beta * (p - 1) / p
    link = links.get((src.get('device'), dst.get('device')))
    bw = link['bw_gbps'] if link else avg_bw
    lat = link['latency_ms'] if link else avg_lat
    return bytes_ / (bw * 1e9) + (lat / 1000.0)


def compute_time_seconds(task, devices, avg_gflops):
    flops = task.get('flops', 0)
    dev = devices.get(task.get('device'))
    gflops = dev.get('peak_gflops') if dev else avg_gflops
    gflops = gflops if gflops else avg_gflops
    return flops / (gflops * 1e9) if gflops else 0.0


def compute_comm_share(taskflow_path, devices, links):
    with open(taskflow_path) as f:
        data = json.load(f)
    tasks = data.get('tasks', [])
    edges = data.get('edges', [])
    tasks_by_id = {t['id']: t for t in tasks}

    avg_bw, avg_lat = average_link_stats(links)
    gflops_list = [d.get('peak_gflops', 0) for d in devices.values() if d.get('peak_gflops')]
    avg_gflops = sum(gflops_list) / len(gflops_list) if gflops_list else 1.0

    compute_time = defaultdict(float)
    comm_time = defaultdict(float)

    for t in tasks:
        it = iter_of_task_name(t.get('name', ''))
        compute_time[it] += compute_time_seconds(t, devices, avg_gflops)

    for e in edges:
        dst_task = tasks_by_id.get(e.get('dst'))
        if not dst_task:
            continue
        it = iter_of_task_name(dst_task.get('name', ''))
        comm_time[it] += comm_time_seconds(e, tasks_by_id, devices, links, avg_bw, avg_lat)

    iters = sorted(set(compute_time.keys()) | set(comm_time.keys()))
    shares = []
    for it in iters:
        c = compute_time[it]
        m = comm_time[it]
        total = c + m
        shares.append(m / total if total > 0 else 0.0)
    return iters, shares


def write_dot(series, labels, output_path, font, fontsize):
    # Simple line chart using fixed positions with neato.
    # Coordinate system: x in [0, n-1], y in [0,1].
    max_len = max((len(s[0]) for s in series), default=1)
    x_scale = 1.0
    y_scale = 5.0

    colors = ['#1f77b4', '#ff7f0e', '#2ca02c', '#d62728', '#9467bd']
    dot_path = output_path + '.dot'
    legend_x = (max_len - 1) * x_scale * 0.7
    legend_y = y_scale * 0.95

    with open(dot_path, 'w') as f:
        f.write('graph G {\n')
        f.write('  layout=neato;\n')
        f.write('  overlap=false;\n')
        f.write('  splines=true;\n')
        f.write('  outputorder=edgesfirst;\n')
        f.write(f'  fontname="{font}";\n')
        f.write(f'  fontsize={fontsize};\n')
        f.write(f'  node [fontname="{font}", fontsize={fontsize}];\n')
        f.write(f'  edge [penwidth=2];\n')

        # Title aligned with legend (shifted slightly right)
        title_x = legend_x + 0.3
        title_y = y_scale * 1.15
        f.write(f'  title [shape=plaintext, label="Communication Share per Iteration", pos="{title_x:.2f},{title_y:.2f}!"];\n')

        # Axes labels
        f.write(f'  xlabel [shape=plaintext, label="Iteration", pos="{(max_len-1)*x_scale/2:.2f},{-0.6*y_scale:.2f}!"];\n')
        f.write(f'  ylabel [shape=plaintext, label="Communication share (time fraction)", pos="{-0.8*x_scale:.2f},{0.5*y_scale:.2f}!"];\n')

        # Axis lines
        f.write(f'  xaxis [shape=plaintext, label="", pos="{(max_len-1)*x_scale/2:.2f},{-0.1*y_scale:.2f}!"];\n')
        f.write(f'  yaxis [shape=plaintext, label="", pos="{-0.1*x_scale:.2f},{0.5*y_scale:.2f}!"];\n')
        f.write(f'  xaxis -- yaxis [style=invis];\n')

        # Ticks
        for i in range(max_len):
            f.write(f'  xtick{i} [shape=plaintext, label="{i}", pos="{i*x_scale:.2f},{-0.3*y_scale:.2f}!"];\n')
        for t in range(6):
            y = (t/5.0) * y_scale
            f.write(f'  ytick{t} [shape=plaintext, label="{t/5.0:.1f}", pos="{-0.4*x_scale:.2f},{y:.2f}!"];\n')

        # Series lines
        for si, (iters, shares) in enumerate(series):
            color = colors[si % len(colors)]
            prev = None
            for i, (it, share) in enumerate(zip(iters, shares)):
                x = it * x_scale
                y = share * y_scale
                name = f's{si}_p{i}'
                f.write(f'  {name} [shape=circle, label="", width=0.08, fixedsize=true, color="{color}", pos="{x:.2f},{y:.2f}!"];\n')
                if prev is not None:
                    f.write(f'  {prev} -- {name} [color="{color}"];\n')
                prev = name

        # Legend
        for si, label in enumerate(labels):
            color = colors[si % len(colors)]
            ly = legend_y - si * 0.4
            f.write(f'  leg{si} [shape=plaintext, label="{label}", pos="{legend_x+0.3:.2f},{ly:.2f}!"];\n')
            f.write(f'  legpt{si} [shape=circle, label="", width=0.08, fixedsize=true, color="{color}", pos="{legend_x:.2f},{ly:.2f}!"];\n')
            f.write(f'  legpt{si} -- leg{si} [style=invis];\n')

        f.write('}\n')

    return dot_path


def render_png(dot_path, output_path):
    subprocess.run(['neato', '-n2', '-Tpng', dot_path, '-o', output_path], check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--hardware', required=True)
    ap.add_argument('--inputs', nargs='+', required=True)
    ap.add_argument('--labels', nargs='*')
    ap.add_argument('--output', required=True)
    ap.add_argument('--font', default='Helvetica')
    ap.add_argument('--fontsize', type=int, default=18)
    args = ap.parse_args()

    devices, links = load_hardware(args.hardware)

    labels = args.labels or [os.path.basename(p) for p in args.inputs]
    if len(labels) != len(args.inputs):
        raise SystemExit('labels count must match inputs')

    series = []
    for path in args.inputs:
        series.append(compute_comm_share(path, devices, links))

    dot_path = write_dot(series, labels, args.output, args.font, args.fontsize)
    render_png(dot_path, args.output)


if __name__ == '__main__':
    main()
