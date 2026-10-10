# Quick start

From a fresh clone to a running pipeline. The [README](../README.md) explains everything here in more depth.

## 1. Check the machine

You need Linux on x86-64 (WSL2 is fine), kernel 5.14 or newer, and these tools:

```
cmake --version      # 3.25 or newer
ninja --version
g++ --version        # GCC 13, or Clang 18
```

On Ubuntu 24.04:

```
sudo apt install build-essential cmake ninja-build
```

GoogleTest is downloaded during the first configure if it is not installed, so that first build needs network access. To avoid the download, `sudo apt install libgtest-dev`.

## 2. Build

```
./build.sh build
```

This configures and compiles the `debug` preset into `build/debug/`. The first build takes a minute or two.

## 3. Test

```
./build.sh test
```

Expect 84 tests, all passing. One or two may report as skipped: the hardware-counter test skips where `perf_event_open` is not permitted, and the multicast test skips where loopback multicast is unavailable. Neither is a failure.

Run a subset by name:

```
./build.sh test -f 'Book*'
./build.sh test -f 'SimLoop*'
```

Run everything under the sanitizers:

```
./build.sh test -p asan
./build.sh test -p tsan
```

[TESTING.md](TESTING.md) describes each test.

## 4. Run the pipeline

```
./build.sh run
```

This builds the `release` preset, starts the four processes, waits ten seconds, and prints a summary. A healthy run ends like this:

```
venue_sim: itch_msgs=... client_orders=1035 client_fills=996 rejected=0 ...
feed_handler: ... dropped=0 gaps=0 lost=0 ... unknown_orders=0 ...
strategy: ... orders=1035 fills=996 rejects=0 refused=0 ...
order_gateway: requests=1035 sent=1035 risk_rejects=0 not_connected=0 backpressure=0 ... reports_dropped=0 session=down
tick-to-trade      n=1035 ...
feed-to-decision   n=1035 ...
decision-to-wire   n=1035 ...
  ring-hop         n=1035 ...
  gateway-send     n=1035 ...
```

Three checks tell you it worked:

1. `gaps`, `lost`, `dropped`, `unknown_orders`, `rejects`, `risk_rejects`, `backpressure` and `reports_dropped` are all 0.
2. The gateway's `requests` equals its `sent`, and both equal the venue's `client_orders`.
3. `fills` is at or slightly below `orders`.

`session=down` at the end is normal: the venue has shut down.

## 5. Get numbers worth reading

The default run is a smoke test. For latency figures, give each process its own core and let it busy-spin:

```
lscpu -e
./build.sh run --cpus 2,3,4,5 --idle-us 0 -s 60
```

| Part | Meaning |
|---|---|
| `--cpus 2,3,4,5` | Pin `feed_handler`, `strategy`, `order_gateway`, `venue_sim` to those cores, in that order |
| `--idle-us 0` | Never sleep when idle. Uses 100% of four cores for the whole run |
| `-s 60` | Run for 60 seconds, for enough orders to make the percentiles meaningful |

Use `lscpu -e` to choose four numbers with different `CORE` values, and leave core 0 alone. If neighbouring CPU numbers share a core on your machine, use `--cpus 2,4,6,8`.

For more samples, raise the venue's flow rate and the gateway's order-rate limit together:

```
./build.sh run --cpus 2,3,4,5 --idle-us 0 -s 60 -r 20000 --max-rate 5000
```

## 6. Run the benchmarks

```
./build.sh bench --cpus 2,3
```

The last one, `pipeline_bench`, runs the whole pipeline in one thread with no kernel in the path. Compare its tick-to-trade with the figure from step 5 to see how much of the latency is the code and how much is the operating system.

## If something goes wrong

| Symptom | Fix |
|---|---|
| `feed_handler is already running` | `./build.sh stop` |
| A previous run was interrupted and the next one misbehaves | `./build.sh stop`, which also removes `/dev/shm/hotpath-*` |
| `cannot pin to the requested cpu` | That CPU number does not exist or is not available to you. Check `nproc` and `lscpu -e` |
| `'ninja' is not installed` | `sudo apt install ninja-build` |
| The configure step cannot download GoogleTest | Install `libgtest-dev`, or build once with network access |
| `risk_rejects` is not 0 | The flow rate is high enough to hit the order-rate limit. Add `--max-rate 5000` |
| Latencies of 50 microseconds or more | The processes are sleeping. Use `--idle-us 0` |
| `hardware counters unavailable` in the benchmarks | `sudo sysctl kernel.perf_event_paranoid=1`. Timings are still valid without it |
| A process exited with an error | Read `build/release/logs/<process>.log` |
| A stale or confused build directory | `./build.sh distclean`, then build again |

## Command summary

```
./build.sh build                     compile (debug)
./build.sh build -p release          compile (release)
./build.sh test                      run all tests
./build.sh test -f 'Feed*'           run some tests
./build.sh test -p asan              run tests under sanitizers
./build.sh run                       ten-second pipeline run
./build.sh run -s 30                 thirty-second run
./build.sh run --cpus 2,3,4,5 --idle-us 0 -s 60
./build.sh bench --cpus 2,3          all benchmarks
./build.sh bench -f levels           one benchmark
./build.sh stop                      stop leftover processes
./build.sh clean                     remove build/debug
./build.sh distclean                 remove build/
./build.sh help                      full usage
```
