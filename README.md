# mapper

## 简介
- C++20 任务映射与追踪生成工具
- 读取 `workload` 与 `hardware topology`，输出 `taskflow.json` 与可视化结果

## 功能
- 加载硬件拓扑与工作负载 JSON
- 支持 `heft`、`greedy` 映射
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
