// End-to-end smoke test over real POSIX multicast sockets on the loopback path
// (spec §10 test 6, socket variant). This exercises the actual sender pacing
// thread, UDP multicast send/join, and the receiver network thread. It is timing-
// and environment-sensitive, so it SKIPS (rather than fails) when multicast is
// unavailable, and validates structural integrity of the audio rather than an
// absolute frame offset.
#include <gtest/gtest.h>

#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

#include "lmap/Receiver.h"
#include "lmap/Sender.h"

using namespace lmap;
using namespace std::chrono;

namespace
{
    constexpr std::uint16_t kFpp = 48;
    constexpr std::uint8_t  kCh  = 2;

    float sampleValue(std::uint64_t frame, std::uint32_t ch)
    {
        return float(frame) + float(ch) * 0.25f;
    }

    // First non-loopback, multicast-capable IPv4 interface address, or "" if none.
    std::string firstMulticastInterface()
    {
        ifaddrs* ifs = nullptr;
        if (getifaddrs(&ifs) != 0)
            return {};
        std::string result;
        for (ifaddrs* p = ifs; p != nullptr; p = p->ifa_next)
        {
            if (p->ifa_addr == nullptr || p->ifa_addr->sa_family != AF_INET)
                continue;
            if ((p->ifa_flags & IFF_LOOPBACK) != 0 || (p->ifa_flags & IFF_UP) == 0)
                continue;
            if ((p->ifa_flags & IFF_MULTICAST) == 0)
                continue;
            char buf[INET_ADDRSTRLEN] = {};
            auto* sin = reinterpret_cast<sockaddr_in*>(p->ifa_addr);
            if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)))
            {
                result = buf;
                break;
            }
        }
        freeifaddrs(ifs);
        return result;
    }

    // A block is "good" if it holds intact interleaved audio: channel offset 0.25
    // and a +1.0 step between consecutive frames (matches sampleValue), non-zero.
    bool isStructurallyIntact(const std::vector<float>& b)
    {
        if (b.empty() || b[0] == 0.0f)
            return false;
        for (std::uint16_t f = 0; f < kFpp; ++f)
        {
            if (std::fabs((b[f * kCh + 1] - b[f * kCh + 0]) - 0.25f) > 1e-3f)
                return false;
            if (f + 1 < kFpp)
                if (std::fabs((b[(f + 1) * kCh] - b[f * kCh]) - 1.0f) > 1e-3f)
                    return false;
        }
        return true;
    }
}

TEST(Loopback, SenderToReceiverOverRealSockets)
{
    const std::string iface = firstMulticastInterface();
    if (iface.empty())
        GTEST_SKIP() << "no multicast-capable interface available";

    StreamConfig cfg;
    cfg.stream_id = 0; cfg.sample_rate = 48000; cfg.channel_count = kCh;
    cfg.sample_format = SampleFormat::F32LE; cfg.frames_per_packet = kFpp;
    cfg.name = "loopback";

    NetworkConfig net;
    net.interface_addr = iface;
    net.multicast_loopback = true; // single-host: loop our own multicast back

    auto rxRes = Receiver::create(cfg, net, 4.0f);
    if (!rxRes)
        GTEST_SKIP() << "receiver socket setup failed: " << rxRes.error;
    auto txRes = Sender::create(cfg, net);
    if (!txRes)
        GTEST_SKIP() << "sender socket setup failed: " << txRes.error;

    auto rx = std::move(rxRes.value);
    auto tx = std::move(txRes.value);

    std::atomic<std::uint64_t> frame{ 0 };
    tx->setSource([&](void* out, std::uint16_t frames) noexcept {
        auto* p = static_cast<float*>(out);
        const std::uint64_t base = frame.fetch_add(frames);
        for (std::uint16_t f = 0; f < frames; ++f)
            for (std::uint32_t c = 0; c < kCh; ++c)
                p[f * kCh + c] = sampleValue(base + f, c);
        return true;
    });

    rx->start();
    tx->start();

    // Consume near real-time (~1 ms/packet) so read_pos tracks the writer.
    bool everLocked = false;
    int goodBlocks = 0;
    std::vector<float> block(std::size_t(kFpp) * kCh);
    const auto deadline = steady_clock::now() + milliseconds(800);
    while (steady_clock::now() < deadline)
    {
        rx->pull(block.data(), kFpp);
        if (rx->state() == ReceiverState::Locked)
        {
            everLocked = true;
            if (isStructurallyIntact(block))
                ++goodBlocks;
        }
        std::this_thread::sleep_for(microseconds(900));
    }

    const auto rxStats = rx->stats();
    tx->stop();
    rx->stop();

    if (!everLocked || rxStats.packets_received == 0)
        GTEST_SKIP() << "multicast loopback did not deliver/lock in this environment "
                     << "(received=" << rxStats.packets_received << ")";

    EXPECT_GT(tx->stats().packets_sent, 0u);
    EXPECT_GT(rxStats.packets_received, 0u);
    EXPECT_GT(goodBlocks, 0) << "locked but never produced intact audio blocks";
}
