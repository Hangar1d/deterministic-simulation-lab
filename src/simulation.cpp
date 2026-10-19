#include "simlab/simulation.hpp"

#include <algorithm>
#include <random>
#include <set>
#include <tuple>
#include <type_traits>
#include <utility>

namespace simlab {
namespace {

std::uint32_t normalized_seed(std::uint32_t seed) noexcept {
    const auto reduced = seed % random_modulus;
    return reduced == 0 ? 1 : reduced;
}

void require(bool condition, const char* message) {
    if (!condition) {
        throw Error(message);
    }
}

void enqueue(Snapshot& state, Tick at, EventKind kind, JobId job) {
    const Event event{at, kind, state.next_sequence++, job};
    state.events.insert(std::lower_bound(state.events.begin(), state.events.end(),
                                        event, event_before), event);
}

std::uint32_t sample_duration(Snapshot& state) {
    // minstd_rand's state is its last output. Re-seeding with that state resumes
    // the sequence without relying on implementation-specific stream formats.
    std::minstd_rand engine(state.random_state);
    const std::uint64_t range = std::minstd_rand::max() - std::minstd_rand::min() + 1ULL;
    const std::uint64_t span = state.config.max_service - state.config.min_service + 1ULL;
    const auto limit = range - range % span;
    std::uint64_t value = 0;
    do {
        state.random_state = static_cast<std::uint32_t>(engine());
        value = state.random_state - std::minstd_rand::min();
    } while (value >= limit);
    return state.config.min_service + static_cast<std::uint32_t>(value % span);
}

void start(Snapshot& state, JobId job, std::vector<Transition>& trace) {
    const auto duration = sample_duration(state);
    require(duration <= max_tick - state.now, "completion exceeds maximum tick");
    state.active = ActiveJob{job, state.now + duration};
    enqueue(state, state.active->finishes_at, EventKind::completion, job);
    trace.push_back({state.now, TransitionKind::start, job, duration});
}

} // namespace

bool event_before(const Event& left, const Event& right) noexcept {
    return std::tie(left.at, left.kind, left.sequence) <
           std::tie(right.at, right.kind, right.sequence);
}

bool operator==(const Transition& left, const Transition& right) noexcept {
    return std::tie(left.at, left.kind, left.job, left.service) ==
           std::tie(right.at, right.kind, right.job, right.service);
}

void validate(const Config& config) {
    require(config.min_service >= 1 && config.max_service <= max_duration &&
                config.min_service <= config.max_service,
            "service bounds must satisfy 1 <= min <= max <= 1000000");
}

void validate(const Snapshot& state) {
    validate(state.config);
    require(state.now <= max_tick, "clock exceeds maximum tick");
    require(state.registered.size() <= max_jobs, "too many jobs");
    require(state.waiting.size() <= max_jobs && state.completed.size() <= max_jobs &&
                state.events.size() <= max_jobs + 1, "snapshot collection exceeds limit");
    require(state.random_state >= 1 && state.random_state < random_modulus,
            "invalid random state");
    require(state.arrived <= state.registered.size(), "arrived count exceeds registered count");
    require(state.processed == state.arrived + state.completed.size(),
            "processed count does not match arrivals and completions");
    require(state.next_sequence == state.registered.size() + state.completed.size() +
                                       (state.active ? 1ULL : 0ULL),
            "sequence count does not match scheduled events");
    require(state.arrived == state.waiting.size() + state.completed.size() +
                                 (state.active ? 1ULL : 0ULL),
            "job accounting does not balance");
    require(state.active || state.waiting.empty(), "idle server cannot have waiting jobs");
    require(state.processed != 0 || state.now == 0, "unstarted clock must be zero");
    if (state.next_sequence == state.registered.size()) {
        require(state.random_state == normalized_seed(state.config.seed),
                "unstarted random state does not match seed");
    }

    require(std::is_sorted(state.registered.begin(), state.registered.end()) &&
                std::adjacent_find(state.registered.begin(), state.registered.end()) ==
                    state.registered.end(),
            "registered IDs must be strictly increasing");
    require(state.registered.empty() || state.registered.front() != 0, "job ID zero is invalid");
    std::set<JobId> accounted;
    const auto account = [&](JobId id) {
        require(std::binary_search(state.registered.begin(), state.registered.end(), id),
                "state refers to an unregistered job");
        require(accounted.insert(id).second, "job appears in multiple states");
    };
    for (const auto id : state.waiting) { account(id); }
    for (const auto id : state.completed) { account(id); }
    if (state.active) {
        account(state.active->job);
        require(state.active->finishes_at > state.now && state.active->finishes_at <= max_tick,
                "active completion must be in the future");
    }

    std::set<std::uint64_t> sequences;
    std::size_t completions = 0;
    for (std::size_t i = 0; i < state.events.size(); ++i) {
        const auto& event = state.events[i];
        require(event.at >= state.now && event.at <= max_tick, "event is outside the time range");
        require(event.sequence < state.next_sequence && sequences.insert(event.sequence).second,
                "invalid or duplicate event sequence");
        require(i == 0 || event_before(state.events[i - 1], event), "events are not strictly ordered");
        if (event.kind == EventKind::arrival) {
            account(event.job);
        } else if (event.kind == EventKind::completion) {
            require(state.active && event.job == state.active->job &&
                        event.at == state.active->finishes_at,
                    "completion does not match the active job");
            ++completions;
        } else {
            throw Error("unknown event kind");
        }
    }
    require(completions == (state.active ? 1U : 0U), "expected exactly one active completion");
    require(accounted.size() == state.registered.size(), "registered job is missing from state");
}

Simulation::Simulation(Config config) {
    validate(config);
    state_.config = config;
    state_.random_state = normalized_seed(config.seed);
}

void Simulation::schedule(Arrival arrival) {
    require(arrival.job != 0, "job ID zero is invalid");
    require(arrival.at >= state_.now && arrival.at <= max_tick, "arrival is outside the time range");
    require(state_.registered.size() < max_jobs, "too many jobs");
    require(!std::binary_search(state_.registered.begin(), state_.registered.end(), arrival.job),
            "duplicate job ID");
    auto next = state_;
    next.registered.insert(std::lower_bound(next.registered.begin(), next.registered.end(),
                                          arrival.job), arrival.job);
    enqueue(next, arrival.at, EventKind::arrival, arrival.job);
    validate(next);
    state_ = std::move(next);
}

std::optional<std::vector<Transition>> Simulation::step() {
    if (state_.events.empty()) { return std::nullopt; }
    auto next = state_;
    const auto event = next.events.front();
    next.events.erase(next.events.begin());
    next.now = event.at;
    ++next.processed;
    std::vector<Transition> trace;
    if (event.kind == EventKind::arrival) {
        ++next.arrived;
        trace.push_back({next.now, TransitionKind::arrive, event.job, 0});
        if (next.active) {
            next.waiting.push_back(event.job);
        } else {
            start(next, event.job, trace);
        }
    } else {
        trace.push_back({next.now, TransitionKind::finish, event.job, 0});
        next.completed.push_back(event.job);
        next.active.reset();
        if (!next.waiting.empty()) {
            const auto job = next.waiting.front();
            next.waiting.erase(next.waiting.begin());
            start(next, job, trace);
        }
    }
    validate(next);
    // All potentially throwing work precedes the commit of this transition.
    static_assert(std::is_nothrow_move_assignable<Snapshot>::value,
                  "snapshot commit must not throw");
    state_ = std::move(next);
    return trace;
}

Snapshot Simulation::snapshot() const { return state_; }

void Simulation::restore(Snapshot snapshot) {
    validate(snapshot);
    state_ = std::move(snapshot);
}

std::vector<Transition> drain(Simulation& simulation) {
    std::vector<Transition> trace;
    while (auto step = simulation.step()) {
        trace.insert(trace.end(), step->begin(), step->end());
    }
    return trace;
}

} // namespace simlab
