# Version 1 formats

The formats use decimal unsigned integers and ASCII keywords. Plan and snapshot
parsers accept classic-locale whitespace, including CRLF, and reject trailing
non-whitespace, missing fields, unsupported versions, and out-of-range integers.
Formatting is canonical: fixed field order, one space between fields, LF line
endings, no timestamps or filesystem paths. Parsing is limited to 4 MiB.

## Plan

```text
SIMLAB_PLAN 1
config 1 2 5
jobs 4
job 0 10
job 0 20
job 2 30
job 4 40
end
```

`config` is `seed min_service max_service`. `job` is `tick id`; its position in
the plan is its submission order, even if the tick values are unsorted.
All submissions are scheduled before the CLI starts processing events.

| Field | Accepted range |
| --- | --- |
| Seed | 0 through 4,294,967,295 |
| Service bounds | 1 ≤ minimum ≤ maximum ≤ 1,000,000 ticks |
| Tick | 0 through 1,000,000,000,000 |
| Job ID | 1 through 18,446,744,073,709,551,615; unique for the simulation lifetime |
| Jobs | 0 through 4,096, including completed jobs |

Events sort by tick, then kind (`completion` before `arrival`), then sequence.
Scheduling an event allocates the next sequence number, beginning at zero.
Service starts immediately when an arrival finds the server idle, or when a
completion releases the next waiting job. Starts emit transitions and schedule
completions; they are not separate queued events. Durations are always positive,
so starting service cannot create an infinite same-tick event loop.

## Randomness

The normalized initial state is `seed mod 2147483647`, replacing zero with one.
Each raw draw follows the C++ `minstd_rand` recurrence:

```text
state = (48271 × state) mod 2147483647
value = state - 1
range = 2147483646
span = max_service - min_service + 1
limit = range - (range mod span)
```

Draw until `value < limit`, then use `min_service + (value mod span)`.
Rejected draws still advance state. A draw occurs on every service start, even
for fixed service bounds. The snapshot stores the last state; constructing
`minstd_rand` from it continues the specified recurrence. This avoids dependence
on library-specific distribution mappings or serialized engine internals.

Seeds 0, 1, and 2147483647 consequently share the same random sequence. The seed
field remains in snapshots, so differently written plans can still have different
serialized results even when their traces match.

## Snapshot

See [queue.after4.snapshot](../examples/queue.after4.snapshot). It contains:

| Field | Meaning |
| --- | --- |
| `config` | Original seed and service bounds |
| `clock` | Last processed event tick; zero before the first event |
| `rng` | Last raw random state, or normalized seed before service starts |
| `next_sequence` | Next event insertion sequence |
| `processed`, `arrived` | Number of processed events and processed arrivals |
| `registered N ...` | Strictly increasing list of all submitted job IDs |
| `waiting N ...` | FIFO list of arrived, inactive jobs |
| `completed N ...` | IDs in completion order |
| `active 0` or `active 1 ID FINISH` | Server state and planned completion |
| `events N` | Number of pending event lines |
| `event T KIND SEQ ID` | One event in strictly increasing total order |

Each registered job must occur in exactly one lifecycle state: pending arrival,
waiting, active, or completed. An active job has exactly one matching future
completion; an idle server has no waiting jobs. Pending event sequences must be
unique and below `next_sequence`. Counters must balance:

```text
arrived = waiting + completed + active
processed = arrived + completed
next_sequence = registered + completed + active
```

Here `active` is 0 or 1 and collection names denote sizes. Restoring a snapshot
replaces the complete configuration and state. Validation cannot prove that an
edited but structurally valid snapshot actually arose from its stated history.

## Result and record

[queue.expected.txt](../examples/queue.expected.txt) specifies a complete result:
`SIMLAB_RESULT 1`, a trace count, trace lines, `state`, a full snapshot, then
`end_result`. Trace lines are `T arrive ID`, `T start ID duration D`, or
`T finish ID`. Resume results contain only the remaining trace plus final state.

A record is `SIMLAB_RECORD 1`, a full plan, and `result-bytes N` followed by
**exactly one LF and N bytes** of canonical result. No trailing bytes are accepted.
Record files are written/read in binary mode, so the count is not affected by
host text newline translation. Replay reruns the plan and rejects any byte
difference in the canonical result. This detects accidental drift, not malicious
rewriting of both the plan and its expected result.

CLI exit codes: 0 for success, 1 for input/replay/I/O errors, and 2 for invalid
command usage. Diagnostics go to stderr; successful result text goes to stdout.
