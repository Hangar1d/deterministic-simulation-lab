#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

namespace simlab {

using Tick = std::uint64_t;
using JobId = std::uint64_t;
inline constexpr Tick max_tick = 1'000'000'000'000ULL;
inline constexpr std::size_t max_jobs = 4096;
inline constexpr std::uint32_t max_duration = 1'000'000;
inline constexpr std::uint32_t random_modulus = 2'147'483'647;

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Config {
    std::uint32_t seed = 1;
    std::uint32_t min_service = 2;
    std::uint32_t max_service = 5;
};

struct Arrival {
    Tick at;
    JobId job;
};

// Completion wins a same-tick tie. Sequence is the final, unique tie-breaker.
enum class EventKind { completion, arrival };
struct Event {
    Tick at;
    EventKind kind;
    std::uint64_t sequence;
    JobId job;
};
bool event_before(const Event& left, const Event& right) noexcept;

struct ActiveJob {
    JobId job;
    Tick finishes_at;
};

// A complete value: no pointers, callbacks, clock handles, or hidden RNG state.
struct Snapshot {
    Config config;
    Tick now = 0;
    std::uint32_t random_state = 1;
    std::uint64_t next_sequence = 0;
    std::uint64_t processed = 0;
    std::uint64_t arrived = 0;
    std::vector<JobId> registered; // Strictly increasing; all submitted jobs.
    std::vector<JobId> waiting;    // FIFO order.
    std::vector<JobId> completed;  // Completion order.
    std::optional<ActiveJob> active;
    std::vector<Event> events;     // Strictly ordered by event_before.
};

enum class TransitionKind { arrive, start, finish };
struct Transition {
    Tick at;
    TransitionKind kind;
    JobId job;
    std::uint32_t service = 0; // Only start transitions carry a duration.
};
bool operator==(const Transition& left, const Transition& right) noexcept;

void validate(const Config& config);
void validate(const Snapshot& snapshot);

class Simulation {
public:
    explicit Simulation(Config config = {});
    void schedule(Arrival arrival);
    // No pending event => nullopt. One event can emit up to two transitions.
    std::optional<std::vector<Transition>> step();
    Snapshot snapshot() const;
    // Validation/copy failure leaves this simulation unchanged.
    void restore(Snapshot snapshot);

private:
    Snapshot state_;
};

// A batch is not transactional: earlier successful steps remain committed.
std::vector<Transition> drain(Simulation& simulation);

} // namespace simlab
