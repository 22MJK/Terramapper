#pragma once

#include <string>

#include "hardware_topology/topology.h"
#include "mapping/graph.h"
#include "mapping/mapper.h"

namespace taskflow {

class TaskflowWriter {
public:
    static void write(const std::string& path,
                      const std::string& time_unit,
                      const mapping::TaskGraph& graph,
                      const mapping::MappingPlan& mapping_plan,
                      const hardware_topology::HardwareTopology& topology);
};

}  // namespace taskflow
