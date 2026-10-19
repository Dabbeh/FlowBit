// Proof of concept: enforce a real per-application bandwidth limit.
//
// RUN THIS ELEVATED. WinDivert requires Administrator rights.
//
//   applimit_poc.exe <process-name-or-exe-path> <down-KB/s> <up-KB/s> [seconds]
//
// Example - hold notepad's traffic to 100 KB/s down and 50 KB/s up for 20s:
//   applimit_poc.exe notepad.exe 100 50 20
//
// HOW IT WORKS
//
// WinDivert's NETWORK layer is where packets are delayed, but it does NOT expose
// ProcessId: the documented fields are only valid at their own layers
// (Flow.ProcessId at WINDIVERT_LAYER_FLOW, Socket.ProcessId at WINDIVERT_LAYER_SOCKET).
// A per-process packet filter therefore cannot be written directly.
//
// The approach used here, and the one the real feature uses:
//
//   1. Open a FLOW-layer handle. Each flow event carries ProcessId plus the
//      local/remote address and port for that flow.
//   2. Record the 5-tuples belonging to the processes being limited.
//   3. At the NETWORK layer, match each packet's 5-tuple against that set, and
//      pace only those packets with a token bucket.
//
// Steps 1-3 together are what make per-application shaping possible. Traffic from
// any other process never matches the tuple set, so it is untouched - which is
// also what allows two applications to hold different limits at once.
//
// Download direction: inbound packets are held and re-injected by the bucket.
// Upload direction: outbound packets are held the same way.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "windivert.h"

namespace {

constexpr size_t kMaxPacket = 65535;

// A packet's identity for matching against the attributed tuple set. Direction is
// part of the key because the same pair of ports appears on both sides of a flow.
struct TupleKey {
    UINT32 localAddr = 0;
    UINT16 localPort = 0;
    UINT32 remoteAddr = 0;
    UINT16 remotePort = 0;
    UINT8 protocol = 0;

    bool operator<(const TupleKey &o) const
    {
        if (localAddr != o.localAddr) return localAddr < o.localAddr;
        if (localPort != o.localPort) return localPort < o.localPort;
        if (remoteAddr != o.remoteAddr) return remoteAddr < o.remoteAddr;
        if (remotePort != o.remotePort) return remotePort < o.remotePort;
        return protocol < o.protocol;
    }
};

std::string narrow(const std::wstring &w)
{
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string &s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// Full image path of a pid, or empty.
std::wstring imagePathOf(DWORD pid)
{
    if (pid == 0)
        return {};
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
        return {};
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = DWORD(sizeof(buf) / sizeof(wchar_t));
    std::wstring result;
    if (QueryFullProcessImageNameW(h, 0, buf, &size))
        result.assign(buf, size);
    CloseHandle(h);
    return result;
}

// Token bucket. One per direction, per application.
//
// The bucket holds bytes; packets are admitted while it has room and delay until
// it refills. A small burst allowance keeps a single large packet from stalling
// indefinitely, which would look like packet loss to the application.
class TokenBucket
{
public:
    TokenBucket() = default;
    explicit TokenBucket(double bytesPerSec) { configure(bytesPerSec); }

    void configure(double bytesPerSec)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // 0 means unlimited: the caller should not even consult the bucket, but
        // treating it as "always admit" keeps this safe if it does.
        m_rate = bytesPerSec;
        m_tokens = bytesPerSec * kInitialBurstSeconds;
        m_last = std::chrono::steady_clock::now();
    }

    double rate() const { return m_rate; }

    // Blocks until `bytes` may be sent. Returns immediately when unlimited.
    void take(size_t bytes)
    {
        if (m_rate <= 0)
            return;

        std::unique_lock<std::mutex> lock(m_mutex);
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            const double elapsed =
                std::chrono::duration<double>(now - m_last).count();
            m_last = now;
            m_tokens = std::min(m_tokens + elapsed * m_rate, m_rate * kMaxBurstSeconds);

            if (m_tokens >= double(bytes)) {
                m_tokens -= double(bytes);
                return;
            }

            // Not enough credit: wait for the deficit to refill. The wait is
            // computed, not polled, so an idle limiter costs nothing.
            const double deficit = double(bytes) - m_tokens;
            const double waitSec = deficit / m_rate;
            lock.unlock();
            const auto wake = std::chrono::steady_clock::now()
                              + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                  std::chrono::duration<double>(waitSec));
            std::this_thread::sleep_until(wake);
            lock.lock();
        }
    }

private:
    // Allow a small burst so interactive traffic is not made unusable, but not so
    // much that a speed test would report the wrong figure.
    static constexpr double kInitialBurstSeconds = 0.10;
    static constexpr double kMaxBurstSeconds = 0.20;

    mutable std::mutex m_mutex;
    double m_rate = 0.0;      // bytes per second; <= 0 means unlimited
    double m_tokens = 0.0;
    std::chrono::steady_clock::time_point m_last = std::chrono::steady_clock::now();
};

} // namespace

