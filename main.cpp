#include "hardware_topology/topology.h"
#include "mapping/mapper.h"
#include "mapping/strategies.h"
#include "schedule/scheduler.h"
#include "trace_generator/trace.h"
#include "workload/workload.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

int parse_int_arg(const std::string& arg, const std::string& prefix, int default_value) {
    if (arg.rfind(prefix, 0) != 0) {
        return default_value;
    }
    const auto value_str = arg.substr(prefix.size());
    return std::atoi(value_str.c_str());
}

hardware_topology::HardwareTopology build_demo_topology(int node_count) {
    hardware_topology::HardwareTopology topology;
    for (int i = 0; i < node_count; ++i) {
        hardware_topology::ComputeNode node;
        node.name = "node_" + std::to_string(i);
        node.cores = 64;
        node.memory_gb = 256.0;
        node.gflops = 10000.0;
        topology.add_node(std::move(node));
    }

    // Ring links by default to keep topology small but connected.
    if (node_count > 1) {
        for (int i = 0; i < node_count; ++i) {
            const int j = (i + 1) % node_count;
            hardware_topology::Link link;
            link.src = "node_" + std::to_string(i);
            link.dst = "node_" + std::to_string(j);
            link.bandwidth_gbps = 200.0;  // treat as GB/s for the simple simulator
            link.latency_ms = 0.5;
            topology.add_link(std::move(link));
        }
    }

    return topology;
}

}  // namespace

int main(int argc, char** argv) {
    int node_count = 2;
    int depth = 6;
    int parts = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        node_count = parse_int_arg(arg, "--nodes=", node_count);
        depth = parse_int_arg(arg, "--depth=", depth);
        parts = parse_int_arg(arg, "--parts=", parts);
    }
    if (node_count <= 0 || depth <= 0) {
        std::cerr << "Usage: mapper_demo [--nodes=N] [--depth=D] [--parts=P]\n";
        return 2;
    }

    const auto topology = build_demo_topology(node_count);

    workload::WorkloadGenerator generator;
    const auto workload = generator.build("demo", depth);
    const auto graph = workload.to_task_graph();

    std::unique_ptr<mapping::Mapper> mapper;
    if (parts > 0) {
        mapping::LayerPartition partition;
        const auto task_partitions = partition.partition(graph, parts);
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

    schedule::SimpleScheduler scheduler;
    const auto schedule_plan = scheduler.schedule(graph, mapping_plan, topology);

    trace_generator::TraceGenerator trace_generator;
    const auto trace = trace_generator.generate(schedule_plan);
    const auto lines = trace_generator::TraceWriter::to_lines(trace);
    for (const auto& line : lines) {
        std::cout << line << "\n";
    }
    return 0;
}

