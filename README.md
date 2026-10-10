# operatingsystems

Operating-systems coursework and projects.

## Projects

### [Schedulify](Schedulify/) — CPU Scheduling Simulator (C11)

A complete CPU scheduling simulator written entirely in C11. Implements FCFS,
SJF, SRTF, non-preemptive and preemptive Priority, and Round Robin; renders a
to-scale text Gantt chart and a detailed process table; and reports completion,
turnaround, waiting and response times, CPU utilization, throughput, idle time,
context switches and preemptions.

Menu-driven terminal UI with colors and a plain-text fallback, a six-way
comparison mode, a step-by-step educational mode, ten predefined examples, a
reproducible random generator, CSV import/export, a Makefile and CMake build,
and 950 unit-test assertions against hand-calculated schedules.

```sh
cd Schedulify
make && ./bin/schedulify          # Linux / macOS
```
```powershell
cd Schedulify
mingw32-make; .\bin\schedulify.exe   # Windows (MinGW-w64)
```

See [Schedulify/README.md](Schedulify/README.md) for the documented scheduling
assumptions, the metric definitions and the hand-worked derivations.
