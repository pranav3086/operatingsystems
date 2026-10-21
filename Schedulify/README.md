# Schedulify

An interactive CPU scheduling simulator with a **C11 engine** and a
**visual browser dashboard**.

Set arrival times, burst times and priorities in the browser, pick one of six
scheduling algorithms, and watch the processes get scheduled — a Gantt chart
that fills in as a clock runs, per-process swimlanes, a live ready queue, and
the scheduler's own reasoning replayed step by step.

```
make web        # or: ./bin/schedulify --serve
```

opens <http://127.0.0.1:8752/> in your browser.

**Every schedule and every metric is computed in C.** The dashboard is
presentation only: it POSTs the workload to the C engine, receives a finished
timeline plus the metrics derived from it, and animates that. No scheduling
decision and no metric is ever calculated in JavaScript. The same engine also
drives a full terminal UI, so the browser is an interface onto it, not a
reimplementation of it.

```
┌────────┬────────┬────┬────────┬────────────────┬────────────────────┐
│   P1   │   P2   │ P3 │   P2   │       P4       │         P1         │
└────────┴────────┴────┴────────┴────────────────┴────────────────────┘
0        2        4    5        7                11                   16
```

---

## Contents

- [What it does](#what-it-does)
- [The dashboard](#the-dashboard)
- [Predictive workload analysis](#predictive-workload-analysis)
- [Build and run](#build-and-run)
- [Using it](#using-it)
- [HTTP API](#http-api)
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

**Predictive analysis** — a statistically generated possible continuation
of the current workload (Poisson arrivals, resampled bursts), with its
assumptions and stability diagnostics, scheduled by the same six algorithms
and compared against the original. See
[Predictive workload analysis](#predictive-workload-analysis).

**Modes** — the browser dashboard; a menu-driven terminal UI; run one
algorithm; compare all six on the same workload; or walk a run one scheduling
decision at a time in the educational step mode.

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

## The dashboard

```
make web
```

builds the engine, starts a small HTTP server written in C (`src/http.c`) on
the loopback interface, and opens the page in Chrome — falling back to your
default browser if Chrome is not installed.

**Set up the workload.** Add and remove processes, and edit each one's id,
arrival time, burst time and priority in place. Or load any of the ten
predefined examples, which are served from the same C table the terminal UI
uses. Limits (`burst >= 1`, priority 0–99, …) come from the C headers, so the
editor and the engine can never disagree about what is valid.

**Pick an algorithm.** The six are listed with the exact selection and
tie-break rule the C implementation follows; the quantum control appears only
for Round Robin, because the registry says only Round Robin needs one.

**Watch it run.** Press Simulate and the timeline plays:

- a **Gantt chart** that fills left to right under a moving playhead, every
  cell labelled with its process id and the exact boundary times on the ruler;
- **per-process swimlanes**, one row per process, with an arrival marker and
  that process's slices — so you can follow a single process across the whole
  schedule;
- a **live panel**: what is on the CPU right now and how much of its burst is
  left, who is ready and waiting, and who has finished;
- **metric tiles** and the **process table** for the finished schedule;
- the **scheduler's decision log** — the C engine's own explanations
  (`"t=2: PREEMPT P1 (remaining 5) and dispatch P2 (remaining 4) — strictly
  smaller remaining time wins"`) highlighted in sync with the clock.

Play, pause, step to the next or previous event, scrub the timeline, or change
the speed. Space toggles playback and the arrow keys step.

**Compare all six** puts every algorithm's metrics in one table with the best
value in each column starred, above six timeline strips drawn on a shared
scale — the quickest way to see that SRTF minimises waiting time while Round
Robin minimises response time at the cost of extra switches.

Light and dark themes both ship, following the system setting unless you
override it.

## Predictive workload analysis

The **Predictive analysis** block in the dashboard looks at the workload on
screen and asks: *if processes keep arriving the way these did, what might
arrive next, and how would each scheduling algorithm cope?* It estimates how
often processes arrive, generates one statistically plausible continuation of
the workload, and runs the original and the extended workload through the
same, unchanged schedulers so the two can be compared side by side.

Nothing comes from outside the simulator: there is no historical dataset, no
trained model and no prediction service. Every number is computed in C, in
`src/predict.c`, from the processes you entered. The browser only displays
what the engine returns, exactly as it does for ordinary simulations.

### Why it is useful

A scheduler that looks good on a fixed list of processes can behave very
differently once more work keeps arriving. The forecast lets you see, for the
same algorithm:

- how much the **original** processes are delayed by work that arrives after
  them. Under SRTF, for example, a short newcomer can jump ahead of a long job
  that was already waiting;
- how CPU utilization, throughput, idle time and context switches change
  under more load;
- how uncertain the forecast is. The count model shows the whole range of
  plausible arrival counts, not just one number.

It also teaches the statistics that queueing theory and OS textbooks rely on:
Poisson arrivals, exponential inter-arrival times and resampling. Every step
is visible and every assumption is printed.

### Using it, step by step

1. **Enter or load a workload** in the process editor as usual. At least two
   processes with different arrival times are needed unless you enter an
   observation window (step 3).
2. **Forecast horizon** — how many time units into the future to forecast.
   Leave it blank to use the same length as the observation window.
3. **Observation window** — choose *Infer from arrival gaps* (the default) or
   *I know the window* and enter `from t =` / `to t =`. See
   [the observation window](#the-observation-window) below for what this
   changes.
4. **Cap** (optional) — the most predicted processes to generate. Leave it
   blank for no cap other than the engine's total limit of 512 processes.
5. **Seed** — the random seed (default 42). The same workload, settings and
   seed always produce exactly the same forecast; change the seed to see
   another equally plausible future.
6. Press **Generate forecast**. The results panel shows the assumption made,
   any warnings, the rate and count model, an arrival chart, the probability
   chart, the stability diagnostics and the table of predicted processes
   (`F1`, `F2`, …).
7. Pick an algorithm in the normal algorithm list and press **Simulate
   predicted workload**. The timeline plays the extended workload, with the
   predicted processes dashed and tagged *forecast*. A comparison table shows
   every metric before and after, and *Original workload* / *With forecast*
   switch the timeline between the two runs.
8. **Reset** clears the forecast. It never changes your processes.

If you edit the workload after generating, the forecast is marked
**out of date** and *Simulate* is blocked until you generate again, so a
forecast can never be simulated against data it was not computed from.
Undoing the edit makes it valid again.

### How it works

The whole analysis is one pipeline, run by `forecast_run()` in
`src/predict.c`:

```
observed processes
      │
      ▼
1. observation window T  ──►  2. arrival rate λ = N / T  (+ standard error)
                                      │
                                      ▼
                     3. count model K ~ Poisson(λh) over the horizon h
                                      │
                                      ▼
                     4. arrival times from exponential gaps (seeded RNG)
                                      │
                                      ▼
                     5. burst + priority resampled from observed processes
                                      │
                                      ▼
                     6. diagnostics and warnings on the observed data
                                      │
                                      ▼
            predicted processes F1, F2, …  ──►  7. both workloads scheduled
                                                   by the same algorithm
```

#### 1. The observation window

The arrival rate is "arrivals per unit of time", so it needs to know how long
the system was watched. A `Process` only records *when it arrived*, not how
long anyone was looking, so the workload cannot tell us that. There are two
ways to supply it:

| Mode | Window | Events counted | Rate |
|---|---|---|---|
| **Infer from arrival gaps** (default) | first arrival → last arrival | the n − 1 gaps between n arrivals | λ = (n − 1) / (last − first) |
| **I know the window** | the `start`–`end` you enter | all n arrivals | λ = n / (end − start) |

Inferring the window treats the first and last arrivals as its edges. Within
that span there are exactly n − 1 complete gaps between arrivals, so the rate
is the number of gaps divided by the time they cover, which is the reciprocal
of the mean gap. This is the standard estimate when the window is unknown.
It is still an assumption, so the page prints it with every result, e.g.
*"No observation window given. Assumed: observation began at the first
arrival (t=0) and ended at the last (t=5), so lambda = 3 gaps / 5 units."*

Some workloads cannot give a rate, and they are refused with a reason rather
than given a made-up number:

- **one process** — one arrival has no gaps, so there is nothing to measure;
- **every process arriving at the same instant** — the gaps total zero time
  (zero exposure), so λ would be infinite.

In both cases the message asks you to enter the observation window instead.
With an explicit window, the window must contain every observed arrival and
must stay within the engine's arrival-time limits. If only `to t =` is given,
the start is t = 0.

#### 2. The arrival rate and its uncertainty

λ = N / T is the maximum-likelihood estimate of a Poisson process's rate. For
a Poisson count N its standard error is

```
SE(λ) = √N / T = λ / √N
```

so the relative error is 1/√N: about 58% with 3 events, 32% with 10 and 10%
with 100. The page reports SE(λ). When N < 10 the forecast carries a warning
that it is **illustrative** and gives that percentage. Most teaching
workloads (4–8 processes) get this warning, and that is correct: four
arrivals cannot pin down a rate.

#### 3. The count model — Poisson(λh)

If arrivals form a Poisson process with rate λ, the number K that arrives in a
horizon of length h follows a Poisson distribution with mean μ = λh:

```
P(K = k) = μ^k · e^(−μ) / k!          E[K] = Var[K] = μ
P(K = 0) = e^(−μ)                      P(K ≥ 1) = 1 − e^(−μ)
```

The engine reports:

- **Expected arrivals** E[K] = μ;
- **P(no arrivals)** and **P(at least one)**;
- the **central 90% range**: the 5th and 95th percentiles of Poisson(μ),
  i.e. the smallest k with P(K ≤ k) ≥ 0.05 and the smallest with
  P(K ≤ k) ≥ 0.95;
- the **probability of every count**, drawn as the bar chart (up to 61 bars).
  Bars outside the 90% range are faint, and the bar for the count this
  particular draw produced is outlined.

The pmf is computed in log space, as exp(k·ln μ − μ − ln k!) using `lgamma`,
because μ^k and k! overflow a `double` long before μ gets large. Cumulative
probabilities are summed only over μ ± 12√μ (+12), outside which the
probability is below 10⁻³⁰. This keeps them fast and accurate even for
μ = 10⁶.

#### 4. Arrival times — exponential gaps

In a Poisson process the gaps between arrivals are independent and
exponentially distributed with mean 1/λ. A gap is generated by inverse
transform sampling from a uniform random number U in (0, 1):

```
δ = −ln(U) / λ
```

Starting at the end of the observation window, gaps are added one after
another until the running time passes the end of the horizon:

```
t₀ = window end
t₁ = t₀ + δ₁,  t₂ = t₁ + δ₂, …   stop at the first tᵢ > window end + h
```

The number of arrivals that fit is K. This is **one** random process, not
two: the arrival times are not drawn after first drawing a count. Counting
exponential gaps until a time limit produces a Poisson(λh) count by
construction. The Poisson formulas in step 3 describe the distribution this
count comes from. They are not a second, separate draw.

The simulator's clock is integral, so each arrival time is rounded **up**
(`ceil`). A time in (end, end + h] therefore becomes a whole number in
[end + 1, end + h]. Every predicted arrival lands strictly after the observed
workload and inside the horizon. Rounding can put two predicted processes on
the same tick; those ties are counted, reported, and settled by each
scheduler's existing tie-break rule.

**Cap.** If a cap is set and reached before the horizon ends, generation
stops there. The count is then censored: it no longer follows Poisson(λh).
The forecast says so and gives the model's probability of that happening,
P(K > cap) = 1 − P(K ≤ cap). Without a cap the limit is the engine's 512
processes in total (observed + predicted).

#### 5. Burst times and priorities — the empirical bootstrap

Each predicted process copies the burst time **and** priority of an observed
process chosen uniformly at random, with replacement. This is the
nonparametric bootstrap: the observed processes are treated as a sample from
the true distribution and resampled directly.

- No distribution shape is assumed. Burst times are not forced into a normal
  or exponential curve, and no predicted process is simply given the mean.
- Every predicted value is a value that really occurred, so the spread and
  skew of the workload are preserved.
- Burst and priority are resampled **as a pair**, so any relationship between
  them is kept. For example, if short jobs had high priority, predicted short
  jobs do too. Each one on its own is still a draw from its observed
  distribution.

The page reports the observed burst mean, sample standard deviation
(n − 1 denominator), minimum, maximum and number of distinct values, and the
same for priorities. With one observed process the standard deviation is
reported as not estimable. If every burst is the same, a warning explains
that resampling cannot invent values that were never observed.

Predicted processes are named `F1`, `F2`, … in arrival order, skipping any id
already used in the workload (case-insensitively), so ids never clash. Each
one passes the same validation as a hand-entered process.

#### 6. Randomness and reproducibility

The predictor has its **own** random number generator, separate from the
workload generator in `datasets.c`, so forecasting never disturbs anything
else:

- the seed is mixed through **splitmix64** into a 64-bit state, so nearby
  seeds such as 1, 2 and 3 give unrelated streams;
- numbers are drawn with **xorshift64\***;
- uniforms use 53 random bits centred in their cell, so U is never exactly 0
  or 1 and −ln(U) is always finite.

The same workload, settings and seed always give the same forecast on every
platform. The schedulers themselves contain no randomness and are not
touched, so a fixed workload always schedules the same way.

#### 7. Diagnostics — does the model fit?

The Poisson model assumes arrivals at a **constant rate**, **independently**
and **one at a time**. The engine checks the observed arrivals against each of
these assumptions and reports the result, with a warning when one fails:

| Check | What it tests | Method | Needs |
|---|---|---|---|
| Sample size | is λ estimated precisely enough? | SE(λ) = λ/√N; warns when N < 10 | — |
| Rate stability | is the rate the same in both halves of the window? | exact two-sided binomial test, p < 0.05 | 4 events |
| Dispersion | are arrivals as random as Poisson — not clustered, not too regular? | χ² index of dispersion over equal bins, p < 0.05 either tail | 12 events |
| Simultaneous arrivals | do any processes arrive together? | count of shared arrival times | — |
| Burst variety | is there spread to resample? | one sample, or one distinct value | — |
| Extrapolation | is the horizon far beyond what was observed? | horizon > 3 × window | — |

**Rate stability.** If the rate is constant, each of the m arrivals in the
window falls in the first half with probability ½, so the count in the first
half is Binomial(m, ½). The p-value is the total probability of all outcomes
no more likely than the one observed. A small p means arrivals were clearly
front- or back-loaded. The page shows both half-window rates.

**Dispersion.** The window is cut into B equal bins, where B = m/3 clamped to
4–20, and the arrivals in each bin are counted. Poisson counts have
variance = mean, so the variance-to-mean ratio should be near 1. The
statistic

```
D = Σ (cᵢ − c̄)² / c̄   ~   χ²(B − 1)
```

is tested in **both** tails:

- a large D (ratio ≫ 1) means arrivals are **burstier** than Poisson;
- a small D (ratio ≪ 1) means they are **more evenly spaced** than Poisson,
  as in a hand-typed workload with an arrival every 2 units.

The χ² tail probability uses the regularised incomplete gamma function,
computed by its series or by a Lentz continued fraction depending on the
argument. The small tail is computed directly, so it does not lose precision.

**Simultaneous arrivals** cannot happen in a continuous-time Poisson process.
Batch arrivals are common in teaching workloads, so they are reported as a
sign the workload is not Poisson.

**Which arrivals the checks use.** With an explicit window, every arrival
counts. When the window was inferred, the first and last arrivals *define*
the window, so they are not random draws from it. Only the arrivals strictly
between them are tested.

When a check needs more data than it has, the page says so (e.g. *"not run:
needs at least 4 arrivals, has 2"*) instead of reporting a meaningless
p-value. **No confidence
percentage is ever invented.** Passing a check only means the sample does not
contradict the model, not that the model is proven correct.

A forecast that produced **zero** arrivals is not an error. It happens with
probability e^(−μ), and the page says so and suggests another seed or a
longer horizon.

#### 8. Simulating the forecast

*Simulate predicted workload* sends the original processes and the predicted
ones to `/api/forecast/simulate`. The engine:

1. runs the selected algorithm on the **original** workload;
2. merges the original and predicted processes (originals first) and runs the
   same algorithm on that **augmented** workload;
3. derives every metric from each timeline with the usual `src/metrics.c` code;
4. returns both schedules and a comparison table.

| Comparison row | Meaning |
|---|---|
| Avg waiting / turnaround / response | over **all** processes in each run |
| Original processes: avg waiting / turnaround / response | over **your processes only**, in both runs — how much the forecast delays the work that was already there |
| CPU utilization, throughput, idle time, total time | for the whole run |
| Context switches, preemptions | for the whole run |
| Processes | count in each run |

Each row shows the original value, the augmented value and the change,
labelled *higher*, *lower* or *no change*. The labels carry no judgement,
because more throughput can be good while more waiting is bad.

The "original processes only" rows matter because the all-process average
can hide the effect: newcomers with short bursts can pull the overall
average down while the processes that were already there wait longer.

The predictor never schedules anything itself. It only produces a list of
processes, which goes through `algo_run()` exactly like a hand-entered
workload. The six scheduling algorithms were not modified.

### Worked example

The textbook workload `P1(0,7) P2(2,4) P3(4,1) P4(5,4)`
(id(arrival, burst)), with every setting left at its default:

**Window and rate.** Arrivals at 0, 2, 4, 5. The window is inferred as
t = 0 → 5 (T = 5), containing 3 gaps:

```
λ = 3 / 5 = 0.6 arrivals per unit        SE = 0.6 / √3 = 0.346  (58%)
```

**Count model.** The horizon defaults to the window length, h = 5, so
μ = 0.6 × 5 = 3:

```
E[K] = 3
P(K = 0) = e⁻³ = 0.0498  (4.98%)        P(K ≥ 1) = 95.02%
cumulative P(K ≤ k):  k=0 0.050  1 0.199  2 0.423  3 0.647  4 0.815  5 0.916  6 0.966
central 90% range:    1 – 6 arrivals    (first k reaching 0.05, first reaching 0.95)
```

**Bursts.** Observed 7, 4, 1, 4: mean 4, sample sd = √(18/3) = √6 = 2.45,
range 1–7, 3 distinct values. Predicted bursts can only be 7, 4 or 1, with
4 twice as likely as 7 or 1.

**Diagnostics.** With an inferred window only P2 and P3 (2 events) lie
strictly inside, so the rate-stability check (needs 4) and the dispersion
check (needs 12) say they cannot run. One warning is raised: *only 3 gaps
were observed, the rate's standard error is about 58% of the estimate, so
this forecast is illustrative*.

**Simulation.** Suppose a draw produces one forecast process `F1(6,1)`. Under
SRTF:

- the overall average waiting time stays at 3.00;
- the original four processes now average **3.50** instead of 3.00. F1's
  burst of 1 is the shortest remaining at t = 7, so it runs ahead of P4 and
  P1, and each of them finishes one unit later;
- the makespan grows from 16 to 17.

The all-process average hides the delay; the original-processes rows show it.

### Where it lives in the code

| Piece | Location |
|---|---|
| Public API and result struct | `include/predict.h` — `ForecastConfig`, `ForecastResult`, `forecast_run()`, `forecast_merge()` |
| Rate, Poisson pmf/cdf/quantile, exponential gap | `predict_rate()`, `predict_poisson_*()`, `predict_exp_gap()` in `src/predict.c` |
| Arrival generation | `predict_arrivals()` |
| Burst summary | `predict_burst_summary()` |
| Binomial and χ² tests | `predict_binom_two_sided()`, `predict_gamma_p()`, `predict_chi2_sf()` |
| RNG | `predict_rng_seed()`, `predict_rng_uniform()`, `predict_rng_index()` |
| HTTP routes | `route_forecast()`, `route_forecast_simulate()` in `src/webapp.c` |
| Dashboard | the *Predictive analysis* block in `web/index.html`, `generateForecast()` / `simulateForecast()` and the `render*` functions in `web/app.js` |
| Tests | `tests/test_predict.c` |

The tests check:

- every formula against hand-computed values;
- the Poisson count over 2000 seeds: its mean, variance, P(0) and 90% range
  match Poisson(3);
- that the same seed reproduces the same forecast exactly, and that
  forecasting does not disturb the `datasets.c` generator;
- truncation, id clashes and every invalid input;
- the diagnostics on deliberately front-loaded, evenly spaced and batched
  workloads;
- the two routes, including the hand-worked SRTF and FCFS comparisons above,
  for all six algorithms.

### Limitations

- **The model is a homogeneous Poisson process.** It assumes a constant rate
  and independent, single arrivals. Real systems have daily cycles, bursts
  and dependent jobs. The diagnostics can flag clear violations but cannot
  model them.
- **Small workloads give weak estimates.** With 4–8 processes the standard
  error is 35–58% of λ, and the stability checks often lack the data to run.
  The page says so instead of hiding it.
- **The observation window is an assumption** unless you enter it, because
  the data model does not record it.
- **Each forecast is one draw.** It is a single possible future, not an
  average over many. Try several seeds to see the spread, which the count
  model's 90% range also summarises.
- **Bootstrapping can only repeat observed values.** It never produces a
  burst time or priority that was not in the workload.
- **Not validated against real OS traces.** The statistics are checked
  against theory, not against measured workloads.
- **Dashboard and HTTP API only.** The terminal menu and command line do not
  offer forecasting.

---

## Build and run

You need a C11 compiler (GCC 5+ or Clang 3.6+) and either `make` or CMake 3.13+.
There are no third-party dependencies.

### Linux / macOS — GCC or Clang

```sh
cd Schedulify
make web             # build, serve the dashboard, open a browser  <-- start here
make                 # just build bin/schedulify
./bin/schedulify     # the terminal menu instead of the dashboard

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

If `gcc` is still not found in a new terminal, add the package's `mingw64\bin`
folder to `PATH`. winget puts it under
`%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs…\mingw64\bin`;
`cmake` is in the same folder. From Git Bash:

```sh
export PATH="$PATH:$(ls -d "$LOCALAPPDATA"/Microsoft/WinGet/Packages/BrechtSanders.WinLibs*/mingw64/bin | head -1)"
```

Then, from PowerShell or `cmd.exe`:

```powershell
cd Schedulify
mingw32-make web             # build, serve the dashboard, open a browser
mingw32-make                 # just build bin\schedulify.exe
.\bin\schedulify.exe --serve # the dashboard, started by hand
.\bin\schedulify.exe         # the terminal menu instead

mingw32-make test            # build and run the unit tests
mingw32-make analyze         # GCC static analyser
mingw32-make clean
```

The dashboard needs no network access, no package manager and no build step of
its own: the page is plain HTML, CSS and JavaScript with no dependencies, and
the server is the same C binary.

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


### Output width

All prose — headings, notes, help text, menu descriptions, dot-leader metric
lines — wraps to the configured width, so nothing runs off the edge at any
width from 60 to 200. The column-aligned data tables have a fixed minimum:
the six-way comparison table needs 91 columns and the process table 89, so on
a terminal narrower than about 92 columns those two will be soft-wrapped by
the terminal. The default width is detected from the terminal (falling back
to 100) and can be set with `--width N` or from the Settings menu.

### Command line

Any action flag runs non-interactively and skips the menu.

```
The dashboard
  --serve                 start the local web dashboard and open a browser
  --port N                port to listen on (default 8752)
  --no-browser            serve but do not open a browser
  --web-root DIR          where index.html lives (auto-detected)

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
./bin/schedulify --serve                    # the dashboard
./bin/schedulify --serve --port 9000         # on another port
./bin/schedulify --example 2 --algo srtf
./bin/schedulify --csv data/sample_processes.csv --compare --quantum 3
./bin/schedulify --random 8:7 --algo rr --quantum 4 --ascii --no-color
./bin/schedulify --example 2 --algo rr --quantum 2 --step
./bin/schedulify --csv data/classic_sjf_srtf.csv --compare \
                 --export-comparison out.csv
```

---

## HTTP API

The dashboard is an ordinary client of a small JSON API, so you can drive the
engine from `curl`, a notebook or your own front end.

| Route | Method | Returns |
|---|---|---|
| `/api/algorithms` | GET | the six algorithms, their kinds and their exact tie-break rules, plus the validation limits |
| `/api/examples` | GET | the ten predefined workloads |
| `/api/simulate` | POST | one finished schedule: timeline, per-process stats, metrics and the decision trace |
| `/api/compare` | POST | the same, for all six algorithms |
| `/api/forecast` | POST | rate, Poisson count model, diagnostics, warnings and the predicted processes |
| `/api/forecast/simulate` | POST | the original and forecast-augmented schedules plus a metric comparison |

Request body for both POST routes (`algorithm` is omitted for `/api/compare`):

```json
{
  "algorithm": "srtf",
  "quantum": 2,
  "processes": [
    { "id": "P1", "arrival": 0, "burst": 7, "priority": 2 },
    { "id": "P2", "arrival": 2, "burst": 4, "priority": 1 }
  ]
}
```

```sh
curl -s -X POST http://127.0.0.1:8752/api/simulate \
     -H 'Content-Type: application/json' \
     -d '{"algorithm":"srtf","processes":[
          {"id":"P1","arrival":0,"burst":7},
          {"id":"P2","arrival":2,"burst":4}]}'
```

Every field goes through the same `validate.c` predicates as keyboard and CSV
input, so a bad request comes back as `400` with the same wording the terminal
would print: `{"error":"process 1: burst time 0 is outside 1..10000 ..."}`.

`/api/forecast` takes `processes` plus the optional `horizon`,
`observationStart`, `observationEnd`, `maxGenerated` and `seed`.
`/api/forecast/simulate` takes the `/api/simulate` body plus the `predicted`
array that `/api/forecast` returned:

```sh
curl -s -X POST http://127.0.0.1:8752/api/forecast      -H 'Content-Type: application/json'      -d '{"horizon":10,"seed":42,"processes":[
          {"id":"P1","arrival":0,"burst":7},{"id":"P2","arrival":2,"burst":4},
          {"id":"P3","arrival":4,"burst":1},{"id":"P4","arrival":5,"burst":4}]}'
```

The JSON reader and writer are themselves written in C (`src/json.c`) — the
rule that nothing about scheduling leaves C applies to the encoding too.

**Security posture.** The listening socket is bound to `127.0.0.1` only, so
the dashboard is never reachable from the network. Static files are served
from the web root alone: paths containing `..`, absolute paths, drive letters
and anything outside a small extension allow-list are refused before a file is
ever opened.

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
│   ├── predict.h           statistical predictive workload analysis
│   ├── ui.h                colors, box glyphs, headings, wrapping
│   ├── json.h              JSON reader + writer (for the dashboard)
│   ├── http.h              the tiny loopback HTTP server
│   ├── webapp.h            dashboard routes: JSON in, C engine, JSON out
│   └── serve.h             start the server, open a browser
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
│   ├── predict.c           rate, Poisson/exponential models, bootstrap,
│   │                       diagnostics, the predictor's own RNG
│   ├── json.c              JSON codec, in C
│   ├── http.c              sockets (Winsock / BSD), request parsing
│   ├── webapp.c            routes, Schedule <-> JSON, static files
│   ├── serve.c             server startup and browser launch
│   └── main.c              CLI parsing and the menu interface
├── web/                    the dashboard (no dependencies, no build step)
│   ├── index.html
│   ├── style.css           validated palette, light + dark
│   ├── app.js              presentation only -- no scheduling logic
│   └── favicon.svg
└── tests/
    ├── test_util.c/.h      tiny assertion framework + helpers
    ├── suites.h
    ├── test_validation.c   fields, parsing, container, generator
    ├── test_metrics.c      formulas + rejection of impossible timelines
    ├── test_algorithms.c   hand-calculated schedules for all six
    ├── test_csvio.c        import, export, round trip
    ├── test_web.c          JSON codec + the dashboard's HTTP routes
    ├── test_predict.c      the predictor and the two forecast routes
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
make test            # 2004 assertions across 6 suites
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
- **web** — the JSON reader (shapes, escapes, `\u`, integer strictness,
  and ten malformed inputs that must be rejected rather than guessed at),
  the writer (punctuation, nesting, escaping, round trip), and the dashboard
  routes driven without a socket: the same hand-worked SRTF and Round Robin
  timelines read back out of the JSON, eleven bad-request shapes that must
  come back as `400` with a useful message, the method guards, and the
  static-file path traversal defences.
- **predict** — λ = N/T and zero/invalid exposure; Poisson pmf, cdf and
  quantiles against hand-computed values, including μ = 0 and μ = 10⁶;
  exponential gaps (formula, refused inputs, seeded mean and median within
  stated tolerances); burst mean and sd; the binomial and χ² tails; the
  textbook forecast end to end; every invalid window, horizon, cap and seed;
  unique ids even when the workload already uses F1–F3; arrivals inside the
  horizon and resampled (burst, priority) pairs; bit-identical repeats per
  seed; over 2000 seeds the generated count matching Poisson(3) in mean,
  variance and P(K = 0); censoring by a cap and by the process limit; the
  stability checks on front-loaded, evenly spaced and paired arrivals; the
  merged workload under all six algorithms; and both routes, including
  hand-worked original-vs-augmented comparisons and twenty bad requests.

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

**The browser is a client, not a second implementation.** It would have been
easy to port the six algorithms to JavaScript and skip the server. That is
exactly what the project must not do: there would then be two schedulers to
keep in agreement, and the C one — the one with the tests — would be the one
nobody was looking at. Instead the page POSTs the workload and animates the
reply. `web/app.js` carries a comment at the top saying so, and the only
arithmetic in it is layout (time to percentage) and reading the C timeline to
ask "what is on the CPU at t?".

A useful consequence: the terminal UI and the dashboard cannot disagree.
`--example 2 --algo srtf` prints avg turnaround 7.00, 5 context switches and
2 preemptions, and the dashboard shows the same numbers because it is the same
`metrics.c` on the same timeline.

**Chart colour was computed, not chosen.** In a Gantt chart any two processes
can end up adjacent, so a categorical palette would have to survive the
all-pairs colour-blindness test — and only three hues do. Rather than cap the
dashboard at three processes, identity is carried by **position** (one
swimlane row per process) and by a **direct label on every cell**, and colour
carries CPU *state* instead: one blue hue at three lightness steps -- pending,
executed, on the CPU. Lightness differences survive every type of colour
vision, so the three states are always distinguishable, and no process is ever
identified by hue alone. The measured relative luminances step clearly rather
than scraping the bar (light `.459 / .153 / .041`, dark `.086 / .275 / .654`),
and the end nearest the page surface still clears the ordinal gate in both
themes (2.02:1 and 2.26:1).

**Green means exactly one thing.** Blue and green are the one pair this
palette has to be careful with, so green is used only for *finished* -- the
chips in the live panel and the done mark in the process table -- and never to
encode anything a reader must tell apart from a blue chart fill. Both keep
their check glyph, so the state is still readable with no colour vision at
all, and the chart ramp itself stays single-hue. Preemption markers and the
comparison table's winner mark stay **ink**, because both sit directly among
accent-coloured fills and numbers. Red survives in one place, `--critical`,
for invalid input and engine errors.
Idle is a neutral tone with a 45° hatch so it reads as "nothing ran" in print,
in forced-colors mode and for colour-blind readers.

**Motion is decoration over a page that is already correct.** Every keyframe
in the dashboard animates only `opacity`, `transform`, `clip-path` or a
background position, so nothing it does triggers layout and nothing ends up
anywhere the layout did not already put it. Nothing is made visible *by* an
animation either: entrance staggers use a `backwards` fill rather than an
`opacity: 0` base rule, which is what lets one `prefers-reduced-motion` block
switch the whole thing off and still leave the finished page intact. The two
loops that run continuously are tied to real state -- a highlight travels
along whichever bar currently holds the CPU, and the playhead only pulses
while the clock is actually running -- so motion on screen always means
something is happening.

The live panel rebuilds its chips only when the set they show changes, not on
every animation frame: a chip's entrance animation restarts whenever its
element is replaced, so rebuilding at 60 Hz would freeze every chip on its
first keyframe.

**Presentation is isolated.** Every escape sequence and every non-ASCII glyph
goes through `ui.c`, which picks a Unicode or ASCII glyph table and turns
colors on only when stdout is a terminal, `NO_COLOR` is unset, and (on
Windows) virtual-terminal processing could actually be enabled. The simulator
core prints nothing, so the exact same numbers come out whether the output is
a colored terminal or a redirected plain-text file.

---

## License

Released for educational use. No warranty.
