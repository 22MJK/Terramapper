#include "hardware_topology/topology.h"
#include "mapper/mapper.h"
#include "workload/workload.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

int parse_int_arg(const std::string& arg, const std::string& prefix, int default_value) {
    if (arg.rfind(prefix, 0) != 0) {
        return default_value;
    }
    const auto value_str = arg.substr(prefix.size());
    return std::atoi(value_str.c_str());
}

std::string parse_string_arg(const std::string& arg, const std::string& prefix, const std::string& default_value) {
    if (arg.rfind(prefix, 0) != 0) {
        return default_value;
    }
    return arg.substr(prefix.size());
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
    std::string time_unit = "s";
    std::string taskflow_path = "taskflow.json";
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        node_count = parse_int_arg(arg, "--nodes=", node_count);
        depth = parse_int_arg(arg, "--depth=", depth);
        parts = parse_int_arg(arg, "--parts=", parts);
        time_unit = parse_string_arg(arg, "--time_unit=", time_unit);
        taskflow_path = parse_string_arg(arg, "--out=", taskflow_path);
    }
    if (node_count <= 0 || depth <= 0) {
        std::cerr << "Usage: mapper_demo [--nodes=N] [--depth=D] [--parts=P] [--time_unit=UNIT] [--out=PATH]\n";
        return 2;
    }

    const auto topology = build_demo_topology(node_count);

    workload::WorkloadGenerator generator;
    const auto workload = generator.build("demo", depth);

    mapper::Options options;
    options.parts = parts;
    options.time_unit = time_unit;
    mapper::write_taskflow(topology, workload, taskflow_path, options);
    std::cout << "Wrote " << taskflow_path << "\n";
    return 0;
}
