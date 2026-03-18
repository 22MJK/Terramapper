#pragma once

#include <string>
#include <string_view>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace hardware_topology {

struct Link {
    std::string id;
    std::string src;
    std::string dst;
    double bandwidth_gbps{0.0};
    double latency_ms{0.0};
};

struct ComputeNode {
    std::string name;
    int cores{0};
    double memory_gb{0.0};
    double gflops{0.0};
    std::unordered_set<std::string> tags;
    std::vector<std::string> neighbors;

    void add_neighbor(std::string_view neighbor);
};

class HardwareTopology {
public:
    void add_node(ComputeNode node);
    void add_link(Link link);
    const ComputeNode* node(std::string_view name) const;
    std::vector<const ComputeNode*> nodes() const;
    std::vector<Link> links() const;
    std::vector<std::pair<std::string, std::string>> connected_pairs() const;
    std::optional<double> bandwidth(std::string_view src, std::string_view dst) const;
    std::optional<double> latency_ms(std::string_view src, std::string_view dst) const;
    std::optional<std::string> link_id(std::string_view src, std::string_view dst) const;
    std::vector<std::string> shortest_route_link_ids(std::string_view src, std::string_view dst) const;

private:
    std::unordered_map<std::string, ComputeNode> nodes_;
    std::vector<Link> links_;
};

}  // namespace hardware_topology
