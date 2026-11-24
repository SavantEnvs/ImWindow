#!/usr/bin/env bash
#
# mayhem/build.sh — build this repo's fuzz harness(es). EDIT per repo.
#
# Runs inside the commit image (mayhem/Dockerfile) as `mayhem` in /mayhem. The base image
# (ghcr.io/savantenvs/base) already exports the build contract — use these, don't redefine:
#   CC, CXX             stock clang / clang++
#   LIB_FUZZING_ENGINE  -fsanitize=fuzzer   (link into each harness that has a LLVMFuzzer entry)
#   SANITIZER_FLAGS     -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
#                       (ASan + UBSan, both set to HALT — so Mayhem catches memory AND UB defects)
#   DEBUG_FLAGS         -g -gdwarf-3   (DWARF debug info — always on for fuzz/standalone builds,
#                       independent of the sanitizer off-switch; DWARF version must be < 4)
#   RUST_DEBUG_FLAGS    -C debuginfo=2 -C force-frame-pointers=yes -C llvm-args=-gdwarf-3
#                       (thread through RUSTFLAGS on every cargo-fuzz build)
#   GO_DEBUG_FLAGS      -gcflags=all=-N -l
#                       (thread through go build / go-fuzz-build so the linked ELF keeps symbols)
#   SRC                 /mayhem (the repo source)
#
# Contract: build EVERYTHING here — one runnable binary per fuzz harness, AND the project's test
# suite (so mayhem/test.sh only has to RUN it, never compile). Keep it ADDITIVE (build upstream as
# upstream documents; don't edit upstream files). IMPORTANT: build the PROJECT ITSELF with
# $SANITIZER_FLAGS and $DEBUG_FLAGS (not just the harness) so the fuzzed code is instrumented
# AND carries DWARF < 4 symbols — otherwise ASan/UBSan only see the harness, not the library
# you're trying to find bugs in, and backtraces won't resolve project source lines. Build the TEST
# suite with the project's NORMAL flags (a clean, independent build) so test.sh stays an honest
# functional oracle and won't false-fail on benign UB. Leave the test binary/runner where test.sh
# expects it.
set -euo pipefail

# clang rejects SOURCE_DATE_EPOCH='' (empty) — it must be unset or a valid integer.
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

# Build knobs come from the ENVIRONMENT (overridable), with sane defaults — no if-statements,
# just parameter-expansion fallbacks. Default sanitizers are the base's ASan+UBSan (halting); override
# per build via the Dockerfile's `--build-arg SANITIZER_FLAGS="..."`.
# NB: SANITIZER_FLAGS uses `=` (no colon) on purpose — `=` only fills when the var is UNSET, so an
# explicit EMPTY value (`--build-arg SANITIZER_FLAGS=`) is honored and builds with NO sanitizers
# (useful when you want the program's natural crash / full backtrace, not an ASan report). The other
# knobs use `:=` (default on empty too). MAYHEM_JOBS sets build parallelism (falls back to nproc).
: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
# DEBUG_FLAGS carries DWARF debug info INDEPENDENTLY of the sanitizer off-switch (so an empty
# SANITIZER_FLAGS still yields DWARF symbols). DWARF MUST be < 4 (Mayhem triage can't read >=4); clang-19's
# plain `-g` emits DWARF-5, so `-gdwarf-3` is explicit. Apply $DEBUG_FLAGS to the fuzz/harness/standalone
# builds (NOT the test/oracle build). Rust/Go carry the same intent via their language flags below.
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${RUST_DEBUG_FLAGS:=-C debuginfo=2 -C force-frame-pointers=yes -C llvm-args=-gdwarf-3}"
: "${GO_DEBUG_FLAGS:=-gcflags=all=-N -l}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
# COVERAGE_FLAGS: empty by default → no effect on the normal oracle build. Set it via the Dockerfile's
# `--build-arg COVERAGE_FLAGS="-fprofile-instr-generate -fcoverage-mapping"` to instrument the TEST
# build for source-coverage measurement (how much of the project the test suite actually exercises —
# a quality signal for the oracle; complements the anti-reward-hack sabotage check). APPEND it to the
# test build's compile+link flags in step 3 (NOT the fuzz build); after `test.sh` runs, merge with
# `llvm-profdata` and report with `llvm-cov`. Empty value is honored (`=`, not `:=`).
: "${COVERAGE_FLAGS=}"
export SANITIZER_FLAGS DEBUG_FLAGS RUST_DEBUG_FLAGS GO_DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"

