#include "hardware_topology/topology.h"

#include <algorithm>
#include <queue>
#include <unordered_map>

namespace hardware_topology {

void ComputeNode::add_neighbor(std::string_view neighbor) {
    if (std::find(neighbors.begin(), neighbors.end(), neighbor) == neighbors.end()) {
        neighbors.emplace_back(neighbor);
        std::sort(neighbors.begin(), neighbors.end());
    }
}

void HardwareTopology::add_node(ComputeNode node) {
    nodes_.insert_or_assign(node.name, std::move(node));
}

void HardwareTopology::add_link(Link link) {
    if (link.id.empty()) {
        const auto& a = link.src < link.dst ? link.src : link.dst;
        const auto& b = link.src < link.dst ? link.dst : link.src;
        link.id = "link_" + a + "_" + b;
    }
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

std::vector<Link> HardwareTopology::links() const {
    auto links = links_;
    std::sort(links.begin(), links.end(), [](const Link& a, const Link& b) { return a.id < b.id; });
    return links;
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

std::optional<std::string> HardwareTopology::link_id(std::string_view src, std::string_view dst) const {
    for (const auto& link : links_) {
        if ((link.src == src && link.dst == dst) || (link.src == dst && link.dst == src)) {
            return link.id;
        }
    }
    return std::nullopt;
}

std::vector<std::string> HardwareTopology::shortest_route_link_ids(std::string_view src, std::string_view dst) const {
    if (src == dst) {
        return {};
    }
    const auto* src_node = node(src);
    const auto* dst_node = node(dst);
    if (src_node == nullptr || dst_node == nullptr) {
        return {};
    }

    std::queue<std::string> queue;
    std::unordered_map<std::string, std::string> parent;
    parent.emplace(src_node->name, "");
    queue.push(src_node->name);

    while (!queue.empty()) {
        const auto current = queue.front();
        queue.pop();
        if (current == dst_node->name) {
            break;
        }
        const auto* node_ptr = node(current);
        if (node_ptr == nullptr) {
            continue;
        }
        for (const auto& neighbor : node_ptr->neighbors) {
            if (parent.find(neighbor) != parent.end()) {
                continue;
            }
            parent.emplace(neighbor, current);
            queue.push(neighbor);
        }
    }

    if (parent.find(dst_node->name) == parent.end()) {
        return {};
    }

    std::vector<std::string> path_nodes;
    for (std::string cur = dst_node->name; !cur.empty(); cur = parent.at(cur)) {
        path_nodes.push_back(cur);
    }
    std::reverse(path_nodes.begin(), path_nodes.end());
    if (path_nodes.size() < 2) {
        return {};
    }

    std::vector<std::string> route;
    route.reserve(path_nodes.size() - 1);
    for (size_t i = 0; i + 1 < path_nodes.size(); ++i) {
        const auto id = link_id(path_nodes[i], path_nodes[i + 1]);
        if (!id.has_value()) {
            return {};
        }
        route.push_back(*id);
    }
    return route;
}

}  // namespace hardware_topology
