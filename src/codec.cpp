#include "simlab/codec.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <locale>
#include <sstream>
#include <utility>

namespace simlab {
namespace {

class Reader {
public:
    explicit Reader(const std::string& text) : input_(text) {
        if (text.size() > max_text_bytes) { throw Error("text exceeds 4 MiB limit"); }
        input_.imbue(std::locale::classic());
    }

    std::string token() {
        std::string value;
        if (!(input_ >> value)) { throw Error("unexpected end of input"); }
        return value;
    }

    void expect(const char* expected) {
        if (token() != expected) { throw Error(std::string("expected ") + expected); }
    }

    std::uint64_t number() {
        const auto value = token();
        std::uint64_t number = 0;
        const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
            throw Error("expected an unsigned decimal integer");
        }
        return number;
    }

    std::uint32_t number32() {
        const auto value = number();
        if (value > std::numeric_limits<std::uint32_t>::max()) { throw Error("integer exceeds uint32 range"); }
        return static_cast<std::uint32_t>(value);
    }

    std::size_t count(std::size_t limit) {
        const auto value = number();
        if (value > limit) { throw Error("count exceeds format limit"); }
        return static_cast<std::size_t>(value);
    }

    void header(const char* name) {
        expect(name);
        if (number() != 1) { throw Error("unsupported format version"); }
    }

    void end() {
        input_ >> std::ws;
        if (input_.peek() != std::char_traits<char>::eof()) { throw Error("unexpected trailing data"); }
    }

    std::string result_bytes(std::size_t length) {
        if (input_.get() != '\n') { throw Error("result length must be followed by LF"); }
        std::string result(length, '\0');
        input_.read(result.data(), static_cast<std::streamsize>(length));
        if (input_.gcount() != static_cast<std::streamsize>(length)) { throw Error("truncated record result"); }
        if (input_.peek() != std::char_traits<char>::eof()) { throw Error("trailing bytes after record"); }
        return result;
    }

private:
    std::istringstream input_;
};

std::ostringstream writer() {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    return output;
}

Config read_config(Reader& input) {
    Config config;
    // Read in separate statements: token consumption order is part of the format.
    config.seed = input.number32();
    config.min_service = input.number32();
    config.max_service = input.number32();
    validate(config);
    return config;
}

Scenario read_plan(Reader& input) {
    input.header("SIMLAB_PLAN");
    input.expect("config");
    Scenario scenario;
    scenario.config = read_config(input);
    input.expect("jobs");
    const auto count = input.count(max_jobs);
    for (std::size_t i = 0; i < count; ++i) {
        input.expect("job");
        const auto at = input.number();
        const auto id = input.number();
        scenario.arrivals.push_back({at, id});
    }
    input.expect("end");
    // Domain validation is shared with the engine, including duplicate IDs.
    static_cast<void>(make_simulation(scenario));
    return scenario;
}

std::vector<JobId> read_ids(Reader& input, const char* name) {
    input.expect(name);
    const auto count = input.count(max_jobs);
    std::vector<JobId> ids;
    ids.reserve(count);
    for (std::size_t i = 0; i < count; ++i) { ids.push_back(input.number()); }
    return ids;
}

void write_ids(std::ostream& output, const char* name, const std::vector<JobId>& ids) {
    output << name << ' ' << ids.size();
    for (const auto id : ids) { output << ' ' << id; }
    output << '\n';
}

const char* event_name(EventKind kind) {
    switch (kind) {
    case EventKind::arrival: return "arrival";
    case EventKind::completion: return "completion";
    }
    throw Error("unknown event kind");
}

} // namespace

Simulation make_simulation(const Scenario& scenario) {
    Simulation simulation(scenario.config);
    for (const auto& arrival : scenario.arrivals) { simulation.schedule(arrival); }
    return simulation;
}

Scenario parse_plan(const std::string& text) {
    Reader input(text);
    auto scenario = read_plan(input);
    input.end();
    return scenario;
}

std::string format_plan(const Scenario& scenario) {
    static_cast<void>(make_simulation(scenario));
    auto output = writer();
    output << "SIMLAB_PLAN 1\nconfig " << scenario.config.seed << ' '
           << scenario.config.min_service << ' ' << scenario.config.max_service
           << "\njobs " << scenario.arrivals.size() << '\n';
    for (const auto& arrival : scenario.arrivals) {
        output << "job " << arrival.at << ' ' << arrival.job << '\n';
    }
    output << "end\n";
    return output.str();
}

