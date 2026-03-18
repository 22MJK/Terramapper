# mapper (C++)

This project is a small, runnable C++20 "mapping -> trace export" pipeline inspired by
FlexFlow/AstraSim-style simulation stacks.

`Workload` + `HardwareTopology` -> `MappingPlan` -> `taskflow.json`

## What It Does

Given:
- a `workload::Workload` (converted internally to a `mapping::TaskGraph`), and
- a `hardware_topology::HardwareTopology` (compute nodes + network links)

the demo binary:
1) generates a synthetic workload (a chain of stages),
2) maps tasks onto nodes (greedy load-balancing), optionally guided by partitions, and
3) exports a taskflow trace JSON without timestamps (mapper output).

## Directory Layout

- `hardware_topology/`:
  - `topology.h/.cpp`: `ComputeNode`, `Link`, and `HardwareTopology` (bandwidth/latency queries).
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

Outputs:
- `taskflow.json`: tasks and edges (no timestamps)

## taskflow.json schema (current)

- `time_unit`: string (metadata; mapper output does not include times)
- `tasks[]`:
  - `id`: unique integer task ID
  - `kind`: currently `"compute"`
  - `name`: task name
  - `flops`: compute amount (FLOPs)
  - `bytes`: memory bytes (from `Task.memory_gb`)
  - `device`: mapped device ID (must exist in your simulator's `hardware.json`)
- `edges[]`:
  - `id`: unique integer edge ID
  - `src` / `dst`: task IDs (integers)
  - `bytes`: communication bytes (from `TaskEdge.tensor_size_mb`)
  - `route`: array of link IDs (multi-hop allowed). If empty and `src/dst` are on different devices,
    the consumer can attempt a direct single-hop link.

## Extending

Common extension points:
- Add real DAG workloads (replace `workload::WorkloadGenerator` in `workload/workload.{h,cpp}`).
- Implement additional `mapping::Mapper` strategies (e.g., comm-aware mapping).
- Implement different taskflow export conventions (edit `taskflow/taskflow.{h,cpp}`).
