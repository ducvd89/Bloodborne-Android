// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <Ws2tcpip.h>
#include <iphlpapi.h>
#include <winsock2.h>
typedef SOCKET net_socket;
typedef int socklen_t;
#else
#include <cerrno>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int net_socket;
#endif
#if defined(__APPLE__)
#include <net/if_dl.h>
#include <net/route.h>
#endif
#if defined(__linux__) || defined(__FreeBSD__)
#include <fstream>
#include <iostream>
#include <sstream>
#endif

#include <array>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include <string.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/net/net_error.h"
#include "core/libraries/net/net_types.h"
#include "net_util.h"

namespace NetUtil {

#ifdef __ANDROID__
// bbport-android: what the Linux code below reads is closed to an app's process (getifaddrs and the
// SIOCGIFHWADDR ioctl see no hardware addresses, /proc/net/route is unreadable since Android 10): a
// stable MAC derived from the device id, a gateway guessed from the active interface, a netmask with a
// default.
namespace {
std::array<u8, 6> StableFakeMac() {
    const char* id = std::getenv("BB_NET_DEVICE_ID");
    if (!id || !*id) {
        id = std::getenv("BB_SHADNET_NPID");
    }
    if (!id || !*id) {
        id = std::getenv("BB_THD_ONLINE_ID");
    }
    if (!id || !*id) {
        id = "bbport-android";
    }
    u64 hash = 0xcbf29ce484222325ull; // FNV-1a
    for (const char* c = id; *c; ++c) {
        hash = (hash ^ static_cast<u8>(*c)) * 0x100000001b3ull;
    }
    // Locally administered, unicast.
    return {0x02, static_cast<u8>(hash >> 8), static_cast<u8>(hash >> 16), static_cast<u8>(hash >> 24),
            static_cast<u8>(hash >> 32), static_cast<u8>(hash >> 40)};
}

/// The address the default route would use: a UDP socket connected to a public address sends nothing.
bool ActiveInterfaceAddress(in_addr* out) {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return false;
    }
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(53);
    peer.sin_addr.s_addr = inet_addr("1.1.1.1");
    sockaddr_in self{};
    socklen_t length = sizeof(self);
    const bool ok = connect(fd, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) == 0 &&
                    getsockname(fd, reinterpret_cast<sockaddr*>(&self), &length) == 0 &&
                    self.sin_addr.s_addr != 0;
    close(fd);
    if (ok) {
        *out = self.sin_addr;
    }
    return ok;
}

/// The netmask of the interface that has `address`, if getifaddrs shows it.
bool InterfaceNetmask(in_addr address, in_addr* out) {
    ifaddrs* ifap = nullptr;
    if (getifaddrs(&ifap) != 0) {
        return false;
    }
    bool found = false;
    for (const ifaddrs* p = ifap; p && !found; p = p->ifa_next) {
        if (p->ifa_addr && p->ifa_netmask && p->ifa_addr->sa_family == AF_INET &&
            reinterpret_cast<const sockaddr_in*>(p->ifa_addr)->sin_addr.s_addr == address.s_addr) {
            *out = reinterpret_cast<const sockaddr_in*>(p->ifa_netmask)->sin_addr;
            found = true;
        }
    }
    freeifaddrs(ifap);
    return found;
}

bool IsPrivate(u32 host_order) {
    return (host_order >> 24) == 10 || (host_order >> 20) == 0xAC1 || (host_order >> 16) == 0xC0A8;
}
} // namespace
#endif

const std::array<u8, 6>& NetUtilInternal::GetEthernetAddr() const {
    return ether_address;
}

