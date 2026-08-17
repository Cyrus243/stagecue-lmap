#include "lmap/Wire.h"

#include <algorithm>
#include <array>
#include <string>

namespace lmap
{
    std::optional<std::string> validateConfig(const StreamConfig& cfg)
    {
        static constexpr std::array<std::uint32_t, 6> kValidRates{
            44100, 48000, 88200, 96000, 176400, 192000 };
        if (std::find(kValidRates.begin(), kValidRates.end(), cfg.sample_rate) == kValidRates.end())
            return "sample_rate " + std::to_string(cfg.sample_rate) + " is not a supported rate";

        if (cfg.channel_count == 0 || cfg.channel_count > kMaxChannelCount)
            return "channel_count must be in 1.." + std::to_string(kMaxChannelCount);

        if (static_cast<std::uint8_t>(cfg.sample_format) > static_cast<std::uint8_t>(SampleFormat::F32LE))
            return "sample_format is out of range";

        if (cfg.frames_per_packet == 0)
            return "frames_per_packet must be >= 1";

        // A single frame must itself fit within one MTU (spec §4.4): otherwise the
        // deployment must be split into multiple streams carrying disjoint channels.
        const std::size_t bytesPerFrame =
            static_cast<std::size_t>(cfg.channel_count) * bytesPerSample(cfg.sample_format);
        if (bytesPerFrame > kMaxPayloadPerPacket)
            return "a single frame (" + std::to_string(bytesPerFrame)
                 + " bytes) exceeds the MTU budget; split the channels across multiple streams";

        const std::uint16_t maxFpp = maxFramesPerPacket(cfg.channel_count, cfg.sample_format);
        if (cfg.frames_per_packet > maxFpp)
            return "frames_per_packet " + std::to_string(cfg.frames_per_packet)
                 + " exceeds the MTU limit of " + std::to_string(maxFpp)
                 + " for this channel count/format";

        return std::nullopt;
    }

    void encodeMediaHeader(const MediaHeader& h, std::uint8_t* out) noexcept
    {
        std::memcpy(out, h.magic, 4);
        out[4] = h.version;
        out[5] = h.flags;
        writeLE16(out + 6, h.stream_id);
        writeLE64(out + 8, h.media_timestamp);
        writeLE32(out + 16, h.sequence);
        writeLE16(out + 20, h.frame_count);
        out[22] = h.channel_count;
        out[23] = h.sample_format;
    }

    MediaHeader decodeMediaHeader(const std::uint8_t* in) noexcept
    {
        MediaHeader h{};
        std::memcpy(h.magic, in, 4);
        h.version         = in[4];
        h.flags           = in[5];
        h.stream_id       = readLE16(in + 6);
        h.media_timestamp = readLE64(in + 8);
        h.sequence        = readLE32(in + 16);
        h.frame_count     = readLE16(in + 20);
        h.channel_count   = in[22];
        h.sample_format   = in[23];
        return h;
    }

    void encodeAnnounce(const AnnounceFields& a, std::uint8_t* out) noexcept
    {
        std::memcpy(out, kMagic, 4);
        out[4] = kVersion;
        out[5] = static_cast<std::uint8_t>(MsgType::Announce);
        writeLE16(out + 6, a.stream_id);
        writeLE32(out + 8, a.session_id);
        writeLE32(out + 12, a.sample_rate);
        writeLE16(out + 16, a.frames_per_packet);
        out[18] = a.channel_count;
        out[19] = a.sample_format;
        // media_group is network byte order: copy the 4 address bytes verbatim.
        std::memcpy(out + 20, &a.media_group_net, 4);
        writeLE16(out + 24, a.media_port);
        writeLE16(out + 26, a.stream_flags);
        writeLE32(out + 28, 0); // reserved
        std::memcpy(out + 32, a.name, 32);
    }

