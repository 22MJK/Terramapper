#include "hardware_topology/topology.h"
#include "mapper/mapper.h"
#include "hardware_topology/json_io.h"
#include "workload/json_io.h"
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
    topology.set_time_unit("s");
    for (int i = 0; i < node_count; ++i) {
        hardware_topology::Device device;
        device.id = "dev_" + std::to_string(i);
        device.name = "device_" + std::to_string(i);
        device.type = "gpu";
        device.peak_gflops = 10000.0;
        device.mem_bw_gbps = 900.0;
        device.max_concurrent = 4;
        topology.add_device(std::move(device));
    }

    // Ring links by default to keep topology small but connected.
    if (node_count > 1) {
        for (int i = 0; i < node_count; ++i) {
            const int j = (i + 1) % node_count;
            hardware_topology::Link link;
            link.src = "dev_" + std::to_string(i);
            link.dst = "dev_" + std::to_string(j);
            link.bw_gbps = 200.0;
            link.latency_ms = 0.5;
            topology.add_link(std::move(link));

            // Add reverse direction to make the ring bidirectional.
            hardware_topology::Link rev;
            rev.src = "dev_" + std::to_string(j);
            rev.dst = "dev_" + std::to_string(i);
            rev.bw_gbps = 200.0;
            rev.latency_ms = 0.5;
            topology.add_link(std::move(rev));
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
    bool time_unit_set = false;
    std::string taskflow_path = "taskflow.json";
    std::string hardware_path;
    std::string workload_path;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        node_count = parse_int_arg(arg, "--nodes=", node_count);
        depth = parse_int_arg(arg, "--depth=", depth);
        parts = parse_int_arg(arg, "--parts=", parts);
        if (arg.rfind("--time_unit=", 0) == 0) {
            time_unit = parse_string_arg(arg, "--time_unit=", time_unit);
            time_unit_set = true;
        }
        taskflow_path = parse_string_arg(arg, "--out=", taskflow_path);
        hardware_path = parse_string_arg(arg, "--hardware=", hardware_path);
        workload_path = parse_string_arg(arg, "--workload=", workload_path);
    }
    if (node_count <= 0 || depth <= 0) {
        std::cerr
            << "Usage: mapper_demo [--nodes=N] [--depth=D] [--parts=P] [--time_unit=UNIT] [--out=PATH] "
               "[--hardware=PATH] [--workload=PATH]\n";
        return 2;
    }

    hardware_topology::HardwareTopology topology;
    if (!hardware_path.empty()) {
        std::string error;
        if (!hardware_topology::load_from_json(hardware_path, topology, &error)) {
            std::cerr << "Failed to load hardware topology: " << error << "\n";
            return 2;
        }
        if (!time_unit_set) {
            time_unit = topology.time_unit();
        }
    } else {
        topology = build_demo_topology(node_count);
    }

    workload::Workload workload("demo", {});
    if (!workload_path.empty()) {
        std::string error;
        if (!workload::load_from_json(workload_path, workload, &error)) {
            std::cerr << "Failed to load workload: " << error << "\n";
            return 2;
        }
    } else {
        workload::WorkloadGenerator generator;
        workload = generator.build("demo", depth);
    }

    mapper::Options options;
    options.parts = parts;
    options.time_unit = time_unit;
    mapper::write_taskflow(topology, workload, taskflow_path, options);
    std::cout << "Wrote " << taskflow_path << "\n";
    return 0;
}
