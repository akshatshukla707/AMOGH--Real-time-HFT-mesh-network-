#pragma once
#include <map>
#include <unordered_map>
#include <vector>
#include <functional>
#include <iostream>
#include "types.hpp"
#include "memory_pool.hpp"
#include "ring_buffer.hpp"

// ─────────────────────────────────────────────────────────────
// Full L2 Order Book Engine
// Maintains bids and asks as sorted price maps.
// bids: descending (best bid = begin())
// asks: ascending  (best ask = begin())
// Three operations: ADD / CANCEL / EXECUTE — all O(log n) on map,
// O(1) on the order_map hash.
// ─────────────────────────────────────────────────────────────

struct BookOrder {
    uint64_t order_id;
    int64_t  price;
    int32_t  quantity;
    uint8_t  side;         // 0=bid, 1=ask
    uint64_t timestamp_ns;
    size_t   pool_index;   // unused; pool manages via pointer arithmetic
};

struct PriceLevel {
    int64_t              price;
    int64_t              total_qty{0};
    std::vector<uint64_t> order_ids; // FIFO queue of order IDs at this level
};

using BidMap = std::map<int64_t, PriceLevel, std::greater<int64_t>>; // best bid = begin()
using AskMap = std::map<int64_t, PriceLevel>;                         // best ask = begin()

class OrderBook {
public:
    static constexpr size_t MAX_ORDERS = 500'000;

    using OnTradeCallback = std::function<void(const Fill&)>;
    using OnSnapshotCallback = std::function<void(const BookSnapshot&)>;

    OrderBook(const char* symbol, uint8_t venue_id)
        : venue_id_(venue_id)
    {
        strncpy(symbol_, symbol, 15);
    }

    void setTradeCallback(OnTradeCallback cb) { trade_cb_ = std::move(cb); }
    void setSnapshotCallback(OnSnapshotCallback cb) { snap_cb_ = std::move(cb); }

    // ADD order to book
    void addOrder(uint64_t order_id, int64_t price, int32_t qty, uint8_t side,
                  uint64_t ts_ns)
    {
        auto* o = pool_.alloc();
        if (!o) { ++dropped_; return; }
        o->order_id    = order_id;
        o->price       = price;
        o->quantity    = qty;
        o->side        = side;
        o->timestamp_ns = ts_ns;

        order_map_[order_id] = o;

        if (side == 0) { // bid
            auto& level = bids_[price];
            level.price = price;
            level.total_qty += qty;
            level.order_ids.push_back(order_id);
        } else {         // ask
            auto& level = asks_[price];
            level.price = price;
            level.total_qty += qty;
            level.order_ids.push_back(order_id);
        }
        ++updates_;
        maybeSnapshot(ts_ns);
    }

    // CANCEL order
    void cancelOrder(uint64_t order_id, uint64_t ts_ns) {
        auto it = order_map_.find(order_id);
        if (it == order_map_.end()) return;

        BookOrder* o = it->second;
        removeFromLevel(o);
        pool_.free(o);
        order_map_.erase(it);
        ++updates_;
        maybeSnapshot(ts_ns);
    }

    // EXECUTE (partial or full fill)
    void executeOrder(uint64_t order_id, int32_t qty, int64_t price,
                      uint64_t ts_ns)
    {
        auto it = order_map_.find(order_id);
        if (it == order_map_.end()) return;

        BookOrder* o = it->second;
        int32_t filled = std::min(qty, o->quantity);
        o->quantity -= filled;

        // Update level qty
        if (o->side == 0) {
            auto lit = bids_.find(o->price);
            if (lit != bids_.end()) lit->second.total_qty -= filled;
        } else {
            auto lit = asks_.find(o->price);
            if (lit != asks_.end()) lit->second.total_qty -= filled;
        }

        // Emit fill event
        if (trade_cb_) {
            Fill f{};
            strncpy(f.order_id, std::to_string(order_id).c_str(), 31);
            strncpy(f.symbol, symbol_, 15);
            f.fill_price    = price;
            f.fill_qty      = filled;
            f.timestamp_ns  = ts_ns;
            f.venue_id      = venue_id_;
            trade_cb_(f);
        }

        if (o->quantity <= 0) {
            removeFromLevel(o);
            pool_.free(o);
            order_map_.erase(it);
        }
        ++updates_;
        maybeSnapshot(ts_ns);
    }

    // Best bid/ask
    int64_t bestBid() const {
        return bids_.empty() ? 0 : bids_.begin()->first;
    }
    int64_t bestAsk() const {
        return asks_.empty() ? 0 : asks_.begin()->first;
    }
    int64_t midPrice() const {
        if (bids_.empty() || asks_.empty()) return 0;
        return (bestBid() + bestAsk()) / 2;
    }
    int32_t spread() const {
        return (int32_t)(bestAsk() - bestBid());
    }

