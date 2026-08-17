// Ring-by-timestamp jitter buffer (spec §7.2, §7.4). Header-only, real-time safe:
// all storage is preallocated at construction; write() (network thread) and
// read() (audio thread) do no allocation and take no lock, communicating only
// through atomics.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>

namespace lmap
{
    // A packet is written at the slot its timestamp dictates, so out-of-order
    // packets land in the right place and duplicates overwrite identical data
    // harmlessly (spec §7.2). The ring is divided into power-of-two packet-sized
    // slots; each slot records the timestamp of the packet currently occupying it,
    // which both distinguishes a real gap from stale data left over from an earlier
    // lap and serves as the validity marker.
    //
    // Invariant (guaranteed by the sender, spec §6.2): every packet carries exactly
    // `frames_per_packet` frames and its media_timestamp is a multiple of
    // `frames_per_packet`, so packets tile onto slot boundaries.
    class RingByTimestamp
    {
    public:
        static constexpr std::uint64_t kEmptySlot = ~std::uint64_t(0);

        // requiredFrames: R >= max(4*target_delay, 8*fpp, 2048) worth of frames.
        RingByTimestamp(std::uint16_t framesPerPacket,
                        std::uint32_t frameBytes,
                        std::uint32_t requiredFrames)
            : fpp_(framesPerPacket)
            , frameBytes_(frameBytes)
            , numSlots_(computeNumSlots(framesPerPacket, requiredFrames))
            , slotFrames_(std::uint64_t(framesPerPacket))
            , buffer_(std::make_unique<std::uint8_t[]>(std::size_t(numSlots_) * framesPerPacket * frameBytes))
            , slotTs_(std::make_unique<std::atomic<std::uint64_t>[]>(numSlots_))
        {
            clear();
        }

        [[nodiscard]] std::uint32_t capacityFrames() const noexcept { return numSlots_ * fpp_; }
        [[nodiscard]] std::uint16_t framesPerPacket() const noexcept { return fpp_; }

        // Clear all validity and reset the high-water mark. Called on reset (§7.6);
        // runs on the network thread, never the audio thread.
        void clear() noexcept
        {
            for (std::uint32_t i = 0; i < numSlots_; ++i)
                slotTs_[i].store(kEmptySlot, std::memory_order_relaxed);
            writeHighWater_.store(0, std::memory_order_release);
        }

        // Write one packet-aligned span (network thread). `src` is frameCount frames
        // of interleaved samples, or nullptr for a SILENCE packet (zero-filled).
        // Returns false if the span is not packet-aligned (should not happen for a
        // conformant sender; the caller counts it as an anomaly).
        bool write(std::uint64_t ts, std::uint16_t frameCount, const std::uint8_t* src) noexcept
        {
            if (frameCount != fpp_ || (ts % slotFrames_) != 0)
                return false;

            const std::uint32_t slot = std::uint32_t((ts / slotFrames_) & (numSlots_ - 1));
            std::uint8_t* dst = buffer_.get() + std::size_t(slot) * fpp_ * frameBytes_;
            const std::size_t bytes = std::size_t(frameCount) * frameBytes_;

            if (src != nullptr)
                std::memcpy(dst, src, bytes);
            else
                std::memset(dst, 0, bytes);

            // Publish the payload before the validity marker so the audio thread
            // never reads half-written data (release / acquire pairing, §7.8).
            slotTs_[slot].store(ts, std::memory_order_release);

            const std::uint64_t end = ts + frameCount;
            std::uint64_t prev = writeHighWater_.load(std::memory_order_relaxed);
            while (end > prev && !writeHighWater_.compare_exchange_weak(
                       prev, end, std::memory_order_release, std::memory_order_relaxed))
            {
            }
            return true;
        }

        // Read `n` interleaved frames starting at absolute frame `startFrame` (audio
        // thread). Frames whose slot no longer holds the expected packet are treated
        // as gaps and zero-filled (silence concealment, spec §7.7 basic form).
        // Returns true if every frame was valid, false if any frame was concealed.
        bool read(std::uint64_t startFrame, std::uint32_t n, std::uint8_t* out) const noexcept
        {
            bool allValid = true;
            for (std::uint32_t i = 0; i < n; ++i)
            {
                const std::uint64_t sf          = startFrame + i;
                const std::uint64_t packetTs     = (sf / slotFrames_) * slotFrames_;
                const std::uint32_t slot         = std::uint32_t((sf / slotFrames_) & (numSlots_ - 1));
                std::uint8_t*       outFrame      = out + std::size_t(i) * frameBytes_;

                if (slotTs_[slot].load(std::memory_order_acquire) == packetTs)
                {
                    const std::size_t off = (std::size_t(slot) * fpp_ + (sf - packetTs)) * frameBytes_;
                    std::memcpy(outFrame, buffer_.get() + off, frameBytes_);
                }
                else
                {
                    std::memset(outFrame, 0, frameBytes_); // gap -> digital silence
                    allValid = false;
                }
            }
            return allValid;
        }

        [[nodiscard]] std::uint64_t writeHighWater() const noexcept
        {
            return writeHighWater_.load(std::memory_order_acquire);
        }

    private:
        static std::uint32_t computeNumSlots(std::uint16_t fpp, std::uint32_t requiredFrames) noexcept
        {
            std::uint32_t slots = 1;
            while (std::uint64_t(slots) * fpp < requiredFrames)
                slots <<= 1;
            return slots;
        }

        std::uint16_t fpp_;
        std::uint32_t frameBytes_;
        std::uint32_t numSlots_;   // power of two
        std::uint64_t slotFrames_; // == fpp_, cached as u64 for the div/mod

        std::unique_ptr<std::uint8_t[]>                buffer_;
        std::unique_ptr<std::atomic<std::uint64_t>[]>  slotTs_;
        std::atomic<std::uint64_t>                     writeHighWater_{ 0 };
    };
}
