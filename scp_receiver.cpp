    // ================================================================
    //  SCP_RECEIVER.CPP
    //  The receiving side of the SCP-L2 protocol
    //
    //  WHAT THIS DOES:
    //  1. Opens a UDP socket and listens for all incoming frames
    //  2. For EVERY frame received (real AND decoys):
    //     a. Extract the prime_tag and seq_num fields
    //     b. Compute what the prime SHOULD be using our syncer
    //     c. Compare: match = REAL message → process it
    //                 no match = DECOY → discard in ~5ns
    //  3. For real messages: verify CRC32, decode payload, act on it
    //
    //  TO COMPILE:
    //  g++ -std=c++17 -O2 -pthread -o receiver scp_receiver.cpp
    //
    //  TO RUN (in a separate terminal from sender):
    //  ./receiver
    // ================================================================

    #include "scp_common.h"

    #include <iostream>
    #include <iomanip>
    #include <string>
    #include <atomic>
    #include <chrono>
    #include <cstring>

    // POSIX / Linux
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>

    // rdtsc — CPU nanosecond clock, same as sender
    static inline uint64_t rdtsc() {
        uint32_t lo, hi;
        __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
        return ((uint64_t)hi << 32) | lo;
    }

    // ================================================================
    //  RECEIVE STATS
    //  We count every frame we see — real and decoy — to prove
    //  how many decoys we filtered out
    // ================================================================
    struct RecvStats {
        uint64_t total_frames_received  = 0;
        uint64_t decoys_discarded       = 0;
        uint64_t real_messages_accepted = 0;
        uint64_t crc_failures           = 0;
        uint64_t exec_requests          = 0;
        uint64_t capabilities           = 0;
        uint64_t heartbeats             = 0;

        // Latency tracking for real messages
        uint64_t min_filter_ns  = UINT64_MAX;
        uint64_t max_filter_ns  = 0;
        uint64_t total_filter_ns = 0;

        void print() const {
            double filter_us = total_filter_ns / (double)real_messages_accepted / 1000.0;
            std::cout << "\n[STATS] ──────────────────────────────\n"
                    << "  Total frames received  : " << total_frames_received << "\n"
                    << "  Decoys discarded       : " << decoys_discarded << "\n"
                    << "  Real messages accepted : " << real_messages_accepted << "\n"
                    << "  CRC failures           : " << crc_failures << "\n"
                    << "  ─ By type ─────────────────────────\n"
                    << "  EXEC_REQUEST           : " << exec_requests << "\n"
                    << "  CAPABILITY             : " << capabilities << "\n"
                    << "  HEARTBEAT              : " << heartbeats << "\n"
                    << "  ─ Filter performance ──────────────\n"
                    << "  Decoy rejection rate   : "
                    << (decoys_discarded * 100.0 / total_frames_received) << "%\n"
                    << "  Avg filter time/frame  : "
                    << (real_messages_accepted > 0 ? filter_us : 0) << "µs\n"
                    << "──────────────────────────────────────\n\n";
        }
    };


    // ================================================================
    //  HANDLE EXEC_REQUEST
    //  Called when we receive a real EXEC_REQUEST from a partner firm.
    //  In production: this would feed into our risk engine.
    // ================================================================
    void handle_exec_request(const SCPFrame& frame, uint32_t firm_id) {
        // Decode the payload
        ExecRequestPayload req;
        memcpy(&req, frame.payload, sizeof(req));

        std::cout << "\n[RECEIVER] ★ EXEC_REQUEST RECEIVED\n"
                << "  From firm  : " << firm_id << "\n"
                << "  Instrument : " << ntohl(req.instrument_id) << " (1=BTC/USDT)\n"
                << "  Side       : " << (req.side == 0 ? "BUY" : "SELL") << "\n"
                << "  Quantity   : " << ntohl(req.quantity) << " units\n"
                << "  Limit price: $" << req.limit_price / 100.0 << "\n"
                << "  Time limit : " << ntohl(req.time_limit_us) << " µs\n"
                << "  Our share  : " << (int)req.profit_split_pct << "%\n"
                << "\n"
                << "  [IN PRODUCTION: risk engine would auto-check]\n"
                << "  [If approved: route order to exchange immediately]\n"
                << "  [Then send EXEC_CONFIRM back to firm " << firm_id << "]\n\n";
    }


    // ================================================================
    //  HANDLE CAPABILITY
    //  A firm is telling us their current state.
    //  In production: update the capability table in the hub.
    // ================================================================
    void handle_capability(const SCPFrame& frame, uint32_t firm_id) {
        CapabilityPayload cap;
        memcpy(&cap, frame.payload, sizeof(cap));

        std::cout << "[RECEIVER] 📡 CAPABILITY from firm " << firm_id << "\n"
                << "  Capital : $" << cap.capital_cents / 100 << "\n"
                << "  Venues  : 0x" << std::hex << ntohl(cap.venue_mask) << std::dec
                << " (bit 0=Binance, bit 1=Bybit)\n"
                << "  Latency : " << ntohl(cap.latency_us) << " µs\n"
                << "  Slots   : " << ntohl(cap.order_slots) << " orders remaining\n"
                << "  [Capability table updated for firm " << firm_id << "]\n\n";
    }


    // ================================================================
    //  HANDLE HEARTBEAT
    // ================================================================
    void handle_heartbeat(const SCPFrame& frame, uint32_t firm_id) {
        std::cout << "[RECEIVER] ♥ HEARTBEAT from firm "
                << firm_id << " seq=" << frame.seq_num << "\n";
    }


    // ================================================================
    //  PROCESS ONE FRAME
    //
    //  This is the hot function — called for EVERY frame including decoys.
    //  It must be as fast as possible.
    //
    //  The prime check is the FIRST thing we do.
    //  If it fails: return immediately. ~5ns total.
    //  If it passes: we know this is real. Process it fully.
    // ================================================================
    void process_frame(
        const SCPFrame& frame,
        PrimeSyncer&    syncer,
        RecvStats&      stats,
        uint64_t        tick_received
    ) {
        stats.total_frames_received++;

        // ── STEP 1: Check EtherType ──────────────────────────────
        // Quick sanity check — is this even our protocol?
        if (ntohs(frame.ethertype) != SCP_ETHERTYPE) {
            stats.decoys_discarded++;
            return;  // not our frame at all
        }

        // ── STEP 2: THE PRIME CHECK ──────────────────────────────
        // THIS IS THE CORE OF SCP-L2.
        //
        // We extract the seq_num from the frame.
        // We compute what prime we EXPECT for that seq_num.
        // We compare with what we received.
        //
        // Both sender and receiver ran the exact same PrimeSyncer
        // with the exact same SHARED_SECRET_SEED.
        // So generate(seq_num) returns the same value on both sides.
        //
        // If the prime matches: sender knows our secret → real message.
        // If it doesn't match: random decoy → discard immediately.
        //
        // Cost of this check: ~5 nanoseconds.

        uint64_t seq = frame.seq_num;
        uint64_t expected_prime = syncer.generate(seq);

        if (frame.prime_tag != expected_prime) {
            // DECOY — discard immediately
            // We don't even look at msg_type or payload
            stats.decoys_discarded++;
            return;
        }

        // ── REAL MESSAGE DETECTED ────────────────────────────────
        // The prime matched. This is a genuine message from our partner.

        uint64_t tick_verified = rdtsc();
        uint64_t filter_ticks  = tick_verified - tick_received;
        // Rough nanosecond conversion (assumes ~3GHz CPU)
        // In production: calibrate precisely at startup
        uint64_t filter_ns = filter_ticks / 3;

        stats.real_messages_accepted++;
        stats.total_filter_ns += filter_ns;
        stats.min_filter_ns = std::min(stats.min_filter_ns, filter_ns);
        stats.max_filter_ns = std::max(stats.max_filter_ns, filter_ns);

        std::cout << "\n[RECEIVER] ✓ REAL frame detected! "
                << "seq=" << seq
                << " prime=0x" << std::hex << frame.prime_tag << std::dec
                << " filter=" << filter_ns << "ns\n";

        // ── STEP 3: CRC32 Verification ───────────────────────────
        // Now that we know it's real, verify integrity.
        // CRC32 detects any corruption in transit.
        uint32_t computed_crc = frame_crc(frame);
        uint32_t received_crc = ntohl(frame.crc32);

        if (computed_crc != received_crc) {
            std::cout << "[RECEIVER] ✗ CRC32 FAILED — frame corrupted\n"
                    << "  expected: 0x" << std::hex << computed_crc << "\n"
                    << "  received: 0x" << received_crc << std::dec << "\n";
            stats.crc_failures++;
            return;
        }

        // ── STEP 4: Decode and Act ───────────────────────────────
        uint32_t firm_id = ntohl(frame.firm_id);

        switch (static_cast<MsgType>(frame.msg_type)) {

            case MsgType::EXEC_REQUEST:
                stats.exec_requests++;
                handle_exec_request(frame, firm_id);
                break;

            case MsgType::CAPABILITY:
                stats.capabilities++;
                handle_capability(frame, firm_id);
                break;

            case MsgType::HEARTBEAT:
                stats.heartbeats++;
                handle_heartbeat(frame, firm_id);
                break;

            case MsgType::EXEC_CONFIRM: {
                ExecConfirmPayload conf;
                memcpy(&conf, frame.payload, sizeof(conf));
                std::cout << "[RECEIVER] ✓ EXEC_CONFIRM from firm " << firm_id << "\n"
                        << "  fill_price   : $" << conf.fill_price / 100.0 << "\n"
                        << "  fill_qty     : " << ntohl(conf.fill_qty) << "\n"
                        << "  exec_latency : " << ntohl(conf.exec_latency_us) << "µs\n\n";
                break;
            }

            case MsgType::DECOY:
                // A real frame marked as DECOY — shouldn't happen
                // but handle gracefully
                stats.decoys_discarded++;
                break;

            default:
                std::cout << "[RECEIVER] Unknown msg_type="
                        << (int)frame.msg_type << " from firm " << firm_id << "\n";
                break;
        }
    }


    // ================================================================
    //  MAIN — THE RECEIVE LOOP
    // ================================================================
    int main() {
        std::cout << "================================================\n"
                << "  SCP-L2 RECEIVER\n"
                << "  Listening on port " << DEMO_UDP_PORT << "\n"
                << "================================================\n\n"
                << "WHAT IS HAPPENING:\n"
                << "  Every frame received will be checked against\n"
                << "  the synchronized prime. Decoys are discarded\n"
                << "  in ~5ns. Real messages are processed fully.\n\n"
                << "  Shared secret seed: 0x"
                << std::hex << SHARED_SECRET_SEED << std::dec << "\n\n";

        // ── Initialize prime syncer ──────────────────────────────
        // SAME seed as the sender — this is the shared secret
        PrimeSyncer syncer(SHARED_SECRET_SEED);

        std::cout << "Prime syncer ready. First 3 expected primes:\n";
        for (int i = 0; i < 3; i++) {
            std::cout << "  seq=" << i << " → expect prime=0x"
                    << std::hex << syncer.generate(i) << std::dec << "\n";
        }
        std::cout << "\n(These should match what the sender shows)\n\n";

        // ── Open UDP socket ──────────────────────────────────────
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) { perror("socket"); return 1; }

        // SO_REUSEADDR: allows restart without TIME_WAIT delay
        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(DEMO_UDP_PORT);
        addr.sin_addr.s_addr = INADDR_ANY;  // accept from any sender

        if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("bind"); close(sock); return 1;
        }

        std::cout << "Listening on UDP port " << DEMO_UDP_PORT << "...\n"
                << "Waiting for frames (real + decoys)...\n\n";

        // ── Receive loop ─────────────────────────────────────────
        // This is the inner loop that processes every frame.
        // In production with DPDK: this would poll the NIC ring buffer
        // directly instead of calling recv().
        //
        // In production the loop looks like:
        //   while(true) {
        //     nb_rx = rte_eth_rx_burst(port, 0, mbufs, BURST_SIZE);
        //     for(int i=0; i<nb_rx; i++) process_frame(mbufs[i], ...);
        //   }
        //
        // Here we use recv() which goes through the UDP stack.
        // The frame checking logic is identical either way.

        SCPFrame  frame;
        RecvStats stats;
        struct sockaddr_in sender_addr;
        socklen_t sender_len = sizeof(sender_addr);

        uint64_t last_stats_print = rdtsc();
        constexpr uint64_t STATS_INTERVAL_TICKS = 3000000000ULL; // ~1 second at 3GHz

        while (true) {
            // Receive next frame (blocks until one arrives)
            ssize_t bytes = recvfrom(
                sock,
                &frame,
                sizeof(frame),
                0,
                (struct sockaddr*)&sender_addr,
                &sender_len
            );

            if (bytes < 0) {
                perror("recvfrom");
                break;
            }

            if (bytes != sizeof(SCPFrame)) {
                // Wrong size — not our frame
                std::cout << "[RECEIVER] Frame size mismatch: got "
                        << bytes << " expected " << sizeof(SCPFrame) << "\n";
                continue;
            }

            // Timestamp exactly when we received it
            uint64_t tick_received = rdtsc();

            // Process it — prime check, CRC, decode
            process_frame(frame, syncer, stats, tick_received);

            // Print stats every ~1 second
            uint64_t now = rdtsc();
            if (now - last_stats_print > STATS_INTERVAL_TICKS) {
                stats.print();
                last_stats_print = now;
            }
        }

        close(sock);
        stats.print();
        return 0;
    }
