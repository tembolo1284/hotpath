# Testing

`hotpath` has 84 tests: 82 unit tests across eleven files and two integration tests. They build into one executable, `hotpath_unit_tests`, and run in a few seconds.

This document covers how to run them, how they are built, and what each one proves.

## Contents

1. [Running the tests](#running-the-tests)
2. [How the tests are organised](#how-the-tests-are-organised)
3. [Techniques used](#techniques-used)
4. [Test doubles](#test-doubles)
5. [Unit tests, file by file](#unit-tests-file-by-file)
6. [Integration tests](#integration-tests)
7. [Checks that run at compile time](#checks-that-run-at-compile-time)
8. [Sanitizers](#sanitizers)
9. [Tests that may skip](#tests-that-may-skip)
10. [What is not tested](#what-is-not-tested)
11. [Adding a test](#adding-a-test)

## Running the tests

```
./build.sh test                      all tests, debug build
./build.sh test -p release           all tests, optimised build
./build.sh test -p asan              under AddressSanitizer and UBSan
./build.sh test -p tsan              under ThreadSanitizer
./build.sh test -f 'Book*'           one suite
./build.sh test -f 'FeedTest.Gap*'   tests matching a pattern
./build.sh test -f '*Loopback*'      every test that uses real sockets
```

Without `-f`, the script runs `ctest`, which reports one line per test and prints output only for failures. With `-f`, it runs the test executable directly with a GoogleTest filter, so you see the full GoogleTest output.

You can also run the executable yourself:

```
build/debug/tests/hotpath_unit_tests --gtest_list_tests
build/debug/tests/hotpath_unit_tests --gtest_filter='Pipeline.*'
build/debug/tests/hotpath_unit_tests --gtest_filter='RingStress.*' --gtest_repeat=20
```

The tests need no privileges, no network beyond loopback, and no particular core count. They create and remove their own shared-memory objects, named with the process id so that parallel runs do not collide.

## How the tests are organised

```
tests/
├── CMakeLists.txt
├── support/
│   └── fakes.hpp
├── unit/
│   ├── sys_test.cpp
│   ├── mem_test.cpp
│   ├── ipc_test.cpp
│   ├── wire_test.cpp
│   ├── net_test.cpp
│   ├── book_test.cpp
│   ├── feed_test.cpp
│   ├── gateway_test.cpp
│   ├── strategy_test.cpp
│   ├── sim_test.cpp
│   └── perf_test.cpp
└── integration/
    ├── pipeline_test.cpp
    └── sim_loop_test.cpp
```

| File | Suites | Tests | Library folder covered |
|---|---|---|---|
| `sys_test.cpp` | `Core`, `Sys` | 4 | `core/`, `sys/` |
| `mem_test.cpp` | `MemTest` | 4 | `mem/` |
| `ipc_test.cpp` | `RingTest`, `RingStress`, `Shm`, `Channel` | 7 | `ipc/` |
| `wire_test.cpp` | `Fields`, `Itch`, `Mold`, `Soup`, `Ouch` | 14 | `wire/` |
| `net_test.cpp` | `Endpoint`, `Udp`, `Tcp` | 5 | `net/` |
| `book_test.cpp` | `BookTest` | 5 | `book/` |
| `feed_test.cpp` | `Sequencer`, `FeedTest` | 11 | `feed/` |
| `gateway_test.cpp` | `Token`, `Risk`, `SoupSession`, `GatewayTest`, `GatewayTcp` | 11 | `gateway/` |
| `strategy_test.cpp` | `StrategyTest` | 5 | `strategy/` |
| `sim_test.cpp` | `SimTest`, `VenueTest` | 10 | `sim/` |
| `perf_test.cpp` | `Histogram`, `Report`, `Timer`, `Counters` | 6 | `perf/` |
| `pipeline_test.cpp` | `Pipeline` | 1 | feed, strategy and gateway together |
| `sim_loop_test.cpp` | `SimLoop` | 1 | everything, over real sockets |

There is one test file per library folder, so a failing suite name points at the folder to look in.

## Techniques used

**Reference models.** The order map and the book are each run against a simple, obviously correct model (`std::unordered_map`, `std::map`) for hundreds of thousands of random operations, and compared after every step. The random generators use fixed seeds, so a failure reproduces exactly and reports the step number.

**A clock the test controls.** Components that read time take the clock as a template parameter. Tests pass `FakeClock`, set its value by hand, and assert exact tick counts for heartbeats, timeouts, rate limits and latency measurements. Nothing sleeps to make time pass.

**Fake transports.** The SoupBinTCP session and the gateway are written against a `Transport` concept. Tests supply `FakeTransport`, which records what was sent and plays back scripted replies, and which can be told to accept only part of a write, block, fail, or close.

**Real sockets where the socket is the point.** The network layer, feed recovery, the gateway and the full loop each have a test that runs over loopback UDP or TCP. These bind to port 0 and read back the port the kernel chose, so they never clash with a running pipeline or with each other.

**Real processes and threads where concurrency is the point.** The ring is stressed with two threads, and the shared-memory channel is tested with `fork()` and a real second process.

**Byte offsets from the specifications.** Every wire struct has its field offsets asserted against the numbers printed in the Nasdaq documents, so a layout mistake cannot hide behind a round trip that encodes and decodes with the same wrong struct.

**Bounded waits.** Tests that wait for something across a socket or a thread use `spin_until` with an iteration limit. A broken test fails; it does not hang.

## Test doubles

All in `tests/support/fakes.hpp`, namespace `hotpath::test`.

| Type | Stands in for | Behaviour |
|---|---|---|
| `FakeClock` | The TSC clock | `now()` returns a static value the test sets |
| `VectorSink<T>` | The producer side of a ring | Collects published messages in a vector. Setting `full` makes `claim()` fail, to test back-pressure |
| `QueueSource<T>` | The consumer side of a ring | Feeds queued messages through `peek()` and `consume()` |
| `HeapRing<T>` | A shared-memory channel | A real SPSC ring on the heap, for tests that want the genuine ring without `/dev/shm` |
| `FakeTransport` | A TCP connection | Records bytes sent, replays bytes queued by the test. Can limit bytes per send or receive, block, return an error, or signal end of stream. Has helpers to queue a framed SoupBinTCP packet or a login acceptance |

## Unit tests, file by file

### `sys_test.cpp` (4 tests)

| Test | What it proves |
|---|---|
| `Core.QtyAndPriceArithmetic` | `Qty` adds and subtracts to zero, the difference of two `Price` values is a tick count, and `opposite(Buy)` is `Sell` |
| `Core.TscScaleConvertsTicks` | TSC calibration yields a non-zero scale, and two back-to-back timestamp reads convert to well under a millisecond. Skips without an invariant TSC |
| `Sys.PinsToCurrentCpu` | Pinning the thread to the core it is already on succeeds and is confirmed by reading the affinity back; the NUMA node of the current core can be read |
| `Sys.IsolationCheckReads` | The isolated-CPU check either reads the kernel's list or reports that the file does not exist |

### `mem_test.cpp` (4 tests)

Each test starts with a 1 MB mapped and pre-faulted region.

| Test | What it proves |
|---|---|
| `MemTest.ArenaAlignsAndExhausts` | After an odd-sized allocation, an array of 64-bit values comes back correctly aligned and zeroed; asking for more than remains returns null; the arena can be frozen |
| `MemTest.PoolRecyclesLifo` | The pool hands out indices in order, a released index is the next one reused, a full pool returns the nil index, and the in-use count is exact |
| `MemTest.PoolRejectsOversizedCapacity` | A pool larger than the arena fails with `ENOMEM` rather than overrunning |
| `MemTest.RegionMoveTransfersOwnership` | Moving a region carries the mapping and its pre-faulted state, and leaves the source empty |

### `ipc_test.cpp` (7 tests)

| Test | What it proves |
|---|---|
| `RingTest.FormatRejectsBadArguments` | Formatting fails for a capacity that is not a power of two, a buffer that is too small, and memory that is not cache-line aligned |
| `RingTest.AttachValidatesHeader` | Attaching to zeroed memory returns `EAGAIN` (not ready yet); after formatting it succeeds; attaching with a different message type or a wrong size returns `EPROTO` |
| `RingTest.FillsDrainsAndWraps` | Over five rounds the ring fills to capacity, refuses one more, drains partially, refills and empties, with every message arriving in order and intact as the indices wrap |
| `RingTest.ZeroCopyClaimAndPeek` | A claimed slot is invisible to the consumer until published; after publishing, the consumer sees the very same address the producer wrote to |
| `RingStress.PreservesOrderAcrossThreads` | Two million messages pass between two threads through a 1024-slot ring with no loss, duplication, reordering or corruption. This is the test ThreadSanitizer is aimed at |
| `Shm.CreateIsExclusiveAndOpenSeesData` | A segment is rounded up to a page; creating the same name twice fails with `EEXIST`; a second mapping sees bytes written through the first; after unlinking, opening fails with `ENOENT` |
| `Channel.CrossProcessDelivery` | A forked child opens the channel, retrying until the parent has created it, and receives 500,000 messages in order with correct payloads. The child's exit code reports the result |

### `wire_test.cpp` (14 tests)

| Test | What it proves |
|---|---|
| `Fields.BigEndianByteOrderOnTheWire` | Integer fields are stored most-significant byte first, including the 6-byte timestamp |
| `Itch.DispatchDecodesAddOrderInPlace` | An Add Order message is delivered to the handler with reference, price, symbol and timestamp read correctly from the raw bytes |
| `Itch.DispatchSkipsUnhandledAndFlagsBadInput` | A message type the handler has no method for is accepted and ignored; a short message is `Truncated`; an unknown type byte is `Unknown`; empty input is `Truncated` |
| `Itch.FieldOffsetsMatchSpecification` | Thirty field offsets across twelve ITCH messages match the specification, as does the maximum price constant |
| `Itch.DispatchDeliversTradingActionAndBrokenTrade` | Trading-halt and broken-trade messages reach their handlers with the right content |
| `Mold.WriterAndReaderRoundTrip` | A packet of three messages carries the right session, first sequence and count; reading it back yields each message with its own sequence number; a packet cut short anywhere is `Truncated` |
| `Mold.WriterRefusesOverflowAndHandlesHeartbeat` | The writer refuses a message that will not fit and keeps its count unchanged; a heartbeat is a bare header with zero messages |
| `Mold.RequestPacketSharesHeaderLayout` | The retransmission request has its sequence and count at the same offsets as the downstream header |
| `Soup.FramesPacketsFromAPartialStream` | Two packets are framed correctly from a byte stream; a stream cut inside the length or inside the payload is `Truncated`; a zero length is `Malformed` |
| `Soup.LoginAcceptedSessionIsLeftPadded` | Login message offsets match the specification, and the session id is padded on the left, as SoupBinTCP requires, and trims correctly |
| `Soup.LoginMessagesCarryNumericSequence` | A login request has the right length field and its sequence number is space-padded ASCII that parses back |
| `Ouch.DirectionsDispatchSeparately` | Client-to-venue and venue-to-client messages dispatch through separate functions, because some type bytes mean different things in each direction; a venue message offered as a client message is `Unknown` |
| `Ouch.FieldOffsetsMatchSpecification` | Sixty-one field offsets across fifteen OUCH messages match the specification, as do the maximum and market price constants |
| `Ouch.RemainingOutboundTypesDispatch` | The less common venue messages (reference-price execution, self-match cancel, broken trade, cancel pending, cancel reject, priority update, modified) all dispatch, along with the inbound modify |

### `net_test.cpp` (5 tests)

| Test | What it proves |
|---|---|
| `Endpoint.ParsesAndClassifies` | Addresses parse and convert to and from `sockaddr`; multicast addresses are recognised; malformed or null text is `EINVAL` |
| `Udp.LoopbackDatagramsArriveWhole` | An empty socket reports would-block without blocking; two datagrams of different sizes arrive separately, whole and in order |
| `Udp.MulticastOverLoopback` | A receiver joins a multicast group on the loopback interface and receives a datagram sent to it. Skips where the environment does not allow it |
| `Tcp.LoopbackStreamAndOrderlyClose` | Accept with nobody connecting reports `EAGAIN`; data flows both ways; when the client closes, the server sees end of stream, distinct from an error |
| `Tcp.ConnectToClosedPortFails` | Connecting to a port nobody is listening on returns `ECONNREFUSED` and leaves the socket invalid |

### `book_test.cpp` (5 tests)

Each test starts with a 64 MB arena.

| Test | What it proves |
|---|---|
| `BookTest.OrderMapMatchesReferenceUnderChurn` | A capacity that is not a power of two is rejected. Then 200,000 random inserts and erases over 1,500 possible ids, in a 1,024-slot map, agree with `std::unordered_map` at every step: duplicates are detected, the map refuses inserts at its load limit, and erased ids are gone. This exercises backward-shift deletion under heavy collision |
| `BookTest.LevelSideOrdersBestFirstOnBothSides` | Prices added in arbitrary order come out best-first, highest for bids and lowest for asks; orders at the same price share a level; a partial reduce lowers quantity only; removing the last order deletes the level |
| `BookTest.LevelSideKeepsNearestLevelsWhenFull` | With room for four levels, a better fifth price evicts the worst. The side records a horizon, counts orders beyond it without storing them, keeps that count right as those orders are later removed, and reports itself degraded if the visible levels empty while orders remain beyond the horizon |
| `BookTest.BookSetAppliesOrderLifecycle` | Add, partial reduce, remove and replace each update the book and report whether the top of book changed. Every error is distinct: duplicate id, unknown id, over-reduction, bad symbol, zero quantity, negative price |
| `BookTest.BookSetMatchesReferenceModelUnderRandomFlow` | 300,000 random adds, reduces, removes and replaces across several symbols agree with a `std::map` model after every step, both for the top of book and for the "top changed" flag, and at the end for full depth |

### `feed_test.cpp` (11 tests)

The `FeedTest` fixture provides books, a two-symbol watch list (MSFT, NVDA), a packet builder and a `VectorSink` for published updates.

| Test | What it proves |
|---|---|
| `Sequencer.ClassifiesPackets` | Each packet is classified as process, duplicate, heartbeat, end of session, gap (with the number missing) or wrong session; a sequencer configured to join late adopts the first sequence it sees |
| `FeedTest.SymbolTableResolvesWatchedTickers` | Duplicate and over-long tickers are rejected; a locate code resolves only for watched tickers; a ticker that reappears under a new locate code moves to it |
| `FeedTest.BuildsBookAndPublishesTopChanges` | A scripted session of adds, an execution, a cancel, a replace and a delete produces exactly the expected updates, each carrying the sequence number, receive timestamp and exchange timestamp of the message that caused it. A trade print publishes as a trade. A message for an unknown order is counted, not applied |
| `FeedTest.SecondLineDuplicatesAreIgnored` | The same packet arriving on the second line is counted as a duplicate and changes nothing; heartbeats are counted and do not create gaps |
| `FeedTest.TradingHaltIsFlaggedOnUpdates` | A halt publishes a status update, every update while halted is marked untradable, and the resume clears the flag |
| `FeedTest.GapWithoutRecoveryIsSkippedAndFlaggedLossy` | With recovery off, a gap is counted along with the three lost messages, a feed-status update announces the lossy state, and later updates remain untradable |
| `FeedTest.GapIsRecoveredFromRetransmission` | With recovery on, a gap marks the feed stale and stops the sequence advancing; live packets that arrive meanwhile do not advance it either; once the retransmission fills the gap, the stale flag clears, a status update says so, the book is correct, and nothing counts as lost |
| `FeedTest.FullSinkDropsAreCounted` | When the output ring is full the book is still updated and the dropped updates are counted |
| `FeedTest.MalformedPacketsDoNotAdvancePastGoodMessages` | A packet truncated mid-message applies the good messages before the damage, counts as malformed, and leaves the sequence pointing at the first message not applied |
| `FeedTest.PublishesIntoSharedMemoryRing` | The feed handler writes into a real SPSC ring, and a consumer reads the updates with their timestamps |
| `FeedTest.RecoversOverLoopbackSockets` | Over real UDP: packets one and three are sent, the handler sends a retransmission request naming the right session, sequence and count, a test server answers it, and the handler ends with a correct book |

### `gateway_test.cpp` (11 tests)

| Test | What it proves |
|---|---|
| `Token.RoundTripsAcrossTheIdRange` | Order ids across the full 64-bit range encode to a 14-character token and decode back; tokens with out-of-range digits or the wrong shape are rejected |
| `Risk.FailsClosedAndLimitsRate` | A gate with no limits rejects everything. A configured gate rejects zero and oversized quantity and oversized notional, allows two orders in a window, rejects the third, and allows again once the window has passed |
| `SoupSession.LogsInAndDeliversSequencedData` | The login request has the right length, username, password, blank session and sequence 1. On acceptance the session becomes active and adopts the venue's session id and sequence. Sequenced messages are delivered and advance the sequence; heartbeats are counted |
| `SoupSession.HeartbeatsAndTimesOut` | Using the fake clock: no heartbeat before the interval, exactly one after it, traffic from the venue keeps the session alive, and silence past the timeout fails it and notifies the owner |
| `SoupSession.HandlesRejectEndOfSessionAndPeerClose` | A login rejection records its reason code and blocks sending; end of session is reported once; a closed connection and a malformed packet each fail the session |
| `SoupSession.PartialWritesAreBufferedInOrder` | When the socket takes only five bytes, the rest is held and a second message queues behind it; once the socket drains, both arrive complete and in order; a socket error on a later send fails the session |
| `GatewayTest.TranslatesRequestsToOuch` | New, cancel and replace requests become Enter Order, Cancel Order and Replace Order with every field checked on the wire: token, side, shares, symbol, price, time in force, firm, display, capacity. Tick-to-trade is recorded for a request that carries a market data timestamp |
| `GatewayTest.TurnsVenueMessagesIntoReports` | Accepted, executed, cancelled, rejected, replaced and broken-trade messages each become the right report with ids, quantities, prices, reason codes and timestamps. A message whose token is not ours is counted and ignored |
| `GatewayTest.RejectsLocallyWithoutTouchingTheWire` | Orders sent before login, over the quantity or notional limit, for an unknown symbol, or with an unrepresentable price are each rejected with a distinct source and reason, and nothing is written to the socket |
| `GatewayTest.SessionLossRejectsAndCancelFailuresAreNotOrderRejects` | No session event is published during login; session up and session down are each published once. While down, a rejected cancel is reported as a cancel rejection, so the strategy does not conclude that a live order has gone |
| `GatewayTcp.LogsInAndTradesOverLoopback` | Over real TCP against a test server: login, an order appearing on the wire with the right token, a fill coming back as a report, and a non-zero tick-to-trade measurement |

### `strategy_test.cpp` (5 tests)

| Test | What it proves |
|---|---|
| `StrategyTest.OrderTableTracksFillsAndPosition` | The table rejects a bad capacity, a full slot and a bad symbol. Partial and full fills update position and cash; a later sell at a higher price leaves the right profit. A reject closes the order, a cancel rejection does not, a replace moves the order to its new id with its new quantity and price, and reports for unknown orders are flagged |
| `StrategyTest.SenderStampsAndNumbersRequests` | Requests get consecutive ids and carry both the market data timestamp and the decision timestamp; cancel and replace refer to the right ids; when the ring is full the send is refused, counted, and no id is consumed |
| `StrategyTest.TakerTradesOnImbalanceOneOrderAtATime` | A threshold of 50% is rejected as meaningless. No order is sent before the session is up or on a balanced book. Heavy bids trigger a buy at the ask, heavy asks a sell at the bid, each immediate-or-cancel. Only one order per symbol is live at a time, and the position cap stops further buying |
| `StrategyTest.TakerIgnoresUntradableUpdates` | No order is sent on a stale or halted update, a one-sided or crossed book, an unknown symbol, a feed-status message, or while the session is down. A send refused by a full ring is counted and the next signal trades. A rejection frees the symbol for the next order |
| `StrategyTest.RunnerDrainsReportsBeforeMarketData` | With a session-up report and a market update both waiting, one poll handles the report first, so the strategy trades on that same poll. The request carries the market timestamp through from the update |

### `sim_test.cpp` (10 tests)

| Test | What it proves |
|---|---|
| `SimTest.EngineMatchesInPriceTimePriority` | An incoming order trades with the best price first and, within a price, the earliest order first; trades report price, quantity, match number and whether the resting order is finished. A non-crossing order rests; an immediate-or-cancel order fills what it can and cancels the rest |
| `SimTest.EngineReducesRemovesAndBoundsCapacity` | An order can be reduced or removed but not increased; removing twice is harmless; freed slots are reused and a stale handle no longer resolves; with no level or order slot free, a new order is accepted and cancelled back; bad symbol, zero quantity and zero price are refused |
| `SimTest.TokenMapIsInsertOnce` | A token can be inserted once only, and lookups by token and by user reference agree; out-of-range references return nothing |
| `SimTest.PublisherPacketizesSequencesAndRetransmits` | Fifty messages are split into packets no larger than the limit with consecutive sequence numbers; a heartbeat carries the next sequence and no messages; a retransmission returns the stored messages, and requests for sequences not yet sent, zero, or already overwritten return nothing |
| `SimTest.SoupServerLogsInSequencesAndReplays` | Client data before login is ignored. On login the acceptance carries the session and sequence, and messages logged earlier are delivered, even through a socket that takes eleven bytes at a time. Heartbeats go out on schedule. A client that reconnects asking for a later sequence is replayed from there; a wrong session name, or a sequence that has left the log, is rejected; asking for sequence 0 joins at the current position |
| `VenueTest.OpensMarketWithDirectoryAndTradingState` | Opening publishes three system events, then a stock directory and a trading-state message per symbol with the right locate codes and timestamp, and one system event to the order session |
| `VenueTest.ClientTakerGetsAcceptedThenFillThenIocCancel` | A client immediate-or-cancel order for 100 against 60 resting yields accepted, executed for 60 as liquidity remover, and cancelled for 40, while the market data shows the execution with the same match number |
| `VenueTest.RestingClientOrderIsHitCanceledAndReplaced` | A resting client order appears in market data under the reference in its acceptance, is filled as liquidity adder, partially cancelled, replaced (a delete then an add in market data, with the new and previous tokens in the reply), and fully cancelled. Acting on a finished order does nothing |
| `VenueTest.RejectsBadOrdersAndIgnoresDuplicateTokens` | Five kinds of invalid order each get their own reject reason and publish no market data; a repeated token is ignored and counted |
| `VenueTest.LiquidityIsDeterministicAndBounded` | The synthetic order flow never rests more than its limit, produces adds, deletes and executions, and yields identical output for the same seed and different output for another |

### `perf_test.cpp` (6 tests)

| Test | What it proves |
|---|---|
| `Histogram.BucketsBoundEveryValueWithinThreePercent` | For 200,000 random values across the whole range, the bucket contains the value and is no wider than 1/32 of it; adjacent buckets leave no gaps and do not overlap |
| `Histogram.QuantilesTrackAKnownDistribution` | For the values 1 to 100,000, count, minimum, maximum and mean are exact, and p50, p90, p99 and p99.9 are within about 3% above the true values. An empty histogram reports zeros |
| `Histogram.ClampsMergesAndResets` | A value beyond the range is clamped and counted; merging adds counts and combines extremes; reset clears everything |
| `Report.ConvertsTicksToNanoseconds` | With a scale of half a nanosecond per tick the summary halves every figure, the percentiles come out in order, and printing works |
| `Timer.ScopedTimerRecordsElapsedTicks` | A scoped timer records the elapsed ticks when it leaves scope; a clock that goes backwards records zero, not a huge number; the measured clock overhead is sane |
| `Counters.CountInstructionsWhenTheKernelAllowsIt` | Using counters before opening them reports `EBADF`. Once open, a million-iteration loop registers more than a million instructions and a non-zero cycle count. Skips where `perf_event_open` is not permitted |

## Integration tests

### `Pipeline.TickInOrderOutThroughAllThreeStages`

The feed handler, strategy and gateway are wired together in one thread with real SPSC rings between them, a fake clock, and a fake transport standing in for the venue.

1. The gateway logs in; the strategy learns the session is up.
2. At tick 1000, one MoldUDP64 packet arrives holding a stock directory, an offer of 100 shares and a bid of 900.
3. At tick 1040 the strategy polls. Bids are 90% of the top of book, above the 80% threshold, so it decides to buy.
4. At tick 1100 the gateway polls and sends the order.

The test then checks:

- The bytes on the wire are one Enter Order: buy 100 MSFT at the offer price, immediate-or-cancel.
- Tick-to-trade is exactly 100 ticks, feed-to-decision exactly 40, decision-to-wire exactly 60, and the ring-hop and gateway-send split is recorded.
- A fill fed back through the transport reaches the strategy: position 100, no open orders, one fill.

This is the test that pins down what the latency figures mean.

### `SimLoop.StrategyTradesAgainstTheSimulatedVenueOverSockets`

Everything runs in one process but talks over real loopback sockets: the simulated exchange publishes ITCH over UDP and accepts OUCH over TCP, and the feed handler, strategy and gateway are the real components with the real clock.

1. The loop runs until the strategy has at least 25 fills.
2. The venue's background flow is switched off and the loop runs on until everything is quiet: the feed has caught up, all rings are empty, no orders are open, and the client has read every reply.

The test then checks:

- No market data was lost, malformed, rejected or dropped, and no message referred to an unknown order.
- Every order the strategy sent was written by the gateway and entered at the venue; none was rejected.
- Every report matched an order the strategy knew.
- For each symbol, the book the feed handler built equals the venue's own book: best bid and offer price, quantity and order count, and the number of levels on each side.
- The total number of resting orders agrees on both sides.
- Positions stayed within the configured cap.
- The latency histograms are mutually consistent.

It prints the message counts and the measured latency table, which you can see with `./build.sh test -f 'SimLoop*'`. This is the closest thing to an end-to-end proof: an independent book, rebuilt from the wire, ends up identical to the exchange's.

## Checks that run at compile time

A good deal of verification never reaches the test runner. These are `static_assert`s in the headers and fail the build if violated:

- The size of every wire struct, and that each has alignment 1 and is safe to cast from raw bytes.
- That every shared-memory message is exactly one cache line, trivially copyable and standard layout.
- Big-endian encode and decode, padding and numeric formatting of text fields.
- Order-token encoding and decoding, including rejection of bad characters.
- Histogram bucket boundaries.
- Parsing of the kernel's isolated-CPU list format.
- The ring header's size and the lock-freedom of its indices.

## Sanitizers

| Preset | Catches | Notes |
|---|---|---|
| `asan` | Out-of-bounds access, use after free, leaks, undefined behaviour | The `vptr` check is disabled because the code is built without RTTI |
| `tsan` | Data races | Aimed at `RingStress` and the socket tests; built optimised with debug info so the race window is realistic |

The whole suite passes under `debug`, `release`, `asan` and `tsan` with GCC 13, and under a release build with Clang 18.

## Tests that may skip

| Test | Skips when |
|---|---|
| `Counters.CountInstructionsWhenTheKernelAllowsIt` | `perf_event_open` is not permitted: most containers and VMs, or `kernel.perf_event_paranoid` above 2 |
| `Udp.MulticastOverLoopback` | The environment cannot join or send to a multicast group on loopback |
| `Core.TscScaleConvertsTicks` | The CPU reports no invariant TSC |

A skip is reported as such and does not fail the run.

## What is not tested

- **The four applications.** Argument parsing, channel set-up and the poll loops in `apps/` have no automated test. `./build.sh run` exercises them, and its counters are the check.
- **Hardware counters on real hardware.** That test has only ever skipped in the environments used so far.
- **Performance.** No test asserts a latency. Timing belongs to the benchmarks and to `./build.sh run`.
- **Packet loss on a real network.** Gaps are injected by the tests; loopback does not drop.
- **Huge pages and NUMA binding.** The code paths run, but the tests use 4 KB pages on whatever node they land on.
- **Long runs.** Nothing runs for more than a few seconds, so slow leaks of capacity (a table filling over hours) would not show.

## Adding a test

1. Put it in the file for the folder it covers, or add `tests/unit/<folder>_test.cpp` and list it in `tests/CMakeLists.txt`.
2. Take time from `FakeClock` and I/O from `FakeTransport`, `VectorSink` or `QueueSource` unless the real thing is what you are testing.
3. If you need a socket, bind to port 0. If you need shared memory, put the process id in the name and unlink it at the end.
4. If you wait for something, use `ipc::spin_until` with a limit.
5. For randomised tests, fix the seed and include the step number in the assertion message.
6. Run it under all four presets before trusting it:

```
for p in debug release asan tsan; do ./build.sh test -p $p -f 'MySuite*'; done
```
