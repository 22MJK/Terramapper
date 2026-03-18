#include "hardware_topology/topology.h"

#include <algorithm>

namespace hardware_topology {

void ComputeNode::add_neighbor(std::string_view neighbor) {
    if (std::find(neighbors.begin(), neighbors.end(), neighbor) == neighbors.end()) {
        neighbors.emplace_back(neighbor);
    }
}

void HardwareTopology::add_node(ComputeNode node) {
    nodes_.insert_or_assign(node.name, std::move(node));
}

void HardwareTopology::add_link(Link link) {
    const auto src_it = nodes_.find(link.src);
    const auto dst_it = nodes_.find(link.dst);
    if (src_it != nodes_.end()) {
        src_it->second.add_neighbor(link.dst);
    }
    if (dst_it != nodes_.end()) {
        dst_it->second.add_neighbor(link.src);
    }
    links_.push_back(std::move(link));
}

const ComputeNode* HardwareTopology::node(std::string_view name) const {
    const auto it = nodes_.find(std::string(name));
    if (it == nodes_.end()) {
        return nullptr;
    }
    return &it->second;
}

std::vector<const ComputeNode*> HardwareTopology::nodes() const {
    std::vector<const ComputeNode*> result;
    result.reserve(nodes_.size());
    for (const auto& kv : nodes_) {
        result.push_back(&kv.second);
    }
    std::sort(result.begin(), result.end(), [](const ComputeNode* a, const ComputeNode* b) {
        return a->name < b->name;
    });
    return result;
}

std::vector<std::pair<std::string, std::string>> HardwareTopology::connected_pairs() const {
    std::vector<std::pair<std::string, std::string>> pairs;
    pairs.reserve(links_.size());
    for (const auto& link : links_) {
        pairs.emplace_back(link.src, link.dst);
    }
    return pairs;
}

std::optional<double> HardwareTopology::bandwidth(std::string_view src, std::string_view dst) const {
    for (const auto& link : links_) {
        if ((link.src == src && link.dst == dst) || (link.src == dst && link.dst == src)) {
            return link.bandwidth_gbps;
        }
    }
    return std::nullopt;
}

std::optional<double> HardwareTopology::latency_ms(std::string_view src, std::string_view dst) const {
    for (const auto& link : links_) {
        if ((link.src == src && link.dst == dst) || (link.src == dst && link.dst == src)) {
            return link.latency_ms;
        }
    }
    return std::nullopt;
}

}  // namespace hardware_topology
