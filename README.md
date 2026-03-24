# mapper (C++)

This project is a small, runnable C++20 "mapping -> trace export" pipeline inspired by
FlexFlow/AstraSim-style simulation stacks.

`Workload` + `HardwareTopology` -> `MappingPlan` -> `taskflow.json`

## What It Does

Given:
- a `workload::Workload` (converted internally to a `mapping::TaskGraph`), and
- a `hardware_topology::HardwareTopology` (devices + directed links)

the demo binary:
1) loads hardware and workload from JSON,
2) maps tasks onto devices (greedy load-balancing), optionally guided by partitions, and
3) exports a taskflow trace JSON without timestamps (mapper output).

## Directory Layout

- `hardware_topology/`:
  - `topology.h/.cpp`: `Device`, `Link`, and `HardwareTopology` (bandwidth/latency queries + routing).
  - `json_io.h/.cpp`: load `hardware.json` into `HardwareTopology`.
- `mapping/`:
  - `graph.h/.cpp`: `Task`, `TaskEdge`, `TaskGraph` (topological order, deps/succ queries).
  - `mapper.h/.cpp`: `Mapper` interface, `GreedyMapper`, and `PartitionerMapper`.
  - `strategies.h/.cpp`: `PartitionStrategy` and `LayerPartition` (splits topo-order into parts).
- `mapper/`:
  - `mapper.h/.cpp`: project entrypoint (`mapper::write_taskflow`) that binds tasks to devices and emits `taskflow.json`.
- `taskflow/`:
  - `taskflow.h/.cpp`: writer for `taskflow.json`.
  - `json.h/.cpp`: minimal JSON utilities (no external deps).
- `workload/`:
  - `workload.h/.cpp`: `WorkloadGenerator` and `Workload` -> `TaskGraph`.
  - `json_io.h/.cpp`: load `workload.json` into `Workload`.
- `main.cpp`: demo binary wiring everything end-to-end.

## Build

This repo ships with a simple Makefile (no CMake dependency).

Requirements:
- A C++20 compiler (`clang++` or `g++`)
- `make`

```bash
make
```

Clean:

```bash
make clean
```

## Run

```bash
./mapper_demo --hardware=hardware.json --workload=workload.json
./mapper_demo --hardware=hardware.json --workload=workload.json --parts=3
```

Arguments:
- `--parts=P`: if `P > 0`, partition tasks into `P` blocks before mapping (default: 0 = disabled)
- `--time_unit=UNIT`: written to `taskflow.json` (default: `s`)
- `--out=PATH`: output path for `taskflow.json` (default: `taskflow.json`)
- `--hardware=PATH`: required, load hardware topology from JSON
  - If `--time_unit` is not provided, it will use `time_unit` from the hardware JSON.
- `--workload=PATH`: required, load workload DAG from JSON

Outputs:
- `taskflow.json`: tasks and edges (no timestamps)

## hardware.json schema (current)

```json
{
  "time_unit": "s",
  "devices": [
    {
      "id": "dev_0",
      "name": "device_0",
      "type": "gpu",
      "peak_gflops": 10000,
      "mem_bw_gbps": 900,
      "max_concurrent": 4
    }
  ],
  "links": [
    {
      "id": "link_dev_0_to_dev_1",
      "src": "dev_0",
      "dst": "dev_1",
      "bw_gbps": 200,
      "latency_ms": 0.5
    }
  ]
}
```

Notes:
- Links are directed. For bidirectional connectivity, include both directions.

## workload.json schema (current)

```json
{
  "name": "demo",
  "tasks": [
    {
      "id": 0,
      "name": "spmv_Ap",
      "type": "compute",
      "subtype": "spmv",
      "compute_flops": 50,
      "comm_bytes": 0,
      "dependencies": []
    },
    {
      "id": 1,
      "name": "allreduce_alpha",
      "type": "communication",
      "subtype": "allreduce",
      "compute_flops": 0,
      "comm_bytes": 4096,
      "dependencies": [0]
    }
  ]
}
```

Notes:
- `type` is required and must be `compute`. Communication tasks are inserted by the mapper.
- `subtype` is optional and free-form (e.g. `spmv`).
- Supported compute subtypes: `spmv`, `dot`, `axpy`, `scalar`.
- `compute_flops` and `comm_bytes` are optional; if omitted they default to `0.0`.
- Use `dependencies` (task IDs) to express DAG edges; communication is represented by taskflow
  edges when a dependency crosses devices.
- For each dependency edge, the mapper uses the dependent task's `comm_bytes` if it is non-zero;
  otherwise the edge carries `0` bytes. If the two tasks are mapped to the same device, the
  resulting taskflow edge will have `bytes = 0` and an empty route.

## taskflow.json schema (current)

- `time_unit`: string (metadata; mapper output does not include times)
- `tasks[]`:
  - `id`: unique integer task ID
  - `kind`: `compute` or `communication`
  - `subtype`: optional string (forwarded from workload)
  - `name`: task name
  - `flops`: compute amount (FLOPs)
  - `bytes`: communication bytes (from `Task.comm_bytes`)
  - `device`: mapped device ID (must exist in your simulator's `hardware.json`)
- `edges[]`:
  - `id`: unique integer edge ID
  - `src` / `dst`: task IDs (integers)
  - `bytes`: communication bytes (from `TaskEdge.tensor_bytes`)
  - `route`: array of link IDs (multi-hop allowed). If empty and `src/dst` are on different devices,
    the consumer can attempt a direct single-hop link.

## Visualization

Use the Python visualizer to render `taskflow.json` as Mermaid or Graphviz DOT.

```bash
python3 visualize/taskflow_viz.py --input taskflow.json --format mermaid --output taskflow.mmd
python3 visualize/taskflow_viz.py --input taskflow.json --format mermaid --group-by-device
python3 visualize/taskflow_viz.py --input taskflow.json --format dot --output taskflow.dot
python3 visualize/taskflow_viz.py --input taskflow.json --format dot --png taskflow.png
python3 visualize/taskflow_viz.py --input taskflow.json --format png --output taskflow.png
```

## Extending

Common extension points:
- Add real DAG workloads (replace `workload::WorkloadGenerator` in `workload/workload.{h,cpp}`).
- Implement additional `mapping::Mapper` strategies (e.g., comm-aware mapping).
- Implement different taskflow export conventions (edit `taskflow/taskflow.{h,cpp}`).
