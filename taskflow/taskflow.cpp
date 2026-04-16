#include "taskflow/taskflow.h"

#include "taskflow/json.h"

#include <cmath>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace taskflow {
namespace {

std::uint64_t bytes_to_uint64(double bytes) {
    if (!(bytes >= 0.0)) {
        return 0;
    }
    if (bytes >= static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(std::llround(bytes));
}

std::uint64_t flops_to_uint64(double flops) {
    if (!(flops >= 0.0)) {
        return 0;
    }
    if (flops >= static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(std::llround(flops));
}

std::string canonical_comm_kind(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (ch == '-' || ch == ' ') {
            ch = '_';
        }
    }
    if (value == "all_reduce") {
        return "allreduce";
    }
    if (value == "all_gather") {
        return "allgather";
    }
    if (value == "reduce_scatter") {
        return "reducescatter";
    }
    if (value == "all_to_all") {
        return "alltoall";
    }
    return value;
}

bool is_collective_kind(const std::string& kind) {
    return kind == "allreduce" || kind == "allgather" || kind == "reducescatter" || kind == "broadcast" ||
           kind == "reduce" || kind == "alltoall";
}

}  // namespace

void TaskflowWriter::write(const std::string& path,
                           const std::string& time_unit,
                           const mapping::TaskGraph& graph,
                           const mapping::MappingPlan& mapping_plan,
                           const hardware_topology::HardwareTopology& topology) {
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("Failed to open " + path);
    }

    const auto topo_tasks = graph.topological_order();
    std::unordered_map<std::string, std::uint64_t> task_id;
    task_id.reserve(topo_tasks.size());
    for (std::uint64_t i = 0; i < topo_tasks.size(); ++i) {
        task_id.emplace(topo_tasks[i].name, i);
    }

    out << "{\n";
    out << "  \"time_unit\": ";
    json::write_string(out, time_unit);
    out << ",\n";

    out << "  \"tasks\": [\n";
    for (std::uint64_t i = 0; i < topo_tasks.size(); ++i) {
        const auto& task = topo_tasks[i];
        out << "    {\n";
        out << "      \"id\": ";
        json::write_uint64(out, i);
        out << ",\n";
        out << "      \"kind\": ";
        if (task.type.empty()) {
            json::write_string(out, "compute");
        } else {
            json::write_string(out, task.type);
        }
        out << ",\n";
        if (!task.subtype.empty()) {
            out << "      \"subtype\": ";
            json::write_string(out, task.subtype);
            out << ",\n";
        }
        out << "      \"name\": ";
        json::write_string(out, task.name);
        out << ",\n";
        out << "      \"flops\": ";
        json::write_uint64(out, flops_to_uint64(task.compute_flops));
        out << ",\n";
        out << "      \"device\": ";
        json::write_string(out, mapping_plan.node_for(task.name));
        out << "\n";
        out << "    }";
        out << (i + 1 == topo_tasks.size() ? "\n" : ",\n");
    }
    out << "  ],\n";

    out << "  \"edges\": [\n";
    std::uint64_t edge_id = 0;
    bool first_edge = true;
    for (const auto& task : topo_tasks) {
        for (const auto& edge : graph.successors(task.name)) {
            if (!first_edge) {
                out << ",\n";
            }
            first_edge = false;

            out << "    {\n";
            out << "      \"id\": ";
            json::write_uint64(out, edge_id++);
            out << ",\n";
            out << "      \"src\": ";
            json::write_uint64(out, task_id.at(edge.src));
            out << ",\n";
            out << "      \"dst\": ";
            json::write_uint64(out, task_id.at(edge.dst));
            out << ",\n";
            const auto& src_device = mapping_plan.node_for(edge.src);
            const auto& dst_device = mapping_plan.node_for(edge.dst);
            const std::string comm_kind = edge.comm_kind.empty() ? "p2p" : canonical_comm_kind(edge.comm_kind);
            const double raw_bytes = edge.tensor_bytes;
            const bool needs_transfer = (src_device != dst_device) || is_collective_kind(comm_kind);
            const double edge_bytes = needs_transfer ? raw_bytes : 0.0;
            out << "      \"bytes\": ";
            json::write_uint64(out, bytes_to_uint64(edge_bytes));
            out << ",\n";
            out << "      \"kind\": ";
            json::write_string(out, comm_kind);
            out << ",\n";

            std::vector<std::string> route;
            if (needs_transfer && !is_collective_kind(comm_kind)) {
                route = topology.shortest_route_link_ids(src_device, dst_device, bytes_to_uint64(edge_bytes));
                if (route.empty()) {
                    const auto direct = topology.link_id(src_device, dst_device);
                    if (direct.has_value()) {
                        route.push_back(*direct);
                    }
                }
            }

            out << "      \"route\": [";
            for (size_t i = 0; i < route.size(); ++i) {
                if (i != 0) {
                    out << ", ";
                }
                json::write_string(out, route[i]);
            }
            out << "]\n";
            out << "    }";
        }
    }
    if (!first_edge) {
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
}

}  // namespace taskflow
