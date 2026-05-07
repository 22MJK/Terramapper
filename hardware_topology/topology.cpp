#include "hardware_topology/topology.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <queue>
#include <limits>
#include <unordered_map>

namespace hardware_topology {

std::size_t HardwareTopology::TransferCacheKeyHash::operator()(const TransferCacheKey& key) const {
    std::size_t h = std::hash<std::string>{}(key.src);
    h ^= std::hash<std::string>{}(key.dst) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<std::uint64_t>{}(key.bytes) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
}

void HardwareTopology::set_time_unit(std::string time_unit) {
    time_unit_ = std::move(time_unit);
}

const std::string& HardwareTopology::time_unit() const {
    return time_unit_;
}

void HardwareTopology::add_device(Device device) {
    devices_.insert_or_assign(device.id, std::move(device));
    invalidate_caches();
}

void HardwareTopology::add_link(Link link) {
    if (link.id.empty()) {
        link.id = "link_" + link.src + "_to_" + link.dst;
    }
    links_.push_back(std::move(link));
    invalidate_caches();
}

const Device* HardwareTopology::device(std::string_view id) const {
    const auto it = devices_.find(std::string(id));
    if (it == devices_.end()) {
        return nullptr;
    }
    return &it->second;
}

const std::vector<const Device*>& HardwareTopology::devices() const {
    if (!devices_cache_valid_) {
        rebuild_device_cache();
    }
    return devices_cache_;
}

const std::vector<Link>& HardwareTopology::links() const {
    if (!links_cache_valid_) {
        rebuild_link_caches();
    }
    return links_cache_;
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

    if (!links_cache_valid_) {
        rebuild_link_caches();
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
    for (const auto& device_ptr : devices()) {
        dist.emplace(device_ptr->id, kInf);
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
        const auto out_it = outgoing_cache_.find(current);
        if (out_it == outgoing_cache_.end()) {
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
    TransferCacheKey key{std::string(src), std::string(dst), static_cast<std::uint64_t>(bytes)};
    const auto cached = transfer_time_cache_.find(key);
    if (cached != transfer_time_cache_.end()) {
        return cached->second;
    }
    const double total = shortest_route_cost_seconds(src, dst, bytes);
    transfer_time_cache_.emplace(std::move(key), total);
    return total;
}

void HardwareTopology::invalidate_caches() {
    devices_cache_valid_ = false;
    links_cache_valid_ = false;
    transfer_time_cache_.clear();
}

void HardwareTopology::rebuild_device_cache() const {
    devices_cache_.clear();
    devices_cache_.reserve(devices_.size());
    for (const auto& kv : devices_) {
        devices_cache_.push_back(&kv.second);
    }
    std::sort(devices_cache_.begin(), devices_cache_.end(), [](const Device* a, const Device* b) {
        return a->id < b->id;
    });
    devices_cache_valid_ = true;
}

void HardwareTopology::rebuild_link_caches() const {
    links_cache_ = links_;
    std::sort(links_cache_.begin(), links_cache_.end(), [](const Link& a, const Link& b) { return a.id < b.id; });

    outgoing_cache_.clear();
    outgoing_cache_.reserve(devices_.size());
    link_by_id_cache_.clear();
    link_by_id_cache_.reserve(links_.size());

    for (const auto& link : links_) {
        if (link.bw_gbps <= 0.0) {
            continue;
        }
        outgoing_cache_[link.src].push_back(&link);
        link_by_id_cache_[link.id] = &link;
    }
    for (auto& kv : outgoing_cache_) {
        auto& outgoing = kv.second;
        std::sort(outgoing.begin(), outgoing.end(), [](const Link* a, const Link* b) {
            if (a->dst == b->dst) {
                return a->id < b->id;
            }
            return a->dst < b->dst;
        });
    }
    links_cache_valid_ = true;
}

double HardwareTopology::shortest_route_cost_seconds(std::string_view src, std::string_view dst, std::size_t bytes) const {
    if (src == dst) {
        return 0.0;
    }
    const auto* src_dev = device(src);
    const auto* dst_dev = device(dst);
    if (src_dev == nullptr || dst_dev == nullptr) {
        return std::numeric_limits<double>::infinity();
    }
    if (!links_cache_valid_) {
        rebuild_link_caches();
    }

    const auto link_cost_seconds = [bytes](const Link& link) {
        const double latency_s = link.latency_ms / 1000.0;
        const double serialize_s = (bytes == 0) ? 0.0 : (static_cast<double>(bytes) / (link.bw_gbps * 1e9));
        return latency_s + serialize_s;
    };

    const double kInf = std::numeric_limits<double>::infinity();
    std::unordered_map<std::string, double> dist;
    dist.reserve(devices_.size());
    for (const auto* device_ptr : devices()) {
        dist.emplace(device_ptr->id, kInf);
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
            return d;
        }
        const auto out_it = outgoing_cache_.find(current);
        if (out_it == outgoing_cache_.end()) {
            continue;
        }
        for (const auto* link : out_it->second) {
            const double candidate = d + link_cost_seconds(*link);
            auto next_it = dist.find(link->dst);
            if (next_it == dist.end() || candidate >= next_it->second) {
                continue;
            }
            next_it->second = candidate;
            pq.push({candidate, link->dst});
        }
    }

    return std::numeric_limits<double>::infinity();
}

}  // namespace hardware_topology
