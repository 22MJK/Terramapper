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
2) tries one or more parallelization modes (`none` / `hint` / `all`) and picks the best candidate by estimated makespan, then
3) maps tasks onto devices (`heft` or `greedy`), optionally guided by partitions, and
4) exports a taskflow trace JSON without timestamps (mapper output).

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
  - `workload.h/.cpp`: `Workload` schema + tensor/task modeling -> `TaskGraph`.
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
./mapper_demo --hardware=hardware.json --workload=workload.json --mapper=heft --parallel=auto
```

Arguments:
- `--parts=P`: if `P > 0`, partition tasks into `P` blocks before mapping (default: 0 = disabled)
- `--time_unit=UNIT`: written to `taskflow.json` (default: `s`)
- `--out=PATH` / `--output=PATH`: output path for `taskflow.json` (default: `taskflow.json`)
- `--hardware=PATH`: required, load hardware topology from JSON
  - If `--time_unit` is not provided, it will use `time_unit` from the hardware JSON.
- `--workload=PATH`: required, load workload DAG from JSON
- `--mapper=heft|greedy`: mapping backend (default: `heft`)
- `--parallel=auto|none|hint|all`:
  - `auto`: evaluate `hint`/`none`/`all` and pick the lowest estimated makespan (default)
  - `none`: disable data-parallel expansion
  - `hint`: only expand tasks with `placement_hint.parallelism = data_parallel`
  - `all`: expand all tasks data-parallel across their target group (or all devices)
  - expanded shards are not hard-pinned to a specific device; mapper selects eligible devices in the group
- Visualization guardrails (enabled by default):
  - `--no-viz`: disable auto rendering entirely
  - `--viz-max-tasks=N`: skip rendering if task count exceeds `N` (default: `2500`, `<=0` disables this check)
  - `--viz-max-edges=N`: skip rendering if edge count exceeds `N` (default: `10000`, `<=0` disables this check)
  - `--viz-force`: force rendering even if graph is over thresholds
  - `--viz-summary=PATH`: optionally write visualizer text summary when rendering is skipped

Outputs:
- `taskflow.json`: tasks and edges (no timestamps)
- terminal schedule summary after each run:
  - graph shape (`tasks`, `edges`, `dag_depth`, `sources`, `sinks`)
  - communication totals (`transfer_bytes`, `cross_device_transfer_bytes`, cross-device ratio)
  - top task subtypes, tasks per device, top communication kinds by bytes
  - suggested abstraction commands for large/sparse-readable schedule views

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
  "version": 2,
  "device_groups": [
    { "id": "all_devices", "members": "all" }
  ],
  "tensors": [
    {
      "id": "A",
      "name": "A",
      "dtype": "fp32",
      "shape": [1048576, 1048576],
      "num_elements": 28311552,
      "size_bytes": 113246208,
      "distribution": { "kind": "block", "axis": 0, "group": "all_devices" },
      "partition": { "type": "block", "axis": 0, "num_parts": 8 },
      "access_pattern": "sparse_csr",
      "producer": null
    }
  ],
  "tasks": [
    {
      "id": 0,
      "name": "spmv_Ap",
      "op": "spmv",
      "compute_flops": 50,
      "inputs": [
        { "tensor": "A", "access": "dense" }
      ],
      "outputs": [
        { "tensor": "Ap" }
      ],
      "placement_hint": { "group": "all_devices" }
    }
  ]
}
```

Notes:
- Workload contains only computation; communication is inferred by the mapper.
- `tensors[]` define data semantics (shape, dtype, distribution, access patterns).
- `size_bytes` is required; `num_elements` is optional.
- `partition` describes the logical split used for local/global access.
- `tasks[]` consume/produce tensors; dependencies are implied by tensor `producer` and task inputs.
- `distribution.kind`: `none | replicated | block | cyclic`.
- `access_pattern`: `dense | sparse_csr | row-wise | col-wise`.
- `inputs[].access`: `dense | sparse_csr | row-wise | col-wise`. Use `role` to distinguish operands (e.g. dot).
- `replication.mode`: `broadcast | cached` for replicated tensors.
- `collective_hint.type`: `allreduce | allgather | reducescatter | broadcast | reduce | alltoall`.
- Data-parallel expansion can infer merge/fanout collectives on edges (`allgather` / `broadcast` / `alltoall`) when hints are absent.
- Collective cost is charged once per logical tensor event (including merge/fanout), then shared by dependent edges.
- Data-parallel split ratio is inferred from heterogeneous device capability (compute + memory bandwidth), not uniform-only split.

## taskflow.json schema (current)

- `time_unit`: string (metadata; mapper output does not include times)
- `tasks[]`:
  - `id`: unique integer task ID
  - `kind`: `compute` or `communication`
  - `subtype`: optional string (forwarded from workload)
  - `name`: task name
  - `flops`: compute amount (FLOPs)
  - `device`: mapped device ID (must exist in your simulator's `hardware.json`)
- `edges[]`:
  - `id`: unique integer edge ID
  - `src` / `dst`: task IDs (integers)
  - `bytes`: inferred communication bytes (from tensor metadata)
  - `kind`: communication primitive (`p2p`, `allreduce`, `allgather`, `reducescatter`, `broadcast`, `reduce`, `alltoall`)
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

For large graphs, the visualizer now auto-skips rendering by default and prints a schedule summary + suggested abstraction commands.
You can control this behavior directly:

```bash
python3 visualize/taskflow_viz.py --input taskflow.json --max-nodes 2500 --max-edges 10000 --summary taskflow.schedule_summary.txt
python3 visualize/taskflow_viz.py --input taskflow.json --max-nodes 2500 --max-edges 10000 --quiet-skip-summary
python3 visualize/taskflow_viz.py --input taskflow.json --force-render --output taskflow.svg
```

`--quiet-skip-summary` is useful when another component (for example `mapper_demo`) already prints a run summary and you only want the skip warning.

Render `workload.json` with Graphviz:

```bash
python3 visualize/workload_viz.py --input workload.json --output workload.dot
python3 visualize/workload_viz.py --input workload.json --png workload.png
```

## Extending

Common extension points:
- Add richer workload parsers (extend `workload/json_io.{h,cpp}`).
- Implement additional `mapping::Mapper` strategies (e.g., comm-aware mapping).
- Implement different taskflow export conventions (edit `taskflow/taskflow.{h,cpp}`).
