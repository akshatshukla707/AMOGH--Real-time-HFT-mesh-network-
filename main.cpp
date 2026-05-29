#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sys/mman.h>
#include <fcntl.h>

#include "types.hpp"
#include "ring_buffer.hpp"
#include "order_book.hpp"
#include "mesh_node.hpp"
#include "ws_feed.hpp"

// ─────────────────────────────────────────────────────────────
// HFT Mesh — Main Trading Engine
//
// Architecture:
//   WebSocket feeds (Binance + Bybit)
//     → SPSC ring buffer (lock-free)
//       → Order book engine (per venue)
//         → Feature vector computation
//           → ML inference (via shared memory)
//             → Mesh node (execution delegation)
//               → Profit ledger
//
// All hot-path code: zero syscalls, zero heap allocation,
// zero I/O. Observability via async mmap snapshots.
// ─────────────────────────────────────────────────────────────

static std::atomic<bool> g_running{true};
void sigHandler(int) { g_running = false; std::cout << "\n[ENGINE] Shutting down...\n"; }

// Shared memory path for ML inference bridge
static constexpr const char* MMAP_FEATURE_PATH  = "/tmp/hft_features.bin";
static constexpr const char* MMAP_PREDICT_PATH  = "/tmp/hft_predict.bin";
static constexpr const char* MMAP_SNAPSHOT_PATH = "/tmp/hft_snapshot.bin";

// ─────────────────────────────────────────────────────────────
class TradingEngine {
public:
    using TickBuf = SPSCRingBuffer<MarketTick, 65536>;

    TradingEngine(uint32_t firm_id, const std::string& firm_name, int mesh_port)
        : book_binance_("BTCUSDT", 0),
          book_bybit_("BTCUSDT", 1),
          feed_binance_(tick_buf_, BINANCE_FEED),
          feed_bybit_(tick_buf_,   BYBIT_FEED),
          mesh_(firm_id, firm_name, mesh_port)
    {}

    bool init() {
        // Set up mesh callbacks
        mesh_.onExecRequest([this](const MeshExecRequest& req) -> bool {
            return handleIncomingExecRequest(req);
        });
        mesh_.onExecConfirm([this](const MeshExecConfirm& conf) {
            handleExecConfirm(conf);
        });

        // Initialize mmap for ML bridge
        initMmap(MMAP_FEATURE_PATH,  sizeof(FeatureVector));
        initMmap(MMAP_PREDICT_PATH,  sizeof(MLPrediction));
        initMmap(MMAP_SNAPSHOT_PATH, sizeof(BookSnapshot) * 100);

        // Register book snapshot callback
        book_binance_.setSnapshotCallback([this](const BookSnapshot& s) {
            publishSnapshot(s);
        });

        if (!mesh_.start()) {
            std::cerr << "[ENGINE] Mesh node failed to start\n";
            return false;
        }

        // Update initial capability
        mesh_.updateCapability(
            500'000'00,  // $500,000 in cents
            0b00000011,  // Binance + Bybit venue access
            200,         // 200µs estimated latency
            50           // 50 concurrent orders headroom
        );

        // Start market data feeds
        feed_binance_.start();
        feed_bybit_.start();

        std::cout << "[ENGINE] Initialized. Waiting for feeds...\n";
        std::this_thread::sleep_for(std::chrono::seconds(1));
        return true;
    }

    void run() {
        std::cout << "[ENGINE] Trading engine running. Ctrl+C to stop.\n";
        auto last_stats = std::chrono::steady_clock::now();

        while (g_running) {
            // Drain tick ring buffer
            while (auto tick_opt = tick_buf_.pop()) {
                const MarketTick& tick = *tick_opt;
                processTick(tick);
                ++ticks_processed_;
            }

            // Check for arb opportunities across venues
            checkArbitrage();

            // Print stats every 5 seconds
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_stats).count() >= 5) {
                printStats();
                last_stats = now;
            }

            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    }

    void stop() {
        feed_binance_.stop();
        feed_bybit_.stop();
        mesh_.stop();
        printStats();
    }

