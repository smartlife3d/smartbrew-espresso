#!/usr/bin/env bash
# Builds and runs the pressure-loop plant check (see pid_plant_sim.cpp). No hardware needed.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT
"${CXX:-c++}" -std=gnu++17 -O1 -I"$HERE/stubs" -I"$ROOT/include" -I"$ROOT/src" \
    -include "$ROOT/include/pins.h" \
    "$HERE/pid_plant_sim.cpp" "$ROOT/src/control/pid_controller.cpp" -o "$OUT/pid_plant_sim"
"$OUT/pid_plant_sim"
