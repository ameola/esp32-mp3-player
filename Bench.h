#pragma once
// Step-0 benchmarks for the library plan. Build with:
//   arduino-cli compile --fqbn esp32:esp32:esp32 \
//     --build-property compiler.cpp.extra_flags=-DRUN_BENCH=N .
// N=1: step 0 (SD clocks, listing, tag timing). N=2: step 1 (tag reader).
// N=3: step 2 (library format: build, verify, crash and corruption tests).
// N=4: step 3 (indexer: full, unchanged, add/delete/retag, crash + resume).
// Results go to Serial (115200). Test files are written under /_bench.

void runBench();
