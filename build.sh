#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

PRESETS="debug release asan tsan"
APPS="feed_handler venue_sim order_gateway strategy"
BENCHES="ring_bench wire_bench levels_bench book_bench pipeline_bench"

usage() {
    cat <<'TEXT'
hotpath build script

Usage: ./build.sh <command> [options]

Commands:
  build      Configure and compile
  test       Build, then run the test suite
  run        Build, then run the four-process pipeline against the simulated venue
  bench      Build, then run the benchmarks
  stop       Stop pipeline processes left running
  clean      Remove build output for one preset
  distclean  Remove the whole build directory
  rebuild    clean, then build
  help       Show this text

Options:
  -p, --preset NAME    debug | release | asan | tsan
                       (default: debug for build and test, release for run and bench)
  -f, --filter EXPR    test: GoogleTest filter, e.g. 'Book*' or 'SimLoop*'
                       bench: run only the benchmarks whose name contains EXPR
  -j, --jobs N         build: parallel jobs (default: all cores)
  -s, --seconds N      run: how long to run (default: 10)
  -r, --rate N         run: simulated order-flow actions per second (default: 2000)
      --max-rate N     run: gateway risk limit in orders per second (default: 1000)
      --symbols LIST   run: comma-separated tickers (default: MSFT,NVDA,AAPL,AMZN)
      --cpus A,B,C,D   run: pin feed_handler, strategy, order_gateway, venue_sim
                       bench: pin the measuring thread to A and the second thread to B
      --idle-us N      run: sleep N microseconds when a process has no work;
                       0 busy-spins (default: 0 with 6+ cores, otherwise 20)
      --hugepages      run: try 2M huge pages (default: 4K pages)
      --mlock          run: lock process memory

Examples:
  ./build.sh build
  ./build.sh test -f 'Feed*'
  ./build.sh test -p tsan
  ./build.sh run -s 30
  ./build.sh run --cpus 2,3,4,5 --idle-us 0
  ./build.sh run --cpus 2,3,4,5 --idle-us 0 -s 60 -r 20000 --max-rate 5000
  ./build.sh bench --cpus 2,3
  ./build.sh bench -f levels --cpus 2
TEXT
}

die() {
    echo "build.sh: $*" >&2
    exit 1
}

need() {
    command -v "$1" >/dev/null 2>&1 || die "'$1' is not installed"
}

COMMAND="${1:-help}"
[ $# -gt 0 ] && shift

PRESET=""
FILTER=""
JOBS=""
SECONDS_TO_RUN=10
RATE=2000
MAX_RATE=""
SYMBOLS=""
CPUS=""
IDLE_US=""
HUGEPAGES=0
MLOCK=0

while [ $# -gt 0 ]; do
    case "$1" in
        -p|--preset)  PRESET="${2:?--preset needs a value}"; shift 2 ;;
        -f|--filter)  FILTER="${2:?--filter needs a value}"; shift 2 ;;
        -j|--jobs)    JOBS="${2:?--jobs needs a value}"; shift 2 ;;
        -s|--seconds) SECONDS_TO_RUN="${2:?--seconds needs a value}"; shift 2 ;;
        -r|--rate)    RATE="${2:?--rate needs a value}"; shift 2 ;;
        --max-rate)   MAX_RATE="${2:?--max-rate needs a value}"; shift 2 ;;
        --symbols)    SYMBOLS="${2:?--symbols needs a value}"; shift 2 ;;
        --cpus)       CPUS="${2:?--cpus needs a value}"; shift 2 ;;
        --idle-us)    IDLE_US="${2:?--idle-us needs a value}"; shift 2 ;;
        --hugepages)  HUGEPAGES=1; shift ;;
        --mlock)      MLOCK=1; shift ;;
        -h|--help)    usage; exit 0 ;;
        *)            die "unknown option '$1' (try ./build.sh help)" ;;
    esac
done

if [ -z "$PRESET" ]; then
    case "$COMMAND" in
        run|bench) PRESET="release" ;;
        *)         PRESET="debug" ;;
    esac
fi
case " $PRESETS " in
    *" $PRESET "*) ;;
    *) die "unknown preset '$PRESET' (choose from: $PRESETS)" ;;
esac

BUILD_DIR="$ROOT/build/$PRESET"
BIN_DIR="$BUILD_DIR/bin"
LOG_DIR="$BUILD_DIR/logs"

do_build() {
    need cmake
    need ninja
    if [ ! -f "$BUILD_DIR/build.ninja" ]; then
        cmake --preset "$PRESET"
    fi
    if [ -n "$JOBS" ]; then
        cmake --build --preset "$PRESET" --parallel "$JOBS"
    else
        cmake --build --preset "$PRESET"
    fi
}

do_test() {
    do_build
    if [ -n "$FILTER" ]; then
        "$BUILD_DIR/tests/hotpath_unit_tests" --gtest_filter="$FILTER"
    else
        ctest --preset "$PRESET"
    fi
}

do_stop() {
    local stopped=0
    for app in $APPS; do
        if pkill -TERM -x "$app" 2>/dev/null; then
            stopped=1
        fi
    done
    rm -f /dev/shm/hotpath-market /dev/shm/hotpath-requests /dev/shm/hotpath-reports
    if [ "$stopped" -eq 1 ]; then echo "stopped running pipeline processes"; fi
}

