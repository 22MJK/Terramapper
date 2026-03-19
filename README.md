# mapper (C++)

This project is a small, runnable C++20 "mapping -> trace export" pipeline inspired by
FlexFlow/AstraSim-style simulation stacks.

`Workload` + `HardwareTopology` -> `MappingPlan` -> `taskflow.json`

## What It Does

Given:
- a `workload::Workload` (converted internally to a `mapping::TaskGraph`), and
- a `hardware_topology::HardwareTopology` (devices + directed links)

the demo binary:
1) generates a synthetic workload (a chain of stages),
2) maps tasks onto nodes (greedy load-balancing), optionally guided by partitions, and
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
./mapper_demo
./mapper_demo --nodes=4 --depth=10
./mapper_demo --nodes=4 --depth=10 --parts=3
```

Arguments:
- `--nodes=N`: number of compute nodes in the synthetic topology (default: 2)
- `--depth=D`: number of stages in the synthetic workload (default: 6)
- `--parts=P`: if `P > 0`, partition tasks into `P` blocks before mapping (default: 0 = disabled)
- `--time_unit=UNIT`: written to `taskflow.json` (default: `s`)
- `--out=PATH`: output path for `taskflow.json` (default: `taskflow.json`)
- `--hardware=PATH`: load hardware topology from JSON; if set, `--nodes` is ignored
  - If `--time_unit` is not provided, it will use `time_unit` from the hardware JSON.
- `--workload=PATH`: load workload DAG from JSON; if set, `--depth` is ignored

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
      "name": "stage_0",
      "type": "compute",
      "subtype": "spmv",
      "compute_flops": 50,
      "comm_bytes": 0,
      "dependencies": []
    }
  ],
  "edges": [
    {
      "src": 0,
      "dst": 1,
      "bytes": 4194304
    }
  ]
}
```

Notes:
- `type` is required and must be `compute` or `communication`.
- `subtype` is optional and free-form (e.g. `spmv`, `allreduce`).
- `compute_flops` and `comm_bytes` are optional; if omitted they default to `0.0`.
- `edges` is optional. If omitted, dependencies from each task are used and tensor size is derived from
  `comm_bytes` (if > 0), otherwise `compute_flops * 0.1 * 1024 * 1024` bytes to preserve previous behavior.
- If `edges` is present, it is used verbatim.

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

## Extending

Common extension points:
- Add real DAG workloads (replace `workload::WorkloadGenerator` in `workload/workload.{h,cpp}`).
- Implement additional `mapping::Mapper` strategies (e.g., comm-aware mapping).
- Implement different taskflow export conventions (edit `taskflow/taskflow.{h,cpp}`).
