// ================================================================
//  SCP_SENDER.CPP
//  The sending side of the SCP-L2 protocol
//
//  WHAT THIS DOES:
//  1. Opens a UDP socket (demo) or raw socket (production)
//  2. Starts a CHAFF INJECTOR THREAD that continuously sends
//     random fake frames alongside every real message
//  3. Reads your input, builds a tagged real frame,
//     sends it into the flood of fakes
//
//  TO COMPILE:
//  g++ -std=c++17 -O2 -pthread -o sender scp_sender.cpp
//
//  TO RUN:
//  ./sender <receiver_ip> <your_firm_id>
//  Example: ./sender 127.0.0.1 1
// ================================================================

#include "scp_common.h"

#include <iostream>
#include <thread>
#include <atomic>
#include <random>
#include <chrono>
#include <cstring>
#include <string>
#include <sstream>

// POSIX / Linux socket headers
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

// rdtsc — the CPU's nanosecond clock
// One instruction. ~7ns to call. No syscall. No kernel.
// This is how we timestamp every single event.
static inline uint64_t rdtsc() {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// ================================================================
//  GLOBAL STATE
//  Shared between the main thread and the chaff injector thread
// ================================================================

std::atomic<bool> running(true);       // set false to stop everything
std::atomic<uint64_t> real_seq(0);     // monotonically increasing sequence
                                        // for REAL messages only
                                        // (decoys do NOT increment this)

// The sender socket (UDP for demo)
int g_sock = -1;
struct sockaddr_in g_dest_addr;

// Stats
std::atomic<uint64_t> total_real_sent(0);
std::atomic<uint64_t> total_chaff_sent(0);


// ================================================================
//  BUILD A DECOY FRAME
//  Fill everything with random garbage.
//  prime_tag = random (will NOT match receiver's expected prime)
//  payload   = random
//  crc32     = random (we don't bother computing it for decoys)
//
//  This is the key to the security: decoys look EXACTLY like
//  real frames. Same size. Same structure. Just random content.
// ================================================================
void build_decoy(SCPFrame& frame, std::mt19937_64& rng) {
    // Fill the entire frame with random bytes first
    uint8_t* raw = reinterpret_cast<uint8_t*>(&frame);
    for (size_t i = 0; i < sizeof(SCPFrame); i++) {
        raw[i] = rng() & 0xFF;
    }
    // Keep ethertype so receiver can at least filter our protocol
    // (in production you'd also randomize this for more stealth)
    frame.ethertype = htons(SCP_ETHERTYPE);
    frame.msg_type  = static_cast<uint8_t>(MsgType::DECOY);
    // prime_tag remains random = will fail receiver's check = discarded
}


// ================================================================
//  BUILD A REAL FRAME
//  This is the actual message.
//  prime_tag = PrimeSyncer.generate(seq_num) — the synchronized value
//  Both sender and receiver compute this independently.
//  They get the same number. Receiver accepts this frame.
// ================================================================
void build_real_frame(
    SCPFrame&        frame,
    PrimeSyncer&     syncer,
    uint64_t         seq,
    uint32_t         firm_id,
    MsgType          type,
    const uint8_t*   payload_data,
    size_t           payload_len
) {
    memset(&frame, 0, sizeof(SCPFrame));

    // Ethernet header
    // In production: fill with real MAC addresses from arp table
    // For demo: we leave them zero (UDP handles addressing for us)
    frame.ethertype = htons(SCP_ETHERTYPE);

    // SCP header
    frame.prime_tag = syncer.generate(seq);  // THE KEY FIELD
                                              // receiver will compute
                                              // the same value using
                                              // the same seq number
    frame.msg_type  = static_cast<uint8_t>(type);
    frame.firm_id   = htonl(firm_id);
    frame.seq_num   = seq;  // receiver needs this to verify prime

    // Copy payload
    size_t copy_len = std::min(payload_len, sizeof(frame.payload));
    memcpy(frame.payload, payload_data, copy_len);

    // CRC32 over everything except the crc field itself
    frame.crc32 = htonl(frame_crc(frame));
}


// ================================================================
//  CHAFF INJECTOR THREAD
//
//  This runs on its own thread, continuously sending random
//  decoy frames. It never stops until running=false.
//
//  RATE: CHAFF_PER_REAL decoys per real message.
//  But in this thread we just send continuously at max speed.
//  The real messages are sent from the main thread.
//
//  WHY A SEPARATE THREAD?
//  The chaff must flow constantly — before, during, and after
//  real messages. If chaff only appeared when a real message
//  was sent, an attacker could notice the timing pattern.
//  Continuous chaff makes timing analysis impossible.
// ================================================================
void chaff_injector_thread() {
    // Each thread has its OWN random generator
    // seeded differently from the prime syncer
    // We do NOT want chaff to accidentally collide with real primes
    std::mt19937_64 rng(rdtsc() ^ 0xC0FFEE00DEAD0000ULL);

    SCPFrame decoy;

    std::cout << "[CHAFF] Injector thread started — flooding with decoys\n";

    while (running.load()) {
        // Build a random decoy frame
        build_decoy(decoy, rng);

        // Send it (UDP demo)
        sendto(
            g_sock,
            &decoy,
            sizeof(decoy),
            0,
            (struct sockaddr*)&g_dest_addr,
            sizeof(g_dest_addr)
        );

        total_chaff_sent.fetch_add(1, std::memory_order_relaxed);

        // Small sleep to avoid saturating localhost
        // In production on a private VLAN: no sleep, full speed
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    std::cout << "[CHAFF] Injector thread stopped. "
              << "Total decoys sent: " << total_chaff_sent.load() << "\n";
}


// ================================================================
//  SEND A REAL EXEC_REQUEST
//  This is what a firm calls when delegating a trade
// ================================================================
void send_exec_request(
    PrimeSyncer& syncer,
    uint32_t     firm_id,
    uint32_t     instrument_id,
    uint8_t      side,           // 0=buy, 1=sell
    uint32_t     quantity,
    int64_t      limit_price,
    uint32_t     time_limit_us,
    uint8_t      split_pct
) {
    // Build the exec request payload
    ExecRequestPayload req;
    memset(&req, 0, sizeof(req));
    req.instrument_id   = instrument_id;
    req.side            = side;
    req.quantity        = htonl(quantity);
    req.limit_price     = limit_price;
    req.time_limit_us   = htonl(time_limit_us);
    req.profit_split_pct = split_pct;

    // Get next sequence number for real messages
    uint64_t seq = real_seq.fetch_add(1, std::memory_order_relaxed);

    // Timestamp when we decided to send
    uint64_t ts_before = rdtsc();

    // Build the frame with the synchronized prime
    SCPFrame frame;
    build_real_frame(
        frame,
        syncer,
        seq,
        firm_id,
        MsgType::EXEC_REQUEST,
        reinterpret_cast<uint8_t*>(&req),
        sizeof(req)
    );

    // Send it (it goes out mixed into the chaff flood)
    ssize_t sent = sendto(
        g_sock,
        &frame,
        sizeof(frame),
        0,
        (struct sockaddr*)&g_dest_addr,
        sizeof(g_dest_addr)
    );

    uint64_t ts_after = rdtsc();

    if (sent > 0) {
        total_real_sent.fetch_add(1, std::memory_order_relaxed);
        std::cout << "\n[SENDER] ✓ REAL EXEC_REQUEST sent\n"
                  << "         seq=" << seq
                  << " prime=" << std::hex << frame.prime_tag << std::dec
                  << " side=" << (side == 0 ? "BUY" : "SELL")
                  << " qty=" << quantity
                  << " price=" << limit_price
                  << " split=" << (int)split_pct << "%\n"
                  << "         send latency=" << (ts_after - ts_before) << " ticks\n"
                  << "         [this frame is hidden among "
                  << total_chaff_sent.load() << " decoys]\n";
    }
}


// ================================================================
//  SEND A HEARTBEAT
//  Sent every 100ms to prove we are alive
// ================================================================
void send_heartbeat(PrimeSyncer& syncer, uint32_t firm_id) {
    uint64_t seq = real_seq.fetch_add(1, std::memory_order_relaxed);
    uint8_t payload[49] = {};
    SCPFrame frame;
    build_real_frame(frame, syncer, seq, firm_id,
                     MsgType::HEARTBEAT, payload, sizeof(payload));
    sendto(g_sock, &frame, sizeof(frame), 0,
           (struct sockaddr*)&g_dest_addr, sizeof(g_dest_addr));
    std::cout << "[SENDER] ♥ HEARTBEAT seq=" << seq << "\n";
}


// ================================================================
//  SEND CAPABILITY UPDATE
//  Broadcast our current state to the hub every 10ms
// ================================================================
void send_capability(
    PrimeSyncer& syncer,
    uint32_t     firm_id,
    uint64_t     capital_cents,
    uint32_t     venue_mask,
    uint32_t     latency_us
) {
    CapabilityPayload cap;
    memset(&cap, 0, sizeof(cap));
    cap.capital_cents = capital_cents;
    cap.venue_mask    = htonl(venue_mask);
    cap.latency_us    = htonl(latency_us);
    cap.order_slots   = htonl(50);  // can take 50 more orders

    uint64_t seq = real_seq.fetch_add(1, std::memory_order_relaxed);
    SCPFrame frame;
    build_real_frame(frame, syncer, seq, firm_id,
                     MsgType::CAPABILITY,
                     reinterpret_cast<uint8_t*>(&cap), sizeof(cap));
    sendto(g_sock, &frame, sizeof(frame), 0,
           (struct sockaddr*)&g_dest_addr, sizeof(g_dest_addr));
    std::cout << "[SENDER] 📡 CAPABILITY capital=$"
              << capital_cents/100
              << " venues=0x" << std::hex << venue_mask << std::dec
              << " latency=" << latency_us << "µs\n";
}


// ================================================================
//  MAIN
// ================================================================
int main(int argc, char* argv[]) {

    std::string dest_ip = "127.0.0.1";
    uint32_t firm_id    = 1;

    if (argc >= 2) dest_ip  = argv[1];
    if (argc >= 3) firm_id  = std::stoi(argv[2]);

    std::cout << "================================================\n"
              << "  SCP-L2 SENDER — Firm " << firm_id << "\n"
              << "  Destination: " << dest_ip << ":" << DEMO_UDP_PORT << "\n"
              << "================================================\n\n"
              << "WHAT IS HAPPENING:\n"
              << "  Shared secret seed: 0x"
              << std::hex << SHARED_SECRET_SEED << std::dec << "\n"
              << "  Chaff per real msg: " << CHAFF_PER_REAL << " decoys\n"
              << "  Frame size: " << sizeof(SCPFrame) << " bytes\n"
              << "  Protocol: UDP (demo) — production uses raw Ethernet\n\n";

    // ── Setup UDP socket ─────────────────────────────────────
    // In production: replaced with DPDK raw socket
    // In demo: UDP carries our frame as payload
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0) {
        perror("socket");
        return 1;
    }

    memset(&g_dest_addr, 0, sizeof(g_dest_addr));
    g_dest_addr.sin_family      = AF_INET;
    g_dest_addr.sin_port        = htons(DEMO_UDP_PORT);
    g_dest_addr.sin_addr.s_addr = inet_addr(dest_ip.c_str());

    // ── Initialize prime syncer ──────────────────────────────
    // Both sender and receiver use SHARED_SECRET_SEED
    PrimeSyncer syncer(SHARED_SECRET_SEED);

    std::cout << "Prime syncer initialized with seed 0x"
              << std::hex << SHARED_SECRET_SEED << std::dec << "\n";
    std::cout << "First 3 primes this syncer will generate:\n";
    for (int i = 0; i < 3; i++) {
        std::cout << "  seq=" << i << " → prime=0x"
                  << std::hex << syncer.generate(i) << std::dec << "\n";
    }
    std::cout << "\n";

    // ── Start chaff injector thread ──────────────────────────
    std::cout << "Starting chaff injector thread...\n";
    std::thread chaff_thread(chaff_injector_thread);

    // Give chaff a moment to start flooding
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // ── Send initial messages ────────────────────────────────
    // Send HELLO equivalent via CAPABILITY message
    send_capability(syncer, firm_id, 500000 * 100, 0x03, 200);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    send_heartbeat(syncer, firm_id);

    // ── Interactive menu ─────────────────────────────────────
    std::cout << "\n================================================\n"
              << "  COMMANDS:\n"
              << "  e  — send EXEC_REQUEST (delegate a trade)\n"
              << "  h  — send HEARTBEAT\n"
              << "  c  — send CAPABILITY update\n"
              << "  s  — show stats\n"
              << "  q  — quit\n"
              << "================================================\n\n";

    std::string cmd;
    while (std::getline(std::cin, cmd)) {
        if (cmd.empty()) continue;

        if (cmd == "e") {
            // Send a sample exec request
            // In production: filled from signal engine output
            send_exec_request(
                syncer,
                firm_id,
                1,      // instrument: BTC/USDT
                0,      // side: buy
                100,    // quantity: 100 units
                8350000,// limit price: $83,500.00 (in ticks ×100)
                600,    // time limit: 600 microseconds
                40      // partner gets 40% of profit
            );

        } else if (cmd == "h") {
            send_heartbeat(syncer, firm_id);

        } else if (cmd == "c") {
            send_capability(syncer, firm_id,
                250000 * 100,  // $250,000 available
                0x01,          // only Binance access
                220);          // 220µs latency

        } else if (cmd == "s") {
            std::cout << "\n[STATS]\n"
                      << "  Real messages sent : " << total_real_sent.load() << "\n"
                      << "  Decoy messages sent: " << total_chaff_sent.load() << "\n"
                      << "  Current seq_num    : " << real_seq.load() << "\n"
                      << "  Ratio real/total   : "
                      << (double)total_real_sent.load() /
                         (total_real_sent.load() + total_chaff_sent.load()) * 100.0
                      << "%\n\n";

        } else if (cmd == "q") {
            break;
        } else {
            std::cout << "Unknown command. Try: e h c s q\n";
        }
    }

    // ── Shutdown ─────────────────────────────────────────────
    std::cout << "\nShutting down...\n";
    running.store(false);
    chaff_thread.join();
    close(g_sock);

    std::cout << "Final stats:\n"
              << "  Real sent : " << total_real_sent.load() << "\n"
              << "  Chaff sent: " << total_chaff_sent.load() << "\n";

    return 0;
}