bool NetUtilInternal::RetrieveEthernetAddr() {
    std::scoped_lock lock{m_mutex};
#ifdef _WIN32
    std::vector<u8> adapter_infos(sizeof(IP_ADAPTER_INFO));
    ULONG size_infos = sizeof(IP_ADAPTER_INFO);

    if (GetAdaptersInfo(reinterpret_cast<PIP_ADAPTER_INFO>(adapter_infos.data()), &size_infos) ==
        ERROR_BUFFER_OVERFLOW)
        adapter_infos.resize(size_infos);

    if (GetAdaptersInfo(reinterpret_cast<PIP_ADAPTER_INFO>(adapter_infos.data()), &size_infos) ==
            NO_ERROR &&
        size_infos) {
        PIP_ADAPTER_INFO info = reinterpret_cast<PIP_ADAPTER_INFO>(adapter_infos.data());
        memcpy(ether_address.data(), info[0].Address, 6);
        return true;
    }
#elif defined(__APPLE__)
    ifaddrs* ifap;

    if (getifaddrs(&ifap) == 0) {
        ifaddrs* p;
        for (p = ifap; p; p = p->ifa_next) {
            if (p->ifa_addr->sa_family == AF_LINK) {
                sockaddr_dl* sdp = reinterpret_cast<sockaddr_dl*>(p->ifa_addr);
                memcpy(ether_address.data(), sdp->sdl_data + sdp->sdl_nlen, 6);
                freeifaddrs(ifap);
                return true;
            }
        }
        freeifaddrs(ifap);
    }
#elif defined(__ANDROID__)
    ether_address = StableFakeMac();
    return true;
#elif defined(__FreeBSD__)
    // todo
#else
    ifreq ifr;
    ifconf ifc;
    char buf[1024];
    int success = 0;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock == -1)
        return false;

    ifc.ifc_len = sizeof(buf);
    ifc.ifc_buf = buf;
    if (ioctl(sock, SIOCGIFCONF, &ifc) == -1)
        return false;

    ifreq* it = ifc.ifc_req;
    const ifreq* const end = it + (ifc.ifc_len / sizeof(ifreq));

    for (; it != end; ++it) {
        strcpy(ifr.ifr_name, it->ifr_name);
        if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0) {
            if (!(ifr.ifr_flags & IFF_LOOPBACK)) {
                if (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
                    success = 1;
                    break;
                }
            }
        }
    }

    close(sock);

    if (success) {
        memcpy(ether_address.data(), ifr.ifr_hwaddr.sa_data, 6);
        return true;
    }
#endif
    return false;
}

const std::string& NetUtilInternal::GetDefaultGateway() const {
    return default_gateway;
}

bool NetUtilInternal::RetrieveDefaultGateway() {
    std::scoped_lock lock{m_mutex};

#ifdef _WIN32
    ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS;
    ULONG family = AF_INET; // Only IPv4
    ULONG buffer_size = 15000;

    std::vector<BYTE> buffer(buffer_size);
    PIP_ADAPTER_ADDRESSES adapter_addresses =
        reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());

    DWORD result = GetAdaptersAddresses(family, flags, nullptr, adapter_addresses, &buffer_size);
    if (result != NO_ERROR) {
        return false;
    }

    for (PIP_ADAPTER_ADDRESSES adapter = adapter_addresses; adapter != nullptr;
         adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp)
            continue;

        IP_ADAPTER_GATEWAY_ADDRESS_LH* gateway = adapter->FirstGatewayAddress;
        while (gateway) {
            sockaddr* sa = gateway->Address.lpSockaddr;
            if (sa->sa_family == AF_INET) {
                char str[INET_ADDRSTRLEN];
                sockaddr_in* sa_in = reinterpret_cast<sockaddr_in*>(sa);
                if (inet_ntop(AF_INET, &sa_in->sin_addr, str, sizeof(str))) {
                    this->default_gateway = str;
                    return true;
                }
            }
            gateway = gateway->Next;
        }
    }

    return false;
