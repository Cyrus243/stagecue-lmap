#include "lmap/Sender.h"

#include <arpa/inet.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>

#include "lmap/Socket.h"
#include "lmap/Wire.h"

namespace lmap
{
    struct Sender::Dest
    {
        sockaddr_in addr{};
    };

    Sender::Sender(const StreamConfig& stream, const NetworkConfig& net)
        : stream_(stream)
        , net_(net)
        , frameBytes_(std::uint32_t(stream.channel_count) * bytesPerSample(stream.sample_format))
        , payloadBytes_(std::uint32_t(stream.frames_per_packet) * frameBytes_)
        , socket_(std::make_unique<UdpSocket>())
        , mediaDest_(std::make_unique<Dest>())
        , controlDest_(std::make_unique<Dest>())
        , packet_(kMediaHeaderSize + payloadBytes_)
        , sourceBuffer_(payloadBytes_)
    {
    }

    Sender::~Sender()
    {
        stop();
    }

    CreateResult<Sender> Sender::create(const StreamConfig& stream, const NetworkConfig& net)
    {
        if (auto err = validateConfig(stream))
            return { nullptr, *err };

        std::unique_ptr<Sender> s(new Sender(stream, net));
        std::string error;
        if (!s->openSocket(error))
            return { nullptr, error };

        return { std::move(s), {} };
    }

    bool Sender::openSocket(std::string& error)
    {
        if (net_.interface_addr.empty())
        {
            error = "NetworkConfig.interface_addr is required (spec §2.2)";
            return false;
        }
        if (auto err = socket_->openMulticastSender(net_.interface_addr, net_.ttl,
                                                    net_.dscp, net_.multicast_loopback))
        {
            error = *err;
            return false;
        }

        std::string e1, e2;
        auto media   = UdpSocket::makeEndpoint(net_.media_group, net_.media_port, e1);
        auto control = UdpSocket::makeEndpoint(net_.control_group, net_.control_port, e2);
        if (!media)   { error = e1; return false; }
        if (!control) { error = e2; return false; }
        mediaDest_->addr   = *media;
        controlDest_->addr = *control;
        return true;
    }

    void Sender::setSource(SourceFn source)
    {
        source_ = std::move(source);
    }

    void Sender::start()
    {
        if (state_.load() == State::Running)
            return;

        std::random_device rd;
        std::uint32_t sid = 0;
        while (sid == 0)
            sid = rd(); // session_id is random, regenerated on every stream start (§5.1)
        sessionId_.store(sid);

        mediaTimestamp_ = 0;
        sequence_       = 0;
        packetsSent_.store(0);
        sendErrors_.store(0);
        underruns_.store(0);

        state_.store(State::Running);

        sendAnnounce(); // immediately on stream start (§5.1)
        controlThread_ = std::thread(&Sender::controlLoop, this);
        if (source_)
            pacingThread_ = std::thread(&Sender::pacingLoop, this);
    }

    void Sender::stop()
    {
        if (state_.load() != State::Running)
            return;

        state_.store(State::Idle);
        if (pacingThread_.joinable())
            pacingThread_.join();
        if (controlThread_.joinable())
            controlThread_.join();

        sendBye(); // clean shutdown, sent 3x ~5 ms apart (§5.2)
    }

    bool Sender::push(const void* interleaved, std::uint16_t frames) noexcept
    {
        if (state_.load() != State::Running || frames != stream_.frames_per_packet)
            return false;

        MediaHeader h{};
        std::memcpy(h.magic, kMagic, 4);
        h.version         = kVersion;
        h.flags           = (sequence_ < 3) ? flag::STREAM_START : std::uint8_t(0); // first 3 (§6.2)
        h.stream_id       = stream_.stream_id;
        h.media_timestamp = mediaTimestamp_;
        h.sequence        = sequence_;
        h.frame_count     = frames;
        h.channel_count   = stream_.channel_count;
        h.sample_format   = static_cast<std::uint8_t>(stream_.sample_format);

        encodeMediaHeader(h, packet_.data());
        const std::size_t payload = std::size_t(frames) * frameBytes_;
        std::memcpy(packet_.data() + kMediaHeaderSize, interleaved, payload);

        const bool ok = socket_->sendTo(mediaDest_->addr, packet_.data(), kMediaHeaderSize + payload);
        if (ok)
            packetsSent_.fetch_add(1, std::memory_order_relaxed);
        else
            sendErrors_.fetch_add(1, std::memory_order_relaxed);

        mediaTimestamp_ += frames;
        ++sequence_; // u32, wraps naturally
        return ok;
    }

    void Sender::sendAnnounce()
    {
        AnnounceFields a;
        a.stream_id         = stream_.stream_id;
        a.session_id        = sessionId_.load();
        a.sample_rate       = stream_.sample_rate;
        a.frames_per_packet = stream_.frames_per_packet;
        a.channel_count     = stream_.channel_count;
        a.sample_format     = static_cast<std::uint8_t>(stream_.sample_format);

        in_addr group{};
        if (inet_pton(AF_INET, net_.media_group.c_str(), &group) == 1)
            a.media_group_net = group.s_addr; // network byte order (§5.1)
        a.media_port   = net_.media_port;
        a.stream_flags = 0;
        std::memset(a.name, 0, sizeof(a.name));
        std::memcpy(a.name, stream_.name.data(),
                    std::min<std::size_t>(stream_.name.size(), sizeof(a.name)));

        std::uint8_t buf[kAnnounceSize];
        encodeAnnounce(a, buf);
        socket_->sendTo(controlDest_->addr, buf, sizeof(buf));
    }

    void Sender::sendBye()
    {
        ByeFields b;
        b.stream_id  = stream_.stream_id;
        b.session_id = sessionId_.load();
        std::uint8_t buf[kByeSize];
        encodeBye(b, buf);
        for (int i = 0; i < 3; ++i) // unreliable, so sent 3x (§5.2)
        {
            socket_->sendTo(controlDest_->addr, buf, sizeof(buf));
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    void Sender::pacingLoop()
    {
        // Absolute deadlines from a fixed origin so truncation never accumulates
        // (spec §6.3). frames_per_packet*1e9/sample_rate is not integer for all
        // rates, so compute in 128-bit.
        const auto origin = std::chrono::steady_clock::now();
        const auto num    = static_cast<unsigned __int128>(stream_.frames_per_packet) * 1'000'000'000ull;

        for (std::uint64_t i = 0; state_.load() == State::Running; ++i)
        {
            const auto ns   = static_cast<std::int64_t>((num * i) / stream_.sample_rate);
            const auto when = origin + std::chrono::nanoseconds(ns);
            std::this_thread::sleep_until(when);
            if (state_.load() != State::Running)
                break;

            if (!source_(sourceBuffer_.data(), stream_.frames_per_packet))
                break;
            push(sourceBuffer_.data(), stream_.frames_per_packet);
        }
    }

    void Sender::controlLoop()
    {
        using namespace std::chrono;
        auto next = steady_clock::now() + seconds(1);
        while (state_.load() == State::Running)
        {
            std::this_thread::sleep_for(milliseconds(20));
            if (steady_clock::now() >= next)
            {
                sendAnnounce(); // every 1000 ms (§5.1)
                next += seconds(1);
            }
        }
    }

    SenderStats Sender::stats() const noexcept
    {
        return { packetsSent_.load(std::memory_order_relaxed),
                 sendErrors_.load(std::memory_order_relaxed),
                 underruns_.load(std::memory_order_relaxed) };
    }
}
