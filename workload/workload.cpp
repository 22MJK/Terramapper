#include "workload/workload.h"

#include <cctype>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace workload {
namespace {

const char* access_kind_name(AccessKind access) {
    switch (access) {
        case AccessKind::DENSE:
            return "dense";
        case AccessKind::SPARSE_CSR:
            return "sparse_csr";
        case AccessKind::ROW_WISE:
            return "row-wise";
        case AccessKind::COL_WISE:
            return "col-wise";
    }
    return "dense";
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

}  // namespace

Workload::Workload(std::string name,
                   std::vector<Task> tasks,
                   std::vector<Tensor> tensors,
                   std::vector<DeviceGroup> device_groups,
                   std::vector<std::string> iteration_inputs,
                   std::vector<std::string> iteration_outputs)
    : name_(std::move(name)),
      tasks_(std::move(tasks)),
      tensors_(std::move(tensors)),
      device_groups_(std::move(device_groups)),
      iteration_inputs_(std::move(iteration_inputs)),
      iteration_outputs_(std::move(iteration_outputs)) {}

mapping::TaskGraph Workload::to_task_graph(const hardware_topology::HardwareTopology& topology) const {
    mapping::TaskGraph graph;
    std::unordered_map<int, std::string> id_to_name;
    id_to_name.reserve(tasks_.size());
    (void)topology;

    std::unordered_map<std::string, Tensor> tensor_map;
    tensor_map.reserve(tensors_.size());
    for (const auto& tensor : tensors_) {
        tensor_map.emplace(tensor.id, tensor);
    }

    for (const auto& task : tasks_) {
        mapping::Task mapped;
        mapped.name = task.name;
        mapped.type = "compute";
        mapped.subtype = task.op;
        mapped.compute_flops = task.compute_flops;
        mapped.comm_bytes = 0.0;

        double estimated_memory_bytes = task.memory_bytes;
        if (estimated_memory_bytes <= 0.0) {
            for (const auto& input : task.inputs) {
                const auto tensor_it = tensor_map.find(input.tensor_id);
                if (tensor_it == tensor_map.end()) {
                    throw std::runtime_error("Input tensor not found: " + input.tensor_id);
                }
                estimated_memory_bytes += static_cast<double>(tensor_it->second.size_bytes);
            }
            for (const auto& output_id : task.outputs) {
                const auto tensor_it = tensor_map.find(output_id);
                if (tensor_it == tensor_map.end()) {
                    continue;
                }
                estimated_memory_bytes += static_cast<double>(tensor_it->second.size_bytes);
            }
        }
        mapped.memory_bytes = estimated_memory_bytes;

        graph.add_task(std::move(mapped));
        id_to_name.emplace(task.id, task.name);
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
            std::string comm_kind = "p2p";
            if (tensor.collective.has_value()) {
                comm_kind = canonical_comm_kind(tensor.collective->type);
                if (comm_kind.empty()) {
                    comm_kind = "p2p";
                }
            }
            const std::string access_pattern = access_kind_name(input.access);
            graph.add_edge(src_it->second, dst_it->second, 0.0, tensor.id, comm_kind, access_pattern);
        }
    }

    return graph;
}

const std::string& Workload::name() const {
    return name_;
}

const std::vector<Task>& Workload::tasks() const {
    return tasks_;
}

const std::vector<Tensor>& Workload::tensors() const {
    return tensors_;
}

const std::vector<DeviceGroup>& Workload::device_groups() const {
    return device_groups_;
}

const std::vector<std::string>& Workload::iteration_inputs() const {
    return iteration_inputs_;
}

const std::vector<std::string>& Workload::iteration_outputs() const {
    return iteration_outputs_;
}

}  // namespace workload
