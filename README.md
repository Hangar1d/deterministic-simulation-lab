# deterministic-simulation-lab

A small C++17 discrete-event simulation of a single-server job queue. The project
focuses on explicit ordering, reproducible execution, and recoverable state.
It uses the standard library, CMake, and CTest, with no third-party runtime or
test framework.

## Build and test

Requires a C++17 compiler with `std::filesystem` and integer `std::from_chars`,
plus CMake 3.20 or newer. No network access is needed to build or test.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSIMLAB_WARNINGS_AS_ERRORS=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Commands below use `build/simlab`. With a multi-configuration generator on
Windows, use `build/Release/simlab.exe` instead.

```sh
build/simlab run examples/queue.plan
build/simlab record examples/queue.plan run.record
build/simlab replay run.record
build/simlab checkpoint examples/queue.plan 4 checkpoint.snapshot
build/simlab resume checkpoint.snapshot
```

Use fresh output paths: the CLI refuses existing output entries. `checkpoint`
counts processed **events**, not ticks or emitted trace lines. `resume` emits
the remaining transitions and the complete final state.

For Clang/GCC with ASan and UBSan available:

```sh
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DSIMLAB_ENABLE_SANITIZERS=ON
cmake --build build-sanitize --parallel
ctest --test-dir build-sanitize --output-on-failure
```

## Reproducible example

The example submits jobs 10, 20, 30, and 40 at ticks 0, 0, 2, and 4. Seed 1 and
service bounds 2–5 produce this trace prefix:

```text
0 arrive 10
0 start 10 duration 4
0 arrive 20
2 arrive 30
4 finish 10
4 start 20 duration 3
4 arrive 40
```

Job 10's completion runs before the new arrival at tick 4. Waiting job 20 starts
first. The final job finishes at tick 12. The complete result is checked in as
[queue.expected.txt](examples/queue.expected.txt), with a separate
[checkpoint after four events](examples/queue.after4.snapshot).

```sh
build/simlab run examples/queue.plan > first.txt
build/simlab run examples/queue.plan > second.txt
cmake -E compare_files first.txt second.txt
```

The `cli.reproducible` test compares both runs against the checked-in expected
result. `cli.record_replay` also checks identical record bytes and matching
replay output. Terminal newline translation is normalized in these tests;
saved record/snapshot files always use binary-mode LF.

## Architecture and decisions

```text
CLI: file ownership, commands, stdout/stderr
  └─ Codec: versioned plans, snapshots, records, canonical text
       └─ Simulation: typed inputs → ordered events → transitions + state
```

- **Explicit event order.** Events use `(tick, kind, sequence)`, with completions
  before arrivals and a unique increasing sequence as the final tie-breaker.
  Same-tick arrivals follow submission order. Service is FIFO and cannot be
  preempted; a newly arriving job cannot displace existing waiting work.
- **Integer time and defined randomness.** There is no wall clock, floating
  point, thread scheduling, or unordered-container iteration in the model.
  `std::minstd_rand` supplies its specified sequence; a project-defined rejection
  sampler maps it to inclusive service bounds. Standard distribution algorithms
  are implementation-defined, so `std::uniform_int_distribution` is deliberately
  avoided. See the standard draft's [engine parameters](https://eel.is/c++draft/rand.predef)
  and [distribution requirements](https://eel.is/c++draft/rand.dist.general).
- **Complete snapshots.** Checkpoints contain the pending event queue, active
  job, waiting order, completed IDs, counters, and RNG state. Recovery resumes
  the next random draw, not just a seed. Restore validates job conservation,
  event order, sequence uniqueness, and the active completion before committing.
- **Copy, validate, commit.** The core owns values and standard containers.
  `schedule`, `step`, and `restore` leave state unchanged if validation or
  allocation fails before commit. A whole multi-step run is not transactional.
- **Bounded, inspectable storage.** Sorted vectors keep event order and saved
  state easy to inspect. Copying and validation cost time and memory: an event
  step is O(n log n), not a scalable production scheduler. The lifetime limit is
  4,096 jobs. No performance benchmark is claimed.
- **Replay checks behavior.** A record contains the input plan and expected
  canonical trace/final snapshot. Replay recomputes and compares the complete
  result, catching output drift as well as state drift.

See [the version 1 formats](docs/formats.md) for ordering, limits, and validation
rules. Tests cover known traces/RNG vectors, rejection sampling, ordering ties,
recovery at every example event boundary, invalid inputs, rejected-operation
atomicity, and CLI failure behavior. The CI configuration targets Linux, macOS,
Windows, and a Linux sanitizer build; a workflow file alone is not evidence that
those hosted jobs have passed.

## Limits

- This is a single-threaded queue model, not a general simulation framework.
  No cancellation, priorities, real-time input, networking, or external effects.
- Exact-width 32/64-bit unsigned integers and a conforming C++17 library are
  required. Seed normalization has aliases, and the small linear-congruential
  engine is not cryptographically secure or a claim of statistical suitability.
- Format/algorithm version 1 defines reproducibility. Future algorithm changes
  require an explicit version/compatibility decision. Terminal stdout can use
  host line endings; canonical saved files use LF and classic-locale decimals.
- Snapshot validation checks structural invariants, not historical authenticity.
  Records are plain text, not signed or tamper-proof. They capture batch plans,
  not arbitrary interactive calls to the core API.
- Tick overflow is rejected before committing a step. A valid arrival near the
  maximum tick can therefore fail when service would exceed the time horizon.
- File writing is not crash-atomic. Existing-path refusal is best-effort and has
  a check/open race; use a trusted local directory and a single writer. An I/O
  failure can leave a partial output file. Files are limited to 4 MiB on input.
