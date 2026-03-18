#pragma once

#include <string>

#include "hardware_topology/topology.h"
#include "workload/workload.h"

namespace mapper {

struct Options {
    int parts{0};
    std::string time_unit{"s"};
};

// Mapper entrypoint: binds tasks to devices and emits taskflow JSON for simulators.
void write_taskflow(const hardware_topology::HardwareTopology& topology,
                    const workload::Workload& workload,
                    const std::string& taskflow_path,
                    const Options& options = {});

}  // namespace mapper