Snapshot parse_snapshot(const std::string& text) {
    Reader input(text);
    input.header("SIMLAB_SNAPSHOT");
    input.expect("config");
    Snapshot state;
    state.config = read_config(input);
    input.expect("clock"); state.now = input.number();
    input.expect("rng"); state.random_state = input.number32();
    input.expect("next_sequence"); state.next_sequence = input.number();
    input.expect("processed"); state.processed = input.number();
    input.expect("arrived"); state.arrived = input.number();
    state.registered = read_ids(input, "registered");
    state.waiting = read_ids(input, "waiting");
    state.completed = read_ids(input, "completed");
    input.expect("active");
    if (input.count(1) == 1) {
        const auto job = input.number();
        const auto finish = input.number();
        state.active = ActiveJob{job, finish};
    }
    input.expect("events");
    const auto count = input.count(max_jobs + 1);
    for (std::size_t i = 0; i < count; ++i) {
        input.expect("event");
        const auto at = input.number();
        const auto kind = input.token();
        EventKind event_kind;
        if (kind == "arrival") { event_kind = EventKind::arrival; }
        else if (kind == "completion") { event_kind = EventKind::completion; }
        else { throw Error("unknown event kind"); }
        const auto sequence = input.number();
        const auto job = input.number();
        state.events.push_back({at, event_kind, sequence, job});
    }
    input.expect("end");
    input.end();
    validate(state);
    return state;
}

std::string format_snapshot(const Snapshot& state) {
    validate(state);
    auto output = writer();
    output << "SIMLAB_SNAPSHOT 1\nconfig " << state.config.seed << ' '
           << state.config.min_service << ' ' << state.config.max_service
           << "\nclock " << state.now << "\nrng " << state.random_state
           << "\nnext_sequence " << state.next_sequence << "\nprocessed " << state.processed
           << "\narrived " << state.arrived << '\n';
    write_ids(output, "registered", state.registered);
    write_ids(output, "waiting", state.waiting);
    write_ids(output, "completed", state.completed);
    output << "active " << (state.active ? 1 : 0);
    if (state.active) { output << ' ' << state.active->job << ' ' << state.active->finishes_at; }
    output << "\nevents " << state.events.size() << '\n';
    for (const auto& event : state.events) {
        output << "event " << event.at << ' ' << event_name(event.kind) << ' '
               << event.sequence << ' ' << event.job << '\n';
    }
    output << "end\n";
    return output.str();
}

std::string format_result(const std::vector<Transition>& trace, const Snapshot& snapshot) {
    validate(snapshot);
    if (trace.size() > 3 * max_jobs) { throw Error("too many transitions"); }
    auto output = writer();
    output << "SIMLAB_RESULT 1\ntrace " << trace.size() << '\n';
    Tick previous = 0;
    for (const auto& item : trace) {
        if (item.at < previous || item.at > snapshot.now ||
            !std::binary_search(snapshot.registered.begin(), snapshot.registered.end(), item.job)) {
            throw Error("invalid transition time or job");
        }
        previous = item.at;
        output << item.at << ' ';
        switch (item.kind) {
        case TransitionKind::arrive: output << "arrive"; break;
        case TransitionKind::start: output << "start"; break;
        case TransitionKind::finish: output << "finish"; break;
        default: throw Error("unknown transition kind");
        }
        output << ' ' << item.job;
        if (item.kind == TransitionKind::start) {
            if (item.service < snapshot.config.min_service || item.service > snapshot.config.max_service) {
                throw Error("invalid transition duration");
            }
            output << " duration " << item.service;
        } else if (item.service != 0) { throw Error("unexpected transition duration"); }
        output << '\n';
    }
    output << "state\n" << format_snapshot(snapshot) << "end_result\n";
    return output.str();
}

std::string format_record(const Scenario& scenario, const std::string& result) {
    if (result.rfind("SIMLAB_RESULT 1\n", 0) != 0 || result.size() > max_text_bytes) {
        throw Error("invalid record result");
    }
    auto output = writer();
    output << "SIMLAB_RECORD 1\n" << format_plan(scenario)
           << "result-bytes " << result.size() << '\n' << result;
    auto text = output.str();
    if (text.size() > max_text_bytes) { throw Error("record exceeds 4 MiB limit"); }
    return text;
}

Record parse_record(const std::string& text) {
    Reader input(text);
    input.header("SIMLAB_RECORD");
    Record record;
    record.scenario = read_plan(input);
    input.expect("result-bytes");
    record.expected_result = input.result_bytes(input.count(max_text_bytes));
    return record;
}

} // namespace simlab
