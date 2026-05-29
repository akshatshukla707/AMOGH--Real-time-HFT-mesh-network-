#pragma once
#include <cstdint>
#include <string>
#include <array>

// Market tick received from exchange
struct Tick {
    char symbol[16];
    double bid_price;
    double ask_price;
    double last_price;
    double volume;
    int64_t timestamp_ns;   // nanoseconds since epoch
    double bid_size[10];    // Level 2: 10 levels of bid depth
    double ask_size[10];    // Level 2: 10 levels of ask depth
    double bid_levels[10];  // bid price levels
    double ask_levels[10];  // ask price levels
};

// Signal from ML model
struct TradeSignal {
    char symbol[16];
    int direction;          // +1 = BUY, -1 = SELL, 0 = HOLD
    double confidence;      // 0.0 to 1.0
    double quantity;
    int64_t timestamp_ns;
    bool risk_approved;
    int regime;             // 0=trending, 1=mean-reverting, 2=volatile
};

// Order for FIX router
struct Order {
    char order_id[32];
    char symbol[16];
    char side;              // 'B' = Buy, 'S' = Sell
    double quantity;
    double price;
    char type;              // 'M' = Market, 'L' = Limit
    int64_t timestamp_ns;
    char firm_id[8];
    char client_id[8];
};
