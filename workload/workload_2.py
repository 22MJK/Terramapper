import os
import json
import sys
import argparse
from dataclasses import dataclass
from typing import Dict, List, Any, Optional, Tuple

import ssgetpy
from scipy.io import mmread


# ============================================================================
# 配置与统计
# ============================================================================

@dataclass
class CGConfig:
    num_iterations: int = 1
    tolerance: float = 1e-6
    max_iterations: int = 1000

    def validate(self) -> Tuple[bool, str]:
        if self.num_iterations < 1:
            return False, "迭代次数必须 >= 1"
        if self.num_iterations > self.max_iterations:
            return False, f"迭代次数不能超过 {self.max_iterations}"
        if not (0.0 < self.tolerance < 1.0):
            return False, "精度 tolerance 必须在 (0, 1) 之间"
        return True, "配置有效"


@dataclass
class MatrixStats:
    name: str
    rows: int
    cols: int
    nnz: int
    is_square: bool
    bytes: Dict[str, int]
    flops: Dict[str, float]


# ============================================================================
# 1) Matrix Provider
# ============================================================================

class MatrixProvider:
    @staticmethod
    def fetch_matrix(matrix_name: Optional[str], dest_dir: str) -> str:
        os.makedirs(dest_dir, exist_ok=True)

        print("[1/4] Matrix Provider: 搜索/下载 SuiteSparse SPD 矩阵...")

        if matrix_name:
            results = ssgetpy.search(name=matrix_name, isspd=True)
        else:
            results = ssgetpy.search(isspd=True, nzbounds=(10000, 100000), limit=1)

        if not results:
            raise ValueError("未找到符合条件的 SPD 矩阵")

        m = results[0]
        print(f"       目标矩阵: {m.name} (group: {m.group})")
        print(f"       维度: {m.rows}×{m.cols}, nnz={m.nnz}")

        extracted_path, _ = m.download(destpath=dest_dir, extract=True)
        mtx_path = os.path.join(extracted_path, f"{m.name}.mtx")
        if not os.path.exists(mtx_path):
            raise FileNotFoundError(f"未找到 .mtx 文件: {mtx_path}")

        print(f"       .mtx: {mtx_path}")
        return mtx_path


# ============================================================================
# 2) Matrix Analyzer
# ============================================================================

class MatrixAnalyzer:
    @staticmethod
    def analyze(mtx_file_path: str) -> MatrixStats:
        print("[2/4] Matrix Analyzer: 读取矩阵并估算 bytes/FLOPs...")

        A = mmread(mtx_file_path)
        rows, cols = A.shape
        nnz = A.nnz

        # 这里仅判断“是否方阵”，不做昂贵的 A == A.T 检查
        is_square = (rows == cols)

        # bytes 估算（fp32 values + int32 indices）按 CSR
        float_bytes = 4
        int_bytes = 4
        matrix_bytes = nnz * float_bytes + nnz * int_bytes + (rows + 1) * int_bytes
        vector_bytes = rows * float_bytes
        scalar_bytes = float_bytes

        # FLOPs 估算（经典近似）
        spmv_flops = 2 * nnz
        dot_flops = 2 * rows
        axpy_flops = 2 * rows
        scalar_div_flops = 1.0

        stats = MatrixStats(
            name=os.path.basename(mtx_file_path).replace(".mtx", ""),
            rows=rows,
            cols=cols,
            nnz=nnz,
            is_square=is_square,
            bytes={"matrix": int(matrix_bytes), "vector": int(vector_bytes), "scalar": int(scalar_bytes)},
            flops={
                "spmv": float(spmv_flops),
                "dot": float(dot_flops),
                "axpy": float(axpy_flops),
                "scalar_div": float(scalar_div_flops),
            },
        )

        print(f"       维度: {rows}×{cols}, nnz={nnz}, is_square={is_square}")
        print(f"       FLOPs: spmv={spmv_flops}, dot={dot_flops}, axpy={axpy_flops}")
        return stats


