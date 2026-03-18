#pragma once

#include <string>
#include <vector>

#include "schedule/scheduler.h"

namespace trace_generator {

struct TraceEvent {
    double timestamp{0.0};
    double duration{0.0};
    std::string node_name;
    std::string task_name;
};

class Trace {
public:
    explicit Trace(std::vector<TraceEvent> events);
    const std::vector<TraceEvent>& events() const;

private:
    std::vector<TraceEvent> events_;
};

class TraceGenerator {
public:
    Trace generate(const schedule::SchedulePlan& plan) const;
};

class TraceWriter {
public:
    static std::vector<std::string> to_lines(const Trace& trace);
};

}  // namespace trace_generator
