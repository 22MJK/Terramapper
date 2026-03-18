#include "mapping/mapper.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace mapping {

const std::string& MappingPlan::node_for(const std::string& task_name) const {
    return assignments.at(task_name);
}

MappingPlan GreedyMapper::map(const TaskGraph& graph, const hardware_topology::HardwareTopology& topology) const {
    const auto devices = topology.devices();
    if (devices.empty()) {
        throw std::runtime_error("Topology has no devices");
    }

    std::unordered_map<std::string, double> loads;
    for (const auto* device : devices) {
        loads[device->id] = 0.0;
    }

    MappingPlan plan;
    for (const auto& task : graph.topological_order()) {
        const auto* target = *std::min_element(
            devices.begin(),
            devices.end(),
            [&loads](const hardware_topology::Device* a, const hardware_topology::Device* b) {
                const double load_a = loads[a->id] / std::max(1.0, a->peak_gflops);
                const double load_b = loads[b->id] / std::max(1.0, b->peak_gflops);
                return load_a < load_b;
            });
        plan.assignments[task.name] = target->id;
        loads[target->id] += task.compute_flops;
    }
    return plan;
}

PartitionerMapper::PartitionerMapper(std::unique_ptr<Mapper> inner, std::vector<std::vector<std::string>> partitions)
    : inner_(std::move(inner)), partitions_(std::move(partitions)) {}

MappingPlan PartitionerMapper::map(const TaskGraph& graph, const hardware_topology::HardwareTopology& topology) const {
    TaskGraph reordered;
    const auto reordered_tasks = order(graph);
    for (const auto& task : reordered_tasks) {
        reordered.add_task(task);
    }
    for (const auto& task : reordered_tasks) {
        for (const auto& edge : graph.successors(task.name)) {
            reordered.add_edge(edge.src, edge.dst, edge.tensor_size_mb);
        }
    }
    return inner_->map(reordered, topology);
}

std::vector<Task> PartitionerMapper::order(const TaskGraph& graph) const {
    std::vector<Task> ordered;
    std::unordered_set<std::string> seen;
    for (const auto& block : partitions_) {
        for (const auto& name : block) {
            if (!graph.has_task(name)) {
                continue;
            }
            if (seen.insert(name).second) {
                ordered.push_back(graph.task(name));
            }
        }
    }
    for (const auto& task : graph.topological_order()) {
        if (seen.insert(task.name).second) {
            ordered.push_back(task);
        }
    }
    return ordered;
}

}  // namespace mapping
