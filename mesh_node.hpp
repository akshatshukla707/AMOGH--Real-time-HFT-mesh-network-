#pragma once
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <iostream>
#include <functional>
#include <fstream>
#include "types.hpp"

// ─────────────────────────────────────────────────────────────
// MeshNode — epoll-based TCP server implementing the 68-byte
// binary mesh protocol. Each firm runs exactly one MeshNode.
//
// Capabilities:
//  • Accept incoming peer connections (epoll edge-triggered)
//  • Send CAPABILITY broadcasts every 10ms
//  • Send/receive EXEC_REQUEST and EXEC_CONFIRM
//  • Heartbeat every 100ms, drop peers missing 5 heartbeats
//  • Profit split ledger: append-only binary file
// ─────────────────────────────────────────────────────────────

static constexpr size_t MSG_SIZE = 68;

class MeshNode {
public:
    using ExecRequestCallback  = std::function<bool(const MeshExecRequest&)>;
    using ExecConfirmCallback  = std::function<void(const MeshExecConfirm&)>;
    using CapabilityCallback   = std::function<void(const MeshCapability&)>;

    MeshNode(uint32_t firm_id, const std::string& firm_name,
             int listen_port = 7777)
        : firm_id_(firm_id), firm_name_(firm_name),
          listen_port_(listen_port), running_(false), seq_(1)
    {
        ledger_.open("profit_ledger.bin",
            std::ios::binary | std::ios::app | std::ios::out);
    }

    ~MeshNode() { stop(); }

    // Register callbacks
    void onExecRequest(ExecRequestCallback cb)  { exec_req_cb_  = std::move(cb); }
    void onExecConfirm(ExecConfirmCallback cb)  { exec_conf_cb_ = std::move(cb); }
    void onCapability(CapabilityCallback cb)    { cap_cb_       = std::move(cb); }

    bool start() {
        // Create listen socket
        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) { perror("socket"); return false; }

        int opt = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        setNonBlocking(listen_fd_);

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(listen_port_);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("bind"); return false;
        }
        listen(listen_fd_, 16);

        // Create epoll
        epoll_fd_ = epoll_create1(0);
        if (epoll_fd_ < 0) { perror("epoll_create1"); return false; }

        epollAdd(listen_fd_, EPOLLIN);

        running_ = true;
        event_thread_  = std::thread(&MeshNode::eventLoop, this);
        hb_thread_     = std::thread(&MeshNode::heartbeatLoop, this);
        cap_thread_    = std::thread(&MeshNode::capabilityLoop, this);

        std::cout << "[MESH] Node " << firm_name_ << " (id=" << firm_id_
                  << ") listening on :" << listen_port_ << "\n";
        return true;
    }

    void stop() {
        running_ = false;
        if (listen_fd_ >= 0) close(listen_fd_);
        if (epoll_fd_ >= 0) close(epoll_fd_);
        if (event_thread_.joinable())  event_thread_.join();
        if (hb_thread_.joinable())     hb_thread_.join();
        if (cap_thread_.joinable())    cap_thread_.join();
    }

    // Connect to a peer firm
    bool connectToPeer(const std::string& host, int port, uint32_t peer_firm_id) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;

        int flag = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)); // NO Nagle
        setNonBlocking(fd);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port);
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

        int ret = connect(fd, (sockaddr*)&addr, sizeof(addr));
        if (ret < 0 && errno != EINPROGRESS) {
            close(fd);
            std::cerr << "[MESH] Cannot connect to " << host << ":" << port << "\n";
            return false;
        }

        epollAdd(fd, EPOLLIN | EPOLLOUT | EPOLLET);

        {
            std::lock_guard<std::mutex> lk(peers_mu_);
            peer_fds_[fd] = peer_firm_id;
            PeerCapability pc{};
            pc.firm_id   = peer_firm_id;
            pc.connected = true;
            pc.available = true;
            capability_table_[peer_firm_id] = pc;
        }

        // Send HELLO
        sendHello(fd);
        return true;
    }

    // Send an execution request to best available peer
    bool sendExecRequest(const MeshExecRequest& req) {
        int best_fd = selectBestPeer(req.quantity);
        if (best_fd < 0) return false;
        send(best_fd, &req, MSG_SIZE, 0);
        {
            std::lock_guard<std::mutex> lk(pending_mu_);
            pending_requests_[req.request_id] = {best_fd, nowNs()};
        }
        return true;
    }

    // Update our own capability broadcast
    void updateCapability(uint64_t capital_cents, uint32_t venue_mask,
                          uint32_t latency_us, uint32_t risk_headroom) {
        std::lock_guard<std::mutex> lk(cap_mu_);
        my_cap_.available_capital = capital_cents;
        my_cap_.venue_mask        = venue_mask;
        my_cap_.latency_us        = latency_us;
        my_cap_.risk_headroom     = risk_headroom;
    }

    // Write a trade to the profit ledger
    void writeLedger(const LedgerRecord& rec) {
        std::lock_guard<std::mutex> lk(ledger_mu_);
        ledger_.write(reinterpret_cast<const char*>(&rec), sizeof(rec));
        ledger_.flush();
    }

    // Get copy of capability table for display
    std::vector<PeerCapability> getPeers() const {
        std::lock_guard<std::mutex> lk(peers_mu_);
        std::vector<PeerCapability> result;
        for (auto& [id, pc] : capability_table_) result.push_back(pc);
        return result;
    }

