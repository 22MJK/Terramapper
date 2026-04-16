#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#include "hardware_topology/topology.h"
#include "workload/workload.h"

namespace mapper {

struct NamedCount {
    std::string name;
    std::size_t count{0};
};

struct NamedBytes {
    std::string name;
    std::uint64_t bytes{0};
};

struct Options {
    int parts{0};
    std::string time_unit{"s"};
    std::string mapper{"heft"};
    std::string parallel{"auto"};
};

struct RunResult {
    double estimated_makespan_s{0.0};
    std::string selected_parallel{"none"};
    std::size_t task_count{0};
    std::size_t edge_count{0};
    std::size_t source_count{0};
    std::size_t sink_count{0};
    std::size_t dag_depth{0};
    std::uint64_t total_edge_bytes{0};
    std::size_t cross_device_edge_count{0};
    std::uint64_t cross_device_edge_bytes{0};
    std::vector<NamedCount> task_subtype_counts;
    std::vector<NamedCount> device_task_counts;
    std::vector<NamedBytes> comm_kind_bytes;
};

// Mapper entrypoint: binds tasks to devices and emits taskflow JSON for simulators.
RunResult write_taskflow(const hardware_topology::HardwareTopology& topology,
                         const workload::Workload& workload,
                         const std::string& taskflow_path,
                         const Options& options = {});

}  // namespace mapper
