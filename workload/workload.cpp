#include "workload/workload.h"

#include <cmath>

namespace workload {

Workload::Workload(std::string name, std::vector<WorkloadStage> stages)
    : name_(std::move(name)), stages_(std::move(stages)) {}

mapping::TaskGraph Workload::to_task_graph() const {
    mapping::TaskGraph graph;
    std::unordered_map<int, std::string> id_to_name;
    id_to_name.reserve(stages_.size());
    for (const auto& stage : stages_) {
        mapping::Task task;
        task.name = stage.name;
        task.type = stage.type;
        task.subtype = stage.subtype;
        task.compute_flops = stage.compute_flops;
        task.comm_bytes = stage.comm_bytes;
        graph.add_task(std::move(task));
        id_to_name.emplace(stage.id, stage.name);
    }
    for (const auto& stage : stages_) {
        for (const auto& dep : stage.dependencies) {
            const double tensor_bytes = stage.comm_bytes > 0.0 ? stage.comm_bytes : 0.0;
            const auto dep_it = id_to_name.find(dep);
            if (dep_it == id_to_name.end()) {
                throw std::runtime_error("Workload dependency refers to unknown task id");
            }
            graph.add_edge(dep_it->second, stage.name, tensor_bytes);
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
        stage.type = "compute";
        stage.subtype = "";
        stage.compute_flops = compute;
        stage.comm_bytes = 0.0;
        stage.id = index;
        if (index > 0) {
            stage.dependencies.push_back(index - 1);
        }
        stages.push_back(std::move(stage));
    }
    return Workload(name, std::move(stages));
}

}  // namespace workload