private:
    // ── epoll event loop ──────────────────────────────────────
    void eventLoop() {
        constexpr int MAX_EVENTS = 64;
        epoll_event events[MAX_EVENTS];

        while (running_) {
            int n = epoll_wait(epoll_fd_, events, MAX_EVENTS, 5 /*ms timeout*/);
            for (int i = 0; i < n; ++i) {
                int fd = events[i].data.fd;
                if (fd == listen_fd_) {
                    acceptPeer();
                } else {
                    if (events[i].events & EPOLLIN)  handleRead(fd);
                    if (events[i].events & EPOLLHUP) handleDisconnect(fd);
                }
            }
            checkPendingTimeouts();
        }
    }

    void acceptPeer() {
        while (true) {
            sockaddr_in peer_addr{};
            socklen_t peer_len = sizeof(peer_addr);
            int client_fd = accept(listen_fd_, (sockaddr*)&peer_addr, &peer_len);
            if (client_fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                continue;
            }
            int flag = 1;
            setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
            setNonBlocking(client_fd);
            epollAdd(client_fd, EPOLLIN | EPOLLET);

            std::cout << "[MESH] Accepted connection from "
                      << inet_ntoa(peer_addr.sin_addr) << "\n";
            sendHello(client_fd);
        }
    }

    void handleRead(int fd) {
        uint8_t buf[MSG_SIZE];
        while (true) {
            ssize_t n = recv(fd, buf, MSG_SIZE, MSG_DONTWAIT);
            if (n <= 0) {
                if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
                    handleDisconnect(fd);
                break;
            }
            if (n == MSG_SIZE) processMessage(fd, buf);
        }
    }

    void processMessage(int fd, const uint8_t* buf) {
        MeshMsgType type = (MeshMsgType)buf[0];
        switch (type) {
            case MeshMsgType::HELLO: {
                auto* m = reinterpret_cast<const MeshHello*>(buf);
                std::lock_guard<std::mutex> lk(peers_mu_);
                peer_fds_[fd] = m->header.firm_id;
                PeerCapability& pc = capability_table_[m->header.firm_id];
                pc.firm_id   = m->header.firm_id;
                pc.connected = true;
                pc.available = true;
                strncpy(pc.firm_name, m->firm_name, 31);
                std::cout << "[MESH] HELLO from " << m->firm_name << "\n";
                break;
            }
            case MeshMsgType::CAPABILITY: {
                auto* m = reinterpret_cast<const MeshCapability*>(buf);
                {
                    std::lock_guard<std::mutex> lk(peers_mu_);
                    PeerCapability& pc = capability_table_[m->header.firm_id];
                    pc.available_capital = m->available_capital;
                    pc.venue_mask        = m->venue_mask;
                    pc.latency_us        = m->latency_us;
                    pc.risk_headroom     = m->risk_headroom;
                    pc.last_heartbeat_ns = m->header.timestamp_ns;
                }
                if (cap_cb_) cap_cb_(*m);
                break;
            }
            case MeshMsgType::EXEC_REQUEST: {
                auto* m = reinterpret_cast<const MeshExecRequest*>(buf);
                bool approved = exec_req_cb_ ? exec_req_cb_(*m) : true;
                MeshExecConfirm conf{};
                conf.header.type         = MeshMsgType::EXEC_CONFIRM;
                conf.header.firm_id      = firm_id_;
                conf.header.timestamp_ns = nowNs();
                conf.header.seq_num      = seq_++;
                conf.request_id          = m->request_id;
                conf.status              = approved ? 0 : 1;
                conf.fill_price          = m->limit_price;
                conf.fill_qty            = approved ? m->quantity : 0;
                send(fd, &conf, MSG_SIZE, 0);
                break;
            }
            case MeshMsgType::EXEC_CONFIRM: {
                auto* m = reinterpret_cast<const MeshExecConfirm*>(buf);
                if (exec_conf_cb_) exec_conf_cb_(*m);
                std::lock_guard<std::mutex> lk(pending_mu_);
                pending_requests_.erase(m->request_id);
                break;
            }
            case MeshMsgType::HEARTBEAT: {
                auto* m = reinterpret_cast<const MeshHeartbeat*>(buf);
                std::lock_guard<std::mutex> lk(peers_mu_);
                auto it = capability_table_.find(m->header.firm_id);
                if (it != capability_table_.end())
                    it->second.last_heartbeat_ns = nowNs();
                break;
            }
            default: break;
        }
    }

    void handleDisconnect(int fd) {
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        std::lock_guard<std::mutex> lk(peers_mu_);
        auto it = peer_fds_.find(fd);
        if (it != peer_fds_.end()) {
            auto& pc = capability_table_[it->second];
            pc.connected = false;
            pc.available = false;
            std::cout << "[MESH] Peer " << it->second << " disconnected.\n";
            peer_fds_.erase(it);
        }
    }

    void checkPendingTimeouts() {
        uint64_t now = nowNs();
        std::lock_guard<std::mutex> lk(pending_mu_);
        for (auto it = pending_requests_.begin(); it != pending_requests_.end(); ) {
            if (now - it->second.sent_ns > 800'000 /*800µs*/) {
                std::cerr << "[MESH] EXEC_REQUEST " << it->first << " timed out!\n";
                it = pending_requests_.erase(it);
            } else ++it;
        }
    }

    void heartbeatLoop() {
        MeshHeartbeat hb{};
        hb.header.type    = MeshMsgType::HEARTBEAT;
        hb.header.firm_id = firm_id_;

        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            hb.header.timestamp_ns = nowNs();
            hb.header.seq_num      = seq_++;

            std::lock_guard<std::mutex> lk(peers_mu_);
            uint64_t now = nowNs();
            for (auto& [fd, fid] : peer_fds_) {
                send(fd, &hb, MSG_SIZE, 0);
            }
            // Mark peers that missed 5 heartbeats as unavailable
            for (auto& [fid, pc] : capability_table_) {
                if (pc.connected && now - pc.last_heartbeat_ns > 500'000'000ULL) {
                    pc.available = false;
                    std::cerr << "[MESH] Peer " << fid << " missed heartbeats.\n";
                }
            }
        }
    }

    void capabilityLoop() {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            MeshCapability cap{};
            {
                std::lock_guard<std::mutex> lk(cap_mu_);
                cap = my_cap_;
            }
            cap.header.type         = MeshMsgType::CAPABILITY;
            cap.header.firm_id      = firm_id_;
            cap.header.timestamp_ns = nowNs();
            cap.header.seq_num      = seq_++;

            std::lock_guard<std::mutex> lk(peers_mu_);
            for (auto& [fd, fid] : peer_fds_) {
                send(fd, &cap, MSG_SIZE, 0);
            }
        }
    }

    void sendHello(int fd) {
        MeshHello hello{};
        hello.header.type         = MeshMsgType::HELLO;
        hello.header.firm_id      = firm_id_;
        hello.header.timestamp_ns = nowNs();
        hello.header.seq_num      = seq_++;
        hello.protocol_ver        = 1;
        strncpy(hello.firm_name, firm_name_.c_str(), 31);
        send(fd, &hello, MSG_SIZE, 0);
    }

    int selectBestPeer(uint32_t required_qty) {
        std::lock_guard<std::mutex> lk(peers_mu_);
        int best_fd = -1;
        uint32_t best_lat = UINT32_MAX;
        for (auto& [fd, fid] : peer_fds_) {
            auto& pc = capability_table_[fid];
            if (!pc.available) continue;
            if (pc.risk_headroom == 0) continue;
            if (pc.latency_us < best_lat) {
                best_lat = pc.latency_us;
                best_fd  = fd;
            }
        }
        return best_fd;
    }

    void epollAdd(int fd, uint32_t events) {
        epoll_event ev{};
        ev.events  = events;
        ev.data.fd = fd;
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
    }

    static void setNonBlocking(int fd) {
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    static uint64_t nowNs() {
        return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    }

    uint32_t    firm_id_;
    std::string firm_name_;
    int         listen_port_;
    int         listen_fd_{-1};
    int         epoll_fd_{-1};
    std::atomic<bool>     running_;
    std::atomic<uint64_t> seq_;

    std::thread event_thread_, hb_thread_, cap_thread_;

    mutable std::mutex peers_mu_;
    std::unordered_map<int, uint32_t>          peer_fds_;      // fd → firm_id
    std::unordered_map<uint32_t, PeerCapability> capability_table_;

    mutable std::mutex cap_mu_;
    MeshCapability my_cap_{};

    struct PendingReq { int fd; uint64_t sent_ns; };
    std::mutex pending_mu_;
    std::unordered_map<uint64_t, PendingReq> pending_requests_;

    std::mutex   ledger_mu_;
    std::ofstream ledger_;

    ExecRequestCallback exec_req_cb_;
    ExecConfirmCallback exec_conf_cb_;
    CapabilityCallback  cap_cb_;
};
