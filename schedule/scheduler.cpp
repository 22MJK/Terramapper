#include "schedule/scheduler.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>

namespace schedule {

SchedulePlan SimpleScheduler::schedule(const mapping::TaskGraph& graph,
                                       const mapping::MappingPlan& plan,
                                       const hardware_topology::HardwareTopology& topology) const {
    std::unordered_map<std::string, double> node_clock;
    std::unordered_map<std::string, double> finish_time;
    for (const auto* node : topology.nodes()) {
        node_clock[node->name] = 0.0;
    }

    SchedulePlan schedule_plan;
    for (const auto& task : graph.topological_order()) {
        const auto& node_name = plan.node_for(task.name);
        const auto* node = topology.node(node_name);
        if (node == nullptr) {
            throw std::runtime_error("Node " + node_name + " missing from topology");
        }

        double earliest = node_clock[node_name];
        for (const auto& dep : graph.dependencies(task.name)) {
            double ready = finish_time.at(dep.src);
            const auto& src_node = plan.node_for(dep.src);
            if (src_node != node_name) {
                const auto bw = topology.bandwidth(src_node, node_name);
                const auto lat_ms = topology.latency_ms(src_node, node_name);
                if (bw.has_value() && *bw > 0.0) {
                    const double size_gb = dep.tensor_size_mb / 1024.0;
                    ready += size_gb / *bw;
                }
                if (lat_ms.has_value()) {
                    ready += (*lat_ms) / 1000.0;
                }
            }
            earliest = std::max(earliest, ready);
        }

        const double start = earliest;
        const double duration = task.compute_flops / std::max(1.0, node->gflops);
        schedule_plan.slots.push_back({task.name, node_name, start, duration});
        node_clock[node_name] = start + duration;
        finish_time[task.name] = start + duration;
    }
    return schedule_plan;
}

}  // namespace schedule
