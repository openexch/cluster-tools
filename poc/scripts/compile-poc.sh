#!/bin/bash
# Runs INSIDE the builder container. Fast iteration: only compiles poc.cpp.
set -euo pipefail
g++ -O2 -std=c++17 -Wall -o /spike/dist/poc /spike/poc.cpp \
    -I /aeron/aeron-client/src/main/cpp_wrapper \
    -I /aeron/aeron-client/src/main/c \
    -I /spike/gen \
    /spike/dist/libaeron_static.a \
    -lpthread -ldl -lm
echo "=== poc compiled ==="
file /spike/dist/poc
