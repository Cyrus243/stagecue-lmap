// Ring-by-timestamp buffer behaviour (spec §7.2): out-of-order/duplicate handling,
// gap detection, and stale-lap distinction.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "lmap/RingByTimestamp.h"

using namespace lmap;

namespace
{
    constexpr std::uint16_t kFpp        = 4;
    constexpr std::uint32_t kFrameBytes = 4; // one uint32 per frame

    // Write a packet at `ts` whose frame k holds the value (ts + k).
    void writePacket(RingByTimestamp& ring, std::uint64_t ts)
    {
        std::uint32_t buf[kFpp];
        for (std::uint16_t k = 0; k < kFpp; ++k)
            buf[k] = std::uint32_t(ts) + k;
        ring.write(ts, kFpp, reinterpret_cast<const std::uint8_t*>(buf));
    }

    RingByTimestamp makeRing()
    {
        return RingByTimestamp(kFpp, kFrameBytes, 2048);
    }
}

TEST(Ring, WriteThenReadIsExact)
{
    auto ring = makeRing();
    writePacket(ring, 0);

    std::uint32_t out[kFpp] = {};
    EXPECT_TRUE(ring.read(0, kFpp, reinterpret_cast<std::uint8_t*>(out)));
    for (std::uint16_t k = 0; k < kFpp; ++k)
        EXPECT_EQ(out[k], k);
    EXPECT_EQ(ring.writeHighWater(), kFpp);
}

TEST(Ring, GapReadsAsSilence)
{
    auto ring = makeRing();
    // Never written: reading returns silence and reports invalid.
    std::uint32_t out[kFpp] = { 9, 9, 9, 9 };
    EXPECT_FALSE(ring.read(0, kFpp, reinterpret_cast<std::uint8_t*>(out)));
    for (std::uint16_t k = 0; k < kFpp; ++k)
        EXPECT_EQ(out[k], 0u);
}

// Test 7 (reordering): packets written n, n+2, n+1, n+3 read back in order, exact.
TEST(Ring, OutOfOrderWritesLandInPlace)
{
    auto ring = makeRing();
    writePacket(ring, 0);
    writePacket(ring, 2 * kFpp);
    writePacket(ring, 1 * kFpp);
    writePacket(ring, 3 * kFpp);

    std::uint32_t out[4 * kFpp] = {};
    EXPECT_TRUE(ring.read(0, 4 * kFpp, reinterpret_cast<std::uint8_t*>(out)));
    for (std::uint32_t i = 0; i < 4 * kFpp; ++i)
        EXPECT_EQ(out[i], i);
    EXPECT_EQ(ring.writeHighWater(), 4u * kFpp);
}

// Test 8 (duplicates): rewriting the same slot with identical data is harmless.
TEST(Ring, DuplicateWriteIsHarmless)
{
    auto ring = makeRing();
    writePacket(ring, kFpp);
    writePacket(ring, kFpp); // duplicate

    std::uint32_t out[kFpp] = {};
    EXPECT_TRUE(ring.read(kFpp, kFpp, reinterpret_cast<std::uint8_t*>(out)));
    for (std::uint16_t k = 0; k < kFpp; ++k)
        EXPECT_EQ(out[k], std::uint32_t(kFpp) + k);
}

// A later lap overwrites a slot; the old timestamp must now read as a gap, not as
// stale data (the validity marker distinguishes laps).
TEST(Ring, StaleLapIsNotMistakenForValid)
{
    auto ring = makeRing();
    const std::uint64_t cap = ring.capacityFrames();

    writePacket(ring, 0);
    std::uint32_t out[kFpp] = {};
    ASSERT_TRUE(ring.read(0, kFpp, reinterpret_cast<std::uint8_t*>(out)));

    // Write one full lap ahead; this lands in the same slot as ts=0.
    writePacket(ring, cap);

    // Reading the old ts=0 span must now be treated as a gap.
    EXPECT_FALSE(ring.read(0, kFpp, reinterpret_cast<std::uint8_t*>(out)));
    // The new packet at ts=cap reads back exact.
    EXPECT_TRUE(ring.read(cap, kFpp, reinterpret_cast<std::uint8_t*>(out)));
    EXPECT_EQ(out[0], std::uint32_t(cap));
}

TEST(Ring, SilencePacketZeroFills)
{
    auto ring = makeRing();
    ring.write(0, kFpp, nullptr); // SILENCE
    std::uint32_t out[kFpp] = { 7, 7, 7, 7 };
    EXPECT_TRUE(ring.read(0, kFpp, reinterpret_cast<std::uint8_t*>(out))); // valid, but zero
    for (std::uint16_t k = 0; k < kFpp; ++k)
        EXPECT_EQ(out[k], 0u);
}

TEST(Ring, ClearResetsValidityAndHighWater)
{
    auto ring = makeRing();
    writePacket(ring, 0);
    ring.clear();
    EXPECT_EQ(ring.writeHighWater(), 0u);
    std::uint32_t out[kFpp] = {};
    EXPECT_FALSE(ring.read(0, kFpp, reinterpret_cast<std::uint8_t*>(out)));
}
