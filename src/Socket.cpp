#include "lmap/Socket.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace lmap
{
    namespace
    {
        std::string errnoStr(const char* what)
        {
            return std::string(what) + ": " + std::strerror(errno);
        }
    }

    std::optional<sockaddr_in> UdpSocket::makeEndpoint(const std::string& ip,
                                                       std::uint16_t port, std::string& err)
    {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port);
        if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1)
        {
            err = "invalid IPv4 address: " + ip;
            return std::nullopt;
        }
        return addr;
    }

    std::optional<std::string> UdpSocket::openMulticastSender(const std::string& interfaceAddr,
                                                              std::uint8_t ttl, int dscp, bool loopback)
    {
        close();
        fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd_ < 0)
            return errnoStr("socket");

        in_addr iface{};
        if (inet_pton(AF_INET, interfaceAddr.c_str(), &iface) != 1)
        {
            close();
            return "invalid interface address: " + interfaceAddr;
        }
        if (::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof(iface)) < 0)
        {
            auto e = errnoStr("IP_MULTICAST_IF");
            close();
            return e;
        }

        const unsigned char ttlByte = ttl;
        if (::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttlByte, sizeof(ttlByte)) < 0)
        {
            auto e = errnoStr("IP_MULTICAST_TTL");
            close();
            return e;
        }

        const unsigned char loopByte = loopback ? 1 : 0;
        ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loopByte, sizeof(loopByte));

        const int tos = (dscp & 0x3f) << 2; // DSCP 34 -> ToS 0x88 (spec §2.2)
        ::setsockopt(fd_, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

        int sndbuf = 256 * 1024;
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

        const int flags = ::fcntl(fd_, F_GETFL, 0);
        ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK); // audio thread MUST NOT block (spec §2.2)
        return std::nullopt;
    }

    std::optional<std::string> UdpSocket::openMulticastReceiver(const std::string& group,
                                                               std::uint16_t port,
                                                               const std::string& interfaceAddr,
                                                               int rcvbufBytes)
    {
        close();
        fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd_ < 0)
            return errnoStr("socket");

        const int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)); // multiple receivers/host
#endif
        if (rcvbufBytes > 0)
            ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcvbufBytes, sizeof(rcvbufBytes));

        sockaddr_in bindAddr{};
        bindAddr.sin_family      = AF_INET;
        bindAddr.sin_port        = htons(port);
        bindAddr.sin_addr.s_addr = htonl(INADDR_ANY); // bind port; group filtering is via the join
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) < 0)
        {
            auto e = errnoStr("bind");
            close();
            return e;
        }

        ip_mreq mreq{};
        if (inet_pton(AF_INET, group.c_str(), &mreq.imr_multiaddr) != 1)
        {
            close();
            return "invalid multicast group: " + group;
        }
        if (inet_pton(AF_INET, interfaceAddr.c_str(), &mreq.imr_interface) != 1)
        {
            close();
            return "invalid interface address: " + interfaceAddr;
        }
        if (::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
        {
            auto e = errnoStr("IP_ADD_MEMBERSHIP");
            close();
            return e;
        }
        return std::nullopt;
    }

    bool UdpSocket::sendTo(const sockaddr_in& dst, const void* data, std::size_t len) noexcept
    {
        if (fd_ < 0)
            return false;
        const ssize_t n = ::sendto(fd_, data, len, 0,
                                   reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
        return n == static_cast<ssize_t>(len);
    }

    std::ptrdiff_t UdpSocket::recvWithTimeout(void* buf, std::size_t cap, int timeoutMs) noexcept
    {
        if (fd_ < 0)
            return -1;

        pollfd pfd{};
        pfd.fd     = fd_;
        pfd.events = POLLIN;
        const int pr = ::poll(&pfd, 1, timeoutMs);
        if (pr < 0)
            return (errno == EINTR) ? 0 : -1;
        if (pr == 0)
            return 0; // timeout

        const ssize_t n = ::recvfrom(fd_, buf, cap, 0, nullptr, nullptr);
        if (n < 0)
            return (errno == EWOULDBLOCK || errno == EAGAIN) ? 0 : -1;
        return static_cast<std::ptrdiff_t>(n);
    }

    void UdpSocket::close() noexcept
    {
        if (fd_ >= 0)
        {
            ::close(fd_);
            fd_ = -1;
        }
    }
}
