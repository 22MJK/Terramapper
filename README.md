# mapper

## 简介
- C++20 任务映射与追踪生成工具
- 读取 `workload` 与 `hardware topology`，输出 `taskflow.json` 与可视化结果

## 功能
- 加载硬件拓扑与工作负载 JSON
- 支持 `heft`、`peft`、`greedy` 映射
- 支持 `auto/none/hint/all` 并行模式
- 生成调度摘要与 SVG 可视化

## 安装
- 环境：`clang++` 或 `g++`、`make`、`python3`
- 构建：`make`

## 使用
- 运行：`./mapper_demo --hardware=hardware_topology/cg_hardware.json --workload=workload/cg_iteration_workload.json`
- 指定输出：`--out=taskflow.json`
- 渲染图：`python3 visualize/taskflow_viz.py --input taskflow.json --output taskflow.svg`

## 示例
- `./mapper_demo --hardware=hardware_topology/cg_hardware_4gpu.json --workload=workload/cg_iteration_workload_large.json --mapper=heft --parallel=auto`
- `./mapper_demo --hardware=hardware_topology/cg_hardware_4gpu.json --workload=workload/cg_iteration_workload_large.json --mapper=peft --parallel=auto`

## Workload 粗化
- `python3 scripts/coarsen_supernodal_workload.py inputworkload/workload_3.json -o inputworkload/workload_3_coarse.json`
- 该脚本会把 `POTRF + outgoing TRSM/GEMM` 融合为一个 `supernode_macro`，适合把极细粒度的 supernodal workload 粗化后再交给 mapper。
- 生成后的 coarse workload 会为 `supernode_macro` 自动添加 `data_parallel` hint；要触发多卡展开，请使用 `--parallel=hint` 或 `--parallel=auto`。
