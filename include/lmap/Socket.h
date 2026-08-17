// Portable POSIX/BSD multicast UDP socket wrapper (spec §2.2). Works on macOS and
// Linux; deliberately avoids Linux-only recvmmsg/clock_nanosleep. This is an
// internal header — it pulls in <netinet/in.h> and is not part of the public API
// that StageCue includes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include <netinet/in.h>

namespace lmap
{
    class UdpSocket
    {
    public:
        UdpSocket() = default;
        ~UdpSocket() { close(); }
        UdpSocket(const UdpSocket&) = delete;
        UdpSocket& operator=(const UdpSocket&) = delete;

        // Open a non-blocking socket configured for multicast egress on a specific
        // interface (IP_MULTICAST_IF/TTL/LOOP, IP_TOS from dscp, SO_SNDBUF). The
        // interface address MUST be explicit (never INADDR_ANY on a multi-homed
        // host, spec §2.2). Returns an error string on failure.
        std::optional<std::string> openMulticastSender(const std::string& interfaceAddr,
                                                        std::uint8_t ttl, int dscp, bool loopback);

        // Open a socket bound to `port`, joined to `group` on `interfaceAddr`
        // (SO_REUSEADDR/REUSEPORT, IP_ADD_MEMBERSHIP, SO_RCVBUF).
        std::optional<std::string> openMulticastReceiver(const std::string& group,
                                                         std::uint16_t port,
                                                         const std::string& interfaceAddr,
                                                         int rcvbufBytes);

        // Build a destination endpoint from a dotted-quad address + port.
        static std::optional<sockaddr_in> makeEndpoint(const std::string& ip,
                                                       std::uint16_t port, std::string& err);

        // Non-blocking send. Returns true on success; false on error or if the send
        // buffer is full (EWOULDBLOCK) — the caller MUST NOT retry from the audio
        // thread (spec §9).
        bool sendTo(const sockaddr_in& dst, const void* data, std::size_t len) noexcept;

        // Receive one datagram, waiting up to timeoutMs. Returns the byte count
        // (>0), 0 on timeout / no data, or -1 on a hard error.
        std::ptrdiff_t recvWithTimeout(void* buf, std::size_t cap, int timeoutMs) noexcept;

        [[nodiscard]] bool isOpen() const noexcept { return fd_ >= 0; }
        void close() noexcept;

    private:
        int fd_ = -1;
    };
}
