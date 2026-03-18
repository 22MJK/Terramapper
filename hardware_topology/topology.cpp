#include "hardware_topology/topology.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <limits>
#include <unordered_map>

namespace hardware_topology {

void HardwareTopology::set_time_unit(std::string time_unit) {
    time_unit_ = std::move(time_unit);
}

const std::string& HardwareTopology::time_unit() const {
    return time_unit_;
}

void HardwareTopology::add_device(Device device) {
    devices_.insert_or_assign(device.id, std::move(device));
}

void HardwareTopology::add_link(Link link) {
    if (link.id.empty()) {
        link.id = "link_" + link.src + "_to_" + link.dst;
    }
    links_.push_back(std::move(link));
}

const Device* HardwareTopology::device(std::string_view id) const {
    const auto it = devices_.find(std::string(id));
    if (it == devices_.end()) {
        return nullptr;
    }
    return &it->second;
}

std::vector<const Device*> HardwareTopology::devices() const {
    std::vector<const Device*> result;
    result.reserve(devices_.size());
    for (const auto& kv : devices_) {
        result.push_back(&kv.second);
    }
    std::sort(result.begin(), result.end(), [](const Device* a, const Device* b) {
        return a->id < b->id;
    });
    return result;
}

std::vector<Link> HardwareTopology::links() const {
    auto links = links_;
    std::sort(links.begin(), links.end(), [](const Link& a, const Link& b) { return a.id < b.id; });
    return links;
}

std::optional<double> HardwareTopology::bw_gbps(std::string_view src, std::string_view dst) const {
    for (const auto& link : links_) {
        if (link.src == src && link.dst == dst) {
            return link.bw_gbps;
        }
    }
    return std::nullopt;
}

std::optional<double> HardwareTopology::latency_ms(std::string_view src, std::string_view dst) const {
    for (const auto& link : links_) {
        if (link.src == src && link.dst == dst) {
            return link.latency_ms;
        }
    }
    return std::nullopt;
}

std::optional<std::string> HardwareTopology::link_id(std::string_view src, std::string_view dst) const {
    for (const auto& link : links_) {
        if (link.src == src && link.dst == dst) {
            return link.id;
        }
    }
    return std::nullopt;
}

std::vector<std::string> HardwareTopology::shortest_route_link_ids(std::string_view src, std::string_view dst) const {
    if (src == dst) {
        return {};
    }
    const auto* src_dev = device(src);
    const auto* dst_dev = device(dst);
    if (src_dev == nullptr || dst_dev == nullptr) {
        return {};
    }

    // Build outgoing adjacency list once per call (kept simple for now).
    std::unordered_map<std::string, std::vector<std::string>> outgoing;
    outgoing.reserve(devices_.size());
    for (const auto& link : links_) {
        outgoing[link.src].push_back(link.dst);
    }
    for (auto& kv : outgoing) {
        auto& neigh = kv.second;
        std::sort(neigh.begin(), neigh.end());
        neigh.erase(std::unique(neigh.begin(), neigh.end()), neigh.end());
    }

    std::queue<std::string> queue;
    std::unordered_map<std::string, std::string> parent;
    parent.emplace(src_dev->id, "");
    queue.push(src_dev->id);

    while (!queue.empty()) {
        const auto current = queue.front();
        queue.pop();
        if (current == dst_dev->id) {
            break;
        }
        const auto it = outgoing.find(current);
        if (it == outgoing.end()) {
            continue;
        }
        for (const auto& neighbor : it->second) {
            if (parent.find(neighbor) != parent.end()) {
                continue;
            }
            parent.emplace(neighbor, current);
            queue.push(neighbor);
        }
    }

    if (parent.find(dst_dev->id) == parent.end()) {
        return {};
    }

    std::vector<std::string> path_nodes;
    for (std::string cur = dst_dev->id; !cur.empty(); cur = parent.at(cur)) {
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

double HardwareTopology::get_transfer_time(std::string_view src, std::string_view dst, size_t bytes) const {
    if (src == dst || bytes == 0) {
        return 0.0;
    }
    const auto route = shortest_route_link_ids(src, dst);
    if (route.empty()) {
        return std::numeric_limits<double>::infinity();
    }

    // Assumes store-and-forward: each hop pays fixed latency + serialization time.
    // Uses GiB = 1024^3 for conversion from bytes.
    const double gib = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    double total = 0.0;
    for (const auto& link_id_val : route) {
        const auto it = std::find_if(links_.begin(), links_.end(), [&link_id_val](const Link& link) {
            return link.id == link_id_val;
        });
        if (it == links_.end() || it->bw_gbps <= 0.0) {
            return std::numeric_limits<double>::infinity();
        }
        total += (it->latency_ms / 1000.0);
        total += gib / it->bw_gbps;
    }
    return total;
}

}  // namespace hardware_topology
