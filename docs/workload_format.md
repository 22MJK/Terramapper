# Workload JSON 格式规范（基于当前项目解析器）

本文档定义当前实际支持的 workload JSON 格式。

适用范围：
- workload 仅描述计算任务与依赖关系；
- 依赖由 tensor 的 `producer` 与 task 的 `inputs` 自动推导；
- 不要求在 workload 中显式建模通信节点。

## 1. 顶层对象

workload 文件必须是一个 JSON 对象。

当前支持的顶层字段：

- `name`（可选，字符串）
  - 默认值：`"workload"`。
- `device_groups`（可选，数组）
- `iteration_inputs`（可选，字符串数组）
- `iteration_outputs`（可选，字符串数组）
- `tensors`（必填，数组）
- `tasks`（必填，数组）

禁止字段：

- `edges`
  - 如果出现会直接报错：
    `Edges are not supported; use tensor producers and task inputs instead`。

说明：
- 其他未识别字段当前会被解析器忽略。

## 2. `device_groups` 格式

每个元素为对象，字段如下：

- `id`（必填，字符串，且唯一）
- `members`（必填）
  - 可以是字符串 `"all"`；
  - 或字符串数组（设备 id / 名称）。

校验规则：
- `id` 重复会报错；
- `members` 必须是 `"all"` 或字符串数组。

## 3. `tensors` 格式

每个 tensor 元素为对象，核心字段：

- `id`（必填，字符串，且唯一）
- `size_bytes`（必填，整数，>= 0）
  - 兼容历史别名：`bytes`。

可选字段：

- `name`（字符串，默认等于 `id`）
- `dtype`（字符串，默认 `"fp32"`）
  - 允许值：`"fp32"`、`"fp64"`、`"int32"`、`"int64"`。
- `shape`（整数数组）
- `num_elements`（整数，>= 0）
- `distribution`（对象）
  - `kind`：`"none"`、`"replicated"`、`"block"`、`"cyclic"`
  - `axis`：整数
  - `group`：字符串
- `partition`（对象）
  - `type`：`"none"`、`"replicated"`、`"block"`、`"cyclic"`
  - `axis`：整数
  - `num_parts`：整数，且 > 0
- `access_pattern`（字符串）
  - 允许值：`"dense"`、`"sparse_csr"`、`"row-wise"`、`"col-wise"`
  - 默认值：`"dense"`
- `replication`（对象）
  - `mode`：`"broadcast"` 或 `"cached"`
- `collective_hint`（对象）
  - `type`（字符串）、`op`（字符串）、`group`（字符串）
  - 仅当 `type` 非空时生效。
- `producer`（整数 task id）
  - 若省略或为 `null`，表示外部/状态输入张量。

特殊默认行为：
- 若 `distribution.kind == "replicated"` 且未提供 `replication`，
  解析器会默认 `replication.mode = "cached"`。

校验规则：
- 缺失 `id` 或 `size_bytes` 会报错；
- tensor `id` 重复会报错；
- 需要整数的字段若是非法浮点或越界会报错。

## 4. `tasks` 格式

每个 task 元素为对象，必填字段：

- `id`（必填，整数，且唯一）
- `name`（必填，字符串，且唯一）
- `op`（必填，字符串）

可选字段：

- `compute_flops`（数字，默认 `0.0`）
- `memory_bytes`（数字，默认 `0.0`）
  - 若 > 0，映射阶段可直接使用该值建模内存开销；
  - 若未提供或为 0，当前实现会根据输入/输出 tensor 大小自动估算。
- `inputs`（数组，对象元素）
  - 每个 input 对象：
    - `tensor`（必填，字符串，tensor id）
    - `role`（可选，字符串）
    - `access`（可选，与 tensor `access_pattern` 同枚举）
    - `access_pattern`（可选，与 `access` 同义）
      - 若两者都提供，以后解析到的值覆盖前者。
- `outputs`（数组）
  - 每个 output 可以是：
    - 字符串（tensor id），或
    - 对象（需包含 `tensor` 字段）。
- `placement_hint`（对象）
  - `group`（可选，字符串）
  - `parallelism`（可选，字符串）

校验规则：
- `id` / `name` / `op` 缺失会报错；
- task `id` 或 `name` 重复会报错；
- 非法输入/输出结构会报错。

## 5. 依赖关系语义（重点）

当前 workload 不读取 `edges` / `dependencies` 字段。
依赖由以下规则推导：

1. 遍历每个 task 的 `inputs`：
   - 若输入 tensor 的 `producer` 存在：
     - 建立依赖边 `producer_task -> current_task`；
   - 若 `producer` 缺失或 `null`：
     - 不建立前驱边（视为外部/状态输入）。

2. 中间图中的通信类型：
   - 默认 `p2p`；
   - 若 tensor 含 `collective_hint.type`，则使用该类型（如 `allreduce`）。

3. 边上的通信量：
   - 从 workload 转换到 TaskGraph 时初始为 0；
   - 由 mapper 在映射阶段进一步推断并填充。

## 6. 迭代型 workload

- `iteration_inputs` 与 `iteration_outputs` 为可选字符串数组；
- 会被解析并保存在 `Workload` 中；
- 适用于表达跨迭代状态流（如 CG 中 `x,r,p,rr_old -> x_next,r_next,p_next,rr_new`）。

## 7. 最小可用示例

```json
{
  "name": "demo",
  "tensors": [
    { "id": "a", "size_bytes": 1024, "producer": null },
    { "id": "b", "size_bytes": 1024, "producer": 0 }
  ],
  "tasks": [
    {
      "id": 0,
      "name": "task0",
      "op": "spmv",
      "compute_flops": 1000,
      "inputs": [{ "tensor": "a" }],
      "outputs": [{ "tensor": "b" }]
    },
    {
      "id": 1,
      "name": "task1",
      "op": "dot",
      "compute_flops": 200,
      "inputs": [{ "tensor": "b" }],
      "outputs": []
    }
  ]
}
```

## 8. 生成端建议

- 尽量保持稳定且唯一的 task/tensor id；
- 保证 `producer` 与对应 task `outputs` 语义一致；
- 不要输出 `edges` 字段；
- 若希望 mapper 推断 collective 通信，请使用 `collective_hint` 描述。