    // Compute imbalance at top N levels
    float imbalance(int levels = 1) const {
        int64_t bid_q = 0, ask_q = 0;
        int n = 0;
        for (auto& [p, lvl] : bids_) {
            bid_q += lvl.total_qty;
            if (++n >= levels) break;
        }
        n = 0;
        for (auto& [p, lvl] : asks_) {
            ask_q += lvl.total_qty;
            if (++n >= levels) break;
        }
        if (bid_q + ask_q == 0) return 0.f;
        return (float)(bid_q - ask_q) / (float)(bid_q + ask_q);
    }

    // Fill a FeatureVector from current book state
    FeatureVector buildFeatureVector(float arb_gap_bps = 0.f) const {
        FeatureVector fv{};
        fv.timestamp_ns    = nowNs();
        fv.mid_price       = midPrice();
        fv.bid_ask_spread  = spread();
        fv.imbalance_l1    = imbalance(1);
        fv.imbalance_l5    = imbalance(5);
        fv.arb_gap_bps     = arb_gap_bps;

        int n = 0;
        for (auto& [p, lvl] : bids_) {
            fv.total_bid_depth += lvl.total_qty;
            if (++n >= 5) break;
        }
        n = 0;
        for (auto& [p, lvl] : asks_) {
            fv.total_ask_depth += lvl.total_qty;
            if (++n >= 5) break;
        }

        // Rolling return: difference from stored mid prices
        if (mid_history_idx_ > 0) {
            size_t i10 = (mid_history_idx_ - std::min((size_t)10, mid_history_idx_)) & 255;
            size_t i50 = (mid_history_idx_ - std::min((size_t)50, mid_history_idx_)) & 255;
            if (fv.mid_price > 0) {
                fv.mid_return_10t = (float)(fv.mid_price - mid_history_[i10]) / fv.mid_price;
                fv.mid_return_50t = (float)(fv.mid_price - mid_history_[i50]) / fv.mid_price;
            }
        }
        return fv;
    }

    uint64_t totalUpdates() const { return updates_; }
    uint64_t droppedOrders() const { return dropped_; }

private:
    void removeFromLevel(BookOrder* o) {
        if (o->side == 0) {
            auto it = bids_.find(o->price);
            if (it != bids_.end()) {
                it->second.total_qty -= o->quantity;
                auto& ids = it->second.order_ids;
                ids.erase(std::remove(ids.begin(), ids.end(), o->order_id), ids.end());
                if (it->second.total_qty <= 0) bids_.erase(it);
            }
        } else {
            auto it = asks_.find(o->price);
            if (it != asks_.end()) {
                it->second.total_qty -= o->quantity;
                auto& ids = it->second.order_ids;
                ids.erase(std::remove(ids.begin(), ids.end(), o->order_id), ids.end());
                if (it->second.total_qty <= 0) asks_.erase(it);
            }
        }
    }

    void maybeSnapshot(uint64_t ts_ns) {
        // Emit snapshot every 100µs
        if (ts_ns - last_snapshot_ns_ < 100'000) return;
        last_snapshot_ns_ = ts_ns;

        // Record mid-price history
        mid_history_[mid_history_idx_ & 255] = midPrice();
        ++mid_history_idx_;

        if (!snap_cb_) return;
        BookSnapshot s{};
        strncpy(s.symbol, symbol_, 15);
        s.timestamp_ns = ts_ns;
        s.mid_price    = midPrice();
        s.spread_ticks = spread();
        s.imbalance_l1 = imbalance(1);
        s.imbalance_l5 = imbalance(5);
        s.venue_id     = venue_id_;

        int i = 0;
        for (auto& [p, lvl] : bids_) {
            if (i >= 5) break;
            s.bid_price[i] = p;
            s.bid_qty[i]   = lvl.total_qty;
            ++i;
        }
        i = 0;
        for (auto& [p, lvl] : asks_) {
            if (i >= 5) break;
            s.ask_price[i] = p;
            s.ask_qty[i]   = lvl.total_qty;
            ++i;
        }
        snap_cb_(s);
    }

    static uint64_t nowNs() {
        return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    char     symbol_[16]{};
    uint8_t  venue_id_{0};

    BidMap   bids_;
    AskMap   asks_;
    std::unordered_map<uint64_t, BookOrder*> order_map_;
    MemoryPool<BookOrder, 500'000> pool_;

    uint64_t updates_{0};
    uint64_t dropped_{0};
    uint64_t last_snapshot_ns_{0};

    // Rolling mid-price history (256 entries)
    int64_t  mid_history_[256]{};
    size_t   mid_history_idx_{0};

    OnTradeCallback    trade_cb_;
    OnSnapshotCallback snap_cb_;
};
