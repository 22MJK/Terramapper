#include "workload/workload.h"

#include <cmath>

namespace workload {

Workload::Workload(std::string name, std::vector<WorkloadStage> stages, std::vector<WorkloadEdge> edges)
    : name_(std::move(name)), stages_(std::move(stages)), edges_(std::move(edges)) {}

mapping::TaskGraph Workload::to_task_graph() const {
    mapping::TaskGraph graph;
    for (const auto& stage : stages_) {
        mapping::Task task;
        task.name = stage.name;
        task.compute_flops = stage.compute_flops;
        task.memory_gb = stage.memory_gb;
        graph.add_task(std::move(task));
    }
    if (!edges_.empty()) {
        for (const auto& edge : edges_) {
            graph.add_edge(edge.src, edge.dst, edge.tensor_size_mb);
        }
    } else {
        for (const auto& stage : stages_) {
            for (const auto& dep : stage.dependencies) {
                const double tensor_size = stage.compute_flops * 0.1;
                graph.add_edge(dep, stage.name, tensor_size);
            }
        }
    }
    return graph;
}

const std::string& Workload::name() const {
    return name_;
}

WorkloadGenerator::WorkloadGenerator(double base_compute, double growth)
    : base_compute_(base_compute), growth_(growth) {}

Workload WorkloadGenerator::build(const std::string& name, int depth) const {
    std::vector<WorkloadStage> stages;
    stages.reserve(depth);
    for (int index = 0; index < depth; ++index) {
        const double compute = base_compute_ * std::pow(growth_, index);
        WorkloadStage stage;
        stage.name = "stage_" + std::to_string(index);
        stage.compute_flops = compute;
        stage.memory_gb = compute * 0.5;
        if (index > 0) {
            stage.dependencies.push_back("stage_" + std::to_string(index - 1));
        }
        stages.push_back(std::move(stage));
    }
    return Workload(name, std::move(stages));
}

}  // namespace workload
