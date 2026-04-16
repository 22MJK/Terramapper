# Workload 生成器

## 目标
从 SuiteSparse 获取矩阵数据，选择矩阵求解算法（如 CG / Supernodal），生成可直接被本项目 mapper 使用的 workload JSON。

**内容**：workload描述计算与数据依赖，不建模通信；

---

## 输入与输出
### 输入
- 矩阵来源：
  - SuiteSparse Collection
- 算法选择：`CG` / `Supernodal`
- 少量必要参数（如 CG 迭代次数、精度；Supernodal 的排序策略）

### 输出
- `workload.json`：符合项目格式的计算 DAG

---

## 架构概览
```
Matrix Provider  ->  Matrix Analyzer  ->  Workload Builder  ->  JSON Writer
```
- **Matrix Provider**：读取矩阵（mtx / suitesparse）
- **Matrix Analyzer**：提取规模与稀疏结构特征（rows/cols/nnz 等）
- **Workload Builder**：根据算法生成任务 DAG
- **JSON Writer**：输出 workload JSON

---

## 与 Mapper 的接口
- 参考给出的示例
- 输出的 workload JSON 不包含通信节点或通信量
- 任务包含：`id / name / type / subtype / compute_flops / inputs / outputs`
- 依赖通过 tensor 生产与消费关系表达
---

## 备选功能呢哥
- 新增更多算法（GMRES / BiCGSTAB）
- 并行化支持
- 输出统计报告（flops、任务数量、依赖密度）

