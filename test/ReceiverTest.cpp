// Deterministic, in-process receiver conformance (spec §10 tests 6-10 + re-lock).
// The receiver is fed hand-built datagrams via ingest() and read via pull(), with
// clock correction disabled so read_pos advances at exactly the nominal rate and
// output is bit-exact.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "lmap/Receiver.h"
#include "lmap/Wire.h"

using namespace lmap;

namespace
{
    constexpr std::uint16_t kFpp = 48;
    constexpr std::uint8_t  kCh  = 2;
    constexpr std::uint32_t kSampleRate = 48000;
    constexpr std::uint32_t kTargetFrames = 192; // 4 ms @ 48 kHz

    // Deterministic sample generator; `bias` distinguishes sessions.
    float sampleValue(std::uint64_t frame, std::uint32_t ch, float bias = 0.0f)
    {
        return float(frame) + float(ch) * 0.25f + bias;
    }

    struct PacketMaker
    {
        StreamConfig cfg;
        float        bias = 0.0f;

        std::vector<std::uint8_t> make(std::uint64_t ts, std::uint32_t seq, std::uint8_t flags) const
        {
            std::vector<std::uint8_t> pkt(kMediaHeaderSize + std::size_t(kFpp) * kCh * 4);
            MediaHeader h{};
            std::memcpy(h.magic, kMagic, 4);
            h.version = 1; h.flags = flags; h.stream_id = cfg.stream_id;
            h.media_timestamp = ts; h.sequence = seq; h.frame_count = kFpp;
            h.channel_count = kCh; h.sample_format = std::uint8_t(SampleFormat::F32LE);
            encodeMediaHeader(h, pkt.data());

            auto* p = reinterpret_cast<float*>(pkt.data() + kMediaHeaderSize);
            for (std::uint16_t f = 0; f < kFpp; ++f)
                for (std::uint32_t c = 0; c < kCh; ++c)
                    p[f * kCh + c] = sampleValue(ts + f, c, bias);
            return pkt;
        }
    };

    class ReceiverTest : public ::testing::Test
    {
    protected:
        StreamConfig cfg;
        std::unique_ptr<Receiver> rx;
        PacketMaker maker;

        void SetUp() override
        {
            cfg.stream_id = 0; cfg.sample_rate = kSampleRate; cfg.channel_count = kCh;
            cfg.sample_format = SampleFormat::F32LE; cfg.frames_per_packet = kFpp;
            auto res = Receiver::createOffline(cfg, 4.0f);
            ASSERT_TRUE(res) << res.error;
            rx = std::move(res.value);
            rx->setClockCorrectionEnabled(false); // rate_ratio = 1.0 (bit-exact)
            maker.cfg = cfg;
        }

        void feed(std::uint64_t ts, std::uint32_t seq, std::uint8_t flags = 0)
        {
            auto pkt = maker.make(ts, seq, flags);
            rx->ingest(pkt.data(), pkt.size());
        }

        // Feed `n` in-order packets starting at (ts0, seq0); STREAM_START on first 3.
        void feedInOrder(std::uint32_t n, std::uint64_t ts0 = 0, std::uint32_t seq0 = 0)
        {
            for (std::uint32_t i = 0; i < n; ++i)
            {
                const std::uint8_t flags = (seq0 + i < 3) ? flag::STREAM_START : 0;
                feed(ts0 + std::uint64_t(i) * kFpp, seq0 + i, flags);
            }
        }

        std::vector<float> pullBlock()
        {
            std::vector<float> out(std::size_t(kFpp) * kCh, -999.0f);
            rx->pull(out.data(), kFpp);
            return out;
        }
    };
}

// Test 6: bit-exact playout with rate_ratio forced to 1.0, no loss.
TEST_F(ReceiverTest, BitExactPlayout)
{
    feedInOrder(40); // 1920 frames, within ring capacity; locks during feeding
    EXPECT_EQ(rx->state(), ReceiverState::Locked);

    std::uint64_t startFrame = kTargetFrames; // read_pos anchored here on LOCKED
    for (int b = 0; b < 30; ++b)
    {
        auto out = pullBlock();
        for (std::uint16_t f = 0; f < kFpp; ++f)
            for (std::uint32_t c = 0; c < kCh; ++c)
                ASSERT_EQ(out[f * kCh + c], sampleValue(startFrame + f, c))
                    << "block " << b << " frame " << f << " ch " << c;
        startFrame += kFpp;
    }
}

// Test 7: reordering n, n+2, n+1, n+3 — output is still bit-exact.
TEST_F(ReceiverTest, ReorderingIsBitExact)
{
    // Establish the anchor (ts=0) and lock in order, so the first-arriving packet
    // is the true stream start; then deliver the remaining packets in swapped pairs.
    feedInOrder(4); // seq 0..3 -> locks with anchor at 0
    for (std::uint32_t i = 4; i < 40; i += 2)
    {
        feed(std::uint64_t(i + 1) * kFpp, i + 1); // n+1 first
        feed(std::uint64_t(i) * kFpp, i);         // then n
    }
    EXPECT_EQ(rx->state(), ReceiverState::Locked);

    std::uint64_t startFrame = kTargetFrames;
    for (int b = 0; b < 30; ++b)
    {
        auto out = pullBlock();
        for (std::uint16_t f = 0; f < kFpp; ++f)
            for (std::uint32_t c = 0; c < kCh; ++c)
                ASSERT_EQ(out[f * kCh + c], sampleValue(startFrame + f, c));
        startFrame += kFpp;
    }
}

