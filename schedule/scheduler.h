#pragma once
// Scheduler helpers inspired by AstraSim iteration layers.

#include <string>
#include <vector>

#include "hardware_topology/topology.h"
#include "mapping/graph.h"
#include "mapping/mapper.h"

namespace schedule {

struct ExecutionSlot {
    std::string task_name;
    std::string node_name;
    double start_time{0.0};
    double duration{0.0};
};

struct SchedulePlan {
    std::vector<ExecutionSlot> slots;
};

class Scheduler {
public:
    virtual ~Scheduler() = default;
    virtual SchedulePlan schedule(const mapping::TaskGraph& graph,
                                  const mapping::MappingPlan& plan,
                                  const hardware_topology::HardwareTopology& topology) const = 0;
};

class SimpleScheduler : public Scheduler {
public:
    SchedulePlan schedule(const mapping::TaskGraph& graph,
                          const mapping::MappingPlan& plan,
                          const hardware_topology::HardwareTopology& topology) const override;
};

}  // namespace schedule
