// LMAP/1 stream & network configuration and limits (spec §3, §4.4, §9).
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace lmap
{
    // ---- Sample formats (spec §3) ---------------------------------------------
    enum class SampleFormat : std::uint8_t
    {
        S16LE = 0,
        S24LE = 1,
        S32LE = 2,
        F32LE = 3,
    };

    [[nodiscard]] constexpr std::uint32_t bytesPerSample(SampleFormat f) noexcept
    {
        switch (f)
        {
            case SampleFormat::S16LE: return 2;
            case SampleFormat::S24LE: return 3;
            case SampleFormat::S32LE: return 4;
            case SampleFormat::F32LE: return 4;
        }
        return 0; // unreachable for valid input
    }

    // ---- Protocol constants (spec §2, §4.4, §11) ------------------------------
    inline constexpr std::uint8_t  kVersion         = 1;
    inline constexpr std::size_t   kMediaHeaderSize = 24;
    inline constexpr std::size_t   kPathMtu         = 1500;
    inline constexpr std::size_t   kIpUdpOverhead   = 28;   // 20 IPv4 + 8 UDP
    // Usable media payload budget within a 1500-byte MTU: 1500 - 28 - 24 = 1448.
    inline constexpr std::size_t   kMaxPayloadPerPacket = kPathMtu - kIpUdpOverhead - kMediaHeaderSize;

    inline constexpr std::uint16_t kMaxFrameCount   = 1024; // spec §3, §4.5 rule 5
    inline constexpr std::uint8_t  kMaxChannelCount = 64;   // spec §3, §4.5 rule 6

    // ---- Stream configuration (spec §3, §9) -----------------------------------
    struct StreamConfig
    {
        std::uint16_t stream_id         = 0;
        std::uint32_t sample_rate       = 48000;
        std::uint8_t  channel_count     = 8;
        SampleFormat  sample_format     = SampleFormat::F32LE;
        std::uint16_t frames_per_packet = 48;
        std::string   name;
    };

    // ---- Network configuration (spec §2, §9) ----------------------------------
    struct NetworkConfig
    {
        std::string   interface_addr;                  // required, e.g. "192.168.10.5"
        std::string   media_group   = "239.69.83.16";
        std::uint16_t media_port    = 5006;
        std::string   control_group = "239.69.83.1";
        std::uint16_t control_port  = 5004;
        std::uint8_t  ttl           = 1;
        int           dscp          = 34;              // AF41 -> ToS byte 0x88
        bool          multicast_loopback = false;      // set true for single-host testing
    };

    // Bytes carried in a full media packet for the given stream (spec §3.1, §4.4).
    [[nodiscard]] constexpr std::size_t payloadBytes(std::uint16_t frame_count,
                                                     std::uint8_t  channel_count,
                                                     SampleFormat  format) noexcept
    {
        return static_cast<std::size_t>(frame_count) * channel_count * bytesPerSample(format);
    }

    // Largest frames_per_packet that fits one MTU for this channel count/format (spec §4.4).
    [[nodiscard]] constexpr std::uint16_t maxFramesPerPacket(std::uint8_t channel_count,
                                                             SampleFormat format) noexcept
    {
        const std::size_t bytesPerFrame = static_cast<std::size_t>(channel_count) * bytesPerSample(format);
        if (bytesPerFrame == 0)
            return 0;
        return static_cast<std::uint16_t>(kMaxPayloadPerPacket / bytesPerFrame);
    }

    // Validate a StreamConfig at configuration time (spec §4.4). Returns an error
    // string on failure, std::nullopt on success. Never silently fragments.
    [[nodiscard]] std::optional<std::string> validateConfig(const StreamConfig& cfg);

    // ---- C++17 substitute for std::expected (public API is C++17-clean) -------
    template <typename T>
    struct CreateResult
    {
        std::unique_ptr<T> value;
        std::string        error;

        [[nodiscard]] explicit operator bool() const noexcept { return value != nullptr; }
        T* operator->() const noexcept { return value.get(); }
        T& operator*() const noexcept { return *value; }
    };
}
