// LMAP/1 receiver (spec §7, §9). A network thread validates datagrams and writes
// them into a ring-by-timestamp jitter buffer; the audio-thread pull() plays out
// with a clock-recovery correction. The two threads share only the ring and
// atomics — no locks, no allocation on the hot paths.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "lmap/ClockCorrector.h"
#include "lmap/Config.h"
#include "lmap/RingByTimestamp.h"
#include "lmap/Wire.h"

namespace lmap
{
    enum class ReceiverState : int
    {
        Searching = 0, // output silence, waiting for a valid media packet
        Syncing   = 1, // packets arriving, buffer filling, output silence
        Locked    = 2, // output audio, clock recovery active
        Stalled   = 3, // no packets for stall_timeout; fade out, output silence
    };

    struct ReceiverStats
    {
        std::uint64_t packets_received  = 0;
        std::uint64_t packets_lost      = 0;
        std::uint64_t packets_late      = 0;
        std::uint64_t packets_duplicate = 0;
        std::uint32_t buffer_fill_frames = 0;
        std::int32_t  rate_ppm          = 0;
        int           state             = 0; // ReceiverState
    };

    class UdpSocket;

    class Receiver
    {
    public:
        static CreateResult<Receiver> create(const StreamConfig& stream, const NetworkConfig& net,
                                              float target_delay_ms = 4.0f);

        // Construct a receiver with no socket, fed entirely through ingest() by an
        // external transport (or by tests). start()/stop() are no-ops. Useful for
        // deterministic testing and for embedding behind another audio transport.
        static CreateResult<Receiver> createOffline(const StreamConfig& stream,
                                                    float target_delay_ms = 4.0f);

        ~Receiver();
        Receiver(const Receiver&) = delete;
        Receiver& operator=(const Receiver&) = delete;

        void start(); // starts the network receive thread
        void stop();

        // Real-time safe. Called from the output device callback. Always fills
        // `frames` frames; returns false if it output silence/concealment.
        bool pull(void* interleaved_out, std::uint32_t frames) noexcept;

        // Feed one raw datagram into the receive pipeline (spec §7.4). Called by the
        // network thread; also called directly by tests to drive the receiver
        // deterministically without a socket. Not called from the audio thread.
        void ingest(const std::uint8_t* data, std::size_t len) noexcept;

        [[nodiscard]] ReceiverStats stats() const noexcept;
        [[nodiscard]] ReceiverState state() const noexcept { return state_.load(); }

        // When false, the clock-recovery correction is disabled and read_pos
        // advances at exactly the nominal rate (rate_ratio = 1.0). Used by the
        // bit-exact loopback conformance test (spec §10, test 6).
        void setClockCorrectionEnabled(bool enabled) noexcept { correctionEnabled_.store(enabled); }

    private:
        Receiver(const StreamConfig& stream, const NetworkConfig& net, float target_delay_ms);

        bool openSocket(std::string& error);
        void receiveLoop();
        void resetTo(std::uint64_t anchorTs) noexcept; // clear ring + PI state, go SYNCING (§7.6)
        void applyFadeF32(float* out, std::uint32_t frames, float from, float to) noexcept;
        void fillSilence(void* out, std::uint32_t frames) noexcept;

        StreamConfig  stream_;
        NetworkConfig net_;
        std::uint32_t frameBytes_;
        std::uint32_t targetDelayFrames_;
        std::uint32_t stallTimeoutMs_ = 200; // spec §7.1 default

        std::unique_ptr<RingByTimestamp> ring_;
        std::unique_ptr<UdpSocket>       socket_;
        std::vector<std::uint8_t>        recvBuffer_; // preallocated max datagram

        DropInsertCorrector corrector_;     // audio thread only
        std::atomic<bool>   correctionEnabled_{ true };

        // read_pos is owned by the audio thread; the network thread reads it (loads
        // may be slightly stale, which the lateness/capacity heuristics tolerate).
        std::atomic<double>        readPos_{ 0.0 };
        std::atomic<std::uint64_t> anchor_{ 0 };
        std::atomic<ReceiverState> state_{ ReceiverState::Searching };

        // Loss/reorder accounting (network thread).
        std::uint32_t expectedSequence_ = 0;
        std::uint32_t lastSequence_      = 0;
        bool          seenAnyPacket_     = false;

        std::atomic<std::uint64_t> packetsReceived_{ 0 };
        std::atomic<std::uint64_t> packetsLost_{ 0 };
        std::atomic<std::uint64_t> packetsLate_{ 0 };
        std::atomic<std::uint64_t> packetsDuplicate_{ 0 };
        std::atomic<std::int64_t>  lastPacketNs_{ 0 }; // steady_clock ns of last valid packet

        float             fadeEnvelope_ = 0.0f; // audio thread: 0..1, fade in/out
        std::atomic<bool> running_{ false };
        std::thread       netThread_;
    };
}