#elif defined(__APPLE__)
    // adapted from
    // https://github.com/seladb/PcapPlusPlus/blob/a49a79e0b67b402ad75ffa96c1795def36df75c8/Pcap%2B%2B/src/PcapLiveDevice.cpp#L1236
    // route message struct for communication in APPLE device
    struct BSDRoutingMessage {
        struct rt_msghdr header;
        char messageSpace[512];
    };

    struct BSDRoutingMessage routingMessage;
    // It creates a raw socket that can be used for routing-related operations
    int sockfd = socket(PF_ROUTE, SOCK_RAW, 0);
    if (sockfd < 0) {
        return false;
    }
    memset(reinterpret_cast<char*>(&routingMessage), 0, sizeof(routingMessage));
    routingMessage.header.rtm_msglen = sizeof(struct rt_msghdr);
    routingMessage.header.rtm_version = RTM_VERSION;
    routingMessage.header.rtm_type = RTM_GET;
    routingMessage.header.rtm_addrs = RTA_DST | RTA_NETMASK;
    routingMessage.header.rtm_flags = RTF_UP | RTF_GATEWAY | RTF_STATIC;
    routingMessage.header.rtm_msglen += 2 * sizeof(sockaddr_in);

    if (write(sockfd, reinterpret_cast<char*>(&routingMessage), routingMessage.header.rtm_msglen) <
        0) {
        return false;
    }

    // Read the response from the route socket
    if (read(sockfd, reinterpret_cast<char*>(&routingMessage), sizeof(routingMessage)) < 0) {
        return false;
    }

    struct in_addr* gateAddr = nullptr;
    struct sockaddr* sa = nullptr;
    char* spacePtr = (reinterpret_cast<char*>(&routingMessage.header + 1));
    auto rtmAddrs = routingMessage.header.rtm_addrs;
    int index = 1;
    auto roundUpClosestMultiple = [](int multiple, int num) {
        return ((num + multiple - 1) / multiple) * multiple;
    };
    while (rtmAddrs) {
        if (rtmAddrs & 1) {
            sa = reinterpret_cast<sockaddr*>(spacePtr);
            if (index == RTA_GATEWAY) {
                gateAddr = &((sockaddr_in*)sa)->sin_addr;
                break;
            }
            spacePtr += sa->sa_len > 0 ? roundUpClosestMultiple(sizeof(uint32_t), sa->sa_len)
                                       : sizeof(uint32_t);
        }
        index++;
        rtmAddrs >>= 1;
    }

    if (gateAddr == nullptr) {
        return false;
    }

    char str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, gateAddr, str, sizeof(str));
    this->default_gateway = str;
    return true;
#elif defined(__ANDROID__)
    {
        // The routing table when it can be read, else the first address of the active interface's
        // private network (usually the router), else 0.0.0.0.
        std::ifstream route{"/proc/net/route"};
        std::string line;
        std::getline(route, line);
        while (std::getline(route, line)) {
            std::istringstream iss{line};
            std::string iface, destination, gateway;
            iss >> iface >> destination >> gateway;
            if (destination == "00000000") {
                in_addr addr{};
                addr.s_addr = static_cast<u32>(std::strtoul(gateway.c_str(), nullptr, 16));
                char str[INET_ADDRSTRLEN];
                if (addr.s_addr && inet_ntop(AF_INET, &addr, str, sizeof(str))) {
                    this->default_gateway = str;
                    return true;
                }
            }
        }
        in_addr own{}, mask{};
        in_addr guess{};
        if (ActiveInterfaceAddress(&own) && IsPrivate(ntohl(own.s_addr))) {
            if (!InterfaceNetmask(own, &mask)) {
                mask.s_addr = htonl(0xFFFFFF00u);
            }
            guess.s_addr = htonl((ntohl(own.s_addr) & ntohl(mask.s_addr)) | 1u);
        }
        char str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &guess, str, sizeof(str));
        this->default_gateway = str;
        return true;
    }
#elif defined(__FreeBSD__)
    return true;
#else
    std::ifstream route{"/proc/net/route"};
    std::string line;

    std::getline(route, line);
    while (std::getline(route, line)) {
        std::istringstream iss{line};
        std::string iface, destination, gateway;
        int flags;

        iss >> iface >> destination >> gateway >> std::hex >> flags;

        if (destination == "00000000") {
            u64 default_gateway{};
            std::stringstream ss;
            ss << std::hex << gateway;
            ss >> default_gateway;

            char str[INET_ADDRSTRLEN];
            in_addr addr;
            addr.s_addr = default_gateway;
            inet_ntop(AF_INET, &addr, str, sizeof(str));
            this->default_gateway = str;
            return true;
        }
    }
#endif
    return false;
}

const std::string& NetUtilInternal::GetNetmask() const {
    return netmask;
}

bool NetUtilInternal::RetrieveNetmask() {
    std::scoped_lock lock{m_mutex};
    char netmaskStr[INET_ADDRSTRLEN];
    auto success = false;

#ifdef _WIN32
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG family = AF_INET; // Only IPv4
    ULONG buffer_size = 15000;

    std::vector<BYTE> buffer(buffer_size);
    auto adapter_addresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());

    DWORD result = GetAdaptersAddresses(family, flags, nullptr, adapter_addresses, &buffer_size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(buffer_size);
        adapter_addresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        result = GetAdaptersAddresses(family, flags, nullptr, adapter_addresses, &buffer_size);
    }

    if (result != NO_ERROR)
        return false;

    for (auto adapter = adapter_addresses; adapter != nullptr; adapter = adapter->Next) {
        // Skip loopback and down interfaces
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK || adapter->OperStatus != IfOperStatusUp)
            continue;

        for (auto unicast = adapter->FirstUnicastAddress; unicast != nullptr;
             unicast = unicast->Next) {
            if (unicast->Address.lpSockaddr->sa_family == AF_INET) {
                ULONG prefix_length = unicast->OnLinkPrefixLength;
                ULONG mask = prefix_length == 0 ? 0 : 0xFFFFFFFF << (32 - prefix_length);

                in_addr mask_addr{};
                mask_addr.S_un.S_addr = htonl(mask);

                if (inet_ntop(AF_INET, &mask_addr, netmaskStr, INET_ADDRSTRLEN)) {
                    success = true;
                }
            }
        }
    }