PIDS=()

stop_children() {
    for pid in "${PIDS[@]:-}"; do
        [ -n "$pid" ] && kill -TERM "$pid" 2>/dev/null || true
    done
}

launch() {
    local app="$1" cpu="$2"
    shift 2
    local args=("--seconds=$SECONDS_TO_RUN" "--stats-sec=5" "--idle-us=$IDLE_US" "$@")
    [ -n "$cpu" ] && args+=("--cpu=$cpu")
    [ "$MLOCK" -eq 1 ] && args+=("--mlock")
    "$BIN_DIR/$app" "${args[@]}" >"$LOG_DIR/$app.log" 2>&1 &
    PIDS+=("$!")
}

do_run() {
    do_build
    for app in $APPS; do
        [ -x "$BIN_DIR/$app" ] || die "missing $BIN_DIR/$app"
        if pgrep -x "$app" >/dev/null 2>&1; then
            die "$app is already running (use ./build.sh stop)"
        fi
    done

    if [ -z "$IDLE_US" ]; then
        if [ "$(nproc)" -ge 6 ]; then IDLE_US=0; else IDLE_US=20; fi
    fi
    local cpu_feed="" cpu_strategy="" cpu_gateway="" cpu_venue=""
    if [ -n "$CPUS" ]; then
        IFS=',' read -r cpu_feed cpu_strategy cpu_gateway cpu_venue <<<"$CPUS"
        [ -n "$cpu_venue" ] || die "--cpus needs four comma-separated cpu numbers"
    fi

    local symbol_args=()
    [ -n "$SYMBOLS" ] && symbol_args+=("--symbols=$SYMBOLS")
    local page_args=()
    [ "$HUGEPAGES" -eq 0 ] && page_args+=("--no-hugepages")
    local gateway_args=()
    [ -n "$MAX_RATE" ] && gateway_args+=("--max-rate=$MAX_RATE")

    mkdir -p "$LOG_DIR"
    rm -f "$LOG_DIR"/*.log
    rm -f /dev/shm/hotpath-market /dev/shm/hotpath-requests /dev/shm/hotpath-reports
    trap 'stop_children' INT TERM

    echo "running the pipeline for ${SECONDS_TO_RUN}s (preset $PRESET, idle ${IDLE_US}us, logs in ${LOG_DIR#"$ROOT"/})"
    launch feed_handler "$cpu_feed" "${symbol_args[@]}" "${page_args[@]}"
    sleep 0.3
    launch venue_sim "$cpu_venue" "--rate=$RATE" "${symbol_args[@]}" "${page_args[@]}"
    launch order_gateway "$cpu_gateway" "${symbol_args[@]}" "${gateway_args[@]}"
    launch strategy "$cpu_strategy" "${symbol_args[@]}" "${page_args[@]}"

    local failed=0
    for pid in "${PIDS[@]}"; do
        wait "$pid" || failed=1
    done
    trap - INT TERM

    echo
    for app in venue_sim feed_handler strategy; do
        tail -n 1 "$LOG_DIR/$app.log" 2>/dev/null || true
    done
    grep -E '^(order_gateway: requests|tick-to-trade|feed-to-decision|decision-to-wire|  ring-hop|  gateway-send)' \
        "$LOG_DIR/order_gateway.log" | tail -n 6 || true
    rm -f /dev/shm/hotpath-market /dev/shm/hotpath-requests /dev/shm/hotpath-reports
    if [ "$failed" -ne 0 ]; then
        die "a process exited with an error; see ${LOG_DIR#"$ROOT"/}"
    fi
}

do_bench() {
    do_build
    local cpu_a="" cpu_b="" rest=""
    if [ -n "$CPUS" ]; then
        IFS=',' read -r cpu_a cpu_b rest <<<"$CPUS"
    fi
    local args=()
    [ -n "$cpu_a" ] && args+=("--cpu=$cpu_a")
    [ -n "$cpu_b" ] && args+=("--cpu2=$cpu_b")

    local ran=0
    for bench in $BENCHES; do
        case "$bench" in
            *"$FILTER"*) ;;
            *) continue ;;
        esac
        [ -x "$BUILD_DIR/bench/$bench" ] || die "missing $BUILD_DIR/bench/$bench"
        "$BUILD_DIR/bench/$bench" "${args[@]}"
        echo
        ran=1
    done
    [ "$ran" -eq 1 ] || die "no benchmark matches '$FILTER' (choose from: $BENCHES)"
}

do_clean() {
    rm -rf "$BUILD_DIR"
    echo "removed build/$PRESET"
}

case "$COMMAND" in
    build)     do_build ;;
    test)      do_test ;;
    run)       do_run ;;
    bench)     do_bench ;;
    stop)      do_stop ;;
    clean)     do_clean ;;
    distclean) rm -rf "$ROOT/build"; echo "removed build/" ;;
    rebuild)   do_clean; do_build ;;
    help|-h|--help) usage ;;
    *)         die "unknown command '$COMMAND' (try ./build.sh help)" ;;
esac
