#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "hardware_topology/topology.h"
#include "mapping/graph.h"

namespace workload {

enum class DType { FP32, FP64, INT32, INT64 };
enum class DistKind { NONE, REPLICATED, BLOCK, CYCLIC };
enum class AccessKind { DENSE, SPARSE_CSR, ROW_WISE, COL_WISE };

struct Distribution {
    DistKind kind{DistKind::NONE};
    int axis{-1};
    std::string group;
};

struct Partition {
    DistKind type{DistKind::NONE};
    int axis{-1};
    int num_parts{0};
};

struct Replication {
    std::string mode;
};

struct CollectiveHint {
    std::string type;
    std::string op;
    std::string group;
};

struct Tensor {
    std::string id;
    std::string name;
    DType dtype{DType::FP32};
    std::vector<std::int64_t> shape;
    std::uint64_t size_bytes{0};
    std::optional<std::uint64_t> num_elements;
    Distribution distribution;
    std::optional<Partition> partition;
    std::optional<Replication> replication;
    AccessKind access_pattern{AccessKind::DENSE};
    std::optional<CollectiveHint> collective;
    std::optional<int> producer_task;
};

struct TensorUse {
    std::string tensor_id;
    std::string role;
    AccessKind access{AccessKind::DENSE};
};

struct Task {
    int id{0};
    std::string name;
    std::string op;
    double compute_flops{0.0};
    double memory_bytes{0.0};
    std::vector<TensorUse> inputs;
    std::vector<std::string> outputs;
    std::string placement_group;
    std::string placement_parallelism;
};

struct DeviceGroup {
    std::string id;
    std::vector<std::string> members;
};

class Workload {
public:
    Workload(std::string name,
             std::vector<Task> tasks,
             std::vector<Tensor> tensors,
             std::vector<DeviceGroup> device_groups,
             std::vector<std::string> iteration_inputs = {},
             std::vector<std::string> iteration_outputs = {});
    mapping::TaskGraph to_task_graph(const hardware_topology::HardwareTopology& topology) const;
    const std::string& name() const;
    const std::vector<Task>& tasks() const;
    const std::vector<Tensor>& tensors() const;
    const std::vector<DeviceGroup>& device_groups() const;
    const std::vector<std::string>& iteration_inputs() const;
    const std::vector<std::string>& iteration_outputs() const;

private:
    std::string name_;
    std::vector<Task> tasks_;
    std::vector<Tensor> tensors_;
    std::vector<DeviceGroup> device_groups_;
    std::vector<std::string> iteration_inputs_;
    std::vector<std::string> iteration_outputs_;
};

}  // namespace workload