    bool decodeAnnounce(const std::uint8_t* in, std::size_t len, AnnounceFields& out) noexcept
    {
        if (len < kAnnounceSize)
            return false;
        if (std::memcmp(in, kMagic, 4) != 0 || in[4] != kVersion
            || in[5] != static_cast<std::uint8_t>(MsgType::Announce))
            return false;

        out.stream_id         = readLE16(in + 6);
        out.session_id        = readLE32(in + 8);
        out.sample_rate       = readLE32(in + 12);
        out.frames_per_packet = readLE16(in + 16);
        out.channel_count     = in[18];
        out.sample_format     = in[19];
        std::memcpy(&out.media_group_net, in + 20, 4); // network byte order, verbatim
        out.media_port        = readLE16(in + 24);
        out.stream_flags      = readLE16(in + 26);
        std::memcpy(out.name, in + 32, 32);
        return true;
    }

    void encodeBye(const ByeFields& b, std::uint8_t* out) noexcept
    {
        std::memcpy(out, kMagic, 4);
        out[4] = kVersion;
        out[5] = static_cast<std::uint8_t>(MsgType::Bye);
        writeLE16(out + 6, b.stream_id);
        writeLE32(out + 8, b.session_id);
        writeLE32(out + 12, 0); // reserved
    }

    bool decodeBye(const std::uint8_t* in, std::size_t len, ByeFields& out) noexcept
    {
        if (len < kByeSize)
            return false;
        if (std::memcmp(in, kMagic, 4) != 0 || in[4] != kVersion
            || in[5] != static_cast<std::uint8_t>(MsgType::Bye))
            return false;

        out.stream_id  = readLE16(in + 6);
        out.session_id = readLE32(in + 8);
        return true;
    }

    Reject validateMedia(const std::uint8_t* data, std::size_t len, const StreamConfig& locked) noexcept
    {
        // Rule 1: minimum length to even contain a header.
        if (len < kMediaHeaderSize)
            return Reject::TooShort;

        // Rule 2: magic compared as a 4-byte sequence.
        if (std::memcmp(data, kMagic, 4) != 0)
            return Reject::BadMagic;

        // Rule 3: version.
        if (data[4] != kVersion)
            return Reject::BadVersion;

        const std::uint8_t  flags        = data[5];
        const std::uint16_t stream_id    = readLE16(data + 6);
        const std::uint16_t frame_count  = readLE16(data + 20);
        const std::uint8_t  channel_count = data[22];
        const std::uint8_t  sample_format = data[23];

        // Rule 4: stream_id must match the subscribed stream.
        if (stream_id != locked.stream_id)
            return Reject::StreamIdMismatch;

        // Rule 5: frame_count range.
        if (frame_count == 0 || frame_count > kMaxFrameCount)
            return Reject::BadFrameCount;

        // Rule 6: channel_count range.
        if (channel_count == 0 || channel_count > kMaxChannelCount)
            return Reject::BadChannelCount;

        // Rule 7: sample_format range (0..3).
        if (sample_format > static_cast<std::uint8_t>(SampleFormat::F32LE))
            return Reject::BadSampleFormat;

        // Rule 8: channel_count / sample_format must match the locked config.
        if (channel_count != locked.channel_count
            || sample_format != static_cast<std::uint8_t>(locked.sample_format))
            return Reject::ConfigMismatch;

        // Rule 9: SILENCE packets carry no payload.
        if ((flags & flag::SILENCE) != 0)
        {
            if (len != kMediaHeaderSize)
                return Reject::SilenceLengthMismatch;
            return Reject::None;
        }

        // Rule 10 (security-critical): derive payload length from the header and
        // verify it against the ACTUAL datagram length before any payload read.
        const std::size_t expected =
            kMediaHeaderSize + payloadBytes(frame_count, channel_count,
                                            static_cast<SampleFormat>(sample_format));
        if (len != expected)
            return Reject::LengthMismatch;

        return Reject::None;
    }
}
