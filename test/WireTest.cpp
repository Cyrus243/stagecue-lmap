// LMAP/1 wire-format conformance (spec §4.1, §4.5, §5, §10 tests 1-3).
#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <vector>

#include "lmap/Wire.h"

using namespace lmap;

namespace
{
    // A stream locked to 2ch/F32LE/1 frame per packet, matching the golden packet.
    StreamConfig lockedConfig()
    {
        StreamConfig c;
        c.stream_id        = 0;
        c.channel_count    = 2;
        c.sample_format    = SampleFormat::F32LE;
        c.frames_per_packet = 1;
        return c;
    }

    // Well-formed media packet matching lockedConfig(): header + 2 float samples.
    std::vector<std::uint8_t> makeValidPacket()
    {
        MediaHeader h{};
        std::memcpy(h.magic, kMagic, 4);
        h.version         = 1;
        h.flags           = 0;
        h.stream_id       = 0;
        h.media_timestamp = 0;
        h.sequence        = 0;
        h.frame_count     = 1;
        h.channel_count   = 2;
        h.sample_format   = std::uint8_t(SampleFormat::F32LE);

        std::vector<std::uint8_t> pkt(kMediaHeaderSize + 8);
        encodeMediaHeader(h, pkt.data());
        const float samples[2] = { 1.0f, -1.0f };
        std::memcpy(pkt.data() + kMediaHeaderSize, samples, 8);
        return pkt;
    }
}

// Test 1: layout matches spec §4.1 exactly (also enforced at compile time).
TEST(Wire, HeaderLayout)
{
    EXPECT_EQ(sizeof(MediaHeader), 24u);
    EXPECT_EQ(offsetof(MediaHeader, magic), 0u);
    EXPECT_EQ(offsetof(MediaHeader, version), 4u);
    EXPECT_EQ(offsetof(MediaHeader, flags), 5u);
    EXPECT_EQ(offsetof(MediaHeader, stream_id), 6u);
    EXPECT_EQ(offsetof(MediaHeader, media_timestamp), 8u);
    EXPECT_EQ(offsetof(MediaHeader, sequence), 16u);
    EXPECT_EQ(offsetof(MediaHeader, frame_count), 20u);
    EXPECT_EQ(offsetof(MediaHeader, channel_count), 22u);
    EXPECT_EQ(offsetof(MediaHeader, sample_format), 23u);
}

// Test 2: a hand-built golden packet decodes to the expected fields and payload.
TEST(Wire, GoldenPacketDecodes)
{
    // 24-byte header: magic, ver=1, flags=STREAM_START, stream_id=0x0007,
    // media_timestamp=0x0102030405060708, sequence=0x11223344, frame_count=1,
    // channel_count=2, sample_format=3. Then two floats 1.0f, -1.0f.
    const std::array<std::uint8_t, 32> golden = {
        'L', 'M', 'A', 'P',       // magic
        0x01,                     // version
        0x01,                     // flags = STREAM_START
        0x07, 0x00,               // stream_id = 7 (LE)
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, // media_timestamp (LE)
        0x44, 0x33, 0x22, 0x11,   // sequence = 0x11223344 (LE)
        0x01, 0x00,               // frame_count = 1 (LE)
        0x02,                     // channel_count = 2
        0x03,                     // sample_format = F32LE
        0x00, 0x00, 0x80, 0x3F,   // 1.0f
        0x00, 0x00, 0x80, 0xBF,   // -1.0f
    };

    const MediaHeader h = decodeMediaHeader(golden.data());
    EXPECT_EQ(0, std::memcmp(h.magic, kMagic, 4));
    EXPECT_EQ(h.version, 1u);
    EXPECT_EQ(h.flags, flag::STREAM_START);
    EXPECT_EQ(h.stream_id, 7u);
    EXPECT_EQ(h.media_timestamp, 0x0102030405060708ull);
    EXPECT_EQ(h.sequence, 0x11223344u);
    EXPECT_EQ(h.frame_count, 1u);
    EXPECT_EQ(h.channel_count, 2u);
    EXPECT_EQ(h.sample_format, std::uint8_t(SampleFormat::F32LE));

    float s0 = 0, s1 = 0;
    std::memcpy(&s0, golden.data() + 24, 4);
    std::memcpy(&s1, golden.data() + 28, 4);
    EXPECT_FLOAT_EQ(s0, 1.0f);
    EXPECT_FLOAT_EQ(s1, -1.0f);
}