#elif defined(__ANDROID__)
    {
        // The active interface's netmask, else /24 (bbport-android: getifaddrs may show nothing).
        in_addr own{}, mask{};
        mask.s_addr = htonl(0xFFFFFF00u);
        if (ActiveInterfaceAddress(&own)) {
            InterfaceNetmask(own, &mask);
        }
        inet_ntop(AF_INET, &mask, netmaskStr, INET_ADDRSTRLEN);
        success = true;
    }
#else
    ifaddrs* ifap;

    if (getifaddrs(&ifap) == 0) {
        ifaddrs* p;
        for (p = ifap; p; p = p->ifa_next) {
            if (p->ifa_addr && p->ifa_addr->sa_family == AF_INET) {
                auto sa = reinterpret_cast<sockaddr_in*>(p->ifa_netmask);
                inet_ntop(AF_INET, &sa->sin_addr, netmaskStr, INET_ADDRSTRLEN);
                success = true;
            }
        }
    }

    freeifaddrs(ifap);
#endif

    if (success) {
        netmask = netmaskStr;
    }
    return success;
}

const std::string& NetUtilInternal::GetIp() const {
    return ip;
}

u32 NetUtilInternal::GetExternalIp() const {
    return external_ip;
}

void NetUtilInternal::SetExternalIp(u32 addr) {
    external_ip = addr;
}

u32 NetUtilInternal::GetNatType() const {
    return nat_type;
}

bool NetUtilInternal::RetrieveIp() {
    std::scoped_lock lock{m_mutex};

#ifdef __ANDROID__
    // bbport-android: a UDP socket tells the local address without connecting out (no wait on a phone
    // with no route).
    auto sockfd = socket(AF_INET, SOCK_DGRAM, 0);
#else
    auto sockfd = socket(AF_INET, SOCK_STREAM, 0);
#endif
    if (sockfd == -1) {
        return false;
    }

    sockaddr_in sa{};
    socklen_t sa_len{sizeof(sa)};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = inet_addr("1.1.1.1");
    sa.sin_port = htons(80);

#ifdef _WIN32
#define close closesocket
#endif

    if (connect(sockfd, (sockaddr*)&sa, sa_len) == -1) {
        close(sockfd);
        return false;
    }

    if (getsockname(sockfd, (struct sockaddr*)&sa, &sa_len) == -1) {
        close(sockfd);
        return false;
    }

    char netmaskStr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &sa.sin_addr, netmaskStr, INET_ADDRSTRLEN);
    ip = netmaskStr;

    close(sockfd);

#ifdef _WIN32
#undef close
#endif

    return true;
}

int NetUtilInternal::ResolveHostname(const char* hostname, Libraries::Net::OrbisNetInAddr* addr) {
    const addrinfo hints = {
        .ai_flags = AI_V4MAPPED | AI_ADDRCONFIG,
        .ai_family = AF_INET,
    };

    addrinfo* info = nullptr;
    auto gai_result = getaddrinfo(hostname, nullptr, &hints, &info);

    auto ret = ORBIS_OK;
    if (gai_result != 0) {
        // handle more errors
        LOG_ERROR(Lib_Net, "address resolution for {} failed: {}", hostname, gai_result);
        ret = ORBIS_NET_ERETURN;
    } else {
        ASSERT(info && info->ai_addr);
        in_addr resolved_addr = ((sockaddr_in*)info->ai_addr)->sin_addr;
        LOG_DEBUG(Lib_Net, "resolved address for {}: {}", hostname, inet_ntoa(resolved_addr));
        addr->inaddr_addr = resolved_addr.s_addr;
    }

    freeaddrinfo(info);

    return ret;
}

} // namespace NetUtil
