#include "mapping/mapper.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace mapping {

const std::string& MappingPlan::node_for(const std::string& task_name) const {
    return assignments.at(task_name);
}

namespace {

constexpr double kComputeEfficiency = 0.1;
double compute_time_seconds(const Task& task, const hardware_topology::Device* device);

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
    const auto k = canonical_comm_kind(kind);
    return k == "allreduce" || k == "allgather" || k == "reducescatter" || k == "broadcast" || k == "reduce" ||
           k == "alltoall";
}

std::string collective_event_key(const TaskEdge& edge) {
    return edge.tensor_id + "|" + canonical_comm_kind(edge.comm_kind);
}

struct CollectiveInfo {
    std::unordered_set<std::string> src_tasks;
    std::unordered_set<std::string> all_tasks;
    double bytes{0.0};
    std::string kind;
};

std::unordered_map<std::string, CollectiveInfo> build_collective_info(const TaskGraph& graph) {
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

std::optional<std::string> pinned_device_tag(const Task& task) {
    for (const auto& tag : task.tags) {
        static const std::string prefix = "device:";
        if (tag.rfind(prefix, 0) == 0 && tag.size() > prefix.size()) {
            return tag.substr(prefix.size());
        }
    }
    return std::nullopt;
}

std::unordered_set<std::string> allowed_devices_from_tags(const Task& task) {
    std::unordered_set<std::string> allowed;
    static const std::string prefix = "allow:";
    for (const auto& tag : task.tags) {
        if (tag.rfind(prefix, 0) != 0 || tag.size() <= prefix.size()) {
            continue;
        }
        allowed.insert(tag.substr(prefix.size()));
    }
    return allowed;
}

}  // namespace

MappingPlan GreedyMapper::map(const TaskGraph& graph, const hardware_topology::HardwareTopology& topology) const {
    const auto& devices = topology.devices();
    if (devices.empty()) {
        throw std::runtime_error("Topology has no devices");
    }

    std::unordered_map<std::string, double> available;
    for (const auto* device : devices) {
        available[device->id] = 0.0;
    }

    MappingPlan plan;
    for (const auto& task : graph.topological_order()) {
        const auto pin = pinned_device_tag(task);
        const auto allowed = allowed_devices_from_tags(task);
        const hardware_topology::Device* target = nullptr;
        double best_finish = std::numeric_limits<double>::infinity();
        if (pin.has_value()) {
            target = topology.device(*pin);
            if (target == nullptr) {
                throw std::runtime_error("Pinned device not found: " + *pin);
            }
            if (!allowed.empty() && allowed.find(target->id) == allowed.end()) {
                throw std::runtime_error("Pinned device " + target->id + " is not in allowed device set");
            }
            best_finish = available[target->id] + compute_time_seconds(task, target);
        } else {
            for (const auto* device : devices) {
                if (!allowed.empty() && allowed.find(device->id) == allowed.end()) {
                    continue;
                }
                const double finish = available[device->id] + compute_time_seconds(task, device);
                if (finish < best_finish || (finish == best_finish && (target == nullptr || device->id < target->id))) {
                    best_finish = finish;
                    target = device;
                }
            }
            if (target == nullptr) {
                throw std::runtime_error("No eligible device for task: " + task.name);
            }
        }
        plan.assignments[task.name] = target->id;
        available[target->id] = best_finish;
    }
    return plan;
}