# ============================================================================
# 3) Workload Builder (CG)
# ============================================================================

class WorkloadBuilder:
    @staticmethod
    def build_cg_workload(stats: MatrixStats, cfg: CGConfig, emit_metadata: bool) -> Dict[str, Any]:
        print("[3/4] Workload Builder: 生成 CG workload DAG...")

        b = stats.bytes
        f = stats.flops

        tensors: List[Dict[str, Any]] = []
        tasks: List[Dict[str, Any]] = []
        next_task_id = 0

        def add_tensor(
            tid: str,
            size_bytes: int,
            producer: Optional[int] = None,
            access_pattern: str = "dense",
            shape: Optional[List[int]] = None,
            dtype: str = "fp32",
        ) -> None:
            t: Dict[str, Any] = {
                "id": tid,
                "size_bytes": int(size_bytes),
                "dtype": dtype,
                "access_pattern": access_pattern,
            }
            if shape is not None:
                t["shape"] = shape
            # producer 省略表示外部/状态输入（更符合规范语义）
            if producer is not None:
                t["producer"] = int(producer)
            tensors.append(t)

        def add_task(name: str, op: str, flops: float, inputs: List[str], outputs: List[str]) -> int:
            nonlocal next_task_id
            tid = next_task_id
            next_task_id += 1
            tasks.append(
                {
                    "id": tid,
                    "name": name,
                    "op": op,
                    "compute_flops": float(flops),
                    "inputs": [{"tensor": x} for x in inputs],
                    "outputs": outputs,  # 规范允许字符串数组
                }
            )
            return tid

        # -------- 外部输入/状态输入 tensors --------
        add_tensor("A", b["matrix"], producer=None, access_pattern="sparse_csr", shape=[stats.rows, stats.cols])
        add_tensor("x", b["vector"], producer=None, shape=[stats.rows])
        add_tensor("r", b["vector"], producer=None, shape=[stats.rows])
        add_tensor("p", b["vector"], producer=None, shape=[stats.rows])
        add_tensor("r_old", b["scalar"], producer=None, shape=[1])

        # -------- 迭代展开（静态图：num_iterations 决定任务数量）--------
        prev = {"x": "x", "r": "r", "p": "p", "r_old": "r_old"}
        last_outputs = None

        for k in range(cfg.num_iterations):
            suf = f"_iter{k}"

            # q = A * p
            q = f"q{suf}"
            add_tensor(q, b["vector"], producer=None, shape=[stats.rows])
            t0 = add_task(f"spmv{suf}", "spmv", f["spmv"], ["A", prev["p"]], [q])
            tensors[-1]["producer"] = t0

            # p_dot_q = p^T q
            p_dot_q = f"p_dot_q{suf}"
            add_tensor(p_dot_q, b["scalar"], producer=None, shape=[1])
            t1 = add_task(f"dot_pdotq{suf}", "dot", f["dot"], [prev["p"], q], [p_dot_q])
            tensors[-1]["producer"] = t1

            # alpha = r_old / p_dot_q
            alpha = f"alpha{suf}"
            add_tensor(alpha, b["scalar"], producer=None, shape=[1])
            t2 = add_task(f"scalar_div_alpha{suf}", "scalar_div", f["scalar_div"], [prev["r_old"], p_dot_q], [alpha])
            tensors[-1]["producer"] = t2

            # x_next = x + alpha * p
            x_next = f"x_next{suf}"
            add_tensor(x_next, b["vector"], producer=None, shape=[stats.rows])
            t3 = add_task(f"axpy_xupdate{suf}", "axpy", f["axpy"], [prev["x"], prev["p"], alpha], [x_next])
            tensors[-1]["producer"] = t3

            # r_next = r - alpha * q
            r_next = f"r_next{suf}"
            add_tensor(r_next, b["vector"], producer=None, shape=[stats.rows])
            t4 = add_task(f"axpy_rupdate{suf}", "axpy", f["axpy"], [prev["r"], q, alpha], [r_next])
            tensors[-1]["producer"] = t4

            # r_new = r_next^T r_next
            r_new = f"r_new{suf}"
            add_tensor(r_new, b["scalar"], producer=None, shape=[1])
            t5 = add_task(f"dot_rnorm{suf}", "dot", f["dot"], [r_next, r_next], [r_new])
            tensors[-1]["producer"] = t5

            # beta = r_new / r_old
            beta = f"beta{suf}"
            add_tensor(beta, b["scalar"], producer=None, shape=[1])
            t6 = add_task(f"scalar_div_beta{suf}", "scalar_div", f["scalar_div"], [r_new, prev["r_old"]], [beta])
            tensors[-1]["producer"] = t6

            # p_next = r_next + beta * p
            p_next = f"p_next{suf}"
            add_tensor(p_next, b["vector"], producer=None, shape=[stats.rows])
            t7 = add_task(f"axpy_pupdate{suf}", "axpy", f["axpy"], [r_next, prev["p"], beta], [p_next])
            tensors[-1]["producer"] = t7

            prev = {"x": x_next, "r": r_next, "p": p_next, "r_old": r_new}
            last_outputs = (x_next, r_next, p_next, r_new)

        assert last_outputs is not None

        workload: Dict[str, Any] = {
            "name": f"cg_solver_{stats.name}_iter{cfg.num_iterations}_tol{cfg.tolerance:.0e}",
            "iteration_inputs": ["x", "r", "p", "r_old"],
            "iteration_outputs": list(last_outputs),
            "tensors": tensors,
            "tasks": tasks,
        }

        if emit_metadata:
            workload["metadata"] = {
                "algorithm": "CG",
                "num_iterations": cfg.num_iterations,
                "tolerance": cfg.tolerance,
                "matrix": stats.name,
                "rows": stats.rows,
                "cols": stats.cols,
                "nnz": stats.nnz,
                "is_square": stats.is_square,
                "note": "tolerance is metadata only (static DAG does not early-stop)",
            }

        print(f"       tasks={len(tasks)} (={cfg.num_iterations}×8), tensors={len(tensors)}")
        return workload


