#pragma once
#include <cstdint>
#include <cstring>
#include <atomic>
#include <chrono>

// ─────────────────────────────────────────────────────────────
// All prices stored as int64 in tick units (e.g. $183.50 = 18350)
// NEVER use floating point for financial arithmetic
// ─────────────────────────────────────────────────────────────

// Raw market tick from exchange
struct MarketTick {
    char     symbol[16];
    int64_t  bid_price;       // best bid in ticks
    int64_t  ask_price;       // best ask in ticks
    int64_t  last_price;
    int64_t  bid_qty;
    int64_t  ask_qty;
    uint64_t timestamp_ns;    // nanoseconds since epoch
    uint8_t  venue_id;        // 0=Binance, 1=Bybit, 2=Alpaca ...
};

// Full Level-2 book snapshot (top 5 levels)
struct BookSnapshot {
    char     symbol[16];
    uint64_t timestamp_ns;
    int64_t  bid_price[5];
    int64_t  ask_price[5];
    int64_t  bid_qty[5];
    int64_t  ask_qty[5];
    int64_t  mid_price;
    int32_t  spread_ticks;
    float    imbalance_l1;    // (bidQ - askQ) / (bidQ + askQ)
    float    imbalance_l5;
    uint8_t  venue_id;
};

// Feature vector for ML model (computed after each book update)
struct FeatureVector {
    uint64_t timestamp_ns;
    uint32_t symbol_id;
    int32_t  bid_ask_spread;
    int64_t  mid_price;
    float    imbalance_l1;
    float    imbalance_l5;
    int64_t  total_bid_depth;
    int64_t  total_ask_depth;
    float    cancel_rate_1s;
    float    trade_intensity_1s;
    float    mid_return_10t;
    float    mid_return_50t;
    float    arb_gap_bps;       // cross-exchange gap in basis points
};

// ML model output (written back to mmap)
struct MLPrediction {
    uint64_t timestamp_ns;
    int8_t   direction;         // +1=UP, -1=DOWN, 0=NEUTRAL
    float    confidence;        // 0.0 to 1.0
    float    fragility_score;   // flash-crash probability
    bool     fragility_alert;   // true if score > 0.80
    char     explanation[128];  // LLM plain-English sentence
};

// Order to be routed to exchange
struct Order {
    char     order_id[32];
    char     symbol[16];
    char     firm_id[8];
    char     client_id[8];
    int64_t  price;           // limit price in ticks (0 = market)
    int32_t  quantity;
    uint8_t  side;            // 0=BUY, 1=SELL
    uint8_t  type;            // 0=MARKET, 1=LIMIT
    uint8_t  venue_id;
    uint64_t timestamp_ns;
};

// Order fill / execution report
struct Fill {
    char     order_id[32];
    char     symbol[16];
    int64_t  fill_price;
    int32_t  fill_qty;
    uint64_t timestamp_ns;
    uint8_t  venue_id;
};

// ─────────────────────────────────────────────────────────────
// Mesh Network Protocol — Fixed 68-byte messages
// ─────────────────────────────────────────────────────────────
#pragma pack(1)

enum class MeshMsgType : uint8_t {
    HELLO        = 1,
    CAPABILITY   = 2,
    EXEC_REQUEST = 3,
    EXEC_CONFIRM = 4,
    HEARTBEAT    = 5,
    FRAGILE_ALERT = 6,
};

struct MeshHeader {
    MeshMsgType type;           // 1 byte
    uint32_t    firm_id;        // 4 bytes
    uint64_t    timestamp_ns;   // 8 bytes
    uint64_t    seq_num;        // 8 bytes
};                              // = 21 bytes

struct MeshHello {
    MeshHeader  header;
    char        firm_name[32];
    uint16_t    protocol_ver;
    uint8_t     reserved[13];
};                              // = 68 bytes total

struct MeshCapability {
    MeshHeader  header;
    uint64_t    available_capital;  // in cents
    uint32_t    venue_mask;         // bit per venue
    uint32_t    latency_us;         // self-reported RTT to best venue
    uint32_t    risk_headroom;      // max additional orders right now
    uint8_t     reserved[23];
};                              // = 68 bytes total

struct MeshExecRequest {
    MeshHeader  header;
    uint64_t    request_id;
    uint32_t    instrument_id;
    uint8_t     side;
    uint32_t    quantity;
    int64_t     limit_price;
    uint32_t    time_limit_us;
    uint8_t     profit_split_pct;   // requesting firm's share
    uint8_t     reserved[13];
};                              // = 68 bytes total

struct MeshExecConfirm {
    MeshHeader  header;
    uint64_t    request_id;
    int64_t     fill_price;
    uint32_t    fill_qty;
    uint32_t    execution_lat_us;
    uint8_t     status;         // 0=OK, 1=REJECTED, 2=TIMEOUT
    uint8_t     reserved[18];
};                              // = 68 bytes total

struct MeshHeartbeat {
    MeshHeader  header;
    uint8_t     reserved[47];
};                              // = 68 bytes total

#pragma pack()

// Peer state entry (in capability table)
struct PeerCapability {
    uint32_t firm_id;
    char     firm_name[32];
    uint64_t available_capital;
    uint32_t venue_mask;
    uint32_t latency_us;
    uint32_t risk_headroom;
    uint64_t last_heartbeat_ns;
    bool     connected;
    bool     available;
};

// Profit ledger record (128 bytes, append-only)
#pragma pack(1)
struct LedgerRecord {
    uint64_t trade_id;
    uint64_t timestamp_ns;
    uint32_t firm_a_id;
    uint32_t firm_b_id;
    char     symbol[16];
    int64_t  fill_price;
    int32_t  fill_qty;
    int64_t  gross_pnl_ticks;
    uint8_t  split_pct_a;
    uint8_t  split_pct_b;
    int64_t  net_pnl_a;
    int64_t  net_pnl_b;
    uint32_t crc32;
    uint8_t  reserved[19];
};                              // = 128 bytes
#pragma pack()
