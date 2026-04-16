#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "hardware_topology/topology.h"
#include "mapping/graph.h"

namespace mapping {

struct MappingPlan {
    std::unordered_map<std::string, std::string> assignments;

    const std::string& node_for(const std::string& task_name) const;
};

class Mapper {
public:
    virtual ~Mapper() = default;
    virtual MappingPlan map(const TaskGraph& graph, const hardware_topology::HardwareTopology& topology) const = 0;
};

class GreedyMapper : public Mapper {
public:
    MappingPlan map(const TaskGraph& graph, const hardware_topology::HardwareTopology& topology) const override;
};

class HeftMapper : public Mapper {
public:
    MappingPlan map(const TaskGraph& graph, const hardware_topology::HardwareTopology& topology) const override;
};

class PartitionerMapper : public Mapper {
public:
    PartitionerMapper(std::unique_ptr<Mapper> inner, std::vector<std::vector<std::string>> partitions);

    MappingPlan map(const TaskGraph& graph, const hardware_topology::HardwareTopology& topology) const override;

private:
    std::vector<Task> order(const TaskGraph& graph) const;

    std::unique_ptr<Mapper> inner_;
    std::vector<std::vector<std::string>> partitions_;
};

}  // namespace mapping