TEST(Wire, HeaderRoundTrip)
{
    auto pkt = makeValidPacket();
    const MediaHeader h = decodeMediaHeader(pkt.data());
    std::array<std::uint8_t, 24> reencoded{};
    encodeMediaHeader(h, reencoded.data());
    EXPECT_EQ(0, std::memcmp(pkt.data(), reencoded.data(), 24));
}

// Test 3: every rejection rule in §4.5 drops the datagram with the right reason.
TEST(Wire, ValidatesGoodPacket)
{
    auto pkt = makeValidPacket();
    EXPECT_EQ(validateMedia(pkt.data(), pkt.size(), lockedConfig()), Reject::None);
}

TEST(Wire, Rejects_TooShort)
{
    auto pkt = makeValidPacket();
    EXPECT_EQ(validateMedia(pkt.data(), 23, lockedConfig()), Reject::TooShort);
}

TEST(Wire, Rejects_BadMagic)
{
    auto pkt = makeValidPacket();
    pkt[0] = 'X';
    EXPECT_EQ(validateMedia(pkt.data(), pkt.size(), lockedConfig()), Reject::BadMagic);
}

TEST(Wire, Rejects_BadVersion)
{
    auto pkt = makeValidPacket();
    pkt[4] = 2;
    EXPECT_EQ(validateMedia(pkt.data(), pkt.size(), lockedConfig()), Reject::BadVersion);
}

TEST(Wire, Rejects_StreamIdMismatch)
{
    auto pkt = makeValidPacket();
    auto cfg = lockedConfig();
    cfg.stream_id = 5;
    EXPECT_EQ(validateMedia(pkt.data(), pkt.size(), cfg), Reject::StreamIdMismatch);
}

TEST(Wire, Rejects_BadFrameCount)
{
    auto cfg = lockedConfig();
    {
        auto pkt = makeValidPacket();
        writeLE16(pkt.data() + 20, 0); // frame_count = 0
        EXPECT_EQ(validateMedia(pkt.data(), pkt.size(), cfg), Reject::BadFrameCount);
    }
    {
        auto pkt = makeValidPacket();
        writeLE16(pkt.data() + 20, 1025); // > 1024
        EXPECT_EQ(validateMedia(pkt.data(), pkt.size(), cfg), Reject::BadFrameCount);
    }
}

TEST(Wire, Rejects_BadChannelCount)
{
    auto pkt = makeValidPacket();
    pkt[22] = 0;
    EXPECT_EQ(validateMedia(pkt.data(), pkt.size(), lockedConfig()), Reject::BadChannelCount);
    pkt[22] = 65; // > 64
    EXPECT_EQ(validateMedia(pkt.data(), pkt.size(), lockedConfig()), Reject::BadChannelCount);
}

TEST(Wire, Rejects_BadSampleFormat)
{
    auto pkt = makeValidPacket();
    pkt[23] = 4; // > 3
    EXPECT_EQ(validateMedia(pkt.data(), pkt.size(), lockedConfig()), Reject::BadSampleFormat);
}

TEST(Wire, Rejects_ConfigMismatch)
{
    {
        auto pkt = makeValidPacket();
        pkt[22] = 3; // valid range, but locked config is 2 channels
        EXPECT_EQ(validateMedia(pkt.data(), pkt.size(), lockedConfig()), Reject::ConfigMismatch);
    }
    {
        auto pkt = makeValidPacket();
        pkt[23] = std::uint8_t(SampleFormat::S32LE); // valid, but locked is F32LE
        // payload length would also mismatch, but rule 8 fires before rule 10.
        EXPECT_EQ(validateMedia(pkt.data(), pkt.size(), lockedConfig()), Reject::ConfigMismatch);
    }
}

TEST(Wire, Rejects_SilenceLengthMismatch)
{
    auto pkt = makeValidPacket();
    pkt[5] |= flag::SILENCE; // SILENCE set but datagram carries a payload
    EXPECT_EQ(validateMedia(pkt.data(), pkt.size(), lockedConfig()), Reject::SilenceLengthMismatch);
}

