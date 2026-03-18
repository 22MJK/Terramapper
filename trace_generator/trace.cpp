#include "trace_generator/trace.h"

#include <algorithm>
#include <iomanip>
#include <iterator>
#include <sstream>

namespace trace_generator {

Trace::Trace(std::vector<TraceEvent> events) : events_(std::move(events)) {}

const std::vector<TraceEvent>& Trace::events() const {
    return events_;
}

Trace TraceGenerator::generate(const schedule::SchedulePlan& plan) const {
    std::vector<TraceEvent> events;
    events.reserve(plan.slots.size());
    for (const auto& slot : plan.slots) {
        events.push_back({slot.start_time, slot.duration, slot.node_name, slot.task_name});
    }
    std::sort(events.begin(), events.end(), [](const TraceEvent& a, const TraceEvent& b) {
        return a.timestamp < b.timestamp;
    });
    return Trace(std::move(events));
}

std::vector<std::string> TraceWriter::to_lines(const Trace& trace) {
    std::vector<std::string> lines;
    lines.reserve(trace.events().size());
    for (const auto& event : trace.events()) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(3);
        oss << event.timestamp << " | " << event.node_name << " | " << event.task_name << " | " << event.duration;
        lines.push_back(oss.str());
    }
    return lines;
}

}  // namespace trace_generator
