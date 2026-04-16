#include "hardware_topology/topology.h"

#include <algorithm>
#include <cmath>
#include <functional>
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

std::vector<std::string> HardwareTopology::shortest_route_link_ids(std::string_view src,
                                                                    std::string_view dst,
                                                                    size_t bytes) const {
    if (src == dst) {
        return {};
    }
    const auto* src_dev = device(src);
    const auto* dst_dev = device(dst);
    if (src_dev == nullptr || dst_dev == nullptr) {
        return {};
    }

    // Build outgoing adjacency list once per call.
    std::unordered_map<std::string, std::vector<const Link*>> outgoing;
    outgoing.reserve(devices_.size());
    for (const auto& link : links_) {
        if (link.bw_gbps <= 0.0) {
            continue;
        }
        outgoing[link.src].push_back(&link);
    }
    for (auto& kv : outgoing) {
        auto& links = kv.second;
        std::sort(links.begin(), links.end(), [](const Link* a, const Link* b) {
            if (a->dst == b->dst) {
                return a->id < b->id;
            }
            return a->dst < b->dst;
        });
    }

    const auto link_cost_seconds = [bytes](const Link& link) {
        const double latency_s = link.latency_ms / 1000.0;
        const double serialize_s = (bytes == 0) ? 0.0 : (static_cast<double>(bytes) / (link.bw_gbps * 1e9));
        return latency_s + serialize_s;
    };

    const double kInf = std::numeric_limits<double>::infinity();
    std::unordered_map<std::string, double> dist;
    std::unordered_map<std::string, std::string> parent_node;
    std::unordered_map<std::string, std::string> parent_link;
    dist.reserve(devices_.size());
    parent_node.reserve(devices_.size());
    parent_link.reserve(devices_.size());
    for (const auto& kv : devices_) {
        dist.emplace(kv.first, kInf);
    }
    dist[src_dev->id] = 0.0;

    using QueueItem = std::pair<double, std::string>;
    std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<QueueItem>> pq;
    pq.push({0.0, src_dev->id});

    while (!pq.empty()) {
        const auto [d, current] = pq.top();
        pq.pop();
        if (d > dist[current]) {
            continue;
        }
        if (current == dst_dev->id) {
            break;
        }
        const auto out_it = outgoing.find(current);
        if (out_it == outgoing.end()) {
            continue;
        }
        for (const auto* link : out_it->second) {
            const double weight = link_cost_seconds(*link);
            const double candidate = d + weight;
            const auto next_it = dist.find(link->dst);
            if (next_it == dist.end()) {
                continue;
            }
            const double old = next_it->second;
            const bool better = candidate < old;
            const bool tie_break = (candidate == old &&
                                    parent_link.find(link->dst) != parent_link.end() &&
                                    link->id < parent_link[link->dst]);
            if (better || tie_break) {
                next_it->second = candidate;
                parent_node[link->dst] = current;
                parent_link[link->dst] = link->id;
                pq.push({candidate, link->dst});
            }
        }
    }

    if (parent_node.find(dst_dev->id) == parent_node.end()) {
        return {};
    }

    std::vector<std::string> route;
    for (std::string cur = dst_dev->id; cur != src_dev->id;) {
        const auto link_it = parent_link.find(cur);
        const auto node_it = parent_node.find(cur);
        if (link_it == parent_link.end() || node_it == parent_node.end()) {
            return {};
        }
        route.push_back(link_it->second);
        cur = node_it->second;
    }
    std::reverse(route.begin(), route.end());
    return route;
}

double HardwareTopology::get_transfer_time(std::string_view src, std::string_view dst, size_t bytes) const {
    if (src == dst || bytes == 0) {
        return 0.0;
    }
    const auto route = shortest_route_link_ids(src, dst, bytes);
    if (route.empty()) {
        return std::numeric_limits<double>::infinity();
    }

    // Assumes store-and-forward: each hop pays fixed latency + serialization time.
    // Uses GB/s (1e9 bytes/s) for bw_gbps.
    const double gb = static_cast<double>(bytes) / 1e9;
    double total = 0.0;
    for (const auto& link_id_val : route) {
        const auto it = std::find_if(links_.begin(), links_.end(), [&link_id_val](const Link& link) {
            return link.id == link_id_val;
        });
        if (it == links_.end() || it->bw_gbps <= 0.0) {
            return std::numeric_limits<double>::infinity();
        }
        total += (it->latency_ms / 1000.0);
        total += gb / it->bw_gbps;
    }
    return total;
}

}  // namespace hardware_topology
