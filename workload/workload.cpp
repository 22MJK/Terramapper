#include "workload/workload.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace workload {
namespace {

std::size_t dtype_size(DType dtype) {
    switch (dtype) {
        case DType::FP32:
        case DType::INT32:
            return 4;
        case DType::FP64:
        case DType::INT64:
            return 8;
    }
    return 4;
}

std::uint64_t tensor_bytes(const Tensor& tensor) {
    if (tensor.bytes.has_value()) {
        return *tensor.bytes;
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

std::size_t group_size(const Distribution& dist,
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

double estimate_transfer_bytes(const Tensor& tensor,
                               AccessKind access,
                               const std::unordered_map<std::string, std::vector<std::string>>& groups) {
    const auto total_bytes = static_cast<double>(tensor_bytes(tensor));
    if (total_bytes <= 0.0) {
        return 0.0;
    }

    const std::size_t gsize = group_size(tensor.distribution, groups);
    switch (tensor.distribution.kind) {
        case DistKind::REPLICATED:
        case DistKind::NONE:
            return total_bytes;
        case DistKind::BLOCK:
        case DistKind::CYCLIC:
            if (access == AccessKind::ROW_WISE || access == AccessKind::COL_WISE) {
                return total_bytes / static_cast<double>(gsize);
            }
            return total_bytes;
    }
    return total_bytes;
}

}  // namespace

Workload::Workload(std::string name,
                   std::vector<Task> tasks,
                   std::vector<Tensor> tensors,
                   std::vector<DeviceGroup> device_groups)
    : name_(std::move(name)),
      tasks_(std::move(tasks)),
      tensors_(std::move(tensors)),
      device_groups_(std::move(device_groups)) {}

mapping::TaskGraph Workload::to_task_graph(const hardware_topology::HardwareTopology& topology) const {
    mapping::TaskGraph graph;
    std::unordered_map<int, std::string> id_to_name;
    id_to_name.reserve(tasks_.size());

    for (const auto& task : tasks_) {
        mapping::Task mapped;
        mapped.name = task.name;
        mapped.type = "compute";
        mapped.subtype = task.op;
        mapped.compute_flops = task.compute_flops;
        mapped.comm_bytes = 0.0;
        graph.add_task(std::move(mapped));
        id_to_name.emplace(task.id, task.name);
    }

    std::unordered_map<std::string, Tensor> tensor_map;
    tensor_map.reserve(tensors_.size());
    for (const auto& tensor : tensors_) {
        tensor_map.emplace(tensor.id, tensor);
    }

    std::unordered_map<std::string, std::vector<std::string>> group_members;
    group_members.reserve(device_groups_.size());
    for (const auto& group : device_groups_) {
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

    for (const auto& task : tasks_) {
        const auto dst_it = id_to_name.find(task.id);
        if (dst_it == id_to_name.end()) {
            throw std::runtime_error("Task id not found while building graph");
        }
        for (const auto& input : task.inputs) {
            const auto tensor_it = tensor_map.find(input.tensor_id);
            if (tensor_it == tensor_map.end()) {
                throw std::runtime_error("Input tensor not found: " + input.tensor_id);
            }
            const auto& tensor = tensor_it->second;
            if (!tensor.producer_task.has_value()) {
                continue;
            }
            const auto src_it = id_to_name.find(*tensor.producer_task);
            if (src_it == id_to_name.end()) {
                throw std::runtime_error("Tensor producer task not found");
            }
            const double bytes = estimate_transfer_bytes(tensor, input.access, group_members);
            std::string comm_kind;
            if (tensor.collective.has_value()) {
                comm_kind = tensor.collective->type;
            }
            graph.add_edge(src_it->second, dst_it->second, bytes, tensor.id, comm_kind);
        }
    }

    return graph;
}

const std::string& Workload::name() const {
    return name_;
}

}  // namespace workload
