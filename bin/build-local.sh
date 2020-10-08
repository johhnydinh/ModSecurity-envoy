#!/usr/bin/env bash
set -e

nb_cpu=$(python -c 'import multiprocessing as mp; print(mp.cpu_count())')
nb_cpu=$((${nb_cpu}+1))

bazel build --jobs=${nb_cpu} -c opt --copt="-Wno-uninitialized" --copt="-Wno-range-loop-analysis"  --cxxopt="-Wno-uninitialized" //:envoy-static.stripped --config=sizeopt