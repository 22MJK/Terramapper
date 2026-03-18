# mapper (C++)

Minimal end-to-end pipeline:

`TaskGraph` + `HardwareTopology` -> `MappingPlan` -> `SchedulePlan` -> `Trace`

## Build

This repo ships with a simple Makefile (no CMake dependency).

```bash
make
```

## Run

```bash
./mapper_demo
./mapper_demo --nodes=4 --depth=10
./mapper_demo --nodes=4 --depth=10 --parts=3
```

Output is a set of trace lines:

`start_time | node | task | duration`
