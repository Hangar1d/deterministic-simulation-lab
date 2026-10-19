#pragma once

#include "simlab/simulation.hpp"

#include <string>

namespace simlab {

inline constexpr std::size_t max_text_bytes = 4 * 1024 * 1024;

struct Scenario {
    Config config;
    std::vector<Arrival> arrivals;
};

struct Record {
    Scenario scenario;
    std::string expected_result;
};

Simulation make_simulation(const Scenario& scenario);
Scenario parse_plan(const std::string& text);
std::string format_plan(const Scenario& scenario);
Snapshot parse_snapshot(const std::string& text);
std::string format_snapshot(const Snapshot& snapshot);
Record parse_record(const std::string& text);
std::string format_record(const Scenario& scenario, const std::string& result);
std::string format_result(const std::vector<Transition>& trace, const Snapshot& snapshot);

} // namespace simlab
