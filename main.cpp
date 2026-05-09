#include "hardware_topology/topology.h"
#include "mapper/mapper.h"
#include "hardware_topology/json_io.h"
#include "workload/json_io.h"
#include "workload/workload.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#ifndef _WIN32
#include <sys/wait.h>
#endif

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

std::string shell_escape(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (char ch : value) {
        if (ch == '"' || ch == '\\') {
            out << '\\';
        }
        out << ch;
    }
    out << '"';
    return out.str();
}

std::string format_top_counts(const std::vector<mapper::NamedCount>& items, std::size_t limit) {
    if (items.empty() || limit == 0) {
        return "N/A";
    }
    const std::size_t n = std::min(limit, items.size());
    std::ostringstream out;
    for (std::size_t i = 0; i < n; ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << items[i].name << ":" << items[i].count;
    }
    return out.str();
}

std::string format_top_bytes(const std::vector<mapper::NamedBytes>& items, std::size_t limit) {
    if (items.empty() || limit == 0) {
        return "N/A";
    }
    std::vector<mapper::NamedBytes> filtered;
    filtered.reserve(items.size());
    for (const auto& item : items) {
        if (item.bytes > 0) {
            filtered.push_back(item);
        }
    }
    if (filtered.empty()) {
        return "N/A";
    }
    const std::size_t n = std::min(limit, filtered.size());
    std::ostringstream out;
    for (std::size_t i = 0; i < n; ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << filtered[i].name << ":" << filtered[i].bytes << "B";
    }
    return out.str();
}

void print_schedule_summary(const mapper::RunResult& result, const std::string& taskflow_path) {
    const std::string quoted_taskflow = shell_escape(taskflow_path);
    double cross_ratio = 0.0;
    if (result.total_edge_bytes > 0) {
        cross_ratio = static_cast<double>(result.cross_device_edge_bytes) /
                      static_cast<double>(result.total_edge_bytes);
    }

    std::cout << "Schedule summary:\n";
    std::cout << "  graph: tasks=" << result.task_count
              << ", edges=" << result.edge_count
              << ", dag_depth=" << result.dag_depth
              << ", sources=" << result.source_count
              << ", sinks=" << result.sink_count << "\n";
    std::cout << "  communication: transfer_bytes=" << result.total_edge_bytes << "B"
              << ", cross_device_transfer_bytes=" << result.cross_device_edge_bytes << "B"
              << " (" << result.cross_device_edge_count << " edges, "
              << std::fixed << std::setprecision(2) << (cross_ratio * 100.0) << "%)\n";
    std::cout << "  top task subtypes: " << format_top_counts(result.task_subtype_counts, 6) << "\n";
    std::cout << "  tasks per device: " << format_top_counts(result.device_task_counts, 8) << "\n";
    std::cout << "  top comm kinds by bytes: " << format_top_bytes(result.comm_kind_bytes, 6) << "\n";
    std::cout << "  suggested views:\n";
    std::cout << "    python3 visualize/taskflow_viz.py --input " << quoted_taskflow
              << " --abstract --abstract-by device --output taskflow_device.svg\n";
    std::cout << "    python3 visualize/taskflow_viz.py --input " << quoted_taskflow
              << " --abstract --abstract-by iter --output taskflow_iter.svg\n";
    std::cout << "    python3 visualize/taskflow_viz.py --input " << quoted_taskflow
              << " --abstract --abstract-by op --output taskflow_op.svg\n";
    std::cout << "    python3 visualize/taskflow_viz.py --input " << quoted_taskflow
              << " --format mermaid --group-by-device --output taskflow.mmd\n";
}