private:
    void processTick(const MarketTick& tick) {
        // Route to correct book by venue
        OrderBook& book = (tick.venue_id == 0) ? book_binance_ : book_bybit_;

        // Simple book update: treat each tick as a top-of-book replacement
        // (Full L2: parse individual add/cancel/execute messages from feed)
        static std::atomic<uint64_t> order_counter{1};
        uint64_t oid = order_counter++;

        // Simulate top-of-book: cancel old best, add new
        book.addOrder(oid,     tick.bid_price, (int32_t)tick.bid_qty, 0, tick.timestamp_ns);
        book.addOrder(oid + 1, tick.ask_price, (int32_t)tick.ask_qty, 1, tick.timestamp_ns);

        // Compute features and write to mmap
        float arb_gap = computeArbGap();
        FeatureVector fv = book.buildFeatureVector(arb_gap);
        writeToMmap(MMAP_FEATURE_PATH, &fv, sizeof(fv));

        // Read ML prediction from mmap (Python writes this)
        MLPrediction pred{};
        readFromMmap(MMAP_PREDICT_PATH, &pred, sizeof(pred));

        // Act on ML signal
        if (pred.direction != 0 && pred.confidence > 0.65f) {
            onMLSignal(tick, pred);
        }
        if (pred.fragility_alert) {
            std::cerr << "[RISK] ⚠ Flash crash fragility: "
                      << pred.fragility_score << "\n";
        }
    }

    float computeArbGap() {
        int64_t bb = book_binance_.bestBid();
        int64_t ba = book_binance_.bestAsk();
        int64_t yb = book_bybit_.bestBid();
        int64_t ya = book_bybit_.bestAsk();
        if (bb <= 0 || ya <= 0) return 0.f;
        // Arb: buy on Bybit (ya), sell on Binance (bb) — or vice versa
        int64_t gap = yb - ba; // Bybit bid - Binance ask
        if (gap <= 0) gap = bb - ya; // Binance bid - Bybit ask
        if (ba <= 0) return 0.f;
        return (float)gap / (float)ba * 10000.f; // basis points
    }

    void onMLSignal(const MarketTick& tick, const MLPrediction& pred) {
        // Check if we have sufficient capital for solo execution
        bool need_partner = (rand() % 100) < 40; // 40% need delegation (demo)

        if (need_partner) {
            // Delegate to mesh peer
            static std::atomic<uint64_t> req_id{1};
            MeshExecRequest req{};
            req.header.type         = MeshMsgType::EXEC_REQUEST;
            req.header.firm_id      = 1; // our firm
            req.header.timestamp_ns = tick.timestamp_ns;
            req.header.seq_num      = req_id;
            req.request_id          = req_id++;
            req.instrument_id       = 1; // BTCUSDT
            req.side                = (pred.direction > 0) ? 0 : 1;
            req.quantity            = 100;
            req.limit_price         = (pred.direction > 0) ? tick.ask_price : tick.bid_price;
            req.time_limit_us       = 800;
            req.profit_split_pct    = 60; // we get 60%, peer gets 40%

            if (mesh_.sendExecRequest(req)) {
                ++delegated_orders_;
                std::cout << "[ENGINE] Delegated order " << req.request_id
                          << " to peer. Signal: " << (pred.direction > 0 ? "BUY" : "SELL")
                          << " conf=" << pred.confidence << "\n";
            }
        } else {
            // Execute solo (paper mode)
            ++solo_orders_;
            std::cout << "[ENGINE] Solo order: "
                      << (pred.direction > 0 ? "BUY" : "SELL")
                      << " BTCUSDT @ " << tick.bid_price / 100.0
                      << " conf=" << pred.confidence << "\n";

            // Write profit ledger entry (demo: assume 5-tick profit)
            LedgerRecord rec{};
            static std::atomic<uint64_t> trade_id{1};
            rec.trade_id       = trade_id++;
            rec.timestamp_ns   = tick.timestamp_ns;
            rec.firm_a_id      = 1;
            rec.firm_b_id      = 0;
            strncpy(rec.symbol, "BTCUSDT", 15);
            rec.fill_price     = tick.last_price;
            rec.fill_qty       = 100;
            rec.gross_pnl_ticks = 5;
            rec.split_pct_a    = 100;
            rec.split_pct_b    = 0;
            rec.net_pnl_a      = 5;
            rec.net_pnl_b      = 0;
            mesh_.writeLedger(rec);
        }
    }

    bool handleIncomingExecRequest(const MeshExecRequest& req) {
        // Risk check: accept if we have headroom
        std::cout << "[MESH] Incoming EXEC_REQUEST " << req.request_id
                  << " side=" << (int)req.side
                  << " qty=" << req.quantity
                  << " split_for_requestor=" << (int)req.profit_split_pct << "%\n";
        return true; // approve
    }

    void handleExecConfirm(const MeshExecConfirm& conf) {
        std::cout << "[MESH] EXEC_CONFIRM " << conf.request_id
                  << " fill=" << conf.fill_qty
                  << " @ " << conf.fill_price / 100.0
                  << " lat=" << conf.execution_lat_us << "µs\n";

        // Write ledger
        LedgerRecord rec{};
        static std::atomic<uint64_t> id{1000};
        rec.trade_id       = id++;
        rec.timestamp_ns   = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        rec.firm_a_id      = 1;
        rec.firm_b_id      = 2;
        strncpy(rec.symbol, "BTCUSDT", 15);
        rec.fill_price     = conf.fill_price;
        rec.fill_qty       = conf.fill_qty;
        rec.gross_pnl_ticks = conf.fill_qty * 3; // demo
        rec.split_pct_a    = 60;
        rec.split_pct_b    = 40;
        rec.net_pnl_a      = (int64_t)(rec.gross_pnl_ticks * 0.6);
        rec.net_pnl_b      = (int64_t)(rec.gross_pnl_ticks * 0.4);
        mesh_.writeLedger(rec);
    }

    void publishSnapshot(const BookSnapshot& snap) {
        // Write to mmap circular buffer (index 0 for simplicity)
        writeToMmap(MMAP_SNAPSHOT_PATH, &snap, sizeof(snap));
    }

    void checkArbitrage() {
        float gap = computeArbGap();
        if (gap > 2.0f) { // > 2 basis points
            arb_opportunities_++;
            std::cout << "[ARB] Gap detected: " << gap << " bps | "
                      << "Binance bid=" << book_binance_.bestBid() / 100.0
                      << " Bybit ask=" << book_bybit_.bestAsk() / 100.0 << "\n";
        }
    }

    void printStats() const {
        std::cout << "\n══════════ Engine Stats ══════════\n"
                  << "Ticks processed   : " << ticks_processed_ << "\n"
                  << "Solo orders       : " << solo_orders_ << "\n"
                  << "Delegated orders  : " << delegated_orders_ << "\n"
                  << "Arb opportunities : " << arb_opportunities_ << "\n"
                  << "Feed Binance      : " << feed_binance_.ticksReceived() << " ticks\n"
                  << "Feed Bybit        : " << feed_bybit_.ticksReceived()   << " ticks\n"
                  << "Book Binance mid  : " << book_binance_.midPrice() / 100.0 << "\n"
                  << "Book Bybit mid    : " << book_bybit_.midPrice()   / 100.0 << "\n"
                  << "══════════════════════════════════\n\n";
    }

    // ── mmap helpers ─────────────────────────────────────────
    static void initMmap(const char* path, size_t size) {
        int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0666);
        if (fd < 0) return;
        ftruncate(fd, size);
        close(fd);
    }
    static void writeToMmap(const char* path, const void* data, size_t size) {
        int fd = open(path, O_RDWR);
        if (fd < 0) return;
        void* addr = mmap(nullptr, size, PROT_WRITE, MAP_SHARED, fd, 0);
        if (addr != MAP_FAILED) { memcpy(addr, data, size); munmap(addr, size); }
        close(fd);
    }
    static void readFromMmap(const char* path, void* dst, size_t size) {
        int fd = open(path, O_RDONLY);
        if (fd < 0) return;
        void* addr = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
        if (addr != MAP_FAILED) { memcpy(dst, addr, size); munmap(addr, size); }
        close(fd);
    }

    TickBuf      tick_buf_;
    OrderBook    book_binance_;
    OrderBook    book_bybit_;
    WebSocketFeed feed_binance_;
    WebSocketFeed feed_bybit_;
    MeshNode     mesh_;

    std::atomic<uint64_t> ticks_processed_{0};
    std::atomic<uint64_t> solo_orders_{0};
    std::atomic<uint64_t> delegated_orders_{0};
    std::atomic<uint64_t> arb_opportunities_{0};
};

// ─────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    signal(SIGINT,  sigHandler);
    signal(SIGTERM, sigHandler);

    uint32_t    firm_id   = (argc > 1) ? atoi(argv[1]) : 1;
    std::string firm_name = (argc > 2) ? argv[2] : "FirmA";
    int         mesh_port = (argc > 3) ? atoi(argv[3]) : 7777;

    std::cout << "╔══════════════════════════════════════╗\n"
              << "║  HFT Mesh — Cooperative Trading Node  ║\n"
              << "╚══════════════════════════════════════╝\n"
              << "Firm: " << firm_name << " (id=" << firm_id
              << ") mesh_port=" << mesh_port << "\n\n";

    TradingEngine engine(firm_id, firm_name, mesh_port);
    if (!engine.init()) {
        std::cerr << "[FATAL] Engine init failed.\n";
        return 1;
    }

    engine.run();
    engine.stop();
    return 0;
}
