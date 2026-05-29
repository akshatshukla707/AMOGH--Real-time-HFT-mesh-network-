// ================================================================
//  SCP_COMMON.H
//  Synchronized Chaff Protocol — Layer 2
//  Shared types used by both sender and receiver
// ================================================================
//
//  WHAT THIS FILE DEFINES:
//
//  1. SCPFrame  — the exact 88-byte packet that travels on the wire
//  2. PrimeSyncer — the "clock twinning" engine that both machines
//                   run independently, always generating the same number
//  3. CRC32     — simple integrity check for each frame
//  4. MsgType   — what kind of message is in the frame
//  5. Payloads  — what the real data looks like inside a frame
//
// ================================================================

#pragma once

#include <cstdint>      // uint8_t, uint64_t etc
#include <random>       // mt19937_64 — our CSPRNG
#include <cstring>      // memcpy, memset
#include <chrono>       // timing
#include <string>

// ================================================================
//  THE FRAME — 88 bytes exactly
//
//  Every packet on our network — real or decoy — is this struct.
//  They are ALL identical in size and layout. An attacker seeing
//  the wire cannot tell which is real by looking at the structure.
//
//  #pragma pack(1) = tell the compiler: NO padding bytes between
//  fields. Without this, the compiler adds invisible bytes to align
//  data on nice boundaries. We need exact byte layout.
// ================================================================
#pragma pack(push, 1)
struct SCPFrame {

    // ── ETHERNET HEADER (14 bytes) ──────────────────────────────
    // This is a standard Ethernet II frame header.
    // In production with DPDK, we build this ourselves.
    // In our demo with UDP, we skip this and start at scp_header.
    uint8_t  dst_mac[6];    // Who is this going to? (MAC address)
    uint8_t  src_mac[6];    // Who sent this? (MAC address)
    uint16_t ethertype;     // 0x88B5 = our custom protocol ID
                            // (0x88B5 is reserved for local/experimental use)

    // ── SCP-L2 HEADER (21 bytes) ────────────────────────────────
    uint64_t prime_tag;     // THE KEY FIELD.
                            // Real message: prime_tag = PrimeSyncer.generate(seq)
                            // Decoy message: prime_tag = random garbage
                            // Receiver generates its own prime, compares.
                            // Match = real. No match = discard.

    uint8_t  msg_type;      // 0=DECOY, 1=EXEC_REQUEST, 2=CAPABILITY,
                            // 3=HEARTBEAT, 4=EXEC_CONFIRM
                            // On a decoy, this field is also random garbage.
                            // The receiver only reads this AFTER prime check.

    uint32_t firm_id;       // Which firm sent this frame
    uint64_t seq_num;       // Monotonically increasing counter.
                            // CRITICAL: This is what drives the prime generator.
                            // Both sides must agree on seq_num.
                            // Sender increments after each REAL message.
                            // Decoys do NOT increment the counter.

    // ── PAYLOAD (49 bytes) ──────────────────────────────────────
    uint8_t  payload[49];   // Real data or random garbage.
                            // Only meaningful on real messages.
                            // On decoys: filled with random bytes.

    // ── INTEGRITY (4 bytes) ─────────────────────────────────────
    uint32_t crc32;         // CRC32 of everything above.
                            // On decoys: also random (we don't bother computing it).
                            // Receiver only checks CRC on messages that passed
                            // the prime_tag check.

};
#pragma pack(pop)

// Compile-time check: if this fails, our frame size math is wrong
static_assert(sizeof(SCPFrame) == 88, "SCPFrame must be exactly 88 bytes");


// ================================================================
//  THE PRIME SYNCER — "Clock Twinning"
//
//  Both sender and receiver create one of these with the SAME seed.
//  Both call generate(seq_num) with the SAME seq_num.
//  Both get the SAME uint64 output.
//  This is the shared secret. No clock needed — just sequence count.
//
//  It uses std::mt19937_64 — Mersenne Twister, a fast and
//  deterministic pseudo-random number generator. Given the same
//  seed, it always produces the same sequence of numbers.
//
//  The seed we use = secret_seed XOR seq_num XOR a fixed constant.
//  This means every message number has a UNIQUE prime.
//  Replaying message #1047 with the right prime won't work for #1048.
// ================================================================
class PrimeSyncer {
private:
    uint64_t secret_seed;       // the shared secret, agreed at setup
    std::mt19937_64 rng;        // the random number generator

public:
    // Constructor: takes the shared secret seed
    // Both machines must use the EXACT same seed
    PrimeSyncer(uint64_t seed)
        : secret_seed(seed)
        , rng(seed)
    {}

    // Generate the prime for a given sequence number.
    // THIS IS THE CORE FUNCTION.
    // Both machines call this. Both get the same result.
    // Same inputs (seed + seq_num) = same output. Always.
    uint64_t generate(uint64_t seq_num) {
        // We re-seed the RNG every time using:
        //   our_secret XOR this_message_number XOR a fixed mixing constant
        // The XOR mixing constant (0xDEADBEEFCAFEBABE) prevents
        // trivial patterns when seq_num is 0 or small.
        rng.seed(secret_seed ^ seq_num ^ 0xDEADBEEFCAFEBABEULL);
        return rng();   // one call = one pseudo-random uint64
    }

