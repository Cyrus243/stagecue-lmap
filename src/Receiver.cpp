#include "lmap/Receiver.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "lmap/Socket.h"

namespace lmap
{
    namespace
    {
        std::int64_t nowNs() noexcept
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }

        std::uint32_t requiredRingFrames(std::uint32_t targetDelayFrames, std::uint16_t fpp) noexcept
        {
            // R >= max(4*target_delay, 8*fpp, 2048)  (spec §7.2)
            return std::max<std::uint32_t>({ 4u * targetDelayFrames, 8u * fpp, 2048u });
        }
    }

    Receiver::Receiver(const StreamConfig& stream, const NetworkConfig& net, float target_delay_ms)
        : stream_(stream)
        , net_(net)
        , frameBytes_(std::uint32_t(stream.channel_count) * bytesPerSample(stream.sample_format))
        , targetDelayFrames_(std::uint32_t(std::lround(double(target_delay_ms) * stream.sample_rate / 1000.0)))
        , recvBuffer_(65536)
    {
        if (targetDelayFrames_ == 0)
            targetDelayFrames_ = 1;
        ring_ = std::make_unique<RingByTimestamp>(
            stream.frames_per_packet, frameBytes_,
            requiredRingFrames(targetDelayFrames_, stream.frames_per_packet));
        socket_ = std::make_unique<UdpSocket>();
    }

    Receiver::~Receiver()
    {
        stop();
    }

    CreateResult<Receiver> Receiver::create(const StreamConfig& stream, const NetworkConfig& net,
                                            float target_delay_ms)
    {
        if (auto err = validateConfig(stream))
            return { nullptr, *err };

        std::unique_ptr<Receiver> r(new Receiver(stream, net, target_delay_ms));
        std::string error;
        if (!r->openSocket(error))
            return { nullptr, error };

        return { std::move(r), {} };
    }

    CreateResult<Receiver> Receiver::createOffline(const StreamConfig& stream, float target_delay_ms)
    {
        if (auto err = validateConfig(stream))
            return { nullptr, *err };

        std::unique_ptr<Receiver> r(new Receiver(stream, NetworkConfig{}, target_delay_ms));
        return { std::move(r), {} }; // no socket opened; driven via ingest()
    }

    bool Receiver::openSocket(std::string& error)
    {
        if (net_.interface_addr.empty())
        {
            error = "NetworkConfig.interface_addr is required (spec §2.2)";
            return false;
        }
        if (auto err = socket_->openMulticastReceiver(net_.media_group, net_.media_port,
                                                      net_.interface_addr, 4 * 1024 * 1024))
        {
            error = *err;
            return false;
        }
        return true;
    }

    void Receiver::start()
    {
        if (!socket_->isOpen()) // offline receiver: driven via ingest(), no net thread
            return;
        if (running_.exchange(true))
            return;
        netThread_ = std::thread(&Receiver::receiveLoop, this);
    }

    void Receiver::stop()
    {
        if (!running_.exchange(false))
            return;
        if (netThread_.joinable())
            netThread_.join();
    }

    void Receiver::receiveLoop()
    {
        while (running_.load())
        {
            const std::ptrdiff_t n =
                socket_->recvWithTimeout(recvBuffer_.data(), recvBuffer_.size(), 20);
            if (n > 0)
                ingest(recvBuffer_.data(), std::size_t(n));

            // Stall detection (spec §7.1): no valid packet for stall_timeout.
            const ReceiverState st = state_.load();
            if (st == ReceiverState::Syncing || st == ReceiverState::Locked)
            {
                const std::int64_t last = lastPacketNs_.load();
                if (last != 0 && (nowNs() - last) > std::int64_t(stallTimeoutMs_) * 1'000'000)
                    state_.store(ReceiverState::Stalled);
            }
        }
    }

    void Receiver::resetTo(std::uint64_t anchorTs) noexcept
    {
        ring_->clear();
        corrector_.reset();
        anchor_.store(anchorTs);
        readPos_.store(double(anchorTs));
        expectedSequence_ = 0;
        lastSequence_     = 0;
        seenAnyPacket_    = false;
        state_.store(ReceiverState::Syncing);
    }

    void Receiver::ingest(const std::uint8_t* data, std::size_t len) noexcept
    {
        if (validateMedia(data, len, stream_) != Reject::None)
            return; // reject: drop and continue (§4.5)

        packetsReceived_.fetch_add(1, std::memory_order_relaxed);
        const MediaHeader hdr = decodeMediaHeader(data);
        lastPacketNs_.store(nowNs());

        // Step 1: reset / anchor (spec §7.4, §7.6).
        ReceiverState st = state_.load();
        const bool streamStart = (hdr.flags & flag::STREAM_START) != 0;
        if (st == ReceiverState::Searching || st == ReceiverState::Stalled)
        {
            resetTo(hdr.media_timestamp);
            st = ReceiverState::Syncing;
        }
        else if (streamStart && hdr.media_timestamp == 0 && st == ReceiverState::Locked)
        {
            resetTo(hdr.media_timestamp); // new session (§7.6)
            st = ReceiverState::Syncing;
        }

        // Step 2: loss / reorder / duplicate accounting from `sequence`.
        const bool isDuplicate = (hdr.flags & flag::DUPLICATE) != 0
                              || (seenAnyPacket_ && hdr.sequence == lastSequence_);
        if (isDuplicate)
        {
            packetsDuplicate_.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            if (seenAnyPacket_)
            {
                const std::int32_t delta =
                    std::int32_t(hdr.sequence - expectedSequence_); // signed wrap (§7.4)
                if (delta == 0)
                    expectedSequence_ = hdr.sequence + 1;
                else if (delta > 0)
                {
                    packetsLost_.fetch_add(std::uint32_t(delta), std::memory_order_relaxed);
                    expectedSequence_ = hdr.sequence + 1;
                }
                // delta < 0: reordered — do not move expected_sequence back.
            }
            else
            {
                expectedSequence_ = hdr.sequence + 1;
            }
            lastSequence_  = hdr.sequence;
            seenAnyPacket_ = true;
        }

        // Step 3: lateness — already past read_pos, nothing to do (§7.4).
        const double rp = readPos_.load();
        if (double(hdr.media_timestamp + hdr.frame_count) <= rp)
        {
            packetsLate_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // Step 4: ring capacity — a far-future packet is treated as a reset (§7.4, §7.6).
        const std::uint64_t capacity = ring_->capacityFrames();
        if (hdr.media_timestamp >= std::uint64_t(rp) + (capacity - stream_.frames_per_packet))
        {
            resetTo(hdr.media_timestamp);
            st = ReceiverState::Syncing;
        }

        // Step 5: write payload into the ring at slot(media_timestamp) (§7.4).
        const std::uint8_t* payload =
            (hdr.flags & flag::SILENCE) ? nullptr : (data + kMediaHeaderSize);
        ring_->write(hdr.media_timestamp, hdr.frame_count, payload);

        // Step 7: SYNCING -> LOCKED once the buffer reaches the target fill (§7.4).
        if (st == ReceiverState::Syncing)
        {
            const std::uint64_t anchor = anchor_.load();
            if (ring_->writeHighWater() >= anchor + targetDelayFrames_)
            {
                readPos_.store(double(anchor + targetDelayFrames_));
                state_.store(ReceiverState::Locked);
            }
        }
    }

    void Receiver::fillSilence(void* out, std::uint32_t frames) noexcept
    {
        std::memset(out, 0, std::size_t(frames) * frameBytes_);
    }

    void Receiver::applyFadeF32(float* out, std::uint32_t frames, float from, float to) noexcept
    {
        if (frames == 0)
            return;
        const std::uint32_t ch = stream_.channel_count;
        const float step = (frames > 1) ? (to - from) / float(frames - 1) : 0.0f;
        for (std::uint32_t i = 0; i < frames; ++i)
        {
            const float g = from + step * float(i);
            float* frame = out + std::size_t(i) * ch;
            for (std::uint32_t c = 0; c < ch; ++c)
                frame[c] *= g;
        }
    }

    bool Receiver::pull(void* out, std::uint32_t frames) noexcept
    {
        const bool  isF32 = stream_.sample_format == SampleFormat::F32LE;
        const bool  corr  = correctionEnabled_.load();
        ReceiverState st  = state_.load();

        if (st == ReceiverState::Locked)
        {
            const double rp  = readPos_.load();
            const double whw = double(ring_->writeHighWater());
            const double errorFrames = (whw - rp) - double(targetDelayFrames_);

            int adj = 0;
            if (corr)
                adj = corrector_.correct(errorFrames);

            const std::uint64_t startFrame = std::uint64_t(std::llround(rp));
            const bool allValid = ring_->read(startFrame, frames, static_cast<std::uint8_t*>(out));
            readPos_.store(rp + double(std::int64_t(frames) + adj));

            if (corr && isF32)
            {
                // Fade in over ~5 ms on (re)entering LOCKED to avoid a click (§7.6).
                const float fadeInFrames = std::max(1.0f, 0.005f * float(stream_.sample_rate));
                const float target = std::min(1.0f, fadeEnvelope_ + float(frames) / fadeInFrames);
                applyFadeF32(static_cast<float*>(out), frames, fadeEnvelope_, target);
                fadeEnvelope_ = target;
            }
            else
            {
                fadeEnvelope_ = 1.0f; // raw passthrough (bit-exact mode, or non-F32)
            }
            return allValid;
        }

        if (st == ReceiverState::Stalled && corr && isF32 && fadeEnvelope_ > 0.0f)
        {
            // Fade out remaining buffered audio over ~10 ms, then silence (§7.6).
            const double rp = readPos_.load();
            ring_->read(std::uint64_t(std::llround(rp)), frames, static_cast<std::uint8_t*>(out));
            readPos_.store(rp + double(frames));

            const float fadeOutFrames = std::max(1.0f, 0.010f * float(stream_.sample_rate));
            const float target = std::max(0.0f, fadeEnvelope_ - float(frames) / fadeOutFrames);
            applyFadeF32(static_cast<float*>(out), frames, fadeEnvelope_, target);
            fadeEnvelope_ = target;
            return false;
        }

        // SEARCHING / SYNCING / drained STALLED: digital silence.
        fillSilence(out, frames);
        fadeEnvelope_ = 0.0f;
        return false;
    }

    ReceiverStats Receiver::stats() const noexcept
    {
        ReceiverStats s;
        s.packets_received  = packetsReceived_.load(std::memory_order_relaxed);
        s.packets_lost      = packetsLost_.load(std::memory_order_relaxed);
        s.packets_late      = packetsLate_.load(std::memory_order_relaxed);
        s.packets_duplicate = packetsDuplicate_.load(std::memory_order_relaxed);

        const double whw = double(ring_->writeHighWater());
        const double rp  = readPos_.load();
        s.buffer_fill_frames = std::uint32_t(std::max(0.0, whw - rp));
        s.rate_ppm = 0; // drop/insert milestone; §7.5a will populate this
        s.state    = int(state_.load());
        return s;
    }
}