void try_generate_taskflow_svg(const std::string& taskflow_path,
                               int viz_max_tasks,
                               int viz_max_edges,
                               bool viz_force,
                               const std::string& viz_summary_path) {
    namespace fs = std::filesystem;
    const fs::path script = fs::path("visualize") / "taskflow_viz.py";
    if (!fs::exists(script)) {
        std::cerr << "Warning: visualize/taskflow_viz.py not found; skip visualization.\n";
        return;
    }
    const fs::path taskflow_file(taskflow_path);
    fs::path out_path = taskflow_file;
    out_path.replace_extension(".svg");
    std::string cmd = "python3 ";
    cmd += shell_escape(script.string());
    cmd += " --input ";
    cmd += shell_escape(taskflow_file.string());
    cmd += " --output ";
    cmd += shell_escape(out_path.string());
    cmd += " --max-nodes ";
    cmd += std::to_string(viz_max_tasks);
    cmd += " --max-edges ";
    cmd += std::to_string(viz_max_edges);
    cmd += " --quiet-skip-summary";
    if (viz_force) {
        cmd += " --force-render";
    }
    if (!viz_summary_path.empty()) {
        cmd += " --summary ";
        cmd += shell_escape(viz_summary_path);
    }
    const int rc = std::system(cmd.c_str());
    int exit_code = rc;
#ifndef _WIN32
    if (rc != -1 && WIFEXITED(rc)) {
        exit_code = WEXITSTATUS(rc);
    }
#endif
    if (exit_code == 3) {
        std::cerr << "Visualization skipped for large taskflow."
                  << " You can force it with --viz-force";
        if (!viz_summary_path.empty()) {
            std::cerr << " (summary: " << viz_summary_path << ")";
        }
        std::cerr << ".\n";
        return;
    }
    if (exit_code != 0) {
        std::cerr << "Warning: taskflow visualization failed (exit " << exit_code << ").\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    int parts = 0;
    std::string time_unit = "s";
    bool time_unit_set = false;
    std::string taskflow_path = "taskflow.json";
    std::string hardware_path;
    std::string workload_path;
    std::string mapper_name = "heft";
    std::string parallel_mode = "none";
    bool enable_viz = true;
    bool viz_force = false;
    int viz_max_tasks = 2500;
    int viz_max_edges = 10000;
    std::string viz_summary_path;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        parts = parse_int_arg(arg, "--parts=", parts);
        if (arg == "--parts" && i + 1 < argc) {
            parts = std::atoi(argv[++i]);
            continue;
        }
        if (arg.rfind("--time_unit=", 0) == 0) {
            time_unit = parse_string_arg(arg, "--time_unit=", time_unit);
            time_unit_set = true;
            continue;
        }
        if (arg == "--time_unit" && i + 1 < argc) {
            time_unit = argv[++i];
            time_unit_set = true;
            continue;
        }
        taskflow_path = parse_string_arg(arg, "--out=", taskflow_path);
        taskflow_path = parse_string_arg(arg, "--output=", taskflow_path);
        if (arg == "--out" && i + 1 < argc) {
            taskflow_path = argv[++i];
            continue;
        }
        if (arg == "--output" && i + 1 < argc) {
            taskflow_path = argv[++i];
            continue;
        }
        hardware_path = parse_string_arg(arg, "--hardware=", hardware_path);
        if (arg == "--hardware" && i + 1 < argc) {
            hardware_path = argv[++i];
            continue;
        }
        workload_path = parse_string_arg(arg, "--workload=", workload_path);
        if (arg == "--workload" && i + 1 < argc) {
            workload_path = argv[++i];
            continue;
        }
        mapper_name = parse_string_arg(arg, "--mapper=", mapper_name);
        if (arg == "--mapper" && i + 1 < argc) {
            mapper_name = argv[++i];
            continue;
        }
        parallel_mode = parse_string_arg(arg, "--parallel=", parallel_mode);
        if (arg == "--parallel" && i + 1 < argc) {
            parallel_mode = argv[++i];
            continue;
        }
        viz_max_tasks = parse_int_arg(arg, "--viz-max-tasks=", viz_max_tasks);
        if (arg == "--viz-max-tasks" && i + 1 < argc) {
            viz_max_tasks = std::atoi(argv[++i]);
            continue;
        }
        viz_max_edges = parse_int_arg(arg, "--viz-max-edges=", viz_max_edges);
        if (arg == "--viz-max-edges" && i + 1 < argc) {
            viz_max_edges = std::atoi(argv[++i]);
            continue;
        }
        viz_summary_path = parse_string_arg(arg, "--viz-summary=", viz_summary_path);
        if (arg == "--viz-summary" && i + 1 < argc) {
            viz_summary_path = argv[++i];
            continue;
        }
        if (arg == "--viz-force") {
            viz_force = true;
            continue;
        }
        if (arg == "--no-viz") {
            enable_viz = false;
            continue;
        }
    }
    if (hardware_path.empty() || workload_path.empty()) {
        std::cerr << "Usage: mapper_demo --hardware=PATH --workload=PATH [--parts=P] [--time_unit=UNIT] "
                     "[--mapper=heft|peft|greedy] [--parallel=auto|none|hint|all] [--out=PATH|--output=PATH] "
                     "[--no-viz] [--viz-max-tasks=N] [--viz-max-edges=N] [--viz-force] [--viz-summary=PATH]\n";
        return 2;
    }
    if (viz_max_tasks < 0) {
        viz_max_tasks = 0;
    }
    if (viz_max_edges < 0) {
        viz_max_edges = 0;
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

    workload::Workload workload("workload", {}, {}, {}, {}, {});
    if (!workload::load_from_json(workload_path, workload, &error)) {
        std::cerr << "Failed to load workload: " << error << "\n";
        return 2;
    }

    mapper::Options options;
    options.parts = parts;
    options.time_unit = time_unit;
    options.mapper = mapper_name;
    options.parallel = parallel_mode;
    const auto mapper_start = std::chrono::steady_clock::now();
    const auto result = mapper::write_taskflow(topology, workload, taskflow_path, options);
    const auto mapper_end = std::chrono::steady_clock::now();
    const std::chrono::duration<double> mapper_runtime = mapper_end - mapper_start;
    std::cout << "Wrote " << taskflow_path << "\n";
    std::cout << "Estimated makespan: " << std::fixed << std::setprecision(6)
              << result.estimated_makespan_s << " s"
              << " (parallel=" << result.selected_parallel
              << ", tasks=" << result.task_count << ")\n";
    std::cout << "Mapper runtime (excluding visualization): "
              << std::fixed << std::setprecision(6)
              << mapper_runtime.count() << " s\n";
    print_schedule_summary(result, taskflow_path);
    std::cout.flush();
    if (enable_viz) {
        try_generate_taskflow_svg(taskflow_path, viz_max_tasks, viz_max_edges, viz_force, viz_summary_path);
    }
    return 0;
}