    // Verify: does the prime in a received frame match what we expect?
    // Returns true if this is the REAL message for this seq_num.
    bool verify(uint64_t received_prime, uint64_t seq_num) {
        return received_prime == generate(seq_num);
    }
};


// ================================================================
//  CRC32 — Simple Integrity Check
//
//  CRC32 detects accidental corruption of a frame.
//  It is NOT encryption — just a checksum.
//  We compute it over all bytes of the frame EXCEPT the crc32 field.
//
//  This is the standard CRC32 algorithm (same as used in zip files,
//  Ethernet hardware, PNG images). The polynomial 0xEDB88320 is
//  the IEEE 802.3 standard.
// ================================================================
inline uint32_t crc32_compute(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;     // initial value
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];            // XOR in the next byte
        for (int j = 0; j < 8; j++) {
            // Standard CRC32 bit-by-bit computation
            // The ternary avoids a branch (faster on modern CPUs)
            crc = (crc >> 1) ^ (0xEDB88320U & -(crc & 1));
        }
    }
    return ~crc;    // final XOR
}

// Compute CRC32 for a frame (all bytes except the crc32 field itself)
inline uint32_t frame_crc(const SCPFrame& f) {
    // Compute over everything except the last 4 bytes (the crc field)
    return crc32_compute(
        reinterpret_cast<const uint8_t*>(&f),
        sizeof(SCPFrame) - sizeof(uint32_t)
    );
}


// ================================================================
//  MESSAGE TYPES
// ================================================================
enum class MsgType : uint8_t {
    DECOY        = 0,   // fake message — receiver discards
    EXEC_REQUEST = 1,   // "please execute this trade for me"
    CAPABILITY   = 2,   // "here is my current state"
    HEARTBEAT    = 3,   // "I am alive"
    EXEC_CONFIRM = 4,   // "trade done, here is the fill"
};

const char* msg_type_name(uint8_t t) {
    switch(t) {
        case 0: return "DECOY";
        case 1: return "EXEC_REQUEST";
        case 2: return "CAPABILITY";
        case 3: return "HEARTBEAT";
        case 4: return "EXEC_CONFIRM";
        default: return "UNKNOWN";
    }
}


// ================================================================
//  PAYLOAD STRUCTS — what goes inside the 49 payload bytes
//
//  Different message types have different payload layouts.
//  All must fit in 49 bytes.
// ================================================================

// EXEC_REQUEST payload — "execute this trade"
// Firm A sends this when delegating to Firm B
#pragma pack(push, 1)
struct ExecRequestPayload {
    uint32_t instrument_id;     // e.g. 1=BTC/USDT, 2=ETH/USDT
    uint8_t  side;              // 0=buy, 1=sell
    uint32_t quantity;          // how many units
    int64_t  limit_price;       // max price in integer ticks ($183.50 = 18350)
    uint32_t time_limit_us;     // cancel if not confirmed in Xµs
    uint8_t  profit_split_pct;  // how much the executor keeps (0-100)
    uint8_t  padding[3];        // fill to align, unused
    // total: 4+1+4+8+4+1+3 = 25 bytes, well within 49
};
#pragma pack(pop)

// CAPABILITY payload — "here is what I can do right now"
// Each firm broadcasts this every 10ms
#pragma pack(push, 1)
struct CapabilityPayload {
    uint64_t capital_cents;     // free capital in cents ($1000 = 100000)
    uint32_t venue_mask;        // bitmask: bit 0=Binance, bit 1=Bybit, etc
    uint32_t latency_us;        // self-reported execution latency
    uint32_t order_slots;       // how many more orders we can send
    uint32_t seq_this_firm;     // this firm's own message counter
    // total: 8+4+4+4+4 = 24 bytes
};
#pragma pack(pop)

// EXEC_CONFIRM payload — "I filled the order"
// Firm B sends this back after execution
#pragma pack(push, 1)
struct ExecConfirmPayload {
    uint64_t request_seq;       // seq_num of the EXEC_REQUEST this confirms
    int64_t  fill_price;        // actual fill price in ticks
    uint32_t fill_qty;          // how much was filled
    uint32_t exec_latency_us;   // how long it took
    // total: 8+8+4+4 = 24 bytes
};
#pragma pack(pop)


// ================================================================
//  SHARED CONSTANTS
// ================================================================

// The shared secret seed. In production: exchanged once over TLS,
// never transmitted again. Here: hardcoded for demo.
// Both sender and receiver must use the same value.
constexpr uint64_t SHARED_SECRET_SEED = 0xF1E2D3C4B5A69788ULL;

// Our custom EtherType (tells the NIC this is our protocol)
// 0x88B5 is reserved for local experimental use by IEEE
constexpr uint16_t SCP_ETHERTYPE = 0x88B5;

// How many decoy frames to send per real frame
// Higher = more security, more bandwidth used
constexpr int CHAFF_PER_REAL = 100;

// Demo: use this UDP port for our demo UDP simulation
constexpr int DEMO_UDP_PORT = 54321;
