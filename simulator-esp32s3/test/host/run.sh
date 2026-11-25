#!/usr/bin/env bash
# =================================================================================================
#  run.sh - Host-side regression tests for the HIL simulator
#
#  Compiles the REAL src/sim_physics.cpp and src/sim_button.cpp natively against a small set of
#  stubs and exercises them the way the firmware exercises them. No ESP32 hardware needed.
#
#  Two bugs that a compile check cannot catch are covered here:
#    1. the hydraulic plant silently never advancing, because the elapsed-time accumulator was
#       advanced on every call instead of on every step;
#    2. the brew button honouring only the FIRST controller pulse, so the machine could be
#       started but never stopped.
#
#  A third test covers the controller's phase transition reason (include/profile_types.h), which
#  is the text the operator reads to find out why a phase ended.
#
#  A fourth test covers the post-shot weight tail (src/control/weight_settle.h): the timing rules
#  that decide when the yield in the cup is final.
#
#  A fifth test covers the rules that route a post-shot tail sample to the shot it belongs to
#  (src/control/shot_tail.h), so the stored curve ends at the yield the ledger records.
#
#  Usage:  ./test/host/run.sh
# =================================================================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIM="$(cd "$HERE/../.." && pwd)"
ROOT="$(cd "$SIM/.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

CXX="${CXX:-g++}"
FLAGS="-std=gnu++17 -Wall -I$HERE/stubs"

# Copy the real sources into the build dir so that their quoted #includes resolve against the
# stubs (sim_dimmer.h, sim_button.h, ...) instead of the real peripherals.
mkdir -p "$OUT/build"
cp "$SIM/src/sim_physics.cpp" "$SIM/src/sim_physics.h" "$OUT/build/"
cp "$SIM/src/sim_button.cpp"  "$SIM/src/sim_button.h"  "$OUT/build/"
cp "$SIM/include/sim_pins.h"                           "$OUT/build/"

echo "Building host tests from the real simulator sources..."
# sim_physics.cpp includes the controller's pump_physics.h, so it needs the controller's include/
# on the path (after the stubs and the build dir, so nothing there can shadow a stub).
"$CXX" $FLAGS -I"$OUT/build" -I"$ROOT/include" -o "$OUT/test_physics" "$HERE/test_physics.cpp" "$OUT/build/sim_physics.cpp"
"$CXX" $FLAGS -I"$OUT/build" -o "$OUT/test_button"  "$HERE/test_button.cpp"  "$OUT/build/sim_button.cpp"

# The transition reason lives in the controller's shared header, so it is compiled straight from
# the real source tree.
"$CXX" $FLAGS -I"$ROOT/include" -o "$OUT/test_transitions" "$HERE/test_transitions.cpp"

# The post-shot weight tail is header-only in the controller's source tree. The ledger dose rule
# lives in the controller's shared header next to it.
"$CXX" $FLAGS -I"$ROOT/include" -I"$ROOT/src/control" -o "$OUT/test_weight_settle" "$HERE/test_weight_settle.cpp"

# The integration test drives the real simulator physics into the real controller settling rules.
# The pump flow model and the scale flow rules (docs/PumpFlow.md) are header-only.
"$CXX" $FLAGS -I"$ROOT/include" -I"$ROOT/src/control" -o "$OUT/test_pump_flow" "$HERE/test_pump_flow.cpp"
"$CXX" $FLAGS -I"$OUT/build" -I"$ROOT/include" -I"$ROOT/src/control" -o "$OUT/test_settle_chain" \
    "$HERE/test_settle_chain.cpp" "$OUT/build/sim_physics.cpp"

# The rules that route a post-shot tail sample to the shot it belongs to are header-only in the
# controller's source tree, so they can be driven straight from the real source.
"$CXX" $FLAGS -I"$ROOT/src/control" -o "$OUT/test_shot_tail" "$HERE/test_shot_tail.cpp"

rc=0
"$OUT/test_physics"       || rc=1
"$OUT/test_button"        || rc=1
"$OUT/test_transitions"   || rc=1
"$OUT/test_weight_settle" || rc=1
"$OUT/test_settle_chain"  || rc=1
"$OUT/test_shot_tail"     || rc=1
"$OUT/test_pump_flow"     || rc=1

if [ $rc -eq 0 ]; then
    echo ""
    echo "Host regression tests PASSED"
else
    echo ""
    echo "Host regression tests FAILED"
fi
exit $rc