# ============================================================================
# 4) JSON Writer
# ============================================================================

class JSONWriter:
    @staticmethod
    def write(data: Dict[str, Any], path: str) -> None:
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2, ensure_ascii=False)
        print(f"[4/4] JSON Writer:  写入 {os.path.abspath(path)}")


# ============================================================================
# main
# ============================================================================

def main(argv: Optional[List[str]] = None) -> None:
    parser = argparse.ArgumentParser(description="CG Workload JSON Generator (minimal)")
    parser.add_argument("--matrix", type=str, default=None, help="SuiteSparse 矩阵名（默认自动选一个 SPD）")
    parser.add_argument("--iterations", type=int, default=300, help="CG 迭代次数（默认 1，范围 1-1000）")
    parser.add_argument("--tolerance", type=float, default=1e-6, help="收敛精度阈值（默认 1e-6，仅写入元数据）")
    parser.add_argument("--emit-metadata", action="store_true", help="在 workload 顶层输出 metadata（默认不输出）")
    parser.add_argument("--out", type=str, default="workload.json", help="输出 workload JSON 路径")

    args = parser.parse_args(argv)

    cfg = CGConfig(num_iterations=args.iterations, tolerance=args.tolerance)
    ok, msg = cfg.validate()
    if not ok:
        print(f" 配置错误: {msg}")
        sys.exit(1)

    print(f"✓ 配置验证: {msg}  (iterations={cfg.num_iterations}, tolerance={cfg.tolerance:.2e})")

    mtx = MatrixProvider.fetch_matrix(args.matrix, dest_dir="./matrix_data")
    stats = MatrixAnalyzer.analyze(mtx)
    workload = WorkloadBuilder.build_cg_workload(stats, cfg, emit_metadata=args.emit_metadata)
    JSONWriter.write(workload, args.out)


if __name__ == "__main__":
    main()