TEST(Wire, AcceptsSilencePacket)
{
    MediaHeader h{};
    std::memcpy(h.magic, kMagic, 4);
    h.version = 1; h.flags = flag::SILENCE; h.stream_id = 0;
    h.frame_count = 1; h.channel_count = 2; h.sample_format = std::uint8_t(SampleFormat::F32LE);
    std::array<std::uint8_t, 24> pkt{};
    encodeMediaHeader(h, pkt.data());
    EXPECT_EQ(validateMedia(pkt.data(), pkt.size(), lockedConfig()), Reject::None);
}

// The security-critical rule: the header must not be trusted to imply a payload
// larger than the datagram actually contains.
TEST(Wire, Rejects_HeaderClaimsMorePayloadThanDatagram)
{
    // Header claims frame_count=1, 2ch, F32LE -> 8 payload bytes (32 total), but the
    // datagram is only 24 bytes.
    MediaHeader h{};
    std::memcpy(h.magic, kMagic, 4);
    h.version = 1; h.flags = 0; h.stream_id = 0;
    h.frame_count = 1; h.channel_count = 2; h.sample_format = std::uint8_t(SampleFormat::F32LE);
    std::array<std::uint8_t, 24> pkt{};
    encodeMediaHeader(h, pkt.data());
    EXPECT_EQ(validateMedia(pkt.data(), pkt.size(), lockedConfig()), Reject::LengthMismatch);
}

TEST(Wire, Rejects_LengthMismatchTooLong)
{
    auto pkt = makeValidPacket();
    pkt.push_back(0); // one extra trailing byte
    EXPECT_EQ(validateMedia(pkt.data(), pkt.size(), lockedConfig()), Reject::LengthMismatch);
}

// ANNOUNCE / BYE round-trips, and the network-byte-order media_group field (§5.1).
TEST(Wire, AnnounceRoundTrip)
{
    AnnounceFields a;
    a.stream_id = 3; a.session_id = 0xDEADBEEF; a.sample_rate = 48000;
    a.frames_per_packet = 48; a.channel_count = 8;
    a.sample_format = std::uint8_t(SampleFormat::F32LE);
    a.media_group_net = 0x1053455Eu; // arbitrary 4 bytes, treated verbatim (network order)
    a.media_port = 5006; a.stream_flags = 1;
    std::memcpy(a.name, "mix", 3);

    std::array<std::uint8_t, kAnnounceSize> buf{};
    encodeAnnounce(a, buf.data());

    AnnounceFields d;
    ASSERT_TRUE(decodeAnnounce(buf.data(), buf.size(), d));
    EXPECT_EQ(d.stream_id, a.stream_id);
    EXPECT_EQ(d.session_id, a.session_id);
    EXPECT_EQ(d.sample_rate, a.sample_rate);
    EXPECT_EQ(d.frames_per_packet, a.frames_per_packet);
    EXPECT_EQ(d.channel_count, a.channel_count);
    EXPECT_EQ(d.sample_format, a.sample_format);
    EXPECT_EQ(d.media_group_net, a.media_group_net); // verbatim, no LE swap
    EXPECT_EQ(d.media_port, a.media_port);
    EXPECT_EQ(d.stream_flags, a.stream_flags);
    EXPECT_EQ(0, std::memcmp(d.name, "mix", 3));
}

TEST(Wire, MediaGroupIsNetworkByteOrderOnWire)
{
    AnnounceFields a;
    a.media_group_net = 0x1053455Eu; // == 239.69.83.16 bytes on the wire, verbatim
    std::array<std::uint8_t, kAnnounceSize> buf{};
    encodeAnnounce(a, buf.data());
    // The 4 wire bytes at offset 20 equal the raw address bytes, not LE-swapped.
    std::uint32_t onWire = 0;
    std::memcpy(&onWire, buf.data() + 20, 4);
    EXPECT_EQ(onWire, 0x1053455Eu);
}

TEST(Wire, ByeRoundTrip)
{
    ByeFields b; b.stream_id = 9; b.session_id = 0x01020304;
    std::array<std::uint8_t, kByeSize> buf{};
    encodeBye(b, buf.data());
    ByeFields d;
    ASSERT_TRUE(decodeBye(buf.data(), buf.size(), d));
    EXPECT_EQ(d.stream_id, 9u);
    EXPECT_EQ(d.session_id, 0x01020304u);
}
