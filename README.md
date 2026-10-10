# operatingsystems

Operating-systems coursework and projects.

## Projects

### [Schedulify](Schedulify/) — CPU Scheduling Simulator (C11 + web dashboard)

An interactive CPU scheduling simulator: a C11 engine with a visual browser
dashboard in front of it. Set arrival times, burst times and priorities, pick
one of six algorithms (FCFS, SJF, SRTF, non-preemptive and preemptive
Priority, Round Robin), and watch the schedule play out — an animated Gantt
chart, per-process swimlanes, a live ready queue, and the scheduler's own
reasoning replayed step by step.

Every schedule and metric is computed in C; the page only animates the
timeline it is sent. Also ships a full menu-driven terminal UI, a six-way
comparison mode, ten predefined examples, a reproducible random generator,
CSV import/export, a JSON HTTP API, Makefile and CMake builds, and 1162
unit-test assertions against hand-calculated schedules.

```sh
cd Schedulify
make web                              # Linux / macOS — opens the dashboard
```
```powershell
cd Schedulify
mingw32-make web                      # Windows (MinGW-w64)
```

That builds the engine, serves it on <http://127.0.0.1:8752/> and opens a
browser. Run the binary with no arguments for the terminal interface instead.

See [Schedulify/README.md](Schedulify/README.md) for the documented scheduling
assumptions, the metric definitions and the hand-worked derivations.
