# Schedulify -- CPU Scheduling Simulator (C11)
#
#   make            build bin/schedulify
#   make run        build and start the interactive menu
#   make test       build and run the unit tests
#   make asan       rebuild everything with AddressSanitizer + UBSan
#   make demo       run a few non-interactive demos
#   make clean      remove build output
#   make help       this list
#
# Works with GCC and Clang on Linux, macOS and Windows (MinGW-w64).
# On Windows use `mingw32-make` if `make` is not on PATH.

# make predefines CC=cc, which `?=` would not override, so only replace the
# built-in default and leave an explicit CC from the environment alone.
ifeq ($(origin CC),default)
  CC := gcc
endif

CSTD    := -std=c11
WARN    := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion \
           -Wcast-qual -Wwrite-strings -Wstrict-prototypes \
           -Wmissing-prototypes -Wold-style-definition -Wredundant-decls \
           -Wundef -Wformat=2 -Wvla
OPT     ?= -O2
CFLAGS  ?= $(CSTD) $(WARN) $(OPT) -Iinclude -Isrc
LDFLAGS ?=
LDLIBS  ?= -lm

# ---- platform bits --------------------------------------------------------
# On Windows, GNU make uses sh.exe when one is on PATH (Git Bash, MSYS2) and
# cmd.exe otherwise, and the two need different mkdir/rm/exec syntax -- so
# branch on the shell make actually picked, not just on the OS.
ifeq ($(OS),Windows_NT)
  EXE := .exe
  ifeq ($(notdir $(SHELL)),cmd.exe)
    MKDIR = @if not exist "$(subst /,\,$1)" mkdir "$(subst /,\,$1)"
    RMDIR = @if exist "$(subst /,\,$1)" rmdir /s /q "$(subst /,\,$1)"
    EXEC  = $(subst /,\,$1)
  else
    MKDIR = @mkdir -p $1
    RMDIR = @rm -rf $1
    EXEC  = ./$1
  endif
else
  EXE   :=
  MKDIR = @mkdir -p $1
  RMDIR = @rm -rf $1
  EXEC  = ./$1
endif

BIN      := bin
OBJ      := build
APP      := $(BIN)/schedulify$(EXE)
TESTS    := $(BIN)/schedulify_tests$(EXE)

CORE_SRC := src/process.c src/validate.c src/ui.c src/table.c \
            src/algorithms.c src/sched_fcfs.c src/sched_sjf.c \
            src/sched_priority.c src/sched_rr.c src/metrics.c \
            src/visualize.c src/compare.c src/csvio.c src/datasets.c \
            src/stepmode.c
APP_SRC  := $(CORE_SRC) src/main.c
TEST_SRC := $(CORE_SRC) tests/test_util.c tests/test_validation.c \
            tests/test_metrics.c tests/test_algorithms.c tests/test_csvio.c \
            tests/test_main.c

APP_OBJ  := $(APP_SRC:%.c=$(OBJ)/%.o)
TEST_OBJ := $(TEST_SRC:%.c=$(OBJ)/%.o)
DEPS     := $(APP_OBJ:.o=.d) $(TEST_OBJ:.o=.d)

.PHONY: all run test asan analyze demo clean help

all: $(APP)

$(APP): $(APP_OBJ)
	$(call MKDIR,$(BIN))
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)
	@echo "built $@"

$(TESTS): $(TEST_OBJ)
	$(call MKDIR,$(BIN))
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)
	@echo "built $@"

$(OBJ)/%.o: %.c
	$(call MKDIR,$(dir $@))
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

run: $(APP)
	$(call EXEC,$(APP))

test: $(TESTS)
	$(call EXEC,$(TESTS))

# AddressSanitizer + UndefinedBehaviorSanitizer.  Works with GCC and Clang on
# Linux and macOS.  MinGW-w64 on Windows ships no sanitizer runtime (there is
# no libasan to link against), so this target checks for it first and tells
# you to use `make analyze` instead rather than failing with a link error.
SAN_FLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all \
             -fno-omit-frame-pointer -g

asan:
	@printf 'int main(void){return 0;}' > .asan_probe.c
	@if $(CC) $(SAN_FLAGS) .asan_probe.c -o .asan_probe$(EXE) >/dev/null 2>&1; \
	 then \
	   rm -f .asan_probe.c .asan_probe$(EXE); \
	   $(MAKE) --no-print-directory clean; \
	   $(MAKE) --no-print-directory test \
	     CFLAGS="$(CSTD) $(WARN) -O1 $(SAN_FLAGS) -Iinclude -Isrc" \
	     LDFLAGS="$(SAN_FLAGS)"; \
	 else \
	   rm -f .asan_probe.c .asan_probe$(EXE); \
	   echo "AddressSanitizer is not available with this toolchain"; \
	   echo "(MinGW-w64 ships no libasan). Options:"; \
	   echo "  make analyze    GCC static analyser, works everywhere"; \
	   echo "  make test       the normal unit tests"; \
	   echo "  run 'make asan' on Linux/macOS or inside WSL"; \
	 fi

# GCC's static analyser: finds leaks, double frees, use-after-free, NULL
# dereferences and out-of-bounds access at compile time.  This is the
# memory-safety check that is available on every platform, including MinGW.
analyze:
	@echo "running the GCC static analyser over src/ and tests/ ..."
	@for f in $(CORE_SRC) src/main.c $(filter tests/%,$(TEST_SRC)); do \
	  $(CC) $(CSTD) -Iinclude -Isrc -Itests -fanalyzer -O1 \
	        -c $$f -o /dev/null || exit 1; \
	done
	@echo "static analysis clean"

demo: $(APP)
	$(call EXEC,$(APP)) --example 2 --algo srtf --ascii --no-color --width 100
	$(call EXEC,$(APP)) --example 2 --compare --ascii --no-color --width 100
	$(call EXEC,$(APP)) --csv data/sample_processes.csv --algo rr --quantum 3 --ascii --no-color --width 100

clean:
	$(call RMDIR,$(OBJ))
	$(call RMDIR,$(BIN))
	@echo "cleaned"

help:
	@echo "Schedulify targets:"
	@echo "  make           build bin/schedulify"
	@echo "  make run       build and start the interactive menu"
	@echo "  make test      build and run the unit tests"
	@echo "  make asan      rebuild and run the tests under ASan/UBSan"
	@echo "  make analyze   run the GCC static analyser (all platforms)"
	@echo "  make demo      run three non-interactive demos"
	@echo "  make clean     remove build output"

-include $(DEPS)
