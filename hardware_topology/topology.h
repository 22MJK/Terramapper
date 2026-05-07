#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hardware_topology {

struct Device {
    std::string id;
    std::string name;
    std::string type;
    double peak_gflops{0.0};
    double mem_bw_gbps{0.0};
    int max_concurrent{1};
};

// Directed link (src -> dst).
struct Link {
    std::string id;
    std::string src;
    std::string dst;
    double bw_gbps{0.0};
    double latency_ms{0.0};
};

class HardwareTopology {
public:
    void set_time_unit(std::string time_unit);
    const std::string& time_unit() const;

    void add_device(Device device);
    void add_link(Link link);

    const Device* device(std::string_view id) const;
    const std::vector<const Device*>& devices() const;
    const std::vector<Link>& links() const;

    // Direct-link queries (directed).
    std::optional<double> bw_gbps(std::string_view src, std::string_view dst) const;
    std::optional<double> latency_ms(std::string_view src, std::string_view dst) const;
    std::optional<std::string> link_id(std::string_view src, std::string_view dst) const;

    // Returns minimum-time route as link-id list via Dijkstra.
    // Cost model per link: latency + serialization(bytes / bw).
    // Empty means "no route found" (or src==dst).
    std::vector<std::string> shortest_route_link_ids(std::string_view src,
                                                     std::string_view dst,
                                                     size_t bytes = 0) const;

    // Transfer time in seconds. If no route exists, returns +inf.
    double get_transfer_time(std::string_view src, std::string_view dst, size_t bytes) const;

private:
    struct TransferCacheKey {
        std::string src;
        std::string dst;
        std::uint64_t bytes{0};

        bool operator==(const TransferCacheKey& other) const {
            return src == other.src && dst == other.dst && bytes == other.bytes;
        }
    };

    struct TransferCacheKeyHash {
        std::size_t operator()(const TransferCacheKey& key) const;
    };

    void invalidate_caches();
    void rebuild_device_cache() const;
    void rebuild_link_caches() const;
    double shortest_route_cost_seconds(std::string_view src, std::string_view dst, std::size_t bytes) const;

    std::string time_unit_{"s"};
    std::unordered_map<std::string, Device> devices_;
    std::vector<Link> links_;
    mutable bool devices_cache_valid_{false};
    mutable bool links_cache_valid_{false};
    mutable std::vector<const Device*> devices_cache_;
    mutable std::vector<Link> links_cache_;
    mutable std::unordered_map<std::string, std::vector<const Link*>> outgoing_cache_;
    mutable std::unordered_map<std::string, const Link*> link_by_id_cache_;
    mutable std::unordered_map<TransferCacheKey, double, TransferCacheKeyHash> transfer_time_cache_;
};

}  // namespace hardware_topology
