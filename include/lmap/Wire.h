// LMAP/1 wire format: packet layouts, little-endian codecs, and receiver-side
// datagram validation (spec §4, §5).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "lmap/Config.h"

namespace lmap
{
    // 4-byte magic, compared as a byte sequence (endianness-independent, spec §4.1).
    inline constexpr std::uint8_t kMagic[4] = { 'L', 'M', 'A', 'P' };

    // Media flags (spec §4.2).
    namespace flag
    {
        inline constexpr std::uint8_t STREAM_START = 0x01;
        inline constexpr std::uint8_t SILENCE      = 0x02;
        inline constexpr std::uint8_t DUPLICATE    = 0x04;
    }

    // Control message types (spec §5).
    enum class MsgType : std::uint8_t
    {
        Announce = 1,
        Bye      = 2,
        Report   = 3,
    };

    inline constexpr std::size_t kAnnounceSize = 64; // spec §5.1
    inline constexpr std::size_t kByeSize      = 16; // spec §5.2

    // ---- Little-endian byte helpers -------------------------------------------
    // All protocol integer fields are little-endian (spec §2). These make encode
    // and decode explicit and endianness-safe; on a little-endian host they are
    // equivalent to a straight struct copy, but they do not depend on it.
    inline void writeLE16(std::uint8_t* p, std::uint16_t v) noexcept
    {
        p[0] = std::uint8_t(v);
        p[1] = std::uint8_t(v >> 8);
    }
    inline void writeLE32(std::uint8_t* p, std::uint32_t v) noexcept
    {
        p[0] = std::uint8_t(v);
        p[1] = std::uint8_t(v >> 8);
        p[2] = std::uint8_t(v >> 16);
        p[3] = std::uint8_t(v >> 24);
    }
    inline void writeLE64(std::uint8_t* p, std::uint64_t v) noexcept
    {
        for (int i = 0; i < 8; ++i)
            p[i] = std::uint8_t(v >> (8 * i));
    }
    inline std::uint16_t readLE16(const std::uint8_t* p) noexcept
    {
        return std::uint16_t(std::uint16_t(p[0]) | (std::uint16_t(p[1]) << 8));
    }
    inline std::uint32_t readLE32(const std::uint8_t* p) noexcept
    {
        return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8)
             | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
    }
    inline std::uint64_t readLE64(const std::uint8_t* p) noexcept
    {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= std::uint64_t(p[i]) << (8 * i);
        return v;
    }

    // ---- Media header (24 bytes, spec §4.1) -----------------------------------
    // The packed struct is the canonical on-wire layout; its size/offsets are
    // asserted below (conformance test 1). Runtime encode/decode goes through the
    // LE helpers rather than a raw struct copy.
#pragma pack(push, 1)
    struct MediaHeader
    {
        std::uint8_t  magic[4];        // 'L','M','A','P'
        std::uint8_t  version;         // = 1
        std::uint8_t  flags;           // spec §4.2
        std::uint16_t stream_id;       // LE
        std::uint64_t media_timestamp; // LE, frame index of first frame in packet
        std::uint32_t sequence;        // LE, +1 per packet, wraps
        std::uint16_t frame_count;     // LE
        std::uint8_t  channel_count;
        std::uint8_t  sample_format;
    };
#pragma pack(pop)

    static_assert(sizeof(MediaHeader) == 24, "MediaHeader must be exactly 24 bytes (spec §4.1)");
    static_assert(offsetof(MediaHeader, magic) == 0);
    static_assert(offsetof(MediaHeader, version) == 4);
    static_assert(offsetof(MediaHeader, flags) == 5);
    static_assert(offsetof(MediaHeader, stream_id) == 6);
    static_assert(offsetof(MediaHeader, media_timestamp) == 8);
    static_assert(offsetof(MediaHeader, sequence) == 16);
    static_assert(offsetof(MediaHeader, frame_count) == 20);
    static_assert(offsetof(MediaHeader, channel_count) == 22);
    static_assert(offsetof(MediaHeader, sample_format) == 23);

    // Serialise a header into the first 24 bytes of `out` (>= 24 bytes).
    void encodeMediaHeader(const MediaHeader& h, std::uint8_t* out) noexcept;
    // Deserialise a header from the first 24 bytes of `in`. Caller guarantees length.
    MediaHeader decodeMediaHeader(const std::uint8_t* in) noexcept;

    // ---- ANNOUNCE (64 bytes, spec §5.1) ---------------------------------------
    // NOTE: media_group is in NETWORK byte order (it is an IP address fed straight
    // to ip_mreq / inet_ntop). Every other multi-byte field here is little-endian,
    // including media_port. Do not confuse the two.
    struct AnnounceFields
    {
        std::uint16_t stream_id         = 0;
        std::uint32_t session_id        = 0;
        std::uint32_t sample_rate       = 0;
        std::uint16_t frames_per_packet = 0;
        std::uint8_t  channel_count     = 0;
        std::uint8_t  sample_format     = 0;
        std::uint32_t media_group_net   = 0;  // network byte order (in_addr.s_addr)
        std::uint16_t media_port        = 0;  // little-endian on the wire
        std::uint16_t stream_flags      = 0;  // bit 0 = redundancy active (spec §8)
        char          name[32]          = {}; // UTF-8, NUL-padded
    };

    void encodeAnnounce(const AnnounceFields& a, std::uint8_t* out) noexcept; // writes 64 bytes
    bool decodeAnnounce(const std::uint8_t* in, std::size_t len, AnnounceFields& out) noexcept;

    // ---- BYE (16 bytes, spec §5.2) --------------------------------------------
    struct ByeFields
    {
        std::uint16_t stream_id  = 0;
        std::uint32_t session_id = 0;
    };

    void encodeBye(const ByeFields& b, std::uint8_t* out) noexcept; // writes 16 bytes
    bool decodeBye(const std::uint8_t* in, std::size_t len, ByeFields& out) noexcept;

    // ---- Receiver validation (spec §4.5) --------------------------------------
    // Rejection reasons, one per rule, so a receiver can keep a per-reason counter.
    enum class Reject : std::uint8_t
    {
        None = 0,               // packet is well-formed and matches the locked config
        TooShort,               // rule 1:  datagram_length < 24
        BadMagic,               // rule 2:  magic != {'L','M','A','P'}
        BadVersion,             // rule 3:  version != 1
        StreamIdMismatch,       // rule 4:  stream_id != subscribed
        BadFrameCount,          // rule 5:  frame_count == 0 or > 1024
        BadChannelCount,        // rule 6:  channel_count == 0 or > 64
        BadSampleFormat,        // rule 7:  sample_format > 3
        ConfigMismatch,         // rule 8:  channel_count/sample_format != locked config
        SilenceLengthMismatch,  // rule 9:  SILENCE set but datagram_length != 24
        LengthMismatch,         // rule 10: datagram_length != 24 + payload_bytes
    };

    inline constexpr std::size_t kRejectReasonCount = 11;

    // Validate a received media datagram against the locked stream config, in the
    // exact order of spec §4.5. Reads no payload bytes beyond what the length
    // checks permit. Returns Reject::None if the packet may be processed.
    [[nodiscard]] Reject validateMedia(const std::uint8_t* data,
                                       std::size_t          len,
                                       const StreamConfig&  locked) noexcept;
}
