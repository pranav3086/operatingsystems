# Schedulify -- CPU Scheduling Simulator (C11)
#
#   cmake -S . -B build
#   cmake --build build
#   ctest --test-dir build --output-on-failure
#
# Options:
#   -DSCHEDULIFY_ASAN=ON   build with AddressSanitizer + UBSan (GCC/Clang)
#   -DSCHEDULIFY_WERROR=ON treat warnings as errors

cmake_minimum_required(VERSION 3.13)

project(Schedulify
        VERSION 1.0.0
        DESCRIPTION "CPU scheduling simulator written in C11"
        LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(SCHEDULIFY_ASAN   "Build with AddressSanitizer and UBSan" OFF)
option(SCHEDULIFY_WERROR "Treat warnings as errors"              OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---- the shared simulator core -------------------------------------------
add_library(schedulify_core STATIC
  src/process.c
  src/validate.c
  src/ui.c
  src/table.c
  src/algorithms.c
  src/sched_fcfs.c
  src/sched_sjf.c
  src/sched_priority.c
  src/sched_rr.c
  src/metrics.c
  src/visualize.c
  src/compare.c
  src/csvio.c
  src/datasets.c
  src/stepmode.c
  src/predict.c
  src/json.c
  src/http.c
  src/webapp.c
  src/serve.c)

target_include_directories(schedulify_core PUBLIC include src)

if(UNIX)
  find_library(M_LIBRARY m)
  if(M_LIBRARY)
    target_link_libraries(schedulify_core PUBLIC ${M_LIBRARY})
  endif()
endif()

# The dashboard needs sockets, and a way to ask the OS to open a browser.
if(WIN32)
  target_link_libraries(schedulify_core PUBLIC ws2_32 shell32)
endif()

# ---- strict warnings ------------------------------------------------------
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
  set(SCHEDULIFY_WARNINGS
      -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
      -Wcast-qual -Wwrite-strings -Wstrict-prototypes -Wmissing-prototypes
      -Wold-style-definition -Wredundant-decls -Wundef -Wformat=2 -Wvla)
  if(SCHEDULIFY_WERROR)
    list(APPEND SCHEDULIFY_WARNINGS -Werror)
  endif()
elseif(MSVC)
  set(SCHEDULIFY_WARNINGS /W4 /permissive-)
  if(SCHEDULIFY_WERROR)
    list(APPEND SCHEDULIFY_WARNINGS /WX)
  endif()
  target_compile_definitions(schedulify_core PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  set(SCHEDULIFY_WARNINGS "")
endif()

target_compile_options(schedulify_core PRIVATE ${SCHEDULIFY_WARNINGS})

# ---- sanitizers -----------------------------------------------------------
if(SCHEDULIFY_ASAN)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    if(MINGW)
      message(WARNING
        "MinGW-w64 does not ship the AddressSanitizer runtime; "
        "SCHEDULIFY_ASAN has no effect here. Use WSL or Linux/macOS.")
    else()
      set(SCHEDULIFY_SAN_FLAGS
          -fsanitize=address,undefined -fno-sanitize-recover=all
          -fno-omit-frame-pointer -g)
      target_compile_options(schedulify_core PUBLIC ${SCHEDULIFY_SAN_FLAGS})
      target_link_options(schedulify_core PUBLIC ${SCHEDULIFY_SAN_FLAGS})
    endif()
  else()
    message(WARNING "SCHEDULIFY_ASAN is only supported with GCC or Clang.")
  endif()
endif()

# ---- the application ------------------------------------------------------
add_executable(schedulify src/main.c)
target_link_libraries(schedulify PRIVATE schedulify_core)
target_compile_options(schedulify PRIVATE ${SCHEDULIFY_WARNINGS})

# ---- the tests ------------------------------------------------------------
enable_testing()

add_executable(schedulify_tests
  tests/test_util.c
  tests/test_validation.c
  tests/test_metrics.c
  tests/test_algorithms.c
  tests/test_csvio.c
  tests/test_web.c
  tests/test_predict.c
  tests/test_main.c)
target_link_libraries(schedulify_tests PRIVATE schedulify_core)
target_include_directories(schedulify_tests PRIVATE tests)
target_compile_options(schedulify_tests PRIVATE ${SCHEDULIFY_WARNINGS})

add_test(NAME unit_tests COMMAND schedulify_tests)

# A couple of smoke tests that drive the real binary non-interactively.
add_test(NAME cli_single_algorithm
         COMMAND schedulify --example 2 --algo srtf --no-color --ascii
                 --width 100)
add_test(NAME cli_compare_all
         COMMAND schedulify --example 9 --compare --quantum 3 --no-color
                 --ascii --width 100)
add_test(NAME cli_csv_import
         COMMAND schedulify --csv
                 ${CMAKE_CURRENT_SOURCE_DIR}/data/sample_processes.csv
                 --compare --no-color --ascii --width 100)
add_test(NAME cli_help COMMAND schedulify --help)

# The dashboard routes are exercised by the unit tests (suite "web"),
# which call webapp_route() directly -- no socket, no browser.

install(TARGETS schedulify RUNTIME DESTINATION bin)
