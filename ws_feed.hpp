#pragma once
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <iostream>
#include <sstream>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "types.hpp"
#include "ring_buffer.hpp"

// ─────────────────────────────────────────────────────────────
// WebSocket feed client connecting to Binance / Bybit public
// streams for real BTC/USDT best bid/ask (no API key needed).
//
// Binance: wss://stream.binance.com:9443/ws/btcusdt@bookTicker
// Bybit:   wss://stream.bybit.com/v5/public/spot
//
// Uses raw OpenSSL TLS + manual WebSocket handshake for minimal
// dependencies. In production, replace with Boost.Beast.
// ─────────────────────────────────────────────────────────────

struct FeedConfig {
    const char* host;
    const char* path;
    const char* port;
    uint8_t     venue_id;
};

static constexpr FeedConfig BINANCE_FEED {
    "stream.binance.com", "/ws/btcusdt@bookTicker", "9443", 0
};
static constexpr FeedConfig BYBIT_FEED {
    "stream.bybit.com", "/v5/public/spot", "443", 1
};

// Simple JSON field extractor (no full parser needed in feed path)
inline double extractJsonDouble(const std::string& s, const char* key) {
    std::string k = std::string("\"") + key + "\":\"";
    auto pos = s.find(k);
    if (pos == std::string::npos) {
        k = std::string("\"") + key + "\":";
        pos = s.find(k);
        if (pos == std::string::npos) return 0.0;
    } else {
        pos += k.size();
        return std::stod(s.substr(pos, 20));
    }
    pos += k.size();
    return std::stod(s.substr(pos, 20));
}

class WebSocketFeed {
public:
    using TickBuffer = SPSCRingBuffer<MarketTick, 65536>;

    WebSocketFeed(TickBuffer& buf, FeedConfig config)
        : buf_(buf), config_(config), running_(false), ssl_ctx_(nullptr), ssl_(nullptr) {}

    ~WebSocketFeed() { stop(); }

    bool start() {
        running_ = true;
        thread_ = std::thread(&WebSocketFeed::runLoop, this);
        return true;
    }

    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        cleanupSSL();
    }

    uint64_t ticksReceived() const { return ticks_received_; }
    bool     isConnected()   const { return connected_; }