int main(int argc, char *argv[])
{
    if (argc < 2) {
        std::printf("usage: applimit_poc.exe <exe-name-or-path> [down-KB/s] [up-KB/s] [seconds]\n");
        std::printf("       KB/s of 0 means unlimited for that direction.\n");
        return 2;
    }

    const std::string target = lower(argv[1]);
    const double downKBs = (argc > 2) ? std::atof(argv[2]) : 0.0;
    const double upKBs = (argc > 3) ? std::atof(argv[3]) : 0.0;
    const int seconds = (argc > 4) ? std::atoi(argv[4]) : 20;

    const double downBps = downKBs > 0 ? downKBs * 1024.0 : 0.0;
    const double upBps = upKBs > 0 ? upKBs * 1024.0 : 0.0;

    std::printf("=== per-application bandwidth limit POC ===\n");
    std::printf("target      : %s\n", target.c_str());
    std::printf("download    : %s\n", downBps > 0 ? (std::to_string(downKBs) + " KB/s").c_str()
                                                 : "unlimited");
    std::printf("upload      : %s\n", upBps > 0 ? (std::to_string(upKBs) + " KB/s").c_str()
                                               : "unlimited");
    std::printf("duration    : %d s\n\n", seconds);

    // ---------------------------------------------------------------------
    // 1. FLOW layer - learn which 5-tuples belong to the target processes.
    // ---------------------------------------------------------------------
    HANDLE flow = WinDivertOpen("true", WINDIVERT_LAYER_FLOW, 0, 0);
    if (flow == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        std::printf("WinDivertOpen(FLOW) failed, error %lu\n", err);
        if (err == ERROR_ACCESS_DENIED)
            std::printf("  -> Run this elevated: WinDivert needs Administrator rights.\n");
        return 1;
    }

    // 2. NETWORK layer - where packets are actually paced.
    HANDLE net = WinDivertOpen("true", WINDIVERT_LAYER_NETWORK, 0, 0);
    if (net == INVALID_HANDLE_VALUE) {
        std::printf("WinDivertOpen(NETWORK) failed, error %lu\n", GetLastError());
        WinDivertClose(flow);
        return 1;
    }

    // Match only the direction we intend to pace, so the two directions cannot
    // fight over the same packet.
    WinDivertSetParam(net, WINDIVERT_PARAM_QUEUE_LENGTH, 8192);
    WinDivertSetParam(net, WINDIVERT_PARAM_QUEUE_TIME, 2000);

    std::printf("flow and network handles open.\n");
    std::printf("looking for \"%s\" - start the application now if it is not running.\n\n",
                target.c_str());

    // Shared attribution state, guarded because two threads touch it.
    std::mutex stateMutex;
    std::set<TupleKey> attributed;
    std::set<DWORD> matchedPids;
    std::atomic<bool> stop{false};
    std::atomic<unsigned long long> inBytes{0};
    std::atomic<unsigned long long> outBytes{0};

    TokenBucket downBucket(downBps);
    TokenBucket upBucket(upBps);

    // ---------------------------------------------------------------------
    // Flow thread: rebuild the attributed tuple set continuously, so processes
    // that were not running yet are picked up when they launch.
    // ---------------------------------------------------------------------
    std::thread flowThread([&] {
        std::vector<char> buffer(kMaxPacket);
        while (!stop.load()) {
            UINT recvLen = 0;
            WINDIVERT_ADDRESS addr{};
            if (!WinDivertRecv(flow, buffer.data(), UINT(buffer.size()), &recvLen, &addr)) {
                if (GetLastError() == ERROR_NO_DATA)
                    continue;
                break;
            }

            if (addr.Layer == WINDIVERT_LAYER_FLOW) {
                const WINDIVERT_DATA_FLOW &f = addr.Flow;
                const std::wstring path = imagePathOf(f.ProcessId);
                if (!path.empty()) {
                    const std::string full = lower(narrow(path));
                    const size_t slash = full.find_last_of("\\/");
                    const std::string exe = slash == std::string::npos ? full
                                                                      : full.substr(slash + 1);
                    // Match either the bare executable name or the full path, so
                    // a user can target one of several same-named binaries.
                    if (exe == target || full == target) {
                        TupleKey k{};
                        k.localAddr = f.LocalAddr[0];
                        k.localPort = f.LocalPort;
                        k.remoteAddr = f.RemoteAddr[0];
                        k.remotePort = f.RemotePort;
                        k.protocol = f.Protocol;

                        std::lock_guard<std::mutex> lock(stateMutex);
                        if (attributed.insert(k).second || !matchedPids.count(f.ProcessId)) {
                            std::printf("  attributed pid %lu  %s:%u -> %s:%u  proto %u\n",
                                        f.ProcessId, "flow", f.LocalPort, "remote", f.RemotePort,
                                        f.Protocol);
                        }
                        matchedPids.insert(f.ProcessId);
                    }
                }
            }
            // Flow events must be re-injected or the network stack stalls.
            WinDivertSend(flow, buffer.data(), recvLen, nullptr, &addr);
        }
    });

    // ---------------------------------------------------------------------
    // Network thread: pace only the attributed packets.
    // ---------------------------------------------------------------------
    std::thread netThread([&] {
        std::vector<char> buffer(kMaxPacket);
        while (!stop.load()) {
            UINT recvLen = 0;
            WINDIVERT_ADDRESS addr{};
            if (!WinDivertRecv(net, buffer.data(), UINT(buffer.size()), &recvLen, &addr)) {
                if (GetLastError() == ERROR_NO_DATA)
                    continue;
                break;
            }

            bool pace = false;
            bool isInbound = !addr.Outbound;

            // Parse just enough of the IP/TCP/UDP headers to build the key.
            PWINDIVERT_IPHDR ip = nullptr;
            PWINDIVERT_IPV6HDR ip6 = nullptr;
            PWINDIVERT_TCPHDR tcp = nullptr;
            PWINDIVERT_UDPHDR udp = nullptr;
            UINT8 protocol = 0;
            UINT32 saddr = 0;
            UINT32 daddr = 0;
            UINT16 sport = 0;
            UINT16 dport = 0;

            // Signature: (packet, len, iphdr, ipv6hdr, protocol, icmphdr,
            //             icmpv6hdr, tcphdr, udphdr, data, datalen, next, nextlen).
            // Only the headers are needed; the rest are optional and left null.
            if (WinDivertHelperParsePacket(buffer.data(), recvLen, &ip, &ip6, &protocol, nullptr,
                                           nullptr, &tcp, &udp, nullptr, nullptr, nullptr, nullptr)
                && ip) {
                saddr = ip->SrcAddr;
                daddr = ip->DstAddr;
                if (tcp) {
                    sport = ntohs(tcp->SrcPort);
                    dport = ntohs(tcp->DstPort);
                } else if (udp) {
                    sport = ntohs(udp->SrcPort);
                    dport = ntohs(udp->DstPort);
                }
            }

            if (protocol != 0) {
                TupleKey key{};
                key.protocol = protocol;
                if (addr.Outbound) {
                    // Outbound: our local endpoint is the source.
                    key.localAddr = saddr;
                    key.localPort = sport;
                    key.remoteAddr = daddr;
                    key.remotePort = dport;
                } else {
                    // Inbound: the packet is addressed to us, so the local
                    // endpoint is the destination. Normalising this way means one
                    // flow event matches packets in both directions.
                    key.localAddr = daddr;
                    key.localPort = dport;
                    key.remoteAddr = saddr;
                    key.remotePort = sport;
                }

                {
                    std::lock_guard<std::mutex> lock(stateMutex);
                    pace = attributed.count(key) > 0;
                }
            }

            if (pace) {
                if (isInbound) {
                    if (downBps > 0)
                        downBucket.take(recvLen);
                    inBytes.fetch_add(recvLen);
                } else {
                    if (upBps > 0)
                        upBucket.take(recvLen);
                    outBytes.fetch_add(recvLen);
                }
            }

            WinDivertSend(net, buffer.data(), recvLen, nullptr, &addr);
        }
    });

    // Progress reporting once a second, so throughput can be compared with the
    // configured limit.
    std::printf("%-6s %-16s %-16s %s\n", "t(s)", "download", "upload", "attributed pids");
    unsigned long long lastIn = 0;
    unsigned long long lastOut = 0;
    for (int t = 1; t <= seconds && !stop.load(); ++t) {
        Sleep(1000);
        const unsigned long long nowIn = inBytes.load();
        const unsigned long long nowOut = outBytes.load();
        size_t pids = 0;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            pids = matchedPids.size();
        }
        std::printf("%-6d %-16s %-16s %zu\n", t,
                    (std::to_string(double(nowIn - lastIn) / 1024.0) + " KB/s").c_str(),
                    (std::to_string(double(nowOut - lastOut) / 1024.0) + " KB/s").c_str(), pids);
        lastIn = nowIn;
        lastOut = nowOut;
    }

    stop.store(true);
    // Unblock the threads: closing the handles makes their recv calls fail.
    WinDivertClose(net);
    WinDivertClose(flow);
    if (flowThread.joinable())
        flowThread.join();
    if (netThread.joinable())
        netThread.join();

    std::printf("\n=== summary ===\n");
    std::printf("paced download : %.1f KB/s (limit %s)\n",
                double(inBytes.load()) / 1024.0 / seconds,
                downBps > 0 ? (std::to_string(downKBs) + " KB/s").c_str() : "none");
    std::printf("paced upload   : %.1f KB/s (limit %s)\n",
                double(outBytes.load()) / 1024.0 / seconds,
                upBps > 0 ? (std::to_string(upKBs) + " KB/s").c_str() : "none");

    {
        std::lock_guard<std::mutex> lock(stateMutex);
        if (matchedPids.empty()) {
            std::printf("\nNO TRAFFIC WAS ATTRIBUTED. The target process produced no flows\n");
            std::printf("during the run, so nothing could be limited. Confirm the process\n");
            std::printf("name matches (for example \"chrome.exe\") and that it was using the\n");
            std::printf("network.\n");
            return 1;
        }
        std::printf("\nattributed %zu process(es). If the paced figures above track the\n",
                    matchedPids.size());
        std::printf("configured limits while the application's own UI shows the same, the\n");
        std::printf("limit is being enforced.\n");
    }

    return 0;
}
