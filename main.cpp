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

}  // namespace

int main(int argc, char** argv) {
    int parts = 0;
    std::string time_unit = "s";
    bool time_unit_set = false;
    std::string taskflow_path = "taskflow.json";
    std::string hardware_path;
    std::string workload_path;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        parts = parse_int_arg(arg, "--parts=", parts);
        if (arg.rfind("--time_unit=", 0) == 0) {
            time_unit = parse_string_arg(arg, "--time_unit=", time_unit);
            time_unit_set = true;
        }
        taskflow_path = parse_string_arg(arg, "--out=", taskflow_path);
        hardware_path = parse_string_arg(arg, "--hardware=", hardware_path);
        workload_path = parse_string_arg(arg, "--workload=", workload_path);
    }
    if (hardware_path.empty() || workload_path.empty()) {
        std::cerr << "Usage: mapper_demo --hardware=PATH --workload=PATH [--parts=P] [--time_unit=UNIT] "
                     "[--out=PATH]\n";
        return 2;
    }

    hardware_topology::HardwareTopology topology;
    std::string error;
    if (!hardware_topology::load_from_json(hardware_path, topology, &error)) {
        std::cerr << "Failed to load hardware topology: " << error << "\n";
        return 2;
    }
    if (!time_unit_set) {
        time_unit = topology.time_unit();
    }

    workload::Workload workload("workload", {}, {}, {});
    if (!workload::load_from_json(workload_path, workload, &error)) {
        std::cerr << "Failed to load workload: " << error << "\n";
        return 2;
    }

    mapper::Options options;
    options.parts = parts;
    options.time_unit = time_unit;
    mapper::write_taskflow(topology, workload, taskflow_path, options);
    std::cout << "Wrote " << taskflow_path << "\n";
    return 0;
}
