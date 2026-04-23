#include "taskflow/taskflow.h"

#include "taskflow/json.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace taskflow {
namespace {

constexpr double kComputeEfficiency = 0.1;

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

bool is_cpu_device(const hardware_topology::Device* device) {
    return device != nullptr && device->type == "CPU";
}

std::string to_et_collective_type(const std::string& kind) {
    const std::string canonical = canonical_comm_kind(kind);
    if (canonical == "allreduce") {
        return "ALL_REDUCE";
    }
    if (canonical == "allgather") {
        return "ALL_GATHER";
    }
    if (canonical == "reducescatter") {
        return "REDUCE_SCATTER";
    }
    if (canonical == "alltoall") {
        return "ALL_TO_ALL";
    }
    if (canonical == "broadcast") {
        return "BROADCAST";
    }
    if (canonical == "reduce") {
        return "REDUCE";
    }
    return "ALL_REDUCE";
}

std::uint64_t estimate_comp_duration_micros(const mapping::Task& task,
                                            const hardware_topology::Device* device) {
    if (device == nullptr) {
        return 1;
    }
    double compute_s = 0.0;
    if (task.compute_flops > 0.0) {
        if (device->peak_gflops <= 0.0) {
            compute_s = 0.0;
        } else {
            const double effective_gflops = std::max(1e-9, device->peak_gflops * kComputeEfficiency);
            compute_s = task.compute_flops / (effective_gflops * 1e9);
        }
    }
    double memory_s = 0.0;
    if (task.memory_bytes > 0.0 && device->mem_bw_gbps > 0.0) {
        memory_s = task.memory_bytes / (device->mem_bw_gbps * 1e9);
    }
    const double duration_s = std::max(compute_s, memory_s);
    if (duration_s <= 0.0) {
        return 1;
    }
    const long double micros = duration_s * 1e6;
    if (micros >= static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    const auto out = static_cast<std::uint64_t>(std::llround(micros));
    return out == 0 ? 1 : out;
}

std::uint64_t stable_device_rank(const std::string& device_id,
                                 const std::unordered_map<std::string, std::uint64_t>& rank_map) {
    const auto it = rank_map.find(device_id);
    if (it != rank_map.end()) {
        return it->second;
    }
    return 0;
}

void sort_and_dedup(std::vector<std::uint64_t>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

struct EtAttr {
    std::string name;
    std::optional<std::uint64_t> uint64_val;
    std::optional<std::int64_t> int64_val;
    std::optional<std::string> string_val;
    std::optional<bool> bool_val;
    std::vector<bool> bool_list_val;
};

struct EtNode {
    std::uint64_t id{0};
    std::string name;
    std::string type;
    std::vector<std::uint64_t> ctrl_deps;
    std::vector<std::uint64_t> data_deps;
    std::optional<std::uint64_t> duration_micros;
    std::vector<EtAttr> attrs;
};

void add_attr_u64(std::vector<EtAttr>& attrs, const std::string& name, std::uint64_t value) {
    EtAttr attr;
    attr.name = name;
    attr.uint64_val = value;
    attrs.push_back(std::move(attr));
}

void add_attr_i64(std::vector<EtAttr>& attrs, const std::string& name, std::int64_t value) {
    EtAttr attr;
    attr.name = name;
    attr.int64_val = value;
    attrs.push_back(std::move(attr));
}

void add_attr_str(std::vector<EtAttr>& attrs, const std::string& name, const std::string& value) {
    EtAttr attr;
    attr.name = name;
    attr.string_val = value;
    attrs.push_back(std::move(attr));
}

void add_attr_bool(std::vector<EtAttr>& attrs, const std::string& name, bool value) {
    EtAttr attr;
    attr.name = name;
    attr.bool_val = value;
    attrs.push_back(std::move(attr));
}

void add_attr_bool_list(std::vector<EtAttr>& attrs, const std::string& name, std::vector<bool> values) {
    EtAttr attr;
    attr.name = name;
    attr.bool_list_val = std::move(values);
    attrs.push_back(std::move(attr));
}

void write_u64_array(std::ostream& out, const std::vector<std::uint64_t>& values) {
    out << "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        json::write_uint64(out, values[i]);
    }
    out << "]";
}

void write_et_attr(std::ostream& out, const EtAttr& attr) {
    out << "{";
    out << "\"name\": ";
    json::write_string(out, attr.name);
    if (attr.uint64_val.has_value()) {
        out << ", \"uint64_val\": ";
        json::write_uint64(out, *attr.uint64_val);
    } else if (attr.int64_val.has_value()) {
        out << ", \"int64_val\": " << *attr.int64_val;
    } else if (attr.string_val.has_value()) {
        out << ", \"string_val\": ";
        json::write_string(out, *attr.string_val);
    } else if (attr.bool_val.has_value()) {
        out << ", \"bool_val\": " << (*attr.bool_val ? "true" : "false");
    } else if (!attr.bool_list_val.empty()) {
        out << ", \"bool_list_val\": [";
        for (std::size_t i = 0; i < attr.bool_list_val.size(); ++i) {
            if (i != 0) {
                out << ", ";
            }
            out << (attr.bool_list_val[i] ? "true" : "false");
        }
        out << "]";
    }
    out << "}";
}

void write_et_node(std::ostream& out, const EtNode& node) {
    out << "    {\n";
    out << "      \"id\": ";
    json::write_uint64(out, node.id);
    out << ",\n";
    out << "      \"name\": ";
    json::write_string(out, node.name);
    out << ",\n";
    out << "      \"type\": ";
    json::write_string(out, node.type);
    out << ",\n";
    out << "      \"ctrl_deps\": ";
    write_u64_array(out, node.ctrl_deps);
    out << ",\n";
    out << "      \"data_deps\": ";
    write_u64_array(out, node.data_deps);
    if (node.duration_micros.has_value()) {
        out << ",\n";
        out << "      \"duration_micros\": ";
        json::write_uint64(out, *node.duration_micros);
    }
    if (!node.attrs.empty()) {
        out << ",\n";
        out << "      \"attr\": [";
        for (std::size_t i = 0; i < node.attrs.size(); ++i) {
            if (i != 0) {
                out << ", ";
            }
            write_et_attr(out, node.attrs[i]);
        }
        out << "]";
    }
    out << "\n";
    out << "    }";
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

    std::unordered_map<std::string, std::uint64_t> device_rank;
    const auto devices = topology.devices();
    device_rank.reserve(devices.size());
    for (std::uint64_t i = 0; i < devices.size(); ++i) {
        device_rank.emplace(devices[i]->id, i);
    }

    std::vector<EtNode> nodes;
    nodes.reserve(topo_tasks.size());
    std::unordered_map<std::string, std::size_t> task_name_to_node_index;
    task_name_to_node_index.reserve(topo_tasks.size());

    for (std::size_t i = 0; i < topo_tasks.size(); ++i) {
        const auto& task = topo_tasks[i];
        EtNode node;
        node.id = static_cast<std::uint64_t>(i);
        node.name = task.name;
        node.type = "COMP_NODE";
        const auto& assigned_device = mapping_plan.node_for(task.name);
        const auto* device = topology.device(assigned_device);
        const bool on_cpu = is_cpu_device(device);
        // Reflect the mapped device type so CPU tasks are emitted as valid compute ops.
        add_attr_bool(node.attrs, "is_cpu_op", on_cpu);
        add_attr_str(node.attrs, "compute_target", on_cpu ? "cpu" : "gpu");
        add_attr_u64(node.attrs, "num_ops", flops_to_uint64(task.compute_flops));
        add_attr_u64(node.attrs, "tensor_size", bytes_to_uint64(task.memory_bytes));
        add_attr_str(node.attrs, "assigned_device", assigned_device);
        if (!task.subtype.empty()) {
            add_attr_str(node.attrs, "subtype", task.subtype);
        }
        node.duration_micros = estimate_comp_duration_micros(task, device);
        task_name_to_node_index.emplace(task.name, nodes.size());
        nodes.push_back(std::move(node));
    }

    std::unordered_map<std::string, std::vector<std::uint64_t>> compute_data_deps;
    std::uint64_t next_node_id = static_cast<std::uint64_t>(nodes.size());
    std::uint64_t comm_tag = 1;

    for (const auto& task : topo_tasks) {
        for (const auto& edge : graph.successors(task.name)) {
            const auto src_it = task_id.find(edge.src);
            const auto dst_it = task_id.find(edge.dst);
            if (src_it == task_id.end() || dst_it == task_id.end()) {
                throw std::runtime_error("Task ID missing while building ET nodes");
            }
            const auto& src_device = mapping_plan.node_for(edge.src);
            const auto& dst_device = mapping_plan.node_for(edge.dst);
            const std::string comm_kind = edge.comm_kind.empty() ? "p2p" : canonical_comm_kind(edge.comm_kind);
            const double raw_bytes = edge.tensor_bytes;
            const bool needs_transfer = (src_device != dst_device) || is_collective_kind(comm_kind);
            if (!needs_transfer) {
                compute_data_deps[edge.dst].push_back(src_it->second);
                continue;
            }
            const std::uint64_t comm_bytes = bytes_to_uint64(raw_bytes);
            if (is_collective_kind(comm_kind)) {
                EtNode coll;
                coll.id = next_node_id++;
                coll.name = "collective_" + comm_kind + "_" + edge.src + "_to_" + edge.dst;
                coll.type = "COMM_COLL_NODE";
                coll.data_deps.push_back(src_it->second);
                add_attr_bool(coll.attrs, "is_cpu_op", false);
                add_attr_str(coll.attrs, "comm_type", to_et_collective_type(comm_kind));
                add_attr_i64(coll.attrs, "comm_priority", 0);
                add_attr_u64(coll.attrs, "comm_size", comm_bytes);
                add_attr_str(coll.attrs, "pg_name", "0");
                add_attr_bool_list(coll.attrs, "involved_dim", {true, true, true, true});
                nodes.push_back(std::move(coll));
                compute_data_deps[edge.dst].push_back(next_node_id - 1);
                continue;
            }

            EtNode send;
            send.id = next_node_id++;
            send.name = "send_" + edge.src + "_to_" + edge.dst;
            send.type = "COMM_SEND_NODE";
            send.data_deps.push_back(src_it->second);
            add_attr_bool(send.attrs, "is_cpu_op", false);
            add_attr_u64(send.attrs, "comm_src", stable_device_rank(src_device, device_rank));
            add_attr_u64(send.attrs, "comm_dst", stable_device_rank(dst_device, device_rank));
            add_attr_u64(send.attrs, "comm_size", comm_bytes);
            add_attr_u64(send.attrs, "comm_tag", comm_tag);
            nodes.push_back(std::move(send));
            const std::uint64_t send_id = next_node_id - 1;

            EtNode recv;
            recv.id = next_node_id++;
            recv.name = "recv_" + edge.src + "_to_" + edge.dst;
            recv.type = "COMM_RECV_NODE";
            recv.data_deps.push_back(send_id);
            add_attr_bool(recv.attrs, "is_cpu_op", false);
            add_attr_u64(recv.attrs, "comm_src", stable_device_rank(src_device, device_rank));
            add_attr_u64(recv.attrs, "comm_dst", stable_device_rank(dst_device, device_rank));
            add_attr_u64(recv.attrs, "comm_size", comm_bytes);
            add_attr_u64(recv.attrs, "comm_tag", comm_tag);
            nodes.push_back(std::move(recv));
            compute_data_deps[edge.dst].push_back(next_node_id - 1);

            if (comm_tag < std::numeric_limits<std::uint64_t>::max()) {
                comm_tag += 1;
            }
        }
    }

    for (const auto& task : topo_tasks) {
        const auto idx_it = task_name_to_node_index.find(task.name);
        if (idx_it == task_name_to_node_index.end()) {
            continue;
        }
        auto deps_it = compute_data_deps.find(task.name);
        if (deps_it == compute_data_deps.end()) {
            continue;
        }
        auto deps = std::move(deps_it->second);
        sort_and_dedup(deps);
        nodes[idx_it->second].data_deps = std::move(deps);
    }

    std::sort(nodes.begin(), nodes.end(), [](const EtNode& a, const EtNode& b) { return a.id < b.id; });

    out << "{\n";
    out << "  \"global_metadata\": {\n";
    out << "    \"version\": ";
    json::write_string(out, "0.0.4");
    out << ",\n";
    out << "    \"time_unit\": ";
    json::write_string(out, time_unit);
    out << "\n";
    out << "  },\n";
    out << "  \"nodes\": [\n";
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        write_et_node(out, nodes[i]);
        out << (i + 1 == nodes.size() ? "\n" : ",\n");
    }
    out << "  ]\n";
    out << "}\n";
}

}  // namespace taskflow
