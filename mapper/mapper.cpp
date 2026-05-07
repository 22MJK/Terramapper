#include "mapper/mapper.h"

#include "mapping/mapper.h"
#include "mapping/strategies.h"
#include "taskflow/taskflow.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mapper {
namespace {

constexpr double kComputeEfficiency = 0.1;

std::size_t dtype_size(workload::DType dtype) {
    switch (dtype) {
        case workload::DType::FP32:
        case workload::DType::INT32:
            return 4;
        case workload::DType::FP64:
        case workload::DType::INT64:
            return 8;
    }
    return 4;
}

std::uint64_t tensor_bytes(const workload::Tensor& tensor) {
    if (tensor.size_bytes > 0) {
        return tensor.size_bytes;
    }
    if (tensor.num_elements.has_value()) {
        return static_cast<std::uint64_t>(*tensor.num_elements * dtype_size(tensor.dtype));
    }
    if (tensor.shape.empty()) {
        return static_cast<std::uint64_t>(dtype_size(tensor.dtype));
    }
    long double total = 1.0L;
    for (auto dim : tensor.shape) {
        if (dim <= 0) {
            return 0;
        }
        total *= static_cast<long double>(dim);
        if (total > static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
            return std::numeric_limits<std::uint64_t>::max();
        }
    }
    total *= static_cast<long double>(dtype_size(tensor.dtype));
    if (total > static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(total);
}

std::optional<workload::AccessKind> parse_access_kind(const std::string& value) {
    if (value == "dense") {
        return workload::AccessKind::DENSE;
    }
    if (value == "sparse_csr") {
        return workload::AccessKind::SPARSE_CSR;
    }
    if (value == "row-wise") {
        return workload::AccessKind::ROW_WISE;
    }
    if (value == "col-wise") {
        return workload::AccessKind::COL_WISE;
    }
    return std::nullopt;
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

std::string canonical_task_subtype(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (ch == '-' || ch == ' ') {
            ch = '_';
        }
    }
    if (value == "scalar") {
        return "scalar_div";
    }
    return value;
}

bool is_collective_kind(const std::string& kind) {
    const auto k = canonical_comm_kind(kind);
    return k == "allreduce" || k == "allgather" || k == "reducescatter" || k == "broadcast" || k == "reduce" ||
           k == "alltoall";
}

std::string infer_parallel_comm_kind(const std::string& original_kind,
                                     bool src_expanded,
                                     bool dst_expanded,
                                     bool cross_product) {
    const std::string kind = canonical_comm_kind(original_kind);
    if (!kind.empty() && kind != "p2p") {
        return kind;
    }
    if (src_expanded && !dst_expanded) {
        // Multiple partition producers converge to one consumer => merge/gather.
        return "allgather";
    }
    if (!src_expanded && dst_expanded) {
        // One producer fans out to all partitions.
        return "broadcast";
    }
    if (src_expanded && dst_expanded && cross_product) {
        return "alltoall";
    }
    return "p2p";
}

std::string collective_event_key(const mapping::TaskEdge& edge) {
    return edge.tensor_id + "|" + canonical_comm_kind(edge.comm_kind);
}

struct CollectiveInfo {
    std::unordered_set<std::string> src_tasks;
    std::unordered_set<std::string> all_tasks;
    double bytes{0.0};
    std::string kind;
};

std::unordered_map<std::string, CollectiveInfo> build_collective_info(const mapping::TaskGraph& graph) {
    std::unordered_map<std::string, CollectiveInfo> info;
    for (const auto& task : graph.topological_order()) {
        for (const auto& edge : graph.successors(task.name)) {
            if (!is_collective_kind(edge.comm_kind)) {
                continue;
            }
            const auto key = collective_event_key(edge);
            auto& item = info[key];
            item.kind = canonical_comm_kind(edge.comm_kind);
            item.bytes = std::max(item.bytes, edge.tensor_bytes);
            item.src_tasks.insert(edge.src);
            item.all_tasks.insert(edge.src);
            item.all_tasks.insert(edge.dst);
        }
    }
    return info;
}

std::unordered_map<std::string, std::vector<std::string>> build_group_members(
    const workload::Workload& workload,
    const hardware_topology::HardwareTopology& topology) {
    std::unordered_map<std::string, std::vector<std::string>> group_members;
    group_members.reserve(workload.device_groups().size());
    for (const auto& group : workload.device_groups()) {
        if (group.members.size() == 1 && group.members[0] == "all") {
            std::vector<std::string> members;
            for (const auto* device : topology.devices()) {
                members.push_back(device->id);
            }
            group_members[group.id] = std::move(members);
        } else {
            group_members[group.id] = group.members;
        }
    }
    return group_members;
}

std::size_t group_size(const workload::Distribution& dist,
                       const std::unordered_map<std::string, std::vector<std::string>>& groups) {
    if (dist.group.empty()) {
        return 1;
    }
    const auto it = groups.find(dist.group);
    if (it == groups.end() || it->second.empty()) {
        return 1;
    }
    return it->second.size();
}

std::size_t partition_parts(const workload::Tensor& tensor,
                            const std::unordered_map<std::string, std::vector<std::string>>& groups) {
    if (tensor.partition.has_value() && tensor.partition->num_parts > 0) {
        return static_cast<std::size_t>(tensor.partition->num_parts);
    }
    return group_size(tensor.distribution, groups);
}

double estimate_transfer_bytes(const workload::Tensor& tensor,
                               workload::AccessKind access,
                               const std::unordered_map<std::string, std::vector<std::string>>& groups) {
    const auto total_bytes = static_cast<double>(tensor_bytes(tensor));
    if (total_bytes <= 0.0) {
        return 0.0;
    }

    const std::size_t parts = partition_parts(tensor, groups);
    switch (tensor.distribution.kind) {
        case workload::DistKind::REPLICATED:
            if (tensor.replication.has_value() && tensor.replication->mode == "cached") {
                return 0.0;
            }
            return total_bytes;
        case workload::DistKind::NONE:
            return total_bytes;
        case workload::DistKind::BLOCK:
        case workload::DistKind::CYCLIC:
            if (parts == 0) {
                return total_bytes;
            }
            if (access == workload::AccessKind::ROW_WISE || access == workload::AccessKind::COL_WISE) {
                return total_bytes / static_cast<double>(parts);
            }
            return total_bytes;
    }
    return total_bytes;
}

double estimate_collective_bytes(const workload::Tensor& tensor,
                                 const std::string& comm_kind,
                                 const std::unordered_map<std::string, std::vector<std::string>>& groups) {
    const auto total_bytes = static_cast<double>(tensor_bytes(tensor));
    if (total_bytes <= 0.0) {
        return 0.0;
    }
    const std::size_t parts = partition_parts(tensor, groups);
    if (parts <= 1) {
        return total_bytes;
    }
    const std::string kind = canonical_comm_kind(comm_kind);
    if (kind == "allgather" || kind == "reducescatter" || kind == "alltoall") {
        return total_bytes / static_cast<double>(parts);
    }
    return total_bytes;
}

mapping::TaskGraph annotate_comm_bytes(const mapping::TaskGraph& graph,
                                       const workload::Workload& workload,
                                       const hardware_topology::HardwareTopology& topology) {
    mapping::TaskGraph annotated;
    const auto& ordered = graph.topological_order();
    for (const auto& task : ordered) {
        annotated.add_task(task);
    }

    std::unordered_map<std::string, workload::Tensor> tensor_map;
    tensor_map.reserve(workload.tensors().size());
    for (const auto& tensor : workload.tensors()) {
        tensor_map.emplace(tensor.id, tensor);
    }

    const auto group_members = build_group_members(workload, topology);

    for (const auto& task : ordered) {
        for (const auto& edge : graph.successors(task.name)) {
            double bytes = 0.0;
            auto it = tensor_map.find(edge.tensor_id);
            if (it != tensor_map.end()) {
                const auto& tensor = it->second;
                const auto access = parse_access_kind(edge.access_pattern).value_or(tensor.access_pattern);
                if (is_collective_kind(edge.comm_kind)) {
                    bytes = estimate_collective_bytes(tensor, edge.comm_kind, group_members);
                } else {
                    bytes = estimate_transfer_bytes(tensor, access, group_members);
                }
            }
            annotated.add_edge(edge.src,
                               edge.dst,
                               bytes,
                               edge.tensor_id,
                               canonical_comm_kind(edge.comm_kind),
                               edge.access_pattern);
        }
    }
    return annotated;
}

struct PlacementInfo {
    std::string group;
    std::string parallelism;
};

enum class ParallelMode {
    NONE,
    HINT,
    ALL,
};

const char* parallel_mode_name(ParallelMode mode) {
    switch (mode) {
        case ParallelMode::NONE:
            return "none";
        case ParallelMode::HINT:
            return "hint";
        case ParallelMode::ALL:
            return "all";
    }
    return "none";
}

std::optional<ParallelMode> parse_parallel_mode(const std::string& value) {
    if (value == "none") {
        return ParallelMode::NONE;
    }
    if (value == "hint") {
        return ParallelMode::HINT;
    }
    if (value == "all") {
        return ParallelMode::ALL;
    }
    return std::nullopt;
}

std::unordered_map<std::string, PlacementInfo> build_placement(const workload::Workload& workload) {
    std::unordered_map<std::string, PlacementInfo> placement;
    placement.reserve(workload.tasks().size());
    for (const auto& task : workload.tasks()) {
        PlacementInfo info;
        info.group = task.placement_group;
        info.parallelism = task.placement_parallelism;
        placement.emplace(task.name, std::move(info));
    }
    return placement;
}

std::vector<std::string> resolve_group_members(
    const std::unordered_map<std::string, std::vector<std::string>>& group_members,
    const std::string& group,
    const hardware_topology::HardwareTopology& topology) {
    if (!group.empty()) {
        const auto it = group_members.find(group);
        if (it != group_members.end() && !it->second.empty()) {
            return it->second;
        }
    }
    std::vector<std::string> members;
    for (const auto* device : topology.devices()) {
        members.push_back(device->id);
    }
    return members;
}

double estimated_task_time_on_device(const mapping::Task& task, const hardware_topology::Device* device) {
    if (device == nullptr) {
        return std::numeric_limits<double>::infinity();
    }
    double compute_t = 0.0;
    if (task.compute_flops > 0.0) {
        if (device->peak_gflops <= 0.0) {
            return std::numeric_limits<double>::infinity();
        }
        const double effective_gflops = std::max(1e-9, device->peak_gflops * kComputeEfficiency);
        compute_t = task.compute_flops / (effective_gflops * 1e9);
    }
    double memory_t = 0.0;
    if (task.memory_bytes > 0.0) {
        if (device->mem_bw_gbps <= 0.0) {
            return std::numeric_limits<double>::infinity();
        }
        memory_t = task.memory_bytes / (device->mem_bw_gbps * 1e9);
    }
    return std::max(compute_t, memory_t);
}

std::vector<double> infer_split_ratios(const mapping::Task& task,
                                       const std::vector<std::string>& members,
                                       const hardware_topology::HardwareTopology& topology) {
    if (members.empty()) {
        return {};
    }
    std::vector<double> scores;
    scores.reserve(members.size());
    double sum = 0.0;
    for (const auto& id : members) {
        const auto* device = topology.device(id);
        double score = 1.0;
        if (device != nullptr) {
            const double t = estimated_task_time_on_device(task, device);
            if (std::isfinite(t) && t > 0.0) {
                score = 1.0 / t;
            } else if (t == 0.0) {
                score = std::max(1.0, device->peak_gflops + device->mem_bw_gbps);
            } else {
                score = 0.0;
            }
        }
        scores.push_back(score);
        sum += score;
    }
    if (!(sum > 0.0)) {
        return std::vector<double>(members.size(), 1.0 / static_cast<double>(members.size()));
    }
    for (double& score : scores) {
        score /= sum;
    }
    return scores;
}

mapping::TaskGraph expand_data_parallel(const mapping::TaskGraph& graph,
                                        const workload::Workload& workload,
                                        const hardware_topology::HardwareTopology& topology,
                                        ParallelMode mode) {
    if (mode == ParallelMode::NONE) {
        return graph;
    }

    const auto placement = build_placement(workload);
    const auto group_members = build_group_members(workload, topology);

    struct ExpandedTask {
        std::vector<std::string> devices;
        std::vector<std::string> shards;
    };

    std::unordered_map<std::string, ExpandedTask> expanded;
    mapping::TaskGraph expanded_graph;

    const auto& ordered = graph.topological_order();
    for (const auto& task : ordered) {
        auto it = placement.find(task.name);
        const bool by_hint = (it != placement.end() && it->second.parallelism == "data_parallel");
        const bool is_data_parallel = (mode == ParallelMode::ALL) || by_hint;
        if (!is_data_parallel) {
            expanded_graph.add_task(task);
            continue;
        }

        const std::string group = (it != placement.end()) ? it->second.group : "";
        const auto members = resolve_group_members(group_members, group, topology);
        if (members.size() <= 1) {
            expanded_graph.add_task(task);
            continue;
        }

        ExpandedTask info;
        info.devices = members;
        const auto ratios = infer_split_ratios(task, members, topology);
        for (std::size_t i = 0; i < members.size(); ++i) {
            mapping::Task split = task;
            split.name = task.name + "@p" + std::to_string(i);
            const double ratio = (i < ratios.size()) ? ratios[i] : (1.0 / static_cast<double>(members.size()));
            split.compute_flops = task.compute_flops * ratio;
            split.memory_bytes = task.memory_bytes * ratio;
            // Keep each shard movable while constraining it to this placement group.
            for (const auto& allowed_device : members) {
                split.tags.insert("allow:" + allowed_device);
            }
            expanded_graph.add_task(split);
            info.shards.push_back(split.name);
        }
        expanded.emplace(task.name, std::move(info));
    }

    for (const auto& task : ordered) {
        for (const auto& edge : graph.successors(task.name)) {
            const auto src_it = expanded.find(edge.src);
            const auto dst_it = expanded.find(edge.dst);
            const bool src_expanded = src_it != expanded.end();
            const bool dst_expanded = dst_it != expanded.end();
            const std::string inferred_kind =
                infer_parallel_comm_kind(edge.comm_kind, src_expanded, dst_expanded, false);
            if (!src_expanded && !dst_expanded) {
                expanded_graph.add_edge(edge.src,
                                        edge.dst,
                                        edge.tensor_bytes,
                                        edge.tensor_id,
                                        inferred_kind,
                                        edge.access_pattern);
                continue;
            }
            if (src_expanded && dst_expanded) {
                const auto& src_shards = src_it->second.shards;
                const auto& dst_shards = dst_it->second.shards;
                const std::size_t paired = std::min(src_shards.size(), dst_shards.size());
                for (std::size_t i = 0; i < paired; ++i) {
                    expanded_graph.add_edge(src_shards[i],
                                            dst_shards[i],
                                            edge.tensor_bytes,
                                            edge.tensor_id,
                                            inferred_kind,
                                            edge.access_pattern);
                }
                if (src_shards.size() != dst_shards.size()) {
                    const std::string cross_kind =
                        infer_parallel_comm_kind(edge.comm_kind, src_expanded, dst_expanded, true);
                    for (const auto& src_name : src_shards) {
                        for (const auto& dst_name : dst_shards) {
                            expanded_graph.add_edge(src_name,
                                                    dst_name,
                                                    edge.tensor_bytes,
                                                    edge.tensor_id,
                                                    cross_kind,
                                                    edge.access_pattern);
                        }
                    }
                }
                continue;
            }
            if (src_expanded) {
                for (const auto& src_name : src_it->second.shards) {
                    expanded_graph.add_edge(src_name,
                                            edge.dst,
                                            edge.tensor_bytes,
                                            edge.tensor_id,
                                            inferred_kind,
                                            edge.access_pattern);
                }
                continue;
            }
            for (const auto& dst_name : dst_it->second.shards) {
                expanded_graph.add_edge(edge.src,
                                        dst_name,
                                        edge.tensor_bytes,
                                        edge.tensor_id,
                                        inferred_kind,
                                        edge.access_pattern);
            }
        }
    }

    return expanded_graph;
}

double task_compute_time_seconds(const mapping::Task& task, const hardware_topology::Device* device) {
    if (task.compute_flops <= 0.0) {
        return 0.0;
    }
    if (device == nullptr || device->peak_gflops <= 0.0) {
        return std::numeric_limits<double>::infinity();
    }
    const double effective_gflops = std::max(1e-9, device->peak_gflops * kComputeEfficiency);
    return task.compute_flops / (effective_gflops * 1e9);
}

double task_memory_time_seconds(const mapping::Task& task, const hardware_topology::Device* device) {
    if (task.memory_bytes <= 0.0) {
        return 0.0;
    }
    if (device == nullptr || device->mem_bw_gbps <= 0.0) {
        return std::numeric_limits<double>::infinity();
    }
    return task.memory_bytes / (device->mem_bw_gbps * 1e9);
}

double task_exec_time_seconds(const mapping::Task& task, const hardware_topology::Device* device) {
    return std::max(task_compute_time_seconds(task, device), task_memory_time_seconds(task, device));
}

struct LinkStats {
    double avg_bw_gbps{0.0};
    double avg_latency_s{0.0};
};

LinkStats average_link_stats(const hardware_topology::HardwareTopology& topology) {
    const auto& links = topology.links();
    if (links.empty()) {
        return {};
    }
    double bw_sum = 0.0;
    double lat_sum = 0.0;
    for (const auto& link : links) {
        bw_sum += link.bw_gbps;
        lat_sum += link.latency_ms;
    }
    return LinkStats{
        bw_sum / static_cast<double>(links.size()),
        (lat_sum / static_cast<double>(links.size())) / 1000.0,
    };
}

double allreduce_time_seconds(double bytes,
                              std::size_t participants,
                              const hardware_topology::HardwareTopology& topology) {
    if (bytes <= 0.0 || participants <= 1) {
        return 0.0;
    }
    const auto stats = average_link_stats(topology);
    if (stats.avg_bw_gbps <= 0.0) {
        return std::numeric_limits<double>::infinity();
    }
    const double payload = bytes / 1e9;
    const double alpha = stats.avg_latency_s;
    const double beta = payload / stats.avg_bw_gbps;
    const double p = static_cast<double>(participants);
    return 2.0 * (p - 1.0) * alpha + 2.0 * ((p - 1.0) / p) * beta;
}

double collective_time_seconds(const std::string& comm_kind,
                               double bytes,
                               std::size_t participants,
                               const hardware_topology::HardwareTopology& topology) {
    if (bytes <= 0.0 || participants <= 1) {
        return 0.0;
    }
    const auto stats = average_link_stats(topology);
    if (stats.avg_bw_gbps <= 0.0) {
        return std::numeric_limits<double>::infinity();
    }

    const double payload = bytes / 1e9;
    const double alpha = stats.avg_latency_s;
    const double beta = payload / stats.avg_bw_gbps;
    const double p = static_cast<double>(participants);
    const std::string kind = canonical_comm_kind(comm_kind);

    if (kind == "allreduce") {
        return allreduce_time_seconds(bytes, participants, topology);
    }
    if (kind == "allgather" || kind == "reducescatter") {
        return (p - 1.0) * alpha + ((p - 1.0) / p) * beta;
    }
    if (kind == "broadcast" || kind == "reduce") {
        const double stages = std::ceil(std::log2(p));
        return stages * alpha + beta;
    }
    if (kind == "alltoall") {
        return (p - 1.0) * alpha + ((p - 1.0) / p) * beta;
    }
    return std::numeric_limits<double>::infinity();
}

double estimate_makespan_seconds(const mapping::TaskGraph& graph,
                                 const mapping::MappingPlan& plan,
                                 const hardware_topology::HardwareTopology& topology) {
    const auto& devices = topology.devices();
    if (devices.empty()) {
        return std::numeric_limits<double>::infinity();
    }

    std::unordered_map<std::string, double> available;
    for (const auto* device : devices) {
        available[device->id] = 0.0;
    }
    std::unordered_map<std::string, double> finish_time;
    std::unordered_map<std::string, double> collective_finish_time;
    const auto collective_info = build_collective_info(graph);

    double makespan = 0.0;
    for (const auto& task : graph.topological_order()) {
        const auto map_it = plan.assignments.find(task.name);
        if (map_it == plan.assignments.end()) {
            return std::numeric_limits<double>::infinity();
        }
        const auto& device_id = map_it->second;
        const auto* device = topology.device(device_id);
        if (device == nullptr) {
            return std::numeric_limits<double>::infinity();
        }

        double ready = available[device_id];
        for (const auto& edge : graph.dependencies(task.name)) {
            const auto pred_assign = plan.assignments.find(edge.src);
            const auto pred_finish = finish_time.find(edge.src);
            if (pred_assign == plan.assignments.end() || pred_finish == finish_time.end()) {
                return std::numeric_limits<double>::infinity();
            }

            double comm = 0.0;
            if (is_collective_kind(edge.comm_kind)) {
                const auto key = collective_event_key(edge);
                const auto cached = collective_finish_time.find(key);
                if (cached != collective_finish_time.end()) {
                    ready = std::max(ready, cached->second);
                    continue;
                }
                auto info_it = collective_info.find(key);
                double collective_start = pred_finish->second;
                std::size_t participants = devices.size();
                double bytes = edge.tensor_bytes;
                std::string kind = edge.comm_kind;
                if (info_it != collective_info.end()) {
                    participants = std::max<std::size_t>(2, info_it->second.all_tasks.size());
                    bytes = std::max(bytes, info_it->second.bytes);
                    kind = info_it->second.kind;
                    collective_start = 0.0;
                    for (const auto& src_task : info_it->second.src_tasks) {
                        const auto src_finish = finish_time.find(src_task);
                        if (src_finish == finish_time.end()) {
                            return std::numeric_limits<double>::infinity();
                        }
                        collective_start = std::max(collective_start, src_finish->second);
                    }
                }
                comm = collective_time_seconds(kind, bytes, participants, topology);
                const double collective_done = collective_start + comm;
                collective_finish_time.emplace(key, collective_done);
                ready = std::max(ready, collective_done);
                continue;
            } else if (pred_assign->second != device_id && edge.tensor_bytes > 0.0) {
                comm = topology.get_transfer_time(pred_assign->second,
                                                 device_id,
                                                 static_cast<size_t>(edge.tensor_bytes));
            }
            ready = std::max(ready, pred_finish->second + comm);
        }

        const double finish = ready + task_exec_time_seconds(task, device);
        available[device_id] = finish;
        finish_time[task.name] = finish;
        makespan = std::max(makespan, finish);
    }

    return makespan;
}

std::unique_ptr<mapping::Mapper> build_mapper_for_graph(const mapping::TaskGraph& graph,
                                                        const mapper::Options& options) {
    std::unique_ptr<mapping::Mapper> base_mapper;
    if (options.mapper == "heft") {
        base_mapper = std::make_unique<mapping::HeftMapper>();
    } else if (options.mapper == "peft") {
        base_mapper = std::make_unique<mapping::PeftMapper>();
    } else if (options.mapper == "greedy") {
        base_mapper = std::make_unique<mapping::GreedyMapper>();
    } else {
        throw std::runtime_error("Unknown mapper: " + options.mapper);
    }

    if (options.parts <= 0) {
        return base_mapper;
    }

    mapping::LayerPartition partition;
    const auto task_partitions = partition.partition(graph, options.parts);
    std::vector<std::vector<std::string>> partitions;
    partitions.reserve(task_partitions.size());
    for (const auto& block : task_partitions) {
        std::vector<std::string> names;
        names.reserve(block.size());
        for (const auto& task : block) {
            names.push_back(task.name);
        }
        partitions.push_back(std::move(names));
    }
    return std::make_unique<mapping::PartitionerMapper>(std::move(base_mapper), std::move(partitions));
}

std::uint64_t safe_edge_bytes(double bytes) {
    if (bytes <= 0.0) {
        return 0;
    }
    const long double rounded = std::llround(bytes);
    if (rounded <= 0.0L) {
        return 0;
    }
    const auto max_u64 = static_cast<long double>(std::numeric_limits<std::uint64_t>::max());
    if (rounded >= max_u64) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(rounded);
}

std::uint64_t saturating_add(std::uint64_t a, std::uint64_t b) {
    if (std::numeric_limits<std::uint64_t>::max() - a < b) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return a + b;
}

std::vector<NamedCount> sort_named_count(const std::unordered_map<std::string, std::size_t>& counts) {
    std::vector<NamedCount> out;
    out.reserve(counts.size());
    for (const auto& entry : counts) {
        out.push_back({entry.first, entry.second});
    }
    std::sort(out.begin(),
              out.end(),
              [](const NamedCount& a, const NamedCount& b) {
                  if (a.count != b.count) {
                      return a.count > b.count;
                  }
                  return a.name < b.name;
              });
    return out;
}

std::vector<NamedBytes> sort_named_bytes(const std::unordered_map<std::string, std::uint64_t>& bytes) {
    std::vector<NamedBytes> out;
    out.reserve(bytes.size());
    for (const auto& entry : bytes) {
        out.push_back({entry.first, entry.second});
    }
    std::sort(out.begin(),
              out.end(),
              [](const NamedBytes& a, const NamedBytes& b) {
                  if (a.bytes != b.bytes) {
                      return a.bytes > b.bytes;
                  }
                  return a.name < b.name;
              });
    return out;
}

}  // namespace

RunResult write_taskflow(const hardware_topology::HardwareTopology& topology,
                         const workload::Workload& workload,
                         const std::string& taskflow_path,
                         const Options& options) {
    const auto base_graph = workload.to_task_graph(topology);

    std::vector<ParallelMode> modes;
    if (options.parallel == "auto") {
        modes = {ParallelMode::HINT, ParallelMode::NONE, ParallelMode::ALL};
    } else {
        const auto mode = parse_parallel_mode(options.parallel);
        if (!mode.has_value()) {
            throw std::runtime_error("Unknown parallel mode: " + options.parallel +
                                     " (expected: auto|none|hint|all)");
        }
        modes = {*mode};
    }

    bool selected = false;
    double best_makespan = std::numeric_limits<double>::infinity();
    std::size_t best_tasks = std::numeric_limits<std::size_t>::max();
    ParallelMode best_mode = ParallelMode::NONE;
    mapping::TaskGraph best_graph;
    mapping::MappingPlan best_plan;

    for (const auto mode : modes) {
        const auto expanded_graph = expand_data_parallel(base_graph, workload, topology, mode);
        const auto annotated_graph = annotate_comm_bytes(expanded_graph, workload, topology);
        auto mapper = build_mapper_for_graph(annotated_graph, options);
        const auto mapping_plan = mapper->map(annotated_graph, topology);
        const double makespan = estimate_makespan_seconds(annotated_graph, mapping_plan, topology);
        const std::size_t task_count = annotated_graph.topological_order().size();

        if (!selected ||
            makespan < best_makespan ||
            (makespan == best_makespan && task_count < best_tasks)) {
            selected = true;
            best_makespan = makespan;
            best_tasks = task_count;
            best_mode = mode;
            best_graph = annotated_graph;
            best_plan = mapping_plan;
        }
    }

    if (!selected) {
        throw std::runtime_error("Failed to build mapping candidate");
    }

    std::unordered_map<std::string, std::size_t> subtype_counts;
    std::unordered_map<std::string, std::size_t> device_counts;
    std::unordered_map<std::string, std::uint64_t> comm_kind_bytes;
    std::unordered_map<std::string, std::size_t> longest_depth_to_task;
    std::size_t edge_count = 0;
    std::size_t source_count = 0;
    std::size_t sink_count = 0;
    std::size_t dag_depth = 0;
    std::size_t cross_device_edge_count = 0;
    std::uint64_t total_edge_bytes = 0;
    std::uint64_t cross_device_edge_bytes = 0;

    const auto& topo = best_graph.topological_order();
    source_count = best_graph.source_tasks().size();
    sink_count = best_graph.sink_tasks().size();

    for (const auto& task : topo) {
        const std::string raw_subtype = task.subtype.empty() ? (task.type.empty() ? "unknown" : task.type) : task.subtype;
        const std::string subtype = canonical_task_subtype(raw_subtype);
        subtype_counts[subtype] += 1;

        const auto assigned = best_plan.assignments.find(task.name);
        const std::string device = assigned == best_plan.assignments.end() ? "?" : assigned->second;
        device_counts[device] += 1;

        std::size_t max_pred_depth = 0;
        for (const auto& dep : best_graph.dependencies(task.name)) {
            const auto it = longest_depth_to_task.find(dep.src);
            if (it != longest_depth_to_task.end()) {
                max_pred_depth = std::max(max_pred_depth, it->second);
            }
        }
        const std::size_t my_depth = max_pred_depth + 1;
        longest_depth_to_task[task.name] = my_depth;
        dag_depth = std::max(dag_depth, my_depth);

        for (const auto& edge : best_graph.successors(task.name)) {
            edge_count += 1;
            const std::string kind = edge.comm_kind.empty() ? "p2p" : canonical_comm_kind(edge.comm_kind);

            const auto src_it = best_plan.assignments.find(edge.src);
            const auto dst_it = best_plan.assignments.find(edge.dst);
            const bool cross_device =
                (src_it != best_plan.assignments.end() && dst_it != best_plan.assignments.end() &&
                 src_it->second != dst_it->second);
            const bool needs_transfer = cross_device || is_collective_kind(kind);
            const std::uint64_t bytes = needs_transfer ? safe_edge_bytes(edge.tensor_bytes) : 0;
            total_edge_bytes = saturating_add(total_edge_bytes, bytes);

            if (bytes > 0) {
                auto& kind_total = comm_kind_bytes[kind];
                kind_total = saturating_add(kind_total, bytes);
            }

            if (cross_device) {
                cross_device_edge_count += 1;
                cross_device_edge_bytes = saturating_add(cross_device_edge_bytes, bytes);
            }
        }
    }

    taskflow::TaskflowWriter::write(taskflow_path, options.time_unit, best_graph, best_plan, topology);
    return RunResult{
        best_makespan,
        parallel_mode_name(best_mode),
        best_tasks,
        edge_count,
        source_count,
        sink_count,
        dag_depth,
        total_edge_bytes,
        cross_device_edge_count,
        cross_device_edge_bytes,
        sort_named_count(subtype_counts),
        sort_named_count(device_counts),
        sort_named_bytes(comm_kind_bytes),
    };
}

}  // namespace mapper
