#include "simlab/codec.hpp"

#include <algorithm>
#include <functional>
#include <iostream>
#include <limits>
#include <locale>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace simlab;

void check(bool condition, const char* expression, int line) {
    if (!condition) { throw std::runtime_error(std::string(expression) + " at line " + std::to_string(line)); }
}
#define CHECK(...) check((__VA_ARGS__), #__VA_ARGS__, __LINE__)

template<class Action>
void rejects(Action action) {
    bool rejected = false;
    try { action(); } catch (const Error&) { rejected = true; }
    CHECK(rejected);
}

Scenario example(std::uint32_t seed = 1) {
    return {{seed, 2, 5}, {{0, 10}, {0, 20}, {2, 30}, {4, 40}}};
}

std::string result(const Scenario& scenario) {
    auto simulation = make_simulation(scenario);
    const auto trace = drain(simulation);
    return format_result(trace, simulation.snapshot());
}

std::vector<Transition> golden_trace() {
    return {
        {0, TransitionKind::arrive, 10, 0}, {0, TransitionKind::start, 10, 4},
        {0, TransitionKind::arrive, 20, 0}, {2, TransitionKind::arrive, 30, 0},
        {4, TransitionKind::finish, 10, 0}, {4, TransitionKind::start, 20, 3},
        {4, TransitionKind::arrive, 40, 0}, {7, TransitionKind::finish, 20, 0},
        {7, TransitionKind::start, 30, 3}, {10, TransitionKind::finish, 30, 0},
        {10, TransitionKind::start, 40, 2}, {12, TransitionKind::finish, 40, 0}
    };
}

void golden() {
    auto simulation = make_simulation(example());
    CHECK(drain(simulation) == golden_trace());
    const auto state = simulation.snapshot();
    CHECK(state.now == 12 && state.arrived == 4 && state.processed == 8);
    CHECK(state.completed == std::vector<JobId>({10, 20, 30, 40}));
    CHECK(state.events.empty() && state.waiting.empty() && !state.active);
}

void determinism() {
    for (std::uint32_t seed = 0; seed < 50; ++seed) {
        Scenario scenario{{seed, 1, 100}, {}};
        for (std::uint64_t id = 1; id <= 25; ++id) { scenario.arrivals.push_back({id % 7, id}); }
        CHECK(result(scenario) == result(scenario));
    }
    CHECK(result(example(1)) != result(example(2)));
}

void seed_mapping() {
    auto zero = make_simulation(example(0));
    auto modulus = make_simulation(example(random_modulus));
    auto one = make_simulation(example(1));
    CHECK(drain(zero) == drain(one));
    auto again = make_simulation(example(1));
    CHECK(drain(modulus) == drain(again));
    CHECK(result(example(std::numeric_limits<std::uint32_t>::max())) ==
          result(example(std::numeric_limits<std::uint32_t>::max())));
}

void engine_vectors() {
    Simulation simulation({1, 1, 1});
    const std::vector<std::uint32_t> expected{48271, 182605794, 1291394886, 1914720637};
    JobId id = 1;
    for (const auto state : expected) {
        simulation.schedule({simulation.snapshot().now, id++});
        CHECK(simulation.step().has_value());
        CHECK(simulation.snapshot().random_state == state);
        CHECK(simulation.step().has_value());
    }
}

