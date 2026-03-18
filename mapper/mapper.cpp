#include "mapper/mapper.h"

#include "mapping/mapper.h"
#include "mapping/strategies.h"
#include "taskflow/taskflow.h"

#include <memory>
#include <string>
#include <vector>

namespace mapper {

void write_taskflow(const hardware_topology::HardwareTopology& topology,
                    const workload::Workload& workload,
                    const std::string& taskflow_path,
                    const Options& options) {
    const auto graph = workload.to_task_graph();

    std::unique_ptr<mapping::Mapper> mapper;
    if (options.parts > 0) {
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
        mapper = std::make_unique<mapping::PartitionerMapper>(std::make_unique<mapping::GreedyMapper>(),
                                                              std::move(partitions));
    } else {
        mapper = std::make_unique<mapping::GreedyMapper>();
    }

    const auto mapping_plan = mapper->map(graph, topology);
    taskflow::TaskflowWriter::write(taskflow_path, options.time_unit, graph, mapping_plan, topology);
}

}  // namespace mapper
