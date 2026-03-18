# mapper (C++)

This project is a small, runnable C++20 "mapping -> scheduling -> trace" pipeline inspired by
FlexFlow/AstraSim-style simulation stacks.

`TaskGraph` + `HardwareTopology` -> `MappingPlan` -> `SchedulePlan` -> `Trace`

## What It Does

Given:
- a `mapping::TaskGraph` (DAG of tasks + dependencies), and
- a `hardware_topology::HardwareTopology` (compute nodes + network links)

the demo binary:
1) generates a synthetic workload (a chain of stages),
2) maps tasks onto nodes (greedy load-balancing), optionally guided by partitions,
3) schedules tasks with dependency/communication readiness constraints, and
4) prints a trace (one line per scheduled task).

## Directory Layout

- `hardware_topology/`:
  - `topology.h/.cpp`: `ComputeNode`, `Link`, and `HardwareTopology` (bandwidth/latency queries).
- `mapping/`:
  - `graph.h/.cpp`: `Task`, `TaskEdge`, `TaskGraph` (topological order, deps/succ queries).
  - `mapper.h/.cpp`: `Mapper` interface, `GreedyMapper`, and `PartitionerMapper`.
  - `strategies.h/.cpp`: `PartitionStrategy` and `LayerPartition` (splits topo-order into parts).
- `schedule/`:
  - `scheduler.h/.cpp`: `SimpleScheduler` emits `SchedulePlan` considering dependencies and (simple) comm.
- `trace_generator/`:
  - `trace.h/.cpp`: `TraceGenerator` and `TraceWriter` for printing.
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

Output is a set of trace lines (one per task):

`start_time | node | task | duration`

Example output:

```text
0.000 | node_0 | stage_0 | 0.005
0.006 | node_1 | stage_1 | 0.007
```

## Model Notes (Units/Assumptions)

This code intentionally stays lightweight and does not attempt to be a full simulator.

- **Compute time**: `duration = task.compute_flops / node.gflops`
- **Communication time** (only when dependency crosses nodes):
  - transfer: `(tensor_size_mb / 1024) / bandwidth_gbps`
  - latency: `latency_ms / 1000`
  - both are added to the predecessor finish time to compute readiness
- **Scheduling policy**: each node executes tasks serially; a task starts when:
  - the target node is free, and
  - all dependencies are ready (including cross-node comm).

## Extending

Common extension points:
- Add real DAG workloads (replace `workload::WorkloadGenerator` in `workload/workload.{h,cpp}`).
- Implement additional `mapping::Mapper` strategies (e.g., comm-aware mapping).
- Implement richer scheduling policies (e.g., per-node queues, overlapping comm/compute, priorities).
- Add trace serialization formats (e.g., JSON/Chrome trace) in `trace_generator/`.
