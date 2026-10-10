# Schedulify

A CPU scheduling simulator written entirely in **C11**.

Schedulify implements six classic scheduling policies, simulates them on a
workload you supply, and prints an accurate text Gantt chart, a detailed
per-process table and the full set of performance metrics. Every schedule and
every number comes out of the C code in `src/` — there are no hard-coded
example answers anywhere in the program.

```
┌────────┬────────┬────┬────────┬────────────────┬────────────────────┐
│   P1   │   P2   │ P3 │   P2   │       P4       │         P1         │
└────────┴────────┴────┴────────┴────────────────┴────────────────────┘
0        2        4    5        7                11                   16
```

---

## Contents

- [What it does](#what-it-does)
- [Build and run](#build-and-run)
- [Using it](#using-it)
- [CSV format](#csv-format)
- [Project layout](#project-layout)
- [Scheduling assumptions](#scheduling-assumptions)
- [Metric definitions](#metric-definitions)
- [Hand-worked examples](#hand-worked-examples)
- [Tests and checks](#tests-and-checks)
- [Design notes](#design-notes)

---

## What it does

**Algorithms**

| Key | Algorithm | Kind |
|---|---|---|
| `FCFS` | First Come First Served | non-preemptive |
| `SJF` | Shortest Job First | non-preemptive |
| `SRTF` | Shortest Remaining Time First | preemptive |
| `PRIO-NP` | Priority Scheduling | non-preemptive |
| `PRIO-P` | Priority Scheduling | preemptive |
| `RR` | Round Robin | preemptive, time quantum |

**Per-process metrics** — completion time, turnaround time, waiting time,
response time, first dispatch time.

**Aggregate metrics** — average completion / turnaround / waiting / response
time, CPU utilization, throughput, total elapsed time, CPU busy time, CPU idle
time, context switches, preemptions.

**Workload sources** — ten predefined teaching examples, interactive custom
entry, a reproducible random generator, and CSV import.

**Output** — a to-scale text Gantt chart with an exact time ruler, an exact
segment list, the process table with an averages row, the metric summary, a
six-way comparison table that marks the winner in every column, and a
plain-language verdict.

**Modes** — run one algorithm, compare all six on the same workload, or walk
through a run one scheduling decision at a time in the educational step mode.

**Export** — the workload, one algorithm's full results, one algorithm's
timeline, or the six-way comparison, all to CSV.

**Presentation** — box-drawing glyphs and ANSI colors when the terminal
supports them, with a complete plain-ASCII / no-color fallback that is also
available on demand (`--ascii`, `--no-color`, or the Settings menu).

Edge cases that are handled and tested: simultaneous arrivals, idle CPU
periods, workloads whose first arrival is after *t*=0, ties on every key,
preemption, quantum expiry exactly at an arrival instant, a quantum larger
than every burst, and a single-process workload.

---

## Build and run

You need a C11 compiler (GCC 5+ or Clang 3.6+) and either `make` or CMake 3.13+.
There are no third-party dependencies.

### Linux / macOS — GCC or Clang

```sh
cd Schedulify
make                 # builds bin/schedulify
./bin/schedulify     # start the interactive menu

make test            # build and run the unit tests
make demo            # three non-interactive demos
make asan            # rebuild and run the tests under ASan + UBSan
make analyze         # GCC static analyser
make clean
```

To use Clang: `make CC=clang`.

### Windows — MinGW-w64 GCC

`gcc` and `mingw32-make` must be on `PATH`. If you do not have them, the
quickest route is:

```powershell
winget install BrechtSanders.WinLibs.POSIX.UCRT --scope user
```

Then, from PowerShell or `cmd.exe`:

```powershell
cd Schedulify
mingw32-make                 # builds bin\schedulify.exe
.\bin\schedulify.exe         # start the interactive menu

mingw32-make test            # build and run the unit tests
mingw32-make analyze         # GCC static analyser
mingw32-make clean
```

The Makefile works whether `make` picks `cmd.exe` or a POSIX `sh` (Git Bash,
MSYS2), so the same commands work from either shell.

### Without make — one explicit command

Linux / macOS:

```sh
gcc -std=c11 -Wall -Wextra -Wpedantic -O2 -Iinclude -Isrc \
    src/process.c src/validate.c src/ui.c src/table.c src/algorithms.c \
    src/sched_fcfs.c src/sched_sjf.c src/sched_priority.c src/sched_rr.c \
    src/metrics.c src/visualize.c src/compare.c src/csvio.c src/datasets.c \
    src/stepmode.c src/main.c -o schedulify -lm
./schedulify
```

Windows (PowerShell or `cmd.exe`, one line):

```powershell
gcc -std=c11 -Wall -Wextra -Wpedantic -O2 -Iinclude -Isrc src/process.c src/validate.c src/ui.c src/table.c src/algorithms.c src/sched_fcfs.c src/sched_sjf.c src/sched_priority.c src/sched_rr.c src/metrics.c src/visualize.c src/compare.c src/csvio.c src/datasets.c src/stepmode.c src/main.c -o schedulify.exe
.\schedulify.exe
```

And the test binary (swap `src/main.c` for the test sources):

```sh
gcc -std=c11 -O2 -Iinclude -Isrc -Itests \
    src/process.c src/validate.c src/ui.c src/table.c src/algorithms.c \
    src/sched_fcfs.c src/sched_sjf.c src/sched_priority.c src/sched_rr.c \
    src/metrics.c src/visualize.c src/compare.c src/csvio.c src/datasets.c \
    src/stepmode.c \
    tests/test_util.c tests/test_validation.c tests/test_metrics.c \
    tests/test_algorithms.c tests/test_csvio.c tests/test_main.c \
    -o schedulify_tests -lm
./schedulify_tests
```

### CMake (any platform)

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

On Windows with MinGW: `cmake -S . -B build -G "MinGW Makefiles"`.

Options: `-DSCHEDULIFY_WERROR=ON` (warnings are errors),
`-DSCHEDULIFY_ASAN=ON` (ASan + UBSan, GCC/Clang on Linux/macOS).

---

## Using it

### Interactive menu

```
[1]  Run one algorithm
[2]  Compare all six algorithms on this workload
[3]  Step-by-step educational mode
[4]  Load a predefined example
[5]  Enter a custom workload
[6]  Generate a random workload
[7]  Import a workload from CSV
[8]  Export to CSV
[9]  View or edit the current workload
[10] Settings (quantum, colors, ASCII/Unicode, width)
[11] Help: assumptions, metrics and algorithm rules
[0]  Quit
```

Typing `q` at any prompt backs out one level. Invalid input is explained and
re-asked; it never aborts the program.

In step mode, `Enter` advances one decision, `b` goes back one, `a` plays the
rest, `q` jumps to the finished schedule. Each step shows the scheduler's own
reason for the decision, the Gantt chart as it stands at that instant, and a
state panel with each process's executed and remaining time.

When stdin is a file or a pipe rather than a terminal, Schedulify echoes each
line it consumes, so a scripted run produces a transcript that reads exactly
like a live session:

```sh
printf '4\n2\n\n1\n3\n\n0\n' | ./bin/schedulify --no-color --ascii
```

### Command line

Any action flag runs non-interactively and skips the menu.

```
Workload
  --csv FILE              load a workload from CSV
  --example N             load predefined example N (1..10)
  --random N[:SEED]       generate N random processes (default seed 42)

Actions
  --algo NAME             fcfs | sjf | srtf | prio-np | prio-p | rr
  --compare               run and compare all six
  --step                  step-by-step mode (with --algo)
  --assumptions           print the documented assumptions
  --rules                 print what each algorithm does
  --list-examples         list the predefined examples
  --export-results F      one algorithm's results to F
  --export-comparison F   the comparison table to F
  --export-workload F     the workload to F
  --menu                  start the menu even after an action

Options
  --quantum N             Round Robin time quantum (default 2)
  --color | --no-color    force ANSI colors on or off
  --unicode | --ascii     force box glyphs or plain text
  --width N               output width, 60..200
  -h, --help              usage
  -V, --version           version
```

Examples:

```sh
./bin/schedulify --example 2 --algo srtf
./bin/schedulify --csv data/sample_processes.csv --compare --quantum 3
./bin/schedulify --random 8:7 --algo rr --quantum 4 --ascii --no-color
./bin/schedulify --example 2 --algo rr --quantum 2 --step
./bin/schedulify --csv data/classic_sjf_srtf.csv --compare \
                 --export-comparison out.csv
```

---

## CSV format

```csv
id,arrival,burst,priority
P1,0,5,2
P2,1,3,1
```

- The header row is optional and detected case-insensitively.
- The `priority` column is optional and defaults to 0.
- Blank lines and lines beginning with `#` are comments.
- Surrounding whitespace is ignored; a leading UTF-8 BOM is skipped.
- Every field passes through exactly the same validation as keyboard input,
  and the first bad row is reported with its line number.

Limits: ids are 1–15 characters with no spaces, commas, semicolons or quotes;
arrival 0–100000; burst 1–10000; priority 0–99; at most 512 processes; quantum
1–1000.

Three sample files ship in `data/`: `sample_processes.csv` (eight mixed
processes), `classic_sjf_srtf.csv` (the textbook SJF/SRTF workload) and
`idle_gaps.csv` (arrivals far enough apart to idle the CPU).

Exported result files carry three labelled sections — `process` rows, a
`summary` block and `segment` rows — so one file holds the table, the metrics
and the timeline.

---

## Project layout

```
Schedulify/
├── CMakeLists.txt          CMake build + ctest registration
├── Makefile                make build (GCC/Clang, Windows/Linux/macOS)
├── README.md
├── data/                   sample CSV workloads
├── include/
│   ├── process.h           data structures: Process, Dataset, Segment,
│   │                       ProcStats, Trace, Schedule
│   ├── validate.h          input validation and interactive prompts
│   ├── algorithms.h        the six schedulers + the algorithm registry
│   ├── sched_internal.h    helpers shared by the schedulers
│   ├── metrics.h           timeline → metrics
│   ├── visualize.h         Gantt chart, tables, help text
│   ├── compare.h           all six algorithms side by side
│   ├── csvio.h             CSV import and export
│   ├── datasets.h          predefined examples + random generator
│   ├── stepmode.h          step-by-step educational mode
│   └── ui.h                colors, box glyphs, headings, wrapping
├── src/
│   ├── process.c           container and Schedule/Trace lifecycle
│   ├── validate.c
│   ├── ui.c
│   ├── table.c / table.h   column-aligned table writer
│   ├── algorithms.c        registry, shared helpers, run entry point
│   ├── sched_fcfs.c        FCFS
│   ├── sched_sjf.c         SJF + SRTF
│   ├── sched_priority.c    priority, non-preemptive + preemptive
│   ├── sched_rr.c          Round Robin (FIFO ring buffer)
│   ├── metrics.c
│   ├── visualize.c
│   ├── compare.c
│   ├── csvio.c
│   ├── datasets.c
│   ├── stepmode.c
│   └── main.c              CLI parsing and the menu interface
└── tests/
    ├── test_util.c/.h      tiny assertion framework + helpers
    ├── suites.h
    ├── test_validation.c   fields, parsing, container, generator
    ├── test_metrics.c      formulas + rejection of impossible timelines
    ├── test_algorithms.c   hand-calculated schedules for all six
    ├── test_csvio.c        import, export, round trip
    └── test_main.c         runner
```

---

## Scheduling assumptions

These are the rules the simulator follows. They are also printed by
`--assumptions` and by menu entry 11.

**Model.** A single CPU runs one process at a time. Time is measured in
abstract integer units and every event happens on an integer boundary. There
is no I/O, no blocking and no priority ageing: once a process arrives it stays
ready until it has received its whole burst. Context switching and dispatching
are assumed to cost zero time, so switches are counted but never charged to
the clock — that is what keeps hand-calculated textbook answers reproducible.

**Priority convention.** A **lower priority number means a higher priority**.
0 is the most urgent value accepted, 99 the least. Priority is ignored by
FCFS, SJF, SRTF and Round Robin.

**Deterministic tie-breaking.** Whenever two processes are equally good
candidates, the choice is resolved in this order:

1. the algorithm's own key — arrival time, burst, remaining time or priority;
2. the earlier arrival time;
3. the earlier position in the input.

Because rule 3 always decides, the simulator never has to guess. The same
dataset always produces exactly the same schedule, on any platform and at any
optimisation level.

**Preemption.** SRTF and preemptive priority re-evaluate the decision at every
arrival and every completion. A newly arrived process displaces the running
one only when its key is **strictly** better, so equal values never cause a
gratuitous switch. Round Robin preempts on quantum expiry only; if no other
process is waiting at that instant the running process simply continues, and
that counts as neither a context switch nor a preemption.

**Round Robin queue discipline.** The ready queue is strictly FIFO. At the
instant a quantum expires:

1. every process whose arrival time is ≤ that instant is appended, in arrival
   order (input order for simultaneous arrivals);
2. *then* the process whose quantum expired is appended behind them.

So a newcomer arriving exactly at the boundary gets the CPU before the process
that has just been running. A dispatched process runs for
`min(quantum, remaining)`.

**Observation window.** Metrics are measured over
`[earliest arrival, last completion]`. Time before the first process exists is
not charged to the CPU, so a workload whose first arrival is *t*=5 and whose
last completion is *t*=20 has an elapsed time of 15 units, not 20.

---

## Metric definitions

| Metric | Definition |
|---|---|
| Completion time | end of the process's last CPU segment |
| Turnaround time | completion − arrival |
| Waiting time | turnaround − burst (time ready but not running) |
| Response time | first time on the CPU − arrival |
| Idle time | units inside the window with no process running |
| CPU busy time | window length − idle time (= sum of all bursts) |
| CPU utilization | 100 × busy ÷ window length |
| Throughput | processes completed ÷ window length |
| Context switches | hand-overs between two **different** processes |
| Preemptions | times a process left the CPU with work remaining |

The first dispatch is not a context switch. Adjacent slices belonging to the
same process are merged before counting, which is why Round Robin with a
single ready process reports zero switches and zero preemptions.

---

## Hand-worked examples

Each of these was worked out on paper first and is asserted in
`tests/test_algorithms.c`, timeline and every metric.

### 1. FCFS — the arrival order wins

`P1(arrival 0, burst 5)  P2(1, 3)  P3(2, 8)`

```
| P1 | P2 | P3 |
0    5    8    16
```

| | CT | TAT | WT | RT |
|---|---|---|---|---|
| P1 | 5 | 5 | 0 | 0 |
| P2 | 8 | 7 | 4 | 4 |
| P3 | 16 | 14 | 6 | 6 |

Average turnaround 26/3 = 8.67, average waiting 10/3 = 3.33, average response
10/3 = 3.33. Elapsed 16, idle 0, utilization 100 %, throughput 3/16 = 0.1875,
2 context switches, 0 preemptions.

### 2. SJF vs SRTF on the same workload

`P1(0, 7)  P2(2, 4)  P3(4, 1)  P4(5, 4)` — `data/classic_sjf_srtf.csv`

**SJF (non-preemptive).** P1 is dispatched at *t*=0 and cannot be
interrupted, so it holds the CPU to *t*=7. The ready set is then
{P2(4), P3(1), P4(4)}; P3 is shortest. P2 and P4 are then tied on burst 4, and
rule 2 of the tie-break chain gives it to the earlier arrival, P2.

```
| P1       | P3 | P2   | P4   |
0          7    8      12     16
```

| | CT | TAT | WT | RT |
|---|---|---|---|---|
| P1 | 7 | 7 | 0 | 0 |
| P2 | 12 | 10 | 6 | 6 |
| P3 | 8 | 4 | 3 | 3 |
| P4 | 16 | 11 | 7 | 7 |

Average turnaround **8.00**, average waiting **4.00**, average response 4.00,
3 context switches, 0 preemptions.

**SRTF (preemptive).** Re-decide at each arrival:

| *t* | ready (remaining) | decision |
|---|---|---|
| 0 | P1 7 | run P1 |
| 2 | P1 5, **P2 4** | 4 < 5 → preempt P1, run P2 |
| 4 | P1 5, P2 2, **P3 1** | 1 < 2 → preempt P2, run P3 |
| 5 | P1 5, **P2 2**, P4 4 | P3 finished; run P2 |
| 7 | P1 5, **P4 4** | P2 finished; run P4 |
| 11 | **P1 5** | run P1 to the end |

```
| P1 | P2 | P3| P2 | P4   | P1    |
0    2    4   5    7      11      16
```

| | CT | TAT | WT | RT |
|---|---|---|---|---|
| P1 | 16 | 16 | 9 | 0 |
| P2 | 7 | 5 | 1 | 0 |
| P3 | 5 | 1 | 0 | 0 |
| P4 | 11 | 6 | 2 | 2 |

Average turnaround **7.00**, average waiting **3.00**, average response 0.50,
5 context switches, 2 preemptions. Preemption is counted for P1 at *t*=2 and
P2 at *t*=4; P3 and P2 later leave the CPU because they finished, which is not
a preemption.

### 3. Priority, non-preemptive

All arrive at *t*=0. Lower number = higher priority.

`P1(0, 10, prio 3)  P2(0, 1, 1)  P3(0, 2, 4)  P4(0, 1, 5)  P5(0, 5, 2)`

```
| P2 | P5   | P1       | P3 | P4 |
0    1      6          16   18   19
```

| | CT | TAT | WT | RT |
|---|---|---|---|---|
| P1 | 16 | 16 | 6 | 6 |
| P2 | 1 | 1 | 0 | 0 |
| P3 | 18 | 18 | 16 | 16 |
| P4 | 19 | 19 | 18 | 18 |
| P5 | 6 | 6 | 1 | 1 |

Average turnaround 60/5 = **12.00**, average waiting 41/5 = **8.20**,
4 context switches, 0 preemptions. P3 and P4 are the starvation victims.

### 4. Round Robin, quantum 2 — full queue trace

`P1(0, 5)  P2(1, 4)  P3(2, 2)  P4(4, 1)`

| *t* | queue before | run | after the slice |
|---|---|---|---|
| 0 | `[P1]` | P1 0→2, 3 left | admit P2, P3 → `[P2,P3]`, requeue P1 → `[P2,P3,P1]` |
| 2 | `[P2,P3,P1]` | P2 2→4, 2 left | admit P4 → `[P3,P1,P4]`, requeue P2 → `[P3,P1,P4,P2]` |
| 4 | `[P3,P1,P4,P2]` | P3 4→6, **done** | `[P1,P4,P2]` |
| 6 | `[P1,P4,P2]` | P1 6→8, 1 left | requeue P1 → `[P4,P2,P1]` |
| 8 | `[P4,P2,P1]` | P4 8→9, **done** | `[P2,P1]` |
| 9 | `[P2,P1]` | P2 9→11, **done** | `[P1]` |
| 11 | `[P1]` | P1 11→12, **done** | `[]` |

```
| P1 | P2 | P3 | P1 | P4| P2 | P1|
0    2    4    6    8   9    11  12
```

| | CT | TAT | WT | RT |
|---|---|---|---|---|
| P1 | 12 | 12 | 7 | 0 |
| P2 | 11 | 10 | 6 | 1 |
| P3 | 6 | 4 | 2 | 2 |
| P4 | 9 | 5 | 4 | 4 |

Average turnaround 31/4 = **7.75**, average waiting 19/4 = **4.75**, average
response 7/4 = **1.75**, 6 context switches, 3 preemptions, elapsed 12,
throughput 4/12 = 0.333.

Note that P1 is requeued at *t*=2 *before* P4 arrives at *t*=4, which is why
P1 gets the CPU again at *t*=6 ahead of P4. This is the detail most hand
traces get wrong.

### 5. An idle CPU

`P1(0, 3)  P2(7, 2)` under FCFS

```
| P1  | IDLE   | P2 |
0     3        7    9
```

Both processes wait 0. Elapsed 9, busy 5, idle 4, utilization
5/9 = **55.56 %**, throughput 2/9 = 0.222, 1 context switch.

### 6. Round Robin with one process

`P1(0, 5)` with quantum 2. The quantum expires at *t*=2 and *t*=4, but the
queue is empty both times, so P1 is re-dispatched immediately. The three
slices merge into one segment `P1 [0,5)`: **0 context switches, 0
preemptions**.

---

## Tests and checks

```sh
make test            # 950 assertions across 4 suites
make analyze         # GCC static analyser over every source file
make asan            # ASan + UBSan (Linux/macOS; see below)
ctest --test-dir build --output-on-failure   # unit tests + 4 CLI smoke tests
```

The suites are:

- **validation** — id/arrival/burst/priority/quantum bounds, strict integer
  parsing (rejects `12x`, `3.5`, `0x10`, overflow, empty), whitespace
  trimming, duplicate detection with row numbers, the `Dataset` container
  including capacity exhaustion, generator reproducibility, and that all ten
  predefined examples validate.
- **metrics** — the documented formulas; segment merging; idle accounting; and
  that an impossible timeline is *rejected* (a hole, an overrun past the
  burst, a process that never ran, a process run before it arrives, a
  timeline that ends idle, a timeline that starts before the first arrival).
- **algorithms** — the hand-calculated schedules above plus: SJF/priority
  tie-breaks falling back on input order, SRTF and preemptive priority not
  switching on an *equal* key, a five-deep preemption cascade, Round Robin at
  quantum 1, with a quantum larger than every burst (must equal FCFS), across
  an idle gap, and with an arrival exactly on the quantum boundary. Three
  cross-cutting properties are checked for all six algorithms on a shared
  workload: work conservation (same makespan, same busy time, every process
  gets exactly its burst, no negative waiting/response, response ≤ waiting),
  bit-identical repeat runs, and that nothing beats SJF on average waiting
  time when all processes arrive together.
- **csv** — import with and without a header, comments, blank lines, defaulted
  priority, surrounding whitespace, a UTF-8 BOM; rejection of every malformed
  shape with the right line number; the export→import round trip; and that
  the exported numbers match the computed ones.

**On sanitizers:** MinGW-w64 on Windows ships no `libasan`/`libubsan`, so
`-fsanitize=address` cannot link there. `make asan` detects this and tells you
to use `make analyze` instead; run `make asan` on Linux, macOS or under WSL for
the real thing. `make analyze` runs GCC's static analyser (`-fanalyzer`), which
finds leaks, double frees, use-after-free, NULL dereferences and out-of-bounds
access at compile time and works on every platform. Both the analyser and the
test suite are clean.

**Warnings.** Everything compiles clean under `-std=c11 -Wall -Wextra
-Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wcast-qual -Wwrite-strings
-Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition
-Wredundant-decls -Wundef -Wformat=2 -Wvla`, and under CMake with
`-DSCHEDULIFY_WERROR=ON`.

---

## Design notes

**One source of truth for every number.** A scheduler's only job is to write
the execution timeline — an ordered list of `Segment { proc, start, end }`.
It never computes a metric. `metrics.c` then derives completion, turnaround,
waiting, response, idle, busy, utilization, throughput, context switches and
preemptions from that timeline. Six algorithms therefore cannot disagree about
what "waiting time" means, and the comparison table is apples to apples.

It also makes the simulator self-checking. `metrics_finalize()` refuses a
timeline that could not have come from a correct scheduler: a hole or overlap,
a process given more or less than its burst, a process run before it arrived,
a trailing idle segment, or `busy + idle ≠ window length`. A bug in an
algorithm surfaces as a loud error rather than as a plausible-looking wrong
number.

**Segment merging.** `schedule_add_segment()` merges a new slice into the
previous one when it belongs to the same process and starts exactly where the
last ended. That single rule is what makes the context-switch and preemption
counts correct for Round Robin: a quantum that expires with nobody else ready
produces one continuous segment, so it costs nothing.

**Chunked simulation instead of tick-by-tick.** SRTF and preemptive priority
only need to re-decide at an arrival or a completion. While a process runs it
holds the best key and its key only improves, so nothing can overtake it until
somebody new arrives. The simulation therefore advances in chunks of
`min(remaining, next_arrival − now)`, which gives identical results to a
tick loop without the cost — and conveniently produces exactly one trace event
per real decision, which is what the step-by-step mode replays.

**Dynamic memory.** `Dataset`, the segment list and the trace all grow by
doubling. Every allocation failure is propagated rather than ignored, and the
scheduler entry points return `false` so the caller can report it and carry
on. `Schedule`/`Dataset` are always safe to pass to their `_free` function,
including after a failed initialisation.

**Presentation is isolated.** Every escape sequence and every non-ASCII glyph
goes through `ui.c`, which picks a Unicode or ASCII glyph table and turns
colors on only when stdout is a terminal, `NO_COLOR` is unset, and (on
Windows) virtual-terminal processing could actually be enabled. The simulator
core prints nothing, so the exact same numbers come out whether the output is
a colored terminal or a redirected plain-text file.

---

## License

Released for educational use. No warranty.