// Test 8: duplicates — output bit-exact and the duplicate counter is correct.
TEST_F(ReceiverTest, DuplicatesCountedAndHarmless)
{
    std::uint32_t dupCount = 0;
    for (std::uint32_t i = 0; i < 40; ++i)
    {
        const std::uint8_t flags = (i < 3) ? flag::STREAM_START : 0;
        feed(std::uint64_t(i) * kFpp, i, flags);
        feed(std::uint64_t(i) * kFpp, i, flags | flag::DUPLICATE); // redundant copy
        ++dupCount;
    }
    EXPECT_EQ(rx->state(), ReceiverState::Locked);
    EXPECT_EQ(rx->stats().packets_duplicate, dupCount);

    std::uint64_t startFrame = kTargetFrames;
    for (int b = 0; b < 30; ++b)
    {
        auto out = pullBlock();
        for (std::uint16_t f = 0; f < kFpp; ++f)
            for (std::uint32_t c = 0; c < kCh; ++c)
                ASSERT_EQ(out[f * kCh + c], sampleValue(startFrame + f, c));
        startFrame += kFpp;
    }
}

// Test 9: a lost packet is concealed as silence; no stall, no timing shift.
TEST_F(ReceiverTest, LostPacketConcealedNoTimingShift)
{
    const std::uint32_t lostIndex = 10; // ts = 480, seq = 10 is never delivered
    for (std::uint32_t i = 0; i < 40; ++i)
    {
        if (i == lostIndex)
            continue;
        const std::uint8_t flags = (i < 3) ? flag::STREAM_START : 0;
        feed(std::uint64_t(i) * kFpp, i, flags);
    }
    EXPECT_EQ(rx->state(), ReceiverState::Locked);
    EXPECT_GE(rx->stats().packets_lost, 1u);

    const std::uint64_t gapStart = std::uint64_t(lostIndex) * kFpp; // 480
    std::uint64_t startFrame = kTargetFrames;
    for (int b = 0; b < 30; ++b)
    {
        auto out = pullBlock();
        for (std::uint16_t f = 0; f < kFpp; ++f)
        {
            const std::uint64_t frame = startFrame + f;
            const bool inGap = frame >= gapStart && frame < gapStart + kFpp;
            for (std::uint32_t c = 0; c < kCh; ++c)
            {
                if (inGap)
                    ASSERT_EQ(out[f * kCh + c], 0.0f); // concealed with silence
                else
                    ASSERT_EQ(out[f * kCh + c], sampleValue(frame, c)); // unaffected
            }
        }
        startFrame += kFpp;
    }
    // read_pos advanced by exactly 30 blocks -> still LOCKED, no stall.
    EXPECT_EQ(rx->state(), ReceiverState::Locked);
}

// Test 10: a packet already behind read_pos is counted late, dropped, output intact.
TEST_F(ReceiverTest, LatePacketCountedAndDropped)
{
    feedInOrder(40);
    // Advance read_pos to ~480 by pulling 6 blocks.
    for (int b = 0; b < 6; ++b)
        pullBlock();

    const auto before = rx->stats();
    feed(/*ts=*/kFpp, /*seq=*/1000, /*flags=*/0); // ts=48, far behind read_pos
    const auto after = rx->stats();
    EXPECT_EQ(after.packets_late, before.packets_late + 1);

    // Continuing playout is unaffected.
    std::uint64_t startFrame = kTargetFrames + 6 * kFpp;
    for (int b = 0; b < 10; ++b)
    {
        auto out = pullBlock();
        for (std::uint16_t f = 0; f < kFpp; ++f)
            for (std::uint32_t c = 0; c < kCh; ++c)
                ASSERT_EQ(out[f * kCh + c], sampleValue(startFrame + f, c));
        startFrame += kFpp;
    }
}

// A new session (STREAM_START, ts=0) mid-lock resets and re-locks on the new stream.
TEST_F(ReceiverTest, ReLocksOnNewSession)
{
    feedInOrder(40);
    ASSERT_EQ(rx->state(), ReceiverState::Locked);
    for (int b = 0; b < 5; ++b)
        pullBlock();

    // New session: different data, restarting at ts=0.
    maker.bias = 1000.0f;
    for (std::uint32_t i = 0; i < 6; ++i)
    {
        const std::uint8_t flags = (i < 3) ? flag::STREAM_START : 0;
        feed(std::uint64_t(i) * kFpp, i, flags);
    }
    ASSERT_EQ(rx->state(), ReceiverState::Locked);

    std::uint64_t startFrame = kTargetFrames; // re-anchored at target of the new session
    auto out = pullBlock();
    for (std::uint16_t f = 0; f < kFpp; ++f)
        for (std::uint32_t c = 0; c < kCh; ++c)
            ASSERT_EQ(out[f * kCh + c], sampleValue(startFrame + f, c, 1000.0f));
}