private:
    void runLoop() {
        while (running_) {
            if (!connectAndHandshake()) {
                std::cerr << "[FEED v" << (int)config_.venue_id
                          << "] Reconnecting in 2s...\n";
                std::this_thread::sleep_for(std::chrono::seconds(2));
                cleanupSSL();
                continue;
            }
            connected_ = true;
            std::cout << "[FEED v" << (int)config_.venue_id
                      << "] Connected to " << config_.host << "\n";

            // Subscribe (Bybit needs a JSON sub message; Binance stream in URL)
            if (config_.venue_id == 1) {
                sendSubscribe();
            }

            receiveLoop();
            connected_ = false;
            cleanupSSL();
        }
    }

    bool connectAndHandshake() {
        // Resolve host
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(config_.host, config_.port, &hints, &res) != 0)
            return false;

        int sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sockfd < 0) { freeaddrinfo(res); return false; }

        if (connect(sockfd, res->ai_addr, res->ai_addrlen) < 0) {
            close(sockfd); freeaddrinfo(res); return false;
        }
        freeaddrinfo(res);
        sockfd_ = sockfd;

        // TLS
        ssl_ctx_ = SSL_CTX_new(TLS_client_method());
        ssl_      = SSL_new(ssl_ctx_);
        SSL_set_fd(ssl_, sockfd_);
        SSL_set_tlsext_host_name(ssl_, config_.host);
        if (SSL_connect(ssl_) <= 0) { cleanupSSL(); return false; }

        // WebSocket upgrade handshake
        std::ostringstream req;
        req << "GET " << config_.path << " HTTP/1.1\r\n"
            << "Host: " << config_.host << "\r\n"
            << "Upgrade: websocket\r\n"
            << "Connection: Upgrade\r\n"
            << "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            << "Sec-WebSocket-Version: 13\r\n\r\n";
        std::string r = req.str();
        SSL_write(ssl_, r.c_str(), (int)r.size());

        // Read HTTP 101 response
        char resp[1024];
        int n = SSL_read(ssl_, resp, sizeof(resp) - 1);
        if (n <= 0) return false;
        resp[n] = '\0';
        return (strstr(resp, "101") != nullptr);
    }

    void sendSubscribe() {
        // Bybit subscription message
        const char* sub = R"({"op":"subscribe","args":["orderbook.1.BTCUSDT"]})";
        sendWSFrame(sub, strlen(sub));
    }

    void receiveLoop() {
        char buf[65536];
        while (running_) {
            int n = SSL_read(ssl_, buf, sizeof(buf) - 1);
            if (n <= 0) break;
            buf[n] = '\0';

            // Parse WebSocket frame header (minimal)
            if (n < 2) continue;
            uint8_t* data = (uint8_t*)buf;
            bool fin     = (data[0] & 0x80) != 0;
            uint8_t opcode = data[0] & 0x0F;
            if (opcode == 0x8) break; // close frame
            if (opcode != 0x1 && opcode != 0x0) continue; // not text

            uint64_t payload_len = data[1] & 0x7F;
            int offset = 2;
            if (payload_len == 126) { payload_len = (data[2]<<8)|data[3]; offset=4; }
            else if (payload_len == 127) { payload_len = 0; offset=10; } // skip big frames

            std::string json(buf + offset, (size_t)payload_len);
            parseTick(json);
        }
    }

    void parseTick(const std::string& json) {
        // Binance bookTicker: {"b":"bid","B":"bidQty","a":"ask","A":"askQty","s":"BTCUSDT"}
        // Bybit: {"topic":"orderbook.1.BTCUSDT","data":{"b":[["price","qty"]],"a":[...]}}

        try {
            MarketTick tick{};
            strncpy(tick.symbol, "BTCUSDT", 15);
            tick.venue_id     = config_.venue_id;
            tick.timestamp_ns = nowNs();

            if (config_.venue_id == 0) { // Binance
                double bp = extractJsonDouble(json, "b");
                double ap = extractJsonDouble(json, "a");
                double bq = extractJsonDouble(json, "B");
                double aq = extractJsonDouble(json, "A");
                if (bp <= 0 || ap <= 0) return;
                // Convert to integer ticks (2 decimal places for BTC)
                tick.bid_price = (int64_t)(bp * 100);
                tick.ask_price = (int64_t)(ap * 100);
                tick.bid_qty   = (int64_t)(bq * 1000);
                tick.ask_qty   = (int64_t)(aq * 1000);
                tick.last_price = (tick.bid_price + tick.ask_price) / 2;
            } else { // Bybit — simplified parse
                auto bpos = json.find("\"b\":[[\"");
                auto apos = json.find("\"a\":[[\"");
                if (bpos == std::string::npos || apos == std::string::npos) return;
                double bp = std::stod(json.substr(bpos + 7, 10));
                double ap = std::stod(json.substr(apos + 7, 10));
                if (bp <= 0 || ap <= 0) return;
                tick.bid_price  = (int64_t)(bp * 100);
                tick.ask_price  = (int64_t)(ap * 100);
                tick.last_price = (tick.bid_price + tick.ask_price) / 2;
            }

            if (!buf_.push(tick)) ++dropped_;
            else ++ticks_received_;

        } catch (...) {} // malformed, skip
    }

    void sendWSFrame(const char* data, size_t len) {
        // Build unmasked WebSocket text frame
        uint8_t frame[65536];
        int idx = 0;
        frame[idx++] = 0x81; // FIN + text opcode
        if (len < 126) {
            frame[idx++] = (uint8_t)len;
        } else {
            frame[idx++] = 126;
            frame[idx++] = (len >> 8) & 0xFF;
            frame[idx++] = len & 0xFF;
        }
        memcpy(frame + idx, data, len);
        SSL_write(ssl_, frame, idx + len);
    }

    void cleanupSSL() {
        if (ssl_)     { SSL_shutdown(ssl_); SSL_free(ssl_); ssl_ = nullptr; }
        if (ssl_ctx_) { SSL_CTX_free(ssl_ctx_); ssl_ctx_ = nullptr; }
        if (sockfd_ >= 0) { close(sockfd_); sockfd_ = -1; }
    }

    static uint64_t nowNs() {
        return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    TickBuffer&  buf_;
    FeedConfig   config_;
    std::atomic<bool>     running_;
    std::atomic<bool>     connected_{false};
    std::thread  thread_;
    int          sockfd_{-1};
    SSL_CTX*     ssl_ctx_;
    SSL*         ssl_;
    std::atomic<uint64_t> ticks_received_{0};
    std::atomic<uint64_t> dropped_{0};
};
