# HFT Cooperative Mesh Network
### BuildVerse Hackathon — Idea Submission

---

## SLIDE 1 — TEAM DETAILS

- **Team Name:** [Your Team Name]
- **Theme:** FinTech / Deep Tech Infrastructure
- **Track:** Systems & Low-Latency Computing
- **Project Name:** HFT Mesh — Cooperative High-Frequency Trading Network
- **Team Leader:** [Name]
- **College / Institute Name:** [Institute]

---

## SLIDE 2 — PROBLEM STATEMENT & NEED

### The Problem

High-frequency trading (HFT) is one of the most profitable and technically demanding fields in modern finance. Global HFT firms like Citadel Securities, Optiver, Virtu Financial, and Jane Street collectively execute millions of trades per second, capturing micro-profits that add up to billions annually. They achieve this through three unfair structural advantages:

**1. Infrastructure monopoly.**
Top HFT firms co-locate their servers physically inside exchange data centres (e.g., NASDAQ's facility in Carteret, New Jersey). Their machines sit 10 metres from the exchange's matching engine. This gives them market data in under 500 nanoseconds. A firm outside this ecosystem receives the same data 5 to 50 milliseconds later — an eternity in HFT. Co-location alone costs $15,000–$80,000 per month, per exchange.

**2. Hardware exclusivity.**
Large firms use custom FPGAs (Field Programmable Gate Arrays) — programmable chips that process a market data packet in 50–200 nanoseconds. Software running on a general-purpose CPU cannot match this. A single FPGA development project costs hundreds of thousands of dollars and requires a team of specialist engineers.

**3. Capital and talent concentration.**
A senior C++ engineer at a top HFT firm earns $500,000–$1,000,000 in total compensation. A quant researcher earns similarly. New firms cannot compete for this talent. Without the talent, they cannot build the strategies. Without the strategies, they cannot raise the capital.

### What This Means for Small Firms

A new HFT startup or a small trading firm with limited funds faces a mathematically impossible situation:

- They cannot afford co-location → their data arrives late
- Their data arrives late → their signals are stale
- Their signals are stale → they get adversely selected (big firms trade against their quotes profitably)
- They lose money → they cannot raise capital to upgrade

This is a closed loop. Small firms are structurally locked out of the strategies where the real money is made. They are left competing with each other for the "scraps" — tiny opportunities that the giants ignore because the capacity is too small to move the needle for them. Three or four small firms fighting over these scraps is economically equivalent to collecting coins from the floor after someone drops their wallet.

**The market problem is not that small firms are bad at trading. It is that they operate alone in a war where the enemy has nuclear weapons and they have knives.**

### Why a Technology-Driven Solution is Needed

This is not a regulatory problem or a capital-raising problem. It is a structural, architectural problem. The solution must happen at the infrastructure level — specifically at the network and protocol layer where market data flows, orders are routed, and firms communicate with exchanges.

No existing technology provides a framework for small HFT firms to pool their specialised capabilities, share execution infrastructure, and act as a coordinated unit without sharing proprietary signals or strategies. The gap is not in the trading strategies themselves — it is in the plumbing that connects capabilities to opportunities.

---

## SLIDE 3 — PROPOSED SOLUTION & INNOVATION

### Solution Summary

We propose **HFT Mesh** — a private, low-latency, peer-to-peer cooperative network protocol that allows small HFT firms to pool their specialised capabilities, route execution opportunities between each other in real time, and distribute profits algorithmically — all without any firm ever revealing its proprietary trading signals.

Think of it as a capability-sharing consortium with a purpose-built binary communication protocol, a real-time capability registry, and an automated execution delegation system — operating at sub-millisecond speeds over a private network infrastructure.

### Core Idea of the Product

Each participating firm runs a **Mesh Node** — a C++ software component that sits alongside their existing order book engine. The mesh node does three things:

1. **Broadcasts what the firm CAN do** (capital available, which exchanges it has fast access to, current latency to each venue, risk headroom) — never what it KNOWS (its signals, strategies, or alpha).

2. **Listens to what peer firms can do**, maintaining a live capability table updated every 10 milliseconds.

3. **Delegates execution** when it detects an opportunity it cannot fully execute alone — sending a structured binary request to the most capable peer, which auto-executes and confirms back, all within a 1-millisecond budget.

The insight is that different small firms are often specialised in different layers of the trading stack:
- **Firm A** has built fast network connectivity to Exchange X but has limited capital
- **Firm B** has strong capital reserves and access to Exchange Y but a slower signal engine
- **Firm C** has advanced ML models producing high-quality signals but no co-location

Together, they form a composite capability that approaches what a large firm achieves alone. Separately, they cannot compete. The mesh protocol is the architecture that lets them work together without trusting each other with their intellectual property.

### Key Features of the Solution

**1. Zero signal leakage.**
The capability registry broadcasts only operational parameters — capital, venue access, latency, risk capacity. No trading signals, no price predictions, no strategy logic ever crosses the wire. A firm receiving an execution request knows what to execute, not why.

**2. Sub-millisecond execution delegation.**
Using a fixed-size 68-byte binary message protocol over TCP with TCP_NODELAY (Nagle's algorithm disabled), execution requests and confirmations complete their round-trip in under 1 millisecond over a private co-located network — well within the window of most mid-frequency arbitrage opportunities.

**3. Algorithmic profit splitting.**
Profit distribution is computed by a pre-agreed formula embedded in code, not negotiated by humans after the fact. The formula weights contributions: signal detection, capital deployment, venue access, execution speed. An immutable append-only ledger records every collaborative trade with CRC32 checksums — auditable by both firms and regulators.

**4. Self-healing connectivity.**
Heartbeat messages every 100 milliseconds detect peer failures. If a peer misses 5 heartbeats, it is marked unavailable and removed from the routing table. Execution requests are never sent to unavailable peers. The system degrades gracefully — a firm always falls back to solo execution.

**5. AI-powered retail bridge.**
The same data pipeline that powers the mesh also feeds a dashboard with an LLM-powered plain-English signal explainer, making HFT-grade market intelligence accessible to retail users for the first time — democratising the information advantage that currently exists only behind closed doors in institutional trading floors.

### How the Solution Directly Solves the Problem

The problem is structural isolation — each small firm fighting alone. The solution is cooperative infrastructure — a shared protocol layer that multiplies capability without diluting competitive advantage.

| Problem | Mesh Solution |
|---|---|
| Firm A has fast network but no capital | Execution delegation to capital-rich Firm B |
| Firm B has capital but weak signal | Receives execution requests from signal-strong Firm A |
| Firm C has ML models but no co-location | Contributes prediction layer, execution via co-located peers |
| All three lose individually | All three win collaboratively |

### Unique Innovation

No existing protocol, platform, or regulatory framework enables real-time, pre-trade, peer-to-peer capability delegation between competing HFT firms. The closest analogues — dark pools, prime brokerage, and consortium FX settlement systems like CLS Bank — all operate post-trade or through a centralised intermediary that charges significant fees and holds the power. HFT Mesh is decentralised, real-time, and pre-trade. It is a new category.

### Why It Is Better Than Current Alternatives

**Prime brokerage:** A Goldman Sachs or Morgan Stanley prime broker provides infrastructure access to small firms — but charges 15–30 basis points per trade, retains custody of assets, and the firm has zero ownership of the infrastructure. The prime broker is a landlord, not a partner.

**Dark pools:** Anonymous matching between institutional participants — but there is no collaboration, no capability sharing, and no profit-sharing mechanism. Two firms in a dark pool are still competing against each other, just anonymously.

**Consortium FX settlement (CLS Bank):** Shared post-trade settlement infrastructure — but this operates after execution, not before. It does not help firms capture opportunities they lack the real-time capability to execute alone.

**HFT Mesh is the only solution that operates at the pre-trade, real-time, capability layer — where the actual competitive disadvantage of small firms exists.**

---

## SLIDE 4 — TECHNICAL ARCHITECTURE

### System Overview

The full system comprises five integrated layers, each building on the one below it. Every layer is designed with a single governing constraint: **the hot path (the code executing on market data in real time) must never block, never allocate heap memory, and never call the operating system.**

```
┌─────────────────────────────────────────────────────────────────┐
│  LAYER 5: Dashboard + AI Layer                                  │
│  React frontend · Python FastAPI · WebSocket · LLM explainer    │
├─────────────────────────────────────────────────────────────────┤
│  LAYER 4: AI / ML Signal Layer                                  │
│  Feature engine · XGBoost predictor · Flash crash detector      │
├─────────────────────────────────────────────────────────────────┤
│  LAYER 3: Mesh Network Protocol                                 │
│  Epoll TCP server · Capability registry · Execution delegation  │
├─────────────────────────────────────────────────────────────────┤
│  LAYER 2: Order Book Engine                                     │
│  Lock-free book · Memory pool · SPSC ring buffer · Profiler     │
├─────────────────────────────────────────────────────────────────┤
│  LAYER 1: Market Data Feed                                      │
│  WebSocket / UDP feed · Binary parser · Timestamp              │
└─────────────────────────────────────────────────────────────────┘
```

### Layer 1 — Market Data Feed

**What it does:** Connects to exchange data sources and receives live price data — specifically the best bid price, best bid quantity, best ask price, and best ask quantity for target instruments.

**In development / testing:** WebSocket connections to Binance and Bybit public streams. No API key required for public market data. The `btcusdt@bookTicker` stream delivers best bid/ask the instant it changes.

**In production:** UDP multicast subscription to exchange data feeds (NASDAQ ITCH 5.0 protocol for US equities, or equivalent binary protocols for other venues). Received via kernel bypass networking (DPDK or Solarflare OpenOnload) to eliminate the 5–50µs overhead of the Linux kernel network stack.

**Key technical decisions:**
- All prices stored as `int64_t` in integer tick units. $183.50 = 18350. Never floating point — IEEE 754 rounding errors are unacceptable in financial arithmetic.
- Feed parser thread runs in a busy-poll loop (100% CPU utilisation on a dedicated core) — never sleeps, never yields, never blocks.
- Every received message is timestamped immediately with `__rdtsc()` (CPU cycle counter, ~7ns cost) before any processing.
- Parsed messages are written into a SPSC ring buffer for the order book thread. No mutex. No lock. ~5ns hand-off.

**Data flow:**
```
Exchange → UDP/WebSocket packet → NIC ring buffer → Feed parser thread
→ [timestamp with rdtsc] → [decode binary message] → SPSC ring buffer
```

### Layer 2 — Order Book Engine

**What it does:** Maintains an accurate, microsecond-fresh representation of the market's bid and ask price levels in memory. Every strategy, every signal, every execution decision reads from this book.

**Data structures:**

*Order struct (48 bytes):*
```
order_id    : uint64  — unique ID assigned by exchange
price       : int64   — in ticks (integer)
quantity    : int32   — shares / contracts
side        : uint8   — 0=bid, 1=ask
flags       : uint8   — reserved
timestamp   : uint64  — rdtsc tick when received
pool_index  : uint32  — index in memory pool
```

*PriceLevel struct:*
```
price         : int64   — this level's price
total_qty     : int32   — sum of all orders at this price
order_queue   : intrusive linked list of Order* (FIFO, price-time priority)
```

*Book structure:*
```
bids : std::map<int64_t, PriceLevel>  — sorted ascending by price, best bid = rbegin()
asks : std::map<int64_t, PriceLevel>  — sorted ascending by price, best ask = begin()
order_map : std::unordered_map<uint64_t, Order*>  — O(1) lookup by order_id
```

**Three operations (from the SPSC ring):**

1. **Add order** — Create Order from pool (5ns, no malloc). Insert into bids or asks at its price. Add to order_map. Update PriceLevel total_qty.

2. **Cancel order** — Look up order_id in order_map (O(1)). Remove from PriceLevel queue. Decrement total_qty. If PriceLevel empty, erase from map. Return Order to pool.

3. **Execute order** — Reduce Order quantity. Reduce PriceLevel total_qty. If fully filled, remove same as cancel. Emit trade event to feature engine.

**Memory pool allocator:**
Pre-allocated flat array of 1,000,000 Order structs at startup (~48MB). Free-list implemented as an intrusive stack using the `pool_index` field. Allocation: pop from free list + placement new = ~5ns. Deallocation: destructor + push to free list = ~3ns. Zero heap allocation after startup.

**SPSC ring buffer:**
Fixed-size array of 4,096 MarketEvent structs (power of 2 for bitwise wraparound). `std::atomic<uint64_t> head` owned by feed parser. `std::atomic<uint64_t> tail` owned by book engine. Head and tail on separate 64-byte cache lines (60 bytes of padding between them) to prevent false sharing. Memory ordering: `memory_order_release` on write, `memory_order_acquire` on read.

**Latency profiler:**
`__rdtsc()` stamps at: packet received, message decoded, book updated, signal computed. Deltas stored in a secondary log ring (not printed). Background thread reads log ring, converts ticks to nanoseconds (calibrated at startup via `clock_gettime`), writes to CSV. Identifies which pipeline stage is the bottleneck to within 10ns resolution.

### Layer 3 — Mesh Network Protocol

**What it does:** Enables real-time communication between peer firms. Each firm runs one Mesh Node process alongside its book engine. The node accepts incoming connections from peer firms and maintains outgoing connections to all known peers. All managed in a single epoll event loop — no blocking, no threads per connection.

**Network architecture:**
```
Firm A Mesh Node ←──── private VLAN ────→ Firm B Mesh Node
        ↕                                         ↕
Firm A Order Book                         Firm B Order Book
```

**Server startup sequence:**
1. `socket(AF_INET, SOCK_STREAM, 0)` — create TCP socket
2. `setsockopt(SO_REUSEADDR)` — allow immediate restart
3. `bind()` + `listen()` — attach to port, accept incoming connections
4. `fcntl(O_NONBLOCK)` on all sockets — non-blocking I/O
5. `epoll_create1()` — create epoll instance
6. `epoll_ctl(EPOLL_CTL_ADD)` — register server socket with epoll
7. Enter event loop: `epoll_wait()` → process ready fds → repeat

**TCP_NODELAY:** Set on every peer connection. Disables Nagle's algorithm (which batches small writes, adding up to 40ms delay). Every message is sent immediately — mandatory for sub-millisecond protocol operation.

**Edge-triggered epoll:** All peer sockets registered with `EPOLLET` (edge-triggered). Notification fires once per event. The read handler must call `recv()` in a loop until it returns `EAGAIN` (no more data). More efficient than level-triggered — avoids repeated spurious wake-ups on the same data.

**Message protocol — fixed 68-byte binary struct:**

All messages are fixed-size C++ structs with `#pragma pack(1)`. No JSON. No strings on the wire. No parsing — just a `memcpy` into the struct type. Zero parsing overhead.

```
Header (13 bytes — common to all messages):
  type          : uint8   — message type enum
  firm_id       : uint32  — sender's unique firm ID
  timestamp_ns  : uint64  — rdtsc-calibrated nanoseconds
  seq_num       : uint64  — monotonically increasing per firm

Payload (55 bytes — union, interpretation depends on type):

HELLO (type=1):
  firm_name     : char[32]
  protocol_ver  : uint16
  reserved      : uint8[21]

CAPABILITY (type=2) — sent every 10ms:
  available_capital  : uint64   — in cents (not dollars)
  venue_mask         : uint32   — bit per venue, NYSE=bit0, NASDAQ=bit1...
  latency_us         : uint32   — self-reported RTT to best venue
  risk_headroom      : uint32   — max additional orders absorbable now
  reserved           : uint8[35]

EXEC_REQUEST (type=3):
  request_id         : uint64
  instrument_id      : uint32
  side               : uint8    — 0=buy, 1=sell
  quantity           : uint32
  limit_price        : int64    — in ticks
  time_limit_us      : uint32   — abort if not confirmed within this
  profit_split_pct   : uint8    — requesting firm's share (0-100)
  reserved           : uint8[26]

EXEC_CONFIRM (type=4):
  request_id         : uint64   — matches the original EXEC_REQUEST
  fill_price         : int64
  fill_qty           : uint32
  execution_lat_us   : uint32   — actual execution latency
  reserved           : uint8[31]

HEARTBEAT (type=5):
  reserved           : uint8[55]
```

**Capability registry:**
Each node maintains an in-memory `CapabilityTable` — one row per connected peer, columns = latest values from their CAPABILITY messages. Updated every time a CAPABILITY message arrives. When a firm needs an execution partner, it scans this table: find peers with sufficient capital, access to required venue, latency within the time budget, and available risk headroom. O(n) scan over the peer table — with ≤20 peers this is ~100ns.

**Execution delegation flow:**
1. Firm A's signal engine detects opportunity, cannot execute alone
2. Scan CapabilityTable → select best partner (Firm B)
3. Build EXEC_REQUEST: instrument, side, qty, price, time_limit_us=800, split_pct=60
4. `send()` to Firm B's socket — TCP_NODELAY sends immediately
5. Add request_id to pending table with expiry timer
6. Firm B's epoll fires → recv EXEC_REQUEST → risk check (50ns) → if pass → route to exchange → await fill
7. Firm B receives fill → builds EXEC_CONFIRM → `send()` back to Firm A
8. Firm A receives EXEC_CONFIRM → mark opportunity closed → write to profit ledger
9. If timer expires before EXEC_CONFIRM → cancel own leg if unfilled → mark Firm B temporarily unavailable

**Profit split ledger:**
Append-only binary log file on a neutral co-located server. Fixed 128-byte records: trade_id, timestamp, both firm IDs, instrument, fill_price, fill_qty, gross_pnl, split percentages, each firm's net_pnl after costs, CRC32 checksum. Records written with `pwrite()` (atomic positional write). Write index is an `std::atomic<uint64_t>` at the start of the file. Neither firm can modify or delete existing records. Both firms and regulators can audit the full history.

### Layer 4 — AI / ML Signal Layer

**What it does:** Transforms raw order book state into predictive signals and risk alerts. Runs on a separate thread reading the book's mmap snapshot file. Output is written back to a second mmap region that the dashboard and ML inference process consume.

**Feature engine (C++, runs every tick):**

After every book update, compute a `FeatureVector` struct:

```
timestamp_ns       : uint64   — when computed
symbol_id          : uint32
bid_ask_spread     : int32    — best_ask - best_bid in ticks
mid_price          : int64    — (best_bid + best_ask) / 2
imbalance_l1       : float    — (bid_qty_l1 - ask_qty_l1) / (bid_qty_l1 + ask_qty_l1)
imbalance_l5       : float    — same formula across top 5 levels
total_bid_depth    : int64    — sum of qty across top 5 bid levels
total_ask_depth    : int64    — sum of qty across top 5 ask levels
cancel_rate_1s     : float    — cancels per second, rolling 1s window
trade_intensity_1s : float    — fills per second, rolling 1s window
mid_return_10t     : float    — mid-price change over last 10 ticks
mid_return_50t     : float    — mid-price change over last 50 ticks
arb_gap_bps        : float    — current cross-exchange gap in basis points
```

**Price direction predictor (Python / XGBoost):**

*Training:* Collect `FeatureVector` snapshots from historical data (LOBster dataset — real NASDAQ L2 data). Label each: did mid-price go up or down in the next 100ms? Chronological train/test split — never shuffle financial time series. Train XGBoost binary classifier. Key hyperparameters: `max_depth=5`, `n_estimators=300`, `learning_rate=0.05`.

*Inference:* Python process reads latest FeatureVector from mmap every 100ms. Runs `model.predict_proba()`. Writes prediction (UP/DOWN/NEUTRAL) and confidence (0.0–1.0) back to a 16-byte output struct in a second mmap region. Dashboard reads this struct. Total inference latency: ~2ms (acceptable for a display layer).

**Flash crash early warning classifier (Python / sklearn):**

*Labelling:* In historical data, find all moments where price moved >0.3% in under 2 seconds (flash crash events). Label snapshots 5 seconds before each event as FRAGILE (1). All other snapshots: STABLE (0). Class imbalance is extreme — crashes are rare. Use `class_weight='balanced'` and SMOTE oversampling.

*Features:* Depth thinning rate (depth_now / depth_5s_ago), spread widening ratio, cancel rate spike (cancel_rate_now / cancel_rate_baseline), time since last fill (fill gap), trade intensity drop.

*Output:* Real-time fragility probability 0.0–1.0. If P > 0.80 → emit FRAGILE_ALERT struct to mmap → dashboard shows red banner with contributing features.

**LLM signal explainer (Python / Claude API):**

Every second, serialize latest FeatureVector + ML predictions to JSON. Send to Claude API with system prompt: *"You are a market analyst. Given these order book features and model predictions, write exactly one sentence (maximum 20 words) describing current market conditions in plain English for a retail investor. Avoid technical jargon."*

Response arrives in ~300ms. Cached and pushed to dashboard via WebSocket. If API call exceeds 1 second, display previous explanation — never block UI. This transforms machine-readable HFT signals into human-readable market intelligence accessible to anyone.

### Layer 5 — Dashboard + Observability Layer

**Observability (solving the dark box problem):**

The hot thread (Layers 1–2) must never call `cout`, `fwrite`, or any syscall during trading. The solution: a two-stage async logging pipeline.

The hot thread writes a fixed 32-byte `LogEntry` struct (not a formatted string) to a dedicated log SPSC ring buffer — cost: ~5ns. A background drain thread reads from this ring, formats strings, converts rdtsc ticks to nanoseconds, and writes to a memory-mapped log file via `mmap()`. The OS flushes to disk asynchronously. Zero blocking on the hot thread.

Book state is captured via periodic snapshots (not per-event): every 100µs, write a `BookSnapshot` struct (120 bytes, top-5 bid/ask levels + all features) to a circular mmap buffer. 10,000 snapshots/second — enough for any dashboard, vastly less volume than per-event logging.

**Dashboard architecture:**

```
mmap snapshot file ──→ Python reader process ──→ FastAPI WebSocket server
                                                          ↓
ML prediction mmap ──→ (merged into state object)   Browser frontend
                                                     (React + Recharts)
LLM explainer ───────→ (merged into state object)
Mesh node status ────→ (merged into state object)
```

Python reader polls mmap every 100ms. Merges all data sources into a single JSON state object. Pushes to all connected browser clients over WebSocket. No polling from browser — pure push model.

**Dashboard panels:**

1. **Order book depth chart** — Recharts AreaChart. X axis = price levels. Y axis = cumulative quantity. Bids area green, asks area red. The visual shape instantly shows imbalance and liquidity.

2. **Imbalance gauge** — Horizontal bar −1.0 to +1.0. Current imbalance as needle. Red (sell pressure) → white (neutral) → green (buy pressure). Animated, updates every 100ms.

3. **Arbitrage opportunity panel** — Exchange A best ask, Exchange B best bid, real-time gap in ticks and basis points, gap history chart, opportunity count last 60 seconds, total estimated profit captured.

4. **Mesh network status** — One row per peer firm. Connection dot (green/red). Latest capability values: capital available, venue access bitmask, self-reported latency. Last HEARTBEAT received. Active delegated trades count.

5. **ML prediction panel** — Current direction prediction (UP / DOWN / NEUTRAL), confidence percentage, feature importance bar chart (which feature drove this prediction), prediction accuracy last 1000 calls.

6. **Flash crash alert banner** — Hidden normally. Full-width red banner when fragility > 0.80. Shows: current fragility score, top 3 contributing features, time since fragility first exceeded 0.50. Persists until score drops below 0.40.

7. **LLM plain-English signal** — Large-text panel, updated every second. Smooth fade transition between explanations. This is the retail user's entry point — one sentence telling them what the market is doing right now.

8. **Latency waterfall** — Bar chart: time spent in each pipeline stage (parse, book update, feature compute, signal evaluate) updated every 100ms. Shows p50, p95, p99 per stage. Allows real-time identification of bottlenecks.

---

## SLIDE 5 — PROCESS FLOW DIAGRAM

### End-to-End Flow: From Market Event to Collaborative Execution

```
STEP 1 — MARKET EVENT
Exchange broadcasts price update (UDP multicast / WebSocket)
↓
STEP 2 — FEED PARSING (Firm A, hot thread)
NIC receives packet → Feed parser decodes binary message
→ Timestamps with rdtsc (~7ns)
→ Writes MarketEvent to SPSC ring buffer (~5ns)
↓
STEP 3 — BOOK UPDATE (Firm A, book thread)
Reads from SPSC ring → Updates bids/asks map
→ Allocates/releases Orders via memory pool (~5ns)
→ Updates order_map (O(1) lookup)
→ Writes BookSnapshot to mmap every 100µs
↓
STEP 4 — FEATURE COMPUTATION (Firm A, book thread)
After every update → Computes FeatureVector
→ Imbalance, spread, depth, cancel rate, arb gap
→ Writes FeatureVector to mmap
↓
STEP 5 — OPPORTUNITY DETECTION (Firm A, signal thread)
Reads FeatureVector → Checks arb_gap_bps > threshold
→ Checks Firm A's own resources: insufficient capital for full size
→ OPPORTUNITY IDENTIFIED: needs execution partner
↓
STEP 6 — PARTNER SELECTION (Firm A, mesh thread)
Scans CapabilityTable → Finds Firm B:
  capital: $800,000 ✓
  venue_mask: has Exchange B access ✓
  latency_us: 180µs ✓ (within 800µs budget)
  risk_headroom: 15 orders ✓
→ Firm B selected as execution partner
↓
STEP 7 — EXECUTION REQUEST (Firm A → Firm B)
Build EXEC_REQUEST struct (68 bytes):
  instrument: BTCUSDT
  side: SELL (Firm B sells on Exchange B)
  qty: 5000 units
  limit_price: 18355 (ticks)
  time_limit_us: 800
  profit_split_pct: 60 (Firm A gets 60%)
→ send() over TCP, TCP_NODELAY sends immediately
→ Wire time: ~200µs on co-located private network
↓
STEP 8 — AUTO-DECISION (Firm B, mesh thread)
Receives EXEC_REQUEST → Risk engine checks:
  capital sufficient? YES
  instrument within limits? YES
  time_limit_us achievable? YES
→ AUTO-ACCEPT in ~50ns
→ Routes SELL order to Exchange B via OUCH protocol
↓
SIMULTANEOUS — FIRM A EXECUTES OWN LEG
Firm A buys on Exchange A (BUY leg)
→ Fill received from Exchange A
↓
STEP 9 — EXECUTION CONFIRM (Firm B → Firm A)
Firm B receives fill from Exchange B
→ Build EXEC_CONFIRM: request_id, fill_price, fill_qty, latency
→ send() back to Firm A
→ Total round-trip: ~600µs from request to confirmation
↓
STEP 10 — PROFIT ATTRIBUTION
Both legs filled:
  Firm A bought at 18350, Firm B sold at 18355 → gross gap: 5 ticks
  Firm A's share: 60% → 3 ticks per unit × 5000 units = 15,000 ticks profit
  Firm B's share: 40% → 2 ticks per unit × 5000 units = 10,000 ticks profit
  Minus transaction costs (fees, slippage)
→ Both firms append trade record to shared ledger (CRC32 checksummed)
↓
STEP 11 — DASHBOARD UPDATE
BookSnapshot mmap updated → Python reader detects change
→ FastAPI pushes WebSocket message to browser
→ Dashboard shows: opportunity captured, gap closed, P&L updated
→ LLM explainer processes new features → generates plain-English update
→ Retail user sees: "Bitcoin prices equalised across exchanges after a brief pricing gap"
```

### Failure Handling Flow

```
IF Firm B does not confirm within 800µs:
→ Firm A cancels own leg if not yet filled
→ Marks Firm B unavailable for 5 seconds
→ Logs timeout event to ledger
→ Scans CapabilityTable for next best partner
→ If no partner available → executes solo at reduced size or skips

IF connection to Firm B drops:
→ epoll fires EPOLLHUP event
→ Firm B removed from CapabilityTable immediately
→ All pending requests to Firm B marked as timeout
→ Reconnection attempted every 1 second
→ System continues operating with remaining peers
```

---

## SLIDE 6 — IMPLEMENTATION METHODOLOGY & FEASIBILITY

### Implementation Phases

**Phase 0 — Data feed and arbitrage detection (Week 1–2)**

Build a WebSocket client in C++ connecting to Binance and Bybit public streams. No API key required. Receive live best bid/ask for BTC/USDT from both exchanges simultaneously. Implement gap detector: compute `bybit_bid − binance_ask` every time either price updates. Log opportunities with timestamp, both prices, gap in basis points.

*Outcome:* Working end-to-end price ingestion and opportunity detection. Proof that price gaps between exchanges exist and are detectable.

**Phase 1 — Order book engine (Week 3–6)**

Build the core C++ order book: `std::map`-based bids/asks, three message handlers (add/cancel/execute), memory pool allocator, SPSC ring buffer between feed and book threads, rdtsc latency profiler.

*Outcome:* Microsecond-latency order book updating from live exchange data. Latency waterfall showing time breakdown per pipeline stage.

**Phase 2 — Observability layer (Week 7–8)**

Build async lock-free logger (LogEntry struct → log ring → drain thread → mmap file) and periodic book snapshotter (BookSnapshot every 100µs → circular mmap buffer).

*Outcome:* Hot thread is completely I/O-free. Full system state visible externally via mmap without impacting trading latency.

**Phase 3 — Mesh network protocol (Week 9–14)**

Build epoll TCP server. Implement all five message types. Build capability registry. Implement execution delegation with timeout handling. Build profit split ledger. Test with two nodes on separate machines on a private network.

*Outcome:* Two-firm collaborative execution demonstrated end-to-end. Execution delegation completing in <1ms round-trip on local network.

**Phase 4 — AI / ML layer (Week 15–20)**

Build feature engine in C++. Train XGBoost predictor on LOBster dataset. Train flash crash classifier on labelled historical data. Build LLM explainer using Claude API.

*Outcome:* Real-time ML predictions with demonstrated accuracy on held-out historical data. LLM explanations generating coherent plain-English market commentary.

**Phase 5 — Dashboard and backtester (Week 21–24)**

Build Python FastAPI WebSocket server reading mmap. Build React frontend with all eight dashboard panels. Build tick-data backtester replaying ITCH historical data. Run paper trading for 2+ weeks.

*Outcome:* Full system running end-to-end on live data with paper trading. Dashboard accessible by non-technical users. Backtested P&L metrics available.

### Feasibility Assessment

**Technical feasibility: HIGH.**
Every component uses well-understood, battle-tested technology. C++ epoll TCP servers are the backbone of every high-performance networked system. SPSC ring buffers are a standard HFT pattern. XGBoost on financial tabular data is extensively validated in academic literature. The primary challenge is integration discipline — each layer must interface cleanly with the next.

**Data availability: HIGH.**
Binance and Bybit provide free public WebSocket streams with no authentication required. NASDAQ ITCH historical data is freely downloadable. The LOBster dataset provides real L2 order book data for ML training. No data sourcing cost in development.

**Regulatory feasibility: MEDIUM.**
Collaborative trading between firms requires pre-registration with relevant regulators (SEBI for India, SEC/FINRA for US). The execution delegation protocol is functionally equivalent to a pre-agreed algorithmic joint venture — a structure that exists in FX markets already. The profit split ledger provides the audit trail regulators require. Legal counsel needed for production deployment.

**Competitive moat: HIGH.**
The protocol itself is the innovation. Once firms onboard to the mesh, switching costs are high — they would lose access to the capability network. Network effects apply: each additional firm improves the capability table for all existing members, making the network more valuable as it grows.

### Technology Stack

| Layer | Technology |
|---|---|
| Market data feed | C++ · Boost.Beast · WebSocket / UDP |
| Order book engine | C++ 17 · STL · custom allocator |
| Inter-thread communication | SPSC ring buffer · std::atomic |
| Mesh network protocol | C++ · Linux epoll · TCP/IP |
| Latency profiling | x86 rdtsc · nanosecond resolution |
| Observability | mmap · async drain thread |
| ML feature engine | C++ · fixed FeatureVector struct |
| Price predictor | Python · XGBoost · LOBster dataset |
| Crash detector | Python · scikit-learn · imbalanced-learn |
| LLM explainer | Python · Claude API (claude-sonnet-4-6) |
| Dashboard backend | Python · FastAPI · WebSocket |
| Dashboard frontend | React · Recharts · TailwindCSS |
| Backtester | C++ · NASDAQ ITCH replay |

---

## SLIDE 7 — PROTOTYPE

- **GitHub Repository:** [link]
- **Demo Video (Max 3 Minutes):** [link]

*Prototype demonstrates:*
- Live BTC/USDT price ingestion from Binance + Bybit simultaneously
- Real-time arbitrage gap detection and logging
- Order book depth chart updating live on the dashboard
- Two-node mesh communication with capability broadcasting
- LLM plain-English signal explanation updating every second

---

*Project: HFT Mesh — Cooperative High-Frequency Trading Network*
*BuildVerse Hackathon 2026 · Deep Tech · Real Impact*
