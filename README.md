# hotpath

A zero-allocation tick-to-trade pipeline in C++20: ITCH in, OUCH out, shared-memory IPC between pinned processes, measured to the nanosecond.

`hotpath` is the buy side of a trading system, built to show the mechanics that matter in low-latency work rather than to trade. Market data arrives as Nasdaq ITCH 5.0 over MoldUDP64, a book is built, a strategy decides, and an order leaves as OUCH 4.2 over SoupBinTCP. Three processes do that work, each pinned to its own core and joined by lock-free rings in shared memory. A fourth process is a simulated exchange so the whole loop runs on one machine.

- [Quick start](docs/QUICKSTART.md): build, test and run in five minutes
- [Testing](docs/TESTING.md): what each of the 84 tests proves

## Contents

1. [What it does](#what-it-does)
2. [Requirements](#requirements)
3. [Build, test, run](#build-test-run)
4. [Running the pipeline](#running-the-pipeline)
5. [Reading the output](#reading-the-output)
6. [Benchmarks](#benchmarks)
7. [Running the processes by hand](#running-the-processes-by-hand)
8. [Repository layout](#repository-layout)
9. [Design](#design)
10. [Coding rules](#coding-rules)
11. [Preparing a machine for real numbers](#preparing-a-machine-for-real-numbers)
12. [Known limitations](#known-limitations)
13. [Protocol references](#protocol-references)

## What it does

```
                 MoldUDP64 / ITCH 5.0                         SoupBinTCP / OUCH 4.2
  +-----------+  UDP 127.0.0.1:31001   +--------------+      TCP 127.0.0.1:31003   +-----------+
  | venue_sim | ---------------------> | feed_handler |                            | venue_sim |
  +-----------+  retransmit :31002     +--------------+                            +-----------+
                                              |                                          ^
                                 /hotpath-market (SPSC ring)                             |
                                              v                                          |
                                        +----------+   /hotpath-requests   +---------------+
                                        | strategy | --------------------> | order_gateway |
                                        +----------+ <-------------------- +---------------+
                                                       /hotpath-reports
```

| Process | Reads | Does | Writes |
|---|---|---|---|
| `feed_handler` | MoldUDP64 packets | Sequences packets, recovers gaps, decodes ITCH, builds books | Top-of-book updates to `/hotpath-market` |
| `strategy` | `/hotpath-market`, `/hotpath-reports` | Tracks its orders and position, runs an imbalance taker | Order requests to `/hotpath-requests` |
| `order_gateway` | `/hotpath-requests`, the TCP session | Risk checks, encodes OUCH, runs the SoupBinTCP session, measures latency | OUCH to the venue, execution reports to `/hotpath-reports` |
| `venue_sim` | OUCH from the gateway | Price-time matching engine with synthetic background order flow | ITCH market data, OUCH replies |

The three `/hotpath-*` names are POSIX shared-memory objects, visible under `/dev/shm` while the pipeline runs. Each holds one single-producer single-consumer ring of 64-byte messages.

Every market data packet is stamped with the CPU timestamp counter when the feed handler receives it. That stamp travels with the data through both rings, and the gateway reads the counter again when the order has been handed to the socket. The difference is the tick-to-trade latency.

## Requirements

| Need | Detail |
|---|---|
| Operating system | Linux on x86-64. Kernel 5.14 or newer (the shared memory is pre-faulted with `MADV_POPULATE_WRITE`) |
| CPU | Invariant TSC. Six or more cores if you want every process to busy-spin on its own core |
| Compiler | C++20. Built and tested with GCC 13 and Clang 18 |
| Build tools | CMake 3.25 or newer, Ninja |
| GoogleTest | Found on the system if installed (1.14 or newer); otherwise fetched from GitHub at configure time |

WSL2 works. The numbers there carry more jitter than bare metal because the cores cannot be isolated from the Windows host.

## Build, test, run

Everything goes through `build.sh`.

```
./build.sh build
./build.sh test
./build.sh run
./build.sh bench
./build.sh help
```

### Commands

| Command | What it does |
|---|---|
| `build` | Configures the preset if needed, then compiles |
| `test` | Builds, then runs the test suite |
| `run` | Builds, then runs the four-process pipeline and prints a summary |
| `bench` | Builds, then runs the benchmarks |
| `stop` | Stops pipeline processes left running and removes the shared-memory objects |
| `clean` | Removes build output for one preset |
| `distclean` | Removes the whole `build/` directory |
| `rebuild` | `clean`, then `build` |
| `help` | Prints usage |

### Options

| Option | Applies to | Meaning | Default |
|---|---|---|---|
| `-p`, `--preset NAME` | all | `debug`, `release`, `asan` or `tsan` | `debug` for build and test, `release` for run and bench |
| `-f`, `--filter EXPR` | test, bench | test: a GoogleTest filter such as `'Book*'`. bench: run only benchmarks whose name contains `EXPR` | everything |
| `-j`, `--jobs N` | build | Parallel compile jobs | all cores |
| `-s`, `--seconds N` | run | How long the pipeline runs | 10 |
| `-r`, `--rate N` | run | Simulated order-flow actions per second at the venue | 2000 |
| `--max-rate N` | run | Gateway risk limit, orders per second | 1000 |
| `--symbols LIST` | run | Comma-separated tickers | `MSFT,NVDA,AAPL,AMZN` |
| `--cpus A,B,C,D` | run, bench | run: cores for `feed_handler`, `strategy`, `order_gateway`, `venue_sim`, in that order. bench: `A` is the measuring thread, `B` the second thread | not pinned |
| `--idle-us N` | run | Microseconds to sleep when a process has no work. `0` busy-spins | 0 with six or more cores, otherwise 20 |
| `--hugepages` | run | Try 2 MB huge pages for process-private memory | 4 KB pages |
| `--mlock` | run | Lock process memory so it cannot be paged out | off |

### Presets

| Preset | Build type | Use it for |
|---|---|---|
| `debug` | Unoptimised, all assertions on | Day-to-day development and tests |
| `release` | `-O3 -DNDEBUG -march=native` | Anything you measure |
| `asan` | Debug with AddressSanitizer and UndefinedBehaviorSanitizer | Memory and undefined-behaviour bugs |
| `tsan` | RelWithDebInfo with ThreadSanitizer | Data races in the ring and the multi-threaded tests |

Each preset builds into `build/<preset>/`. Executables land in `build/<preset>/bin/`, benchmarks in `build/<preset>/bench/`, and pipeline logs in `build/<preset>/logs/`.

## Running the pipeline

`./build.sh run` starts `feed_handler`, then `venue_sim`, `order_gateway` and `strategy`. Each process is told how long to run, exits by itself, and writes to its own log file. When all four have finished the script prints the last statistics line from each and the gateway's latency table.

### A measured run, option by option

```
./build.sh run --cpus 2,3,4,5 --idle-us 0 -s 60 -r 10000
```

**`run`** builds the `release` preset if anything changed, then launches the four processes.

**`--cpus 2,3,4,5`** pins one process to each core. The order is fixed:

| Position | Core here | Process |
|---|---|---|
| 1st | 2 | `feed_handler` |
| 2nd | 3 | `strategy` |
| 3rd | 4 | `order_gateway` |
| 4th | 5 | `venue_sim` |

Pinning stops the scheduler from moving a process between cores, which would throw away its warm caches and add tens of microseconds. Three things are worth knowing when you choose the numbers:

- Pinning is not isolation. Other programs and kernel work can still run on those cores. Each process prints `pinned to cpu N (isolated)` or `(not isolated)` at the top of its log so you can tell which you have. See [Preparing a machine for real numbers](#preparing-a-machine-for-real-numbers).
- Avoid core 0. The kernel sends most interrupts and housekeeping there.
- Use four different physical cores. With hyper-threading, two logical CPU numbers can share one physical core, and two busy-spinning processes on the same core slow each other badly. `lscpu -e` shows the mapping: pick four rows with different values in the `CORE` column. On many machines neighbouring numbers are siblings, in which case `--cpus 2,4,6,8` is the right shape.

Without `--cpus` the processes float, and the tail latencies show it.

**`--idle-us 0`** makes every process busy-spin: when a ring or socket has nothing to read, the process asks again immediately instead of sleeping. That is how production low-latency systems run, and it is the only setting that measures the code rather than the scheduler. A sleeping process takes 50 microseconds or more to wake, which swamps a pipeline that takes single-digit microseconds. The cost is that all four processes use 100% of their cores for the whole run. With `--idle-us 20`, a process sleeps 20 microseconds whenever it finds no work, which is kind to a laptop and useless for latency figures. The default is `0` on machines with six or more cores and `20` otherwise.

**`-s 60`** runs for 60 seconds. Latency percentiles need samples: at the default flow rate a 10-second run produces about a thousand orders, so its p99.9 is a single order. A longer run is the cheapest way to make the tail meaningful.

**`-r 10000`** sets how much background order flow the simulated venue generates, in actions per second. An action is one step of the synthetic flow: a new resting order, a cancel, an aggressive order that trades, or a move of the reference price. At the default of 2000, expect roughly 2,000 ITCH messages and 100 strategy orders per second. Both scale close to linearly with the rate, so `-r 10000` gives about 500 orders per second and 30,000 latency samples in a minute.

**`--max-rate N`** matters once the rate goes up. The gateway's risk gate rejects orders beyond 1,000 per second by default, which the strategy reaches at a flow rate of roughly 20,000. Rejections appear as `risk_rejects` in the gateway line and `rejects` in the strategy line. That is the gate doing its job, but rejected orders are not latency samples. Raise the limit alongside the rate:

```
./build.sh run --cpus 2,3,4,5 --idle-us 0 -s 60 -r 20000 --max-rate 5000
```

### Other useful runs

| Command | Purpose |
|---|---|
| `./build.sh run` | Ten-second smoke run with defaults |
| `./build.sh run -s 30 --idle-us 20` | Gentle run on a small machine; correctness only |
| `./build.sh run --cpus 2,3,4,5 --idle-us 0` and then the same without `--cpus` | Shows what pinning buys |
| `./build.sh run --cpus 2,3,4,5 --idle-us 0 --hugepages --mlock` | Adds huge pages and locked memory |
| `./build.sh run -p asan -s 5` | Runs the whole pipeline under the sanitizers |
| `./build.sh run --symbols MSFT,NVDA` | Two instruments instead of four |
| `./build.sh stop` | Cleans up after an interrupted run |

## Reading the output

A pinned ten-second run prints something like this (figures from one run, not a specification):

```
venue_sim: itch_msgs=20968 packets=... trades=3886 client_orders=1035 client_fills=996 rejected=0 ...
feed_handler: packets=15011 msgs=20364 published=7784 dropped=0 gaps=0 lost=0 ... unknown_orders=0 ...
strategy: updates=7784 signals=1035 orders=1035 fills=996 rejects=0 refused=0 open=0 session=down positions: ...
order_gateway: requests=1035 sent=1035 risk_rejects=0 not_connected=0 backpressure=0 reports=2071 reports_dropped=0 session=down
tick-to-trade      n=1035      min=2685    mean=7165    p50=6913    p90=9367    p99=16950   p99.9=23641   max=68261 (ns)
feed-to-decision   n=1035      min=50      mean=578     p50=361     p90=794     p99=6021    p99.9=12266   max=59314 (ns)
decision-to-wire   n=1035      min=2594    mean=6587    p50=6467    p90=8474    p99=15612   p99.9=19180   max=22172 (ns)
  ring-hop         n=...
  gateway-send     n=...
```

### Counters

Check these first. A latency table from a run that lost data means nothing.

| Line | Field | Meaning | Healthy value |
|---|---|---|---|
| `venue_sim` | `itch_msgs`, `packets` | Market data messages published and the UDP packets that carried them | grows with `-r` |
| | `trades` | Matches in the simulated book, from all participants | |
| | `client_orders`, `client_fills` | Orders received from the gateway, and fills sent back | `client_orders` equals the gateway's `sent` |
| | `rejected` | Orders the venue refused | 0 |
| | `retransmits` | Retransmission requests served | 0 on loopback |
| `feed_handler` | `packets`, `msgs` | Packets and ITCH messages processed | slightly below the venue's, see below |
| | `published` | Updates written to the market ring. Only messages that change the top of book publish | |
| | `dropped` | Updates lost because the market ring was full | 0 |
| | `gaps`, `lost` | Sequence gaps seen, and messages never recovered | 0 |
| | `requests`, `dups` | Retransmission requests sent, duplicate packets ignored | 0 on loopback |
| | `unknown_orders` | Messages that referred to an order the book never saw | 0 |
| `strategy` | `updates` | Market updates consumed | equals the feed handler's `published` |
| | `signals`, `orders` | Times the imbalance rule fired, and orders sent | |
| | `fills` | Orders that traded | at or a little below `orders` |
| | `rejects` | Orders rejected by the gateway or the venue | 0 |
| | `refused` | Orders the strategy could not queue because the request ring was full | 0 |
| | `positions` | Net shares held per symbol at the end | within the position cap |
| `order_gateway` | `requests`, `sent` | Requests read from the ring, and orders written to the socket | equal |
| | `risk_rejects` | Orders stopped by the risk gate | 0 unless `-r` is high |
| | `not_connected` | Orders refused because the session was down | 0 |
| | `backpressure` | Orders refused because the socket could not take them | 0 |
| | `reports`, `reports_dropped` | Execution reports published to the strategy, and reports lost to a full ring | `reports_dropped` is 0 |

Three things look odd and are fine:

- `fills` below `orders`. The strategy sends immediate-or-cancel orders. Some arrive after the quote has gone and are cancelled without trading.
- The feed handler's `msgs` below the venue's `itch_msgs`. The feed handler starts first and so reaches its deadline first, while the venue is still publishing. With `gaps=0` and `lost=0` nothing was missed while it ran.
- `session=down` on the last line. The venue has exited and closed the connection.

`reports` is two per order (an acceptance, then a fill or a cancel) plus one or two session events.

### Latency table

All figures are nanoseconds. `n` is the number of orders measured.

| Row | Starts | Ends | What it covers |
|---|---|---|---|
| `tick-to-trade` | Feed handler has the packet in user space | Gateway's `send()` returns | The whole pipeline |
| `feed-to-decision` | Same start | Strategy writes the order request | ITCH decode, book update, market ring, strategy logic |
| `decision-to-wire` | Strategy writes the request | Gateway's `send()` returns | Request ring, risk check, OUCH encode, the socket write |
| `ring-hop` | Strategy writes the request | Gateway takes it off the ring | Cross-core hand-off, plus waiting for the gateway's poll loop to come round |
| `gateway-send` | Gateway takes the request | Gateway's `send()` returns | Risk check, encode, and the kernel TCP send |

`feed-to-decision` plus `decision-to-wire` equals `tick-to-trade` for each order, and `ring-hop` plus `gateway-send` equals `decision-to-wire`. Percentiles do not add up that way, because the slow orders in one stage are not the slow orders in another.

How to read it:

- `p50` is the typical order. `p99` and beyond describe the bad moments, which is where pinned and isolated cores earn their keep.
- With `n` near a thousand, `p99.9` and `max` are each one order. Run longer before drawing conclusions from them.
- `gateway-send` is mostly the kernel. On loopback a TCP `send()` runs the transmit path and the receive path in the caller's context, so it costs more than a send to a real network card. This is the stage that kernel bypass (Solarflare Onload, DPDK) removes.
- The clock starts once the packet is in user space. Time in the kernel's UDP receive path is not counted.
- The histogram behind each row has about 3% resolution, so neighbouring values land in the same bucket.

### Logs

Each process writes to `build/<preset>/logs/<name>.log`: a start-up line, the pinning and isolation status, a statistics line every five seconds, and a final line. The script deletes old logs at the start of every run.

## Benchmarks

`./build.sh bench` runs five programs. Each prints one line per case in the same format as the latency table, and a second line with the average cost measured without per-operation timing. When the kernel allows it, that second line also carries hardware-counter figures per operation: cycles, instructions, instructions per cycle, cache misses and branch mispredictions.

```
./build.sh bench --cpus 2,3
./build.sh bench -f levels --cpus 2
build/release/bench/ring_bench --cpu=2 --cpu2=3 --ops=1000000 --filter=hop
```

| Benchmark | Measures |
|---|---|
| `ring_bench` | The SPSC ring: push and pop on one thread, bursts of 16, and the cross-core hand-off between two pinned threads, both paced and saturated |
| `wire_bench` | ITCH decode, walking a 24-message MoldUDP64 packet, OUCH order encode, order-token parse |
| `levels_bench` | Three ways to hold price levels (the sorted array `hotpath` uses, `std::map` with and without a pool allocator, a direct-indexed price ladder) on books of three different depths |
| `book_bench` | Order book add and remove with 1 thousand, 100 thousand and 1 million resting orders, and the feed handler replaying about a million simulator-generated messages |
| `pipeline_bench` | The complete pipeline in one thread against an in-memory venue, with no kernel in the path |

Notes on reading them:

- `pipeline_bench` is the cost of the code alone. The gap between its tick-to-trade and the four-process figure is the price of the kernel's network stack and of moving cache lines between cores.
- For operations that take less than about 20 ns, the per-operation percentiles are below the resolution of the timing probe. Use the `unprobed avg` figure.
- `--cpu2` is needed for the cross-core cases in `ring_bench`; without it they are skipped.
- Hardware counters need `perf_event_open`. If the first lines say `hardware counters unavailable`, see [Preparing a machine for real numbers](#preparing-a-machine-for-real-numbers).

Benchmark options: `--cpu=N`, `--cpu2=N`, `--ops=N` (operations per case), `--filter=TEXT` (cases whose name contains `TEXT`).

## Running the processes by hand

`build.sh run` is a convenience. The executables in `build/release/bin/` take `--key=value` options and reject any they do not know. Start `feed_handler` first so that it sees the venue's opening messages; the others wait for the shared-memory channels they need.

Options every process accepts:

| Option | Meaning |
|---|---|
| `--cpu=N` | Pin to core N |
| `--idle-us=N` | Sleep N microseconds when idle; 0 busy-spins |
| `--seconds=N` | Exit after N seconds; without it, run until Ctrl-C |
| `--stats-sec=N` | Print a statistics line every N seconds |
| `--mlock` | Lock memory |
| `--no-hugepages` | Use 4 KB pages without trying 2 MB pages first (not on `order_gateway`) |
| `--help` | Print the options |

`feed_handler`

| Option | Meaning | Default |
|---|---|---|
| `--feed=ip:port` | Line A listen address. A multicast group address is joined | `127.0.0.1:31001` |
| `--feed-b=ip:port` | Optional line B, for A/B arbitration | none |
| `--iface=ip:0` | Interface address for multicast joins | any |
| `--retransmit=ip:port` | Retransmission server | `127.0.0.1:31002` |
| `--no-recovery` | Skip gaps and flag the data as lossy instead of re-requesting | recover |
| `--start-seq=N` | First sequence number wanted; 0 joins at the live position | 1 |
| `--symbols=A,B,C` | Watch list | `MSFT,NVDA,AAPL,AMZN` |
| `--max-orders=N` | Order table capacity, a power of two | 1048576 |
| `--levels=N` | Price levels kept per side | 1024 |

`strategy`

| Option | Meaning | Default |
|---|---|---|
| `--symbols=A,B,C` | Watch list | as above |
| `--threshold=N` | Percent of top-of-book size on one side that triggers an order | 80 |
| `--clip=N` | Shares per order | 100 |
| `--max-position=N` | Position cap per symbol, in shares | 500 |

`order_gateway`

| Option | Meaning | Default |
|---|---|---|
| `--venue=ip:port` | Order entry address | `127.0.0.1:31003` |
| `--user=NAME`, `--password=TEXT` | SoupBinTCP login | `HOTPTH`, empty |
| `--firm=MPID` | Firm identifier on each order | empty |
| `--symbols=A,B,C` | Watch list | as above |
| `--max-qty=N` | Largest order, in shares | 1000 |
| `--max-notional=N` | Largest order value, in dollars | 1000000 |
| `--max-rate=N` | Orders allowed per second | 1000 |

`venue_sim`

| Option | Meaning | Default |
|---|---|---|
| `--feed=ip:port` | Where market data is sent | `127.0.0.1:31001` |
| `--iface=ip:0` | Interface address for multicast sends | any |
| `--retransmit=ip:port` | Retransmission listen address | `127.0.0.1:31002` |
| `--order-entry=ip:port` | Order entry listen address | `127.0.0.1:31003` |
| `--symbols=A,B,C` | Instruments to list | as above |
| `--rate=N` | Background order-flow actions per second | 2000 |
| `--seed=N` | Order-flow seed; the same seed gives the same flow | 1 |

`--symbols` must be the same list in the same order in every process. A symbol's position in the list is its numeric identifier in the shared-memory messages.

## Repository layout

```
hotpath/
├── CMakeLists.txt
├── CMakePresets.json
├── README.md
├── build.sh
├── apps/
│   ├── CMakeLists.txt
│   ├── common/app.hpp
│   ├── feed_handler/main.cpp
│   ├── order_gateway/main.cpp
│   ├── strategy/main.cpp
│   └── venue_sim/main.cpp
├── bench/
│   ├── CMakeLists.txt
│   ├── book_bench.cpp
│   ├── levels_bench.cpp
│   ├── pipeline_bench.cpp
│   ├── ring_bench.cpp
│   ├── wire_bench.cpp
│   └── support/
│       ├── bench.hpp
│       └── flow.hpp
├── cmake/
│   └── HotpathFlags.cmake
├── docs/
│   ├── QUICKSTART.md
│   └── TESTING.md
├── include/hotpath/
│   ├── core/
│   ├── sys/
│   ├── mem/
│   ├── ipc/
│   ├── wire/
│   ├── net/
│   ├── book/
│   ├── feed/
│   ├── gateway/
│   ├── strategy/
│   ├── sim/
│   └── perf/
└── tests/
    ├── CMakeLists.txt
    ├── support/fakes.hpp
    ├── unit/
    └── integration/
```

The library is header-only and lives under `include/hotpath/`. The folders are listed bottom-up: each builds on the ones above it. `perf/` is the exception, since the gateway uses its histogram.

| Folder | Namespace | Contents |
|---|---|---|
| `core/` | `hotpath` | Strong types (`Price`, `Qty`, `Side`, typed ids), `Status`, assertions, the TSC clock |
| `sys/` | `hotpath::sys` | Thread pinning, isolation check, NUMA binding, page mapping and locking |
| `mem/` | `hotpath::mem` | `Region` (mapped memory), `Arena` (bump allocator that freezes), `Pool` (fixed-size free list) |
| `ipc/` | `hotpath::ipc` | The SPSC ring, shared-memory segments, `Channel` (a ring in a named segment) |
| `wire/` | `hotpath::wire` | Byte-exact ITCH 5.0, OUCH 4.2, MoldUDP64 and SoupBinTCP message layouts |
| `net/` | `hotpath::net` | Non-blocking UDP and TCP sockets, multicast |
| `book/` | `hotpath::book` | Order map, price levels, per-symbol books |
| `feed/` | `hotpath::feed` | Sequencing and gap recovery, the book builder, the market update message |
| `gateway/` | `hotpath::gateway` | Order tokens, risk gate, SoupBinTCP session, the order gateway |
| `strategy/` | `hotpath::strategy` | Order tracking, the order sender, the runner, the imbalance taker |
| `sim/` | `hotpath::sim` | Matching engine, ITCH publisher, SoupBinTCP server, synthetic liquidity |
| `perf/` | `hotpath::perf` | Latency histogram, scoped timer, report printing, hardware counters |

## Design

**No allocation after start-up.** Each process maps one region of memory, carves everything it needs from an arena, then freezes the arena. Nothing on the data path calls `malloc`, takes a lock, or makes a system call other than the socket reads and writes.

**Zero-copy between processes.** The producer claims a slot in the ring, writes the message in place, and publishes it. The consumer reads it in place. The ring's header keeps the producer's and consumer's positions on separate cache lines, and each side caches the other's position so that the common case touches no shared line at all.

**Wire structs are the wire.** Every protocol message is a C++ struct with alignment 1 whose fields are big-endian byte arrays. Decoding is a pointer cast and a size check; a field is byte-swapped only when it is read. Struct sizes and field offsets are checked against the published specifications at compile time and in tests.

**A book sized for the buy side.** A participant reading ITCH needs the quantity at each price, not the queue of orders behind it. Each order is 16 bytes in an open-addressed hash map, and each side of a book is a sorted array of levels with the best price at the end.

**No dynamic dispatch on the data path.** Components are joined by templates and concepts. A handler implements `on(const Message&)` only for the messages it wants; the rest compile to nothing.

**Fail closed.** The risk gate rejects everything until it has limits. The gateway rejects orders while the session is down rather than queueing them. A feed with a gap flags its updates as stale or lossy, and the strategy will not trade on them.

**Measure in the product.** Latency histograms live inside the gateway, and recording a sample is one array increment. The same histogram type is used by the benchmarks.

## Coding rules

The code follows NASA's "Power of Ten" rules for safety-critical software, adapted to C++20:

| Rule | Here |
|---|---|
| Simple control flow | No exceptions, no RTTI, no recursion. Built with `-fno-exceptions -fno-rtti` |
| Bounded loops | Every loop has a fixed upper bound. The poll loop of each process is the one sanctioned exception |
| No allocation after initialisation | Arenas freeze once start-up is complete |
| Assertions | `HOTPATH_ASSERT` is always on. `HOTPATH_HOT_ASSERT` guards the data path and compiles out in release builds |
| Check every return value | Errors are `Status` values from `[[nodiscard]]` functions; with `-Werror`, ignoring one fails the build |
| No function pointers or virtual calls on the data path | Templates and concepts instead |
| Warnings are errors | `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror` |

Tests and benchmarks relax the allocation and container rules. The library and the applications do not.

## Preparing a machine for real numbers

None of this is needed to build, test or run. It is what separates a tidy latency table from a noisy one. All of it applies to bare-metal Linux; little of it is available under WSL2 or in a cloud VM.

| Step | How | Effect |
|---|---|---|
| Isolate the cores | Add `isolcpus=2-5 nohz_full=2-5 rcu_nocbs=2-5` to the kernel command line and reboot | Keeps the scheduler, the timer tick and RCU callbacks off those cores. Logs then say `(isolated)` |
| Move interrupts away | Stop `irqbalance`; write a mask excluding the cores to `/proc/irq/*/smp_affinity` | Removes interrupt handling from the pinned cores |
| Fix the CPU frequency | `cpupower frequency-set -g performance`; limit deep C-states in the BIOS or with `intel_idle.max_cstate=1` | Removes wake-up and frequency-ramp delays |
| Reserve huge pages | `echo 256 | sudo tee /proc/sys/vm/nr_hugepages`, then run with `--hugepages` | Fewer TLB misses on the order map and books |
| Allow memory locking | `ulimit -l unlimited`, then run with `--mlock` | No page faults or swapping on the data path |
| Allow hardware counters | `sudo sysctl kernel.perf_event_paranoid=1` | Lets the benchmarks read cycle, instruction and miss counts |
| Pick cores on one NUMA node | `lscpu -e`, `numactl --hardware` | Memory is bound to the node of the pinned core |

## Known limitations

- **Kernel networking only.** Sockets are plain BSD sockets. The transport is a template parameter so that a kernel-bypass stack can be dropped in, but none has been, because that needs the hardware.
- **Shared memory uses 4 KB pages.** `--hugepages` covers process-private memory, not the rings.
- **Gap recovery stops and re-requests.** There is no reorder buffer; packets that arrive while a gap is open are dropped and requested again.
- **A full report ring drops execution reports.** It is counted in `reports_dropped`, and the strategy would then hold a stale view of its orders.
- **Risk is per order.** Quantity, notional and rate are checked. There are no position or loss limits in the gateway.
- **The strategy is a demonstration.** It exists to generate realistic order flow through the pipeline.
- **The simulated venue is simplified.** One client connection, no auctions, no market orders, no short-sale rules, and a replace is a delete followed by an add.
- **The price-level structure is not settled.** `levels_bench` shows the sorted array winning only on shallow books, and a direct-indexed ladder beating both the array and a tree on every profile tried.
- **The applications have no automated test.** The library is covered; the four `main.cpp` files are exercised by `build.sh run`.

## Protocol references

Message layouts were checked field by field against the published specifications:

- Nasdaq TotalView-ITCH 5.0
- Nasdaq OUCH 4.2
- Nasdaq MoldUDP64 1.00
- Nasdaq SoupBinTCP 3.00
