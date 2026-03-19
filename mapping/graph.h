#pragma once

#include <deque>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mapping {

struct Task {
    std::string name;
    std::string type;
    std::string subtype;
    double compute_flops{0.0};
    double comm_bytes{0.0};
    std::unordered_set<std::string> tags;
};

struct TaskEdge {
    std::string src;
    std::string dst;
    double tensor_bytes{0.0};
};

class TaskGraph {
public:
    void add_task(Task task);
    void add_edge(const std::string& src, const std::string& dst, double tensor_bytes = 0.0);

    std::vector<TaskEdge> dependencies(const std::string& name) const;
    std::vector<TaskEdge> successors(const std::string& name) const;
    std::vector<Task> topological_order() const;
    std::vector<Task> source_tasks() const;
    std::vector<Task> sink_tasks() const;

    bool has_task(const std::string& name) const;
    const Task& task(const std::string& name) const;

private:
    std::unordered_map<std::string, Task> tasks_;
    std::unordered_map<std::string, std::vector<TaskEdge>> edges_;
    std::unordered_map<std::string, std::vector<TaskEdge>> reverse_edges_;
    std::vector<std::string> insertion_order_;
};

}  // namespace mapping
