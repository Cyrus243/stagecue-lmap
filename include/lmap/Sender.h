// LMAP/1 sender (spec §6, §9). Owns the media clock; emits media packets to the
// media multicast group and 1 Hz ANNOUNCE (+ BYE on stop) to the control group.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "lmap/Config.h"

namespace lmap
{
    struct SenderStats
    {
        std::uint64_t packets_sent = 0;
        std::uint64_t send_errors  = 0;
        std::uint64_t underruns    = 0;
    };

    class UdpSocket;

    class Sender
    {
    public:
        // Returns an error (does not throw) if the config violates §4.4.
        static CreateResult<Sender> create(const StreamConfig& stream, const NetworkConfig& net);

        ~Sender();
        Sender(const Sender&) = delete;
        Sender& operator=(const Sender&) = delete;

        // Free-running source (file playback, synthesis, tests): fills one packet of
        // `frames` interleaved samples in the stream's sample_format; returns false
        // to stop the stream. If set before start(), a pacing thread drives it at the
        // media rate (spec §6.3). If unset, the caller drives push() from its own
        // audio device callback (device-driven, spec §6.3).
        using SourceFn = std::function<bool(void* out, std::uint16_t frames)>;
        void setSource(SourceFn source);

        void start(); // IDLE/CONFIGURED -> RUNNING; emits an immediate ANNOUNCE
        void stop();   // RUNNING -> IDLE; emits BYE x3

        // Real-time safe. `frames` MUST equal the configured frames_per_packet.
        // Interleaved, in the stream's sample_format. Returns false if the socket
        // send buffer was full — the caller MUST NOT retry from the audio thread.
        bool push(const void* interleaved, std::uint16_t frames) noexcept;

        [[nodiscard]] SenderStats stats() const noexcept;
        [[nodiscard]] std::uint32_t sessionId() const noexcept { return sessionId_.load(); }

    private:
        Sender(const StreamConfig& stream, const NetworkConfig& net);

        bool openSocket(std::string& error);
        void sendAnnounce();
        void sendBye();
        void pacingLoop();    // free-running pacing thread (spec §6.3)
        void controlLoop();   // 1 Hz ANNOUNCE thread (spec §5.1)

        enum class State { Idle, Configured, Running };

        StreamConfig  stream_;
        NetworkConfig net_;
        std::uint32_t frameBytes_;
        std::uint32_t payloadBytes_;

        std::unique_ptr<UdpSocket> socket_;
        // Destination endpoints are sockaddr_in, hidden behind the impl to keep this
        // header free of system socket headers.
        struct Dest;
        std::unique_ptr<Dest> mediaDest_;
        std::unique_ptr<Dest> controlDest_;

        std::vector<std::uint8_t> packet_;      // preallocated: 24 + max payload
        std::vector<std::uint8_t> sourceBuffer_; // preallocated: one packet of payload

        std::uint64_t mediaTimestamp_ = 0;
        std::uint32_t sequence_       = 0;

        std::atomic<State>         state_{ State::Configured };
        std::atomic<std::uint32_t> sessionId_{ 0 };
        std::atomic<std::uint64_t> packetsSent_{ 0 };
        std::atomic<std::uint64_t> sendErrors_{ 0 };
        std::atomic<std::uint64_t> underruns_{ 0 };

        SourceFn    source_;
        std::thread pacingThread_;
        std::thread controlThread_;
    };
}