# Upstream builds with GENie (Windows-oriented project files); there is no Linux build to reuse, so the
# library sources are compiled directly. Dependencies come from /opt/toolchains/deps (mayhem/Dockerfile),
# pinned to the commits upstream's submodule gitlinks record:
#   imgui-1.53                 Dear ImGui, prebuilt uninstrumented (lib/libimgui.a, headers in src/)
#   utilscollection-863ab31    JsonStthm, the layout JSON parser: compiled here with the fuzz flags
IMGUI=/opt/toolchains/deps/imgui-1.53
JSON=/opt/toolchains/deps/utilscollection-863ab31/src/JsonStthm
B="$SRC/mayhem/build"   # gitignored by mayhem/.gitignore
mkdir -p "$B"
# JsonStthm is MSVC-oriented and does not build on Linux/LP64 as shipped:
#  - it calls sprintf_s (same argument order as snprintf) -> -Dsprintf_s=snprintf;
#  - ReadNumericValue assigns a `long long` to JsonValue, whose operator= overloads (int64_t = long,
#    double, bool, ...) make that ambiguous on LP64. Its alternative STTHM_USE_CUSTOM_NUMERIC_PARSER path
#    does not compile either (unqualified IsDigit). So a copy is generated here with that one assignment
#    cast to int64_t (what MSVC, where int64_t is long long, already does);
#  - it writes floats with "%.17Lg" but passes a double: fine on MSVC (long double == double), garbage
#    on x86-64 Linux, so the copy uses "%.17g" (same output MSVC produces).
#  The #line keeps backtraces pointing at the pristine dependency source (line numbers are unchanged).
{ printf '#line 1 "%s"\n' "$JSON/JsonStthm.cpp"
  sed -e 's/oValue = iValue;/oValue = (int64_t)iValue;/' -e 's/"%\.17Lg"/"%.17g"/' "$JSON/JsonStthm.cpp"; } > "$B/JsonStthm.cpp.tmp"
[ "$(grep -c 'oValue = (int64_t)iValue;' "$B/JsonStthm.cpp.tmp")" = 1 ] || { echo "JsonStthm LP64 fix did not apply" >&2; exit 1; }
[ "$(grep -c '"%\.17g"' "$B/JsonStthm.cpp.tmp")" = 1 ] || { echo "JsonStthm %Lg fix did not apply" >&2; exit 1; }
mv "$B/JsonStthm.cpp.tmp" "$B/JsonStthm.cpp"
DEFS="-Dsprintf_s=snprintf"
INCS="-I$SRC -I$SRC/ImWindow -I$IMGUI/src -I$JSON"
LIBSRC=("$SRC"/ImWindow/*.cpp "$B/JsonStthm.cpp")

# build_objs <outdir> <flags...>: compile the ImWindow sources + JsonStthm into <outdir>/*.o
build_objs() {
  local out="$1"; shift
  mkdir -p "$out"
  local pids=() f
  for f in "${LIBSRC[@]}"; do
    $CXX -std=c++14 "$@" $DEFS $INCS -c "$f" -o "$out/$(basename "${f%.cpp}").o" &
    pids+=($!)
    if [ "${#pids[@]}" -ge "$MAYHEM_JOBS" ]; then wait "${pids[0]}"; pids=("${pids[@]:1}"); fi
  done
  for f in "${pids[@]}"; do wait "$f"; done
}

# 1) The project (ImWindow + JsonStthm), sanitized + coverage-instrumented, DWARF 3.
FUZZ_CFLAGS="-O1 $SANITIZER_FLAGS $DEBUG_FLAGS -fsanitize=fuzzer-no-link"
build_objs "$B/fuzz" $FUZZ_CFLAGS

# 2) Harness: libFuzzer binary + standalone reproducer. LeakSanitizer is turned off at build time
#    (mayhem/lsan_off.cc); ASan and UBSan stay on and halting.
$CXX $SANITIZER_FLAGS $DEBUG_FLAGS -c "$SRC/mayhem/lsan_off.cc" -o "$B/lsan_off.o"
$CXX -std=c++14 $FUZZ_CFLAGS $DEFS $INCS -c "$SRC/mayhem/fuzz_layout.cpp" -o "$B/fuzz_layout.o"
$CXX $SANITIZER_FLAGS $DEBUG_FLAGS $LIB_FUZZING_ENGINE "$B/fuzz_layout.o" "$B"/fuzz/*.o "$B/lsan_off.o" \
  "$IMGUI/lib/libimgui.a" -o "$SRC/fuzz_layout"
$CC $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o "$B/standalone_main.o"
$CXX $SANITIZER_FLAGS $DEBUG_FLAGS "$B/standalone_main.o" "$B/fuzz_layout.o" "$B"/fuzz/*.o "$B/lsan_off.o" \
  "$IMGUI/lib/libimgui.a" -o "$SRC/fuzz_layout-standalone"

# 3) Functional oracle (mayhem/layout_selftest.cpp) with normal flags, no sanitizers.
build_objs "$B/test" -O1 $COVERAGE_FLAGS
$CXX -std=c++14 -O1 $COVERAGE_FLAGS $DEFS $INCS "$SRC/mayhem/layout_selftest.cpp" "$B"/test/*.o \
  "$IMGUI/lib/libimgui.a" -o "$B/layout_selftest"

echo "built: $SRC/fuzz_layout $SRC/fuzz_layout-standalone $B/layout_selftest"
