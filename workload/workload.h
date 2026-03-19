#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mapping/graph.h"

namespace workload {

struct WorkloadStage {
    std::string name;
    std::string type;
    std::string subtype;
    double compute_flops{0.0};
    double comm_bytes{0.0};
    std::vector<std::string> dependencies;
};

struct WorkloadEdge {
    std::string src;
    std::string dst;
    double tensor_bytes{0.0};
};

class Workload {
public:
    explicit Workload(std::string name, std::vector<WorkloadStage> stages, std::vector<WorkloadEdge> edges = {});
    mapping::TaskGraph to_task_graph() const;
    const std::string& name() const;

private:
    std::string name_;
    std::vector<WorkloadStage> stages_;
    std::vector<WorkloadEdge> edges_;
};

class WorkloadGenerator {
public:
    WorkloadGenerator(double base_compute = 50.0, double growth = 1.5);
    Workload build(const std::string& name, int depth) const;

private:
    double base_compute_;
    double growth_;
};

}  // namespace workload