void rejection() {
    // This seed makes the first output 2147483646. The sampler must discard
    // values outside the largest whole multiple of 1000000 in its input range.
    Simulation simulation({247'665'088, 1, 1'000'000});
    simulation.schedule({0, 1});
    const auto trace = simulation.step();
    CHECK(trace.has_value());
    CHECK(trace->at(1).service == 877'853);
    CHECK(simulation.snapshot().random_state == 1'964'877'853);
}

void ordering() {
    auto simulation = make_simulation({{7, 2, 2}, {{0, 20}, {0, 10}, {2, 30}}});
    const auto trace = drain(simulation);
    CHECK(trace[0].job == 20); // Same-tick arrivals follow submission order, not ID.
    CHECK(trace[3] == Transition{2, TransitionKind::finish, 20, 0});
    CHECK(trace[4] == Transition{2, TransitionKind::start, 10, 2});
    CHECK(trace[5] == Transition{2, TransitionKind::arrive, 30, 0});
    CHECK(simulation.snapshot().completed == std::vector<JobId>({20, 10, 30}));
}

void unsorted_arrivals() {
    auto simulation = make_simulation({{1, 1, 1}, {{5, 1}, {0, 3}, {0, 2}}});
    const auto trace = drain(simulation);
    CHECK(trace.front().job == 3);
    CHECK(simulation.snapshot().completed == std::vector<JobId>({3, 2, 1}));
    CHECK(simulation.snapshot().now == 6);
}

void empty() {
    Simulation simulation;
    const auto before = format_snapshot(simulation.snapshot());
    CHECK(!simulation.step());
    CHECK(drain(simulation).empty());
    CHECK(format_snapshot(simulation.snapshot()) == before);
    CHECK(result({{}, {}}).find("trace 0\n") != std::string::npos);
}

void boundary() {
    Simulation simulation({0, 1, 1});
    const auto largest_id = std::numeric_limits<JobId>::max();
    simulation.schedule({max_tick - 1, largest_id});
    CHECK(drain(simulation).size() == 3);
    CHECK(simulation.snapshot().now == max_tick);
    CHECK(simulation.snapshot().completed == std::vector<JobId>({largest_id}));
}

void invalid_schedule() {
    Simulation simulation({1, 2, 2});
    simulation.schedule({3, 1});
    CHECK(simulation.step().has_value());
    const auto before = format_snapshot(simulation.snapshot());
    rejects([&] { simulation.schedule({2, 2}); });
    rejects([&] { simulation.schedule({3, 1}); });
    rejects([&] { simulation.schedule({3, 0}); });
    rejects([&] { simulation.schedule({max_tick + 1, 2}); });
    CHECK(format_snapshot(simulation.snapshot()) == before);
    CHECK(simulation.step().has_value());
    rejects([&] { simulation.schedule({5, 1}); }); // Completed IDs cannot be reused.
    rejects([] { Simulation invalid({1, 0, 1}); });
    rejects([] { Simulation invalid({1, 3, 2}); });
    rejects([] { Simulation invalid({1, 1, max_duration + 1}); });
}

void overflow_atomic() {
    Simulation simulation;
    simulation.schedule({max_tick, 1});
    const auto before = format_snapshot(simulation.snapshot());
    rejects([&] { static_cast<void>(simulation.step()); });
    CHECK(format_snapshot(simulation.snapshot()) == before); // RNG, time and queue roll back.
}

void snapshot_recovery() {
    const auto scenario = example();
    auto full = make_simulation(scenario);
    const auto expected_trace = drain(full);
    const auto expected_state = format_snapshot(full.snapshot());
    for (std::size_t cut = 0; cut <= 8; ++cut) {
        auto interrupted = make_simulation(scenario);
        std::vector<Transition> combined;
        for (std::size_t i = 0; i < cut; ++i) {
            const auto step = interrupted.step();
            CHECK(step.has_value());
            combined.insert(combined.end(), step->begin(), step->end());
        }
        Simulation recovered({99, 50, 50});
        recovered.restore(parse_snapshot(format_snapshot(interrupted.snapshot())));
        const auto suffix = drain(recovered);
        combined.insert(combined.end(), suffix.begin(), suffix.end());
        CHECK(combined == expected_trace);
        CHECK(format_snapshot(recovered.snapshot()) == expected_state);
    }
}

void snapshot_ownership() {
    auto simulation = make_simulation(example());
    auto checkpoint = simulation.snapshot();
    const auto original = format_snapshot(checkpoint);
    static_cast<void>(drain(simulation));
    CHECK(format_snapshot(checkpoint) == original);
    checkpoint.events.clear();
    CHECK(simulation.snapshot().completed.size() == 4);
}

void invalid_restore() {
    auto simulation = make_simulation(example());
    CHECK(simulation.step().has_value());
    CHECK(simulation.step().has_value());
    const auto valid = simulation.snapshot();
    const auto before = format_snapshot(valid);
    const std::vector<std::function<void(Snapshot&)>> corruptions{
        [](Snapshot& s) { s.random_state = 0; },
        [](Snapshot& s) { ++s.processed; },
        [](Snapshot& s) { ++s.next_sequence; },
        [](Snapshot& s) { s.waiting.push_back(s.waiting.front()); },
        [](Snapshot& s) { s.events.pop_back(); },
        [](Snapshot& s) { std::reverse(s.events.begin(), s.events.end()); },
        [](Snapshot& s) { s.events[1].sequence = s.events[0].sequence; },
        [](Snapshot& s) { s.events[0].kind = static_cast<EventKind>(99); },
        [](Snapshot& s) { s.active->finishes_at = s.now; },
        [](Snapshot& s) { s.registered[0] = 0; },
        [](Snapshot& s) { s.config.min_service = 0; },
        [](Snapshot& s) { s.completed.resize(max_jobs + 1, 1); }
    };
    for (const auto& corrupt : corruptions) {
        auto bad = valid;
        corrupt(bad);
        rejects([&] { simulation.restore(bad); });
        CHECK(format_snapshot(simulation.snapshot()) == before);
    }
    CHECK(drain(simulation).size() == 9); // Valid work remains recoverable after rejection.
}

void malformed_plan() {
    const std::vector<std::string> bad{
        "", "SIMLAB_PLAN 2", "SIMLAB_PLAN 1 config -1 2 5 jobs 0 end",
        "SIMLAB_PLAN 1 config +1 2 5 jobs 0 end", "SIMLAB_PLAN 1 config 4294967296 2 5 jobs 0 end",
        "SIMLAB_PLAN 1 config 1 0 5 jobs 0 end", "SIMLAB_PLAN 1 config 1 2 5 jobs 4097",
        "SIMLAB_PLAN 1 config 1 2 5 jobs 1 job 0 0 end",
        "SIMLAB_PLAN 1 config 1 2 5 jobs 2 job 0 1 job 1 1 end",
        "SIMLAB_PLAN 1 config 1 2 5 jobs 1 job 1000000000001 1 end",
        "SIMLAB_PLAN 1 config 1 2 5 jobs 1 job 18446744073709551616 1 end",
        "SIMLAB_PLAN 1 config 1 2 5 jobs 0 end junk"
    };
    for (const auto& text : bad) { rejects([&] { static_cast<void>(parse_plan(text)); }); }
    rejects([] { static_cast<void>(parse_plan(std::string(max_text_bytes + 1, 'x'))); });
    CHECK(format_plan(parse_plan(format_plan(example()))) == format_plan(example()));
}

void malformed_snapshot() {
    auto simulation = make_simulation(example());
    const auto valid = format_snapshot(simulation.snapshot());
    rejects([&] { static_cast<void>(parse_snapshot(valid + "junk")); });
    rejects([&] { static_cast<void>(parse_snapshot(valid.substr(0, valid.size() - 5))); });
    auto bad = valid;
    bad.replace(bad.find("rng 1"), 5, "rng 0");
    rejects([&] { static_cast<void>(parse_snapshot(bad)); });
    rejects([] { static_cast<void>(parse_snapshot("SIMLAB_SNAPSHOT 9")); });
}

void record_codec() {
    const auto scenario = example();
    const auto expected = result(scenario);
    const auto text = format_record(scenario, expected);
    const auto record = parse_record(text);
    CHECK(record.expected_result == expected);
    CHECK(result(record.scenario) == expected);
    rejects([&] { static_cast<void>(parse_record(text + "x")); });
    rejects([&] { static_cast<void>(parse_record(text.substr(0, text.size() - 1))); });
    auto bad_length = text;
    const auto start = bad_length.find("result-bytes ");
    const auto end = bad_length.find('\n', start);
    bad_length.replace(start, end - start, "result-bytes 4194305");
    rejects([&] { static_cast<void>(parse_record(bad_length)); });
    auto tampered = text;
    tampered.replace(tampered.find("duration 4"), 10, "duration 5");
    const auto modified = parse_record(tampered);
    CHECK(result(modified.scenario) != modified.expected_result);
}

class GroupedNumbers : public std::numpunct<char> {
    char do_thousands_sep() const override { return ','; }
    std::string do_grouping() const override { return "\3"; }
};

void locale() {
    const auto expected = result(example());
    struct RestoreLocale {
        std::locale previous;
        ~RestoreLocale() { std::locale::global(previous); }
    } restore{std::locale()};
    std::locale::global(std::locale(std::locale::classic(), new GroupedNumbers));
    CHECK(result(example()) == expected);
    CHECK(format_plan(parse_plan(format_plan(example()))) == format_plan(example()));
}

void job_limit() {
    Snapshot state;
    for (std::size_t i = 0; i < max_jobs; ++i) {
        const auto id = static_cast<JobId>(i + 1);
        state.registered.push_back(id);
        state.events.push_back({0, EventKind::arrival, static_cast<std::uint64_t>(i), id});
    }
    state.next_sequence = max_jobs;
    Simulation simulation;
    simulation.restore(state);
    rejects([&] { simulation.schedule({0, max_jobs + 1}); });
    CHECK(simulation.snapshot().registered.size() == max_jobs);
}

void conservation() {
    Scenario scenario{{42, 1, 20}, {}};
    for (JobId id = 1; id <= 128; ++id) { scenario.arrivals.push_back({(id * 17) % 53, id}); }
    auto simulation = make_simulation(scenario);
    const auto trace = drain(simulation);
    CHECK(trace.size() == 3 * scenario.arrivals.size());
    auto completed = simulation.snapshot().completed;
    std::sort(completed.begin(), completed.end());
    CHECK(completed == simulation.snapshot().registered);
    CHECK(simulation.snapshot().processed == 2 * scenario.arrivals.size());
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::pair<std::string, std::function<void()>>> cases{
        {"golden", golden}, {"determinism", determinism}, {"seed_mapping", seed_mapping},
        {"engine_vectors", engine_vectors}, {"rejection", rejection}, {"ordering", ordering},
        {"unsorted_arrivals", unsorted_arrivals}, {"empty", empty}, {"boundary", boundary},
        {"invalid_schedule", invalid_schedule}, {"overflow_atomic", overflow_atomic},
        {"snapshot_recovery", snapshot_recovery}, {"snapshot_ownership", snapshot_ownership},
        {"invalid_restore", invalid_restore}, {"malformed_plan", malformed_plan},
        {"malformed_snapshot", malformed_snapshot}, {"record_codec", record_codec},
        {"locale", locale}, {"job_limit", job_limit}, {"conservation", conservation}
    };
    if (argc != 2) { std::cerr << "expected one test name\n"; return 2; }
    for (const auto& item : cases) {
        if (item.first != argv[1]) { continue; }
        try { item.second(); std::cout << "PASS " << item.first << '\n'; return 0; }
        catch (const std::exception& error) { std::cerr << "FAIL " << item.first << ": " << error.what() << '\n'; return 1; }
    }
    std::cerr << "unknown test name\n";
    return 2;
}