namespace {

double compute_only_time_seconds(const Task& task, const hardware_topology::Device* device) {
    if (task.compute_flops <= 0.0) {
        return 0.0;
    }
    if (device == nullptr || device->peak_gflops <= 0.0) {
        return std::numeric_limits<double>::infinity();
    }
    const double effective_gflops = std::max(1e-9, device->peak_gflops * kComputeEfficiency);
    return task.compute_flops / (effective_gflops * 1e9);
}

double memory_only_time_seconds(const Task& task, const hardware_topology::Device* device) {
    if (task.memory_bytes <= 0.0) {
        return 0.0;
    }
    if (device == nullptr || device->mem_bw_gbps <= 0.0) {
        return std::numeric_limits<double>::infinity();
    }
    return task.memory_bytes / (device->mem_bw_gbps * 1e9);
}

double compute_time_seconds(const Task& task, const hardware_topology::Device* device) {
    const double compute_time = compute_only_time_seconds(task, device);
    const double memory_time = memory_only_time_seconds(task, device);
    return std::max(compute_time, memory_time);
}

double average_compute_time(const Task& task, const std::vector<const hardware_topology::Device*>& devices) {
    if (devices.empty()) {
        return std::numeric_limits<double>::infinity();
    }
    double total = 0.0;
    for (const auto* device : devices) {
        total += compute_time_seconds(task, device);
    }
    return total / static_cast<double>(devices.size());
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
    LinkStats stats;
    stats.avg_bw_gbps = bw_sum / static_cast<double>(links.size());
    stats.avg_latency_s = (lat_sum / static_cast<double>(links.size())) / 1000.0;
    return stats;
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

double average_comm_time(const TaskEdge& edge,
                         const std::vector<const hardware_topology::Device*>& devices,
                         const hardware_topology::HardwareTopology& topology) {
    if (edge.tensor_bytes <= 0.0 || devices.size() < 2) {
        return 0.0;
    }
    if (is_collective_kind(edge.comm_kind)) {
        return collective_time_seconds(edge.comm_kind, edge.tensor_bytes, devices.size(), topology);
    }
    double total = 0.0;
    std::size_t count = 0;
    for (const auto* src : devices) {
        for (const auto* dst : devices) {
            if (src->id == dst->id) {
                continue;
            }
            const double time = topology.get_transfer_time(src->id, dst->id,
                                                           static_cast<size_t>(edge.tensor_bytes));
            if (!std::isfinite(time)) {
                continue;
            }
            total += time;
            count += 1;
        }
    }
    if (count == 0) {
        return 0.0;
    }
    return total / static_cast<double>(count);
}

}  // namespace

MappingPlan HeftMapper::map(const TaskGraph& graph, const hardware_topology::HardwareTopology& topology) const {
    const auto& devices = topology.devices();
    if (devices.empty()) {
        throw std::runtime_error("Topology has no devices");
    }

    const auto& topo = graph.topological_order();
    std::unordered_map<std::string, Task> tasks;
    tasks.reserve(topo.size());
    for (const auto& task : topo) {
        tasks.emplace(task.name, task);
    }

    std::unordered_map<std::string, double> avg_comp;
    avg_comp.reserve(topo.size());
    for (const auto& task : topo) {
        avg_comp.emplace(task.name, average_compute_time(task, devices));
    }
    const auto collective_info = build_collective_info(graph);

    // Collective communication is one logical event per produced tensor.
    // Multiple consumer edges should share that cost instead of paying repeatedly.
    std::unordered_map<std::string, std::size_t> collective_fanout;
    for (const auto& task : topo) {
        for (const auto& edge : graph.successors(task.name)) {
            if (!is_collective_kind(edge.comm_kind)) {
                continue;
            }
            collective_fanout[collective_event_key(edge)] += 1;
        }
    }

    std::unordered_map<std::string, double> rank_cache;
    rank_cache.reserve(topo.size());

    std::function<double(const std::string&)> rank_u = [&](const std::string& name) -> double {
        auto it = rank_cache.find(name);
        if (it != rank_cache.end()) {
            return it->second;
        }
        double max_succ = 0.0;
        for (const auto& edge : graph.successors(name)) {
            double comm = 0.0;
            if (is_collective_kind(edge.comm_kind)) {
                const auto key = collective_event_key(edge);
                const auto info_it = collective_info.find(key);
                std::size_t participants = devices.size();
                double bytes = edge.tensor_bytes;
                std::string kind = edge.comm_kind;
                if (info_it != collective_info.end()) {
                    participants = std::max<std::size_t>(2, info_it->second.all_tasks.size());
                    bytes = std::max(bytes, info_it->second.bytes);
                    kind = info_it->second.kind;
                }
                comm = collective_time_seconds(kind, bytes, participants, topology);
                const auto it = collective_fanout.find(collective_event_key(edge));
                const std::size_t fanout = (it == collective_fanout.end() || it->second == 0) ? 1 : it->second;
                comm /= static_cast<double>(fanout);
            } else {
                comm = average_comm_time(edge, devices, topology);
            }
            max_succ = std::max(max_succ, comm + rank_u(edge.dst));
        }
        const double rank = avg_comp.at(name) + max_succ;
        rank_cache.emplace(name, rank);
        return rank;
    };

    for (const auto& task : topo) {
        rank_u(task.name);
    }

    std::vector<std::string> order;
    order.reserve(topo.size());
    for (const auto& task : topo) {
        order.push_back(task.name);
    }
    std::sort(order.begin(), order.end(), [&](const std::string& a, const std::string& b) {
        const double ra = rank_cache.at(a);
        const double rb = rank_cache.at(b);
        if (ra == rb) {
            return a < b;
        }
        return ra > rb;
    });

    const double kInf = std::numeric_limits<double>::infinity();
    std::unordered_map<std::string, std::string> assignment;
    std::unordered_map<std::string, double> finish_time;
    std::unordered_map<std::string, double> collective_finish_time;
    assignment.reserve(topo.size());
    finish_time.reserve(topo.size());
    std::vector<double> available(devices.size(), 0.0);

    for (const auto& name : order) {
        const auto& task = tasks.at(name);
        double best_finish = kInf;
        std::size_t best_device = 0;

        const auto pin = pinned_device_tag(task);
        const auto allowed = allowed_devices_from_tags(task);
        for (std::size_t i = 0; i < devices.size(); ++i) {
            const auto* device = devices[i];
            if (pin.has_value() && device->id != *pin) {
                continue;
            }
            if (!allowed.empty() && allowed.find(device->id) == allowed.end()) {
                continue;
            }
            double ready = available[i];
            for (const auto& edge : graph.dependencies(name)) {
                const auto& pred_name = edge.src;
                const auto& pred_device = assignment.at(pred_name);
                double comm = 0.0;
                if (is_collective_kind(edge.comm_kind)) {
                    const auto key = collective_event_key(edge);
                    const auto cached = collective_finish_time.find(key);
                    if (cached != collective_finish_time.end()) {
                        ready = std::max(ready, cached->second);
                        continue;
                    }
                    auto info_it = collective_info.find(key);
                    double collective_start = finish_time.at(pred_name);
                    std::size_t participants = devices.size();
                    double bytes = edge.tensor_bytes;
                    std::string kind = edge.comm_kind;
                    if (info_it != collective_info.end()) {
                        participants = std::max<std::size_t>(2, info_it->second.all_tasks.size());
                        bytes = std::max(bytes, info_it->second.bytes);
                        kind = info_it->second.kind;
                        collective_start = 0.0;
                        bool all_ready = true;
                        for (const auto& src_task : info_it->second.src_tasks) {
                            const auto src_finish = finish_time.find(src_task);
                            if (src_finish == finish_time.end()) {
                                all_ready = false;
                                break;
                            }
                            collective_start = std::max(collective_start, src_finish->second);
                        }
                        if (!all_ready) {
                            ready = kInf;
                            break;
                        }
                    }
                    comm = collective_time_seconds(kind, bytes, participants, topology);
                    const double collective_done = collective_start + comm;
                    collective_finish_time.emplace(key, collective_done);
                    ready = std::max(ready, collective_done);
                    continue;
                } else if (pred_device != device->id && edge.tensor_bytes > 0.0) {
                    const double t = topology.get_transfer_time(pred_device, device->id,
                                                                static_cast<size_t>(edge.tensor_bytes));
                    comm = std::isfinite(t) ? t : kInf;
                }
                const double pred_finish = finish_time.at(pred_name);
                ready = std::max(ready, pred_finish + comm);
            }

            const double compute_time = compute_time_seconds(task, device);
            const double finish = ready + compute_time;
            if (finish < best_finish || (finish == best_finish && device->id < devices[best_device]->id)) {
                best_finish = finish;
                best_device = i;
            }
        }
        if (!std::isfinite(best_finish)) {
            throw std::runtime_error("No eligible device for task: " + name);
        }

        assignment[name] = devices[best_device]->id;
        finish_time[name] = best_finish;
        available[best_device] = best_finish;
    }

    MappingPlan plan;
    plan.assignments = std::move(assignment);
    return plan;
}

MappingPlan PeftMapper::map(const TaskGraph& graph, const hardware_topology::HardwareTopology& topology) const {
    const auto& devices = topology.devices();
    if (devices.empty()) {
        throw std::runtime_error("Topology has no devices");
    }

    const auto& topo = graph.topological_order();
    std::unordered_map<std::string, Task> tasks;
    tasks.reserve(topo.size());
    for (const auto& task : topo) {
        tasks.emplace(task.name, task);
    }

    const auto collective_info = build_collective_info(graph);
    const auto kInf = std::numeric_limits<double>::infinity();

    auto eligible_device_indices = [&](const Task& task) {
        std::vector<std::size_t> indices;
        indices.reserve(devices.size());

        const auto pin = pinned_device_tag(task);
        const auto allowed = allowed_devices_from_tags(task);
        for (std::size_t i = 0; i < devices.size(); ++i) {
            const auto* device = devices[i];
            if (pin.has_value() && device->id != *pin) {
                continue;
            }
            if (!allowed.empty() && allowed.find(device->id) == allowed.end()) {
                continue;
            }
            indices.push_back(i);
        }

        if (pin.has_value() && indices.empty()) {
            throw std::runtime_error("Pinned device not found or not allowed for task: " + task.name);
        }
        if (indices.empty()) {
            throw std::runtime_error("No eligible device for task: " + task.name);
        }
        return indices;
    };

    std::unordered_map<std::string, std::vector<double>> oct_cache;
    oct_cache.reserve(topo.size());

    std::function<const std::vector<double>&(const std::string&)> compute_oct =
        [&](const std::string& task_name) -> const std::vector<double>& {
        const auto cached = oct_cache.find(task_name);
        if (cached != oct_cache.end()) {
            return cached->second;
        }

        const auto& task = tasks.at(task_name);
        auto eligible = eligible_device_indices(task);
        std::vector<double> oct(devices.size(), kInf);

            const auto& successors = graph.successors(task_name);
        for (const auto device_idx : eligible) {
            double value = 0.0;
            for (const auto& edge : successors) {
                const auto& succ_task = tasks.at(edge.dst);
                const auto succ_eligible = eligible_device_indices(succ_task);
                const auto& succ_oct = compute_oct(edge.dst);

                double best_successor = kInf;
                for (const auto succ_idx : succ_eligible) {
                    double comm = 0.0;
                    if (is_collective_kind(edge.comm_kind)) {
                        auto info_it = collective_info.find(collective_event_key(edge));
                        std::size_t participants = devices.size();
                        double bytes = edge.tensor_bytes;
                        std::string kind = edge.comm_kind;
                        if (info_it != collective_info.end()) {
                            participants = std::max<std::size_t>(2, info_it->second.all_tasks.size());
                            bytes = std::max(bytes, info_it->second.bytes);
                            kind = info_it->second.kind;
                        }
                        comm = collective_time_seconds(kind, bytes, participants, topology);
                    } else if (devices[device_idx]->id != devices[succ_idx]->id && edge.tensor_bytes > 0.0) {
                        comm = topology.get_transfer_time(devices[device_idx]->id,
                                                          devices[succ_idx]->id,
                                                          static_cast<size_t>(edge.tensor_bytes));
                    }

                    const double succ_cost = compute_time_seconds(succ_task, devices[succ_idx]);
                    const double candidate = comm + succ_cost + succ_oct[succ_idx];
                    if (candidate < best_successor) {
                        best_successor = candidate;
                    }
                }
                value = std::max(value, best_successor);
            }
            oct[device_idx] = value;
        }

        return oct_cache.emplace(task_name, std::move(oct)).first->second;
    };

    for (const auto& task : topo) {
        compute_oct(task.name);
    }

    std::unordered_map<std::string, std::string> assignment;
    std::unordered_map<std::string, double> finish_time;
    std::unordered_map<std::string, double> collective_finish_time;
    assignment.reserve(topo.size());
    finish_time.reserve(topo.size());
    std::vector<double> available(devices.size(), 0.0);

    for (const auto& task : topo) {
        const auto eligible = eligible_device_indices(task);
        const auto& oct = oct_cache.at(task.name);

        double best_metric = kInf;
        double best_finish = kInf;
        std::size_t best_device = eligible.front();

        for (const auto device_idx : eligible) {
            const auto* device = devices[device_idx];
            double ready = available[device_idx];

            for (const auto& edge : graph.dependencies(task.name)) {
                const auto& pred_name = edge.src;
                const auto pred_assign = assignment.find(pred_name);
                const auto pred_finish = finish_time.find(pred_name);
                if (pred_assign == assignment.end() || pred_finish == finish_time.end()) {
                    throw std::runtime_error("Task dependencies must be assigned before successors");
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
                        bool all_ready = true;
                        for (const auto& src_task : info_it->second.src_tasks) {
                            const auto src_finish = finish_time.find(src_task);
                            if (src_finish == finish_time.end()) {
                                all_ready = false;
                                break;
                            }
                            collective_start = std::max(collective_start, src_finish->second);
                        }
                        if (!all_ready) {
                            ready = kInf;
                            break;
                        }
                    }

                    comm = collective_time_seconds(kind, bytes, participants, topology);
                    const double collective_done = collective_start + comm;
                    collective_finish_time.emplace(key, collective_done);
                    ready = std::max(ready, collective_done);
                    continue;
                }

                if (pred_assign->second != device->id && edge.tensor_bytes > 0.0) {
                    comm = topology.get_transfer_time(pred_assign->second,
                                                     device->id,
                                                     static_cast<size_t>(edge.tensor_bytes));
                }
                ready = std::max(ready, pred_finish->second + comm);
            }

            if (!std::isfinite(ready)) {
                continue;
            }

            const double exec = compute_time_seconds(task, device);
            const double eft = ready + exec;
            const double metric = eft + oct[device_idx];
            if (metric < best_metric ||
                (metric == best_metric &&
                 (eft < best_finish || (eft == best_finish && device->id < devices[best_device]->id)))) {
                best_metric = metric;
                best_finish = eft;
                best_device = device_idx;
            }
        }

        if (!std::isfinite(best_finish)) {
            throw std::runtime_error("No eligible device for task: " + task.name);
        }

        assignment[task.name] = devices[best_device]->id;
        finish_time[task.name] = best_finish;
        available[best_device] = best_finish;
    }

    MappingPlan plan;
    plan.assignments = std::move(assignment);
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
            reordered.add_edge(edge.src,
                               edge.dst,
                               edge.tensor_bytes,
                               edge.tensor_id,
                               edge.comm_kind,
                               edge.access_pattern);
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
