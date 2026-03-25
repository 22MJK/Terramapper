#include "mapping/graph.h"

namespace mapping {

void TaskGraph::add_task(Task task) {
    const auto [it, inserted] = tasks_.emplace(task.name, task);
    if (inserted) {
        insertion_order_.emplace_back(task.name);
    } else {
        it->second = std::move(task);
    }
}

void TaskGraph::add_edge(const std::string& src,
                         const std::string& dst,
                         double tensor_bytes,
                         std::string tensor_id,
                         std::string comm_kind) {
    if (!has_task(src)) {
        throw std::runtime_error("Source task " + src + " not known");
    }
    if (!has_task(dst)) {
        throw std::runtime_error("Destination task " + dst + " not known");
    }
    TaskEdge edge{src, dst, tensor_bytes, std::move(tensor_id), std::move(comm_kind)};
    edges_[src].push_back(edge);
    reverse_edges_[dst].push_back(edge);
}

std::vector<TaskEdge> TaskGraph::dependencies(const std::string& name) const {
    const auto it = reverse_edges_.find(name);
    if (it == reverse_edges_.end()) {
        return {};
    }
    return it->second;
}

std::vector<TaskEdge> TaskGraph::successors(const std::string& name) const {
    const auto it = edges_.find(name);
    if (it == edges_.end()) {
        return {};
    }
    return it->second;
}

std::vector<Task> TaskGraph::topological_order() const {
    std::unordered_map<std::string, int> indegree;
    for (const auto& name : insertion_order_) {
        indegree[name] = 0;
    }
    for (const auto& entry : edges_) {
        for (const auto& edge : entry.second) {
            indegree[edge.dst] += 1;
        }
    }

    std::deque<std::string> queue;
    for (const auto& name : insertion_order_) {
        if (indegree[name] == 0) {
            queue.push_back(name);
        }
    }

    std::vector<Task> order;
    while (!queue.empty()) {
        const auto current = queue.front();
        queue.pop_front();
        order.push_back(tasks_.at(current));
        for (const auto& edge : successors(current)) {
            indegree[edge.dst] -= 1;
            if (indegree[edge.dst] == 0) {
                queue.push_back(edge.dst);
            }
        }
    }

    if (order.size() != tasks_.size()) {
        throw std::runtime_error("TaskGraph contains a cycle");
    }
    return order;
}

std::vector<Task> TaskGraph::source_tasks() const {
    std::vector<Task> result;
    for (const auto& name : insertion_order_) {
        if (reverse_edges_.find(name) == reverse_edges_.end()) {
            result.push_back(tasks_.at(name));
        }
    }
    return result;
}

std::vector<Task> TaskGraph::sink_tasks() const {
    std::vector<Task> result;
    for (const auto& name : insertion_order_) {
        if (edges_.find(name) == edges_.end()) {
            result.push_back(tasks_.at(name));
        }
    }
    return result;
}

bool TaskGraph::has_task(const std::string& name) const {
    return tasks_.find(name) != tasks_.end();
}

const Task& TaskGraph::task(const std::string& name) const {
    return tasks_.at(name);
}

}  // namespace mapping
