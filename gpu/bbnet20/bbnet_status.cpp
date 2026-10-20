// SPDX-License-Identifier: GPL-2.0-or-later
// bbport-android: the online status file (include/bbnet_status.h).
#include "bbnet_status.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>

#include <unistd.h>

#include "bbgpu.h"

namespace {
std::mutex g_mutex;
BbNetStatus::State g_state = BbNetStatus::State::Connecting;
std::string g_detail;
int g_peers = 0;
std::set<int> g_transports;
std::string g_written;

const char* Name(BbNetStatus::State state) {
    switch (state) {
    case BbNetStatus::State::Connecting:
        return "connecting";
    case BbNetStatus::State::Online:
        return "online";
    case BbNetStatus::State::Offline:
        return "offline";
    case BbNetStatus::State::Rejected:
        return "rejected";
    default:
        return "error";
    }
}

std::string OneLine(std::string text) {
    for (char& c : text) {
        if (c == '\n' || c == '\r' || c == '\t') {
            c = ' ';
        }
    }
    return text;
}

// Written whole under another name and renamed, so the app never reads half a line. The caller holds g_mutex.
void WriteLocked() {
    std::string detail = g_detail;
    if (g_state == BbNetStatus::State::Online && g_peers > 0) {
        detail += g_peers == 1 ? " \xC2\xB7 1 player connected"
                               : " \xC2\xB7 " + std::to_string(g_peers) + " players connected";
    }
    const std::string line = std::string(Name(g_state)) + "|" + OneLine(detail);
    if (line == g_written) {
        return;
    }
    g_written = line;
    std::printf("Online status: %s\n", line.c_str());
    std::fflush(stdout);
    const char* path = std::getenv("BB_NET_STATUS_FILE");
    if (!path || !*path) {
        return;
    }
    const std::string temporary = std::string(path) + ".tmp";
    if (FILE* file = std::fopen(temporary.c_str(), "w")) {
        std::fprintf(file, "%s\n", line.c_str());
        if (std::fclose(file) != 0 || std::rename(temporary.c_str(), path) != 0) {
            unlink(temporary.c_str());
        }
    }
}
} // namespace

namespace BbNetStatus {

const char* PoolName() {
    static const bool thd = [] {
        const char* backend = std::getenv("BB_NET_BACKEND");
        return backend && std::string(backend) == "thd";
    }();
    return thd ? "The Hunter's Dream" : "Seamless Hunt";
}

std::string ServerLabel(const std::string& host, unsigned port) {
    return port == 31313 || port == 0 ? host : host + ":" + std::to_string(port);
}

void Set(State state, std::string detail) {
    std::scoped_lock lock{g_mutex};
    if (g_state == State::Rejected && state != State::Rejected) {
        return;
    }
    g_state = state;
    g_detail = std::move(detail);
    WriteLocked();
}

State Current() {
    std::scoped_lock lock{g_mutex};
    return g_state;
}

void SetPeers(int count) {
    std::scoped_lock lock{g_mutex};
    if (count < 0) {
        count = 0;
    }
    if (count == g_peers) {
        return;
    }
    g_peers = count;
    if (!g_detail.empty()) {
        WriteLocked();
    }
}

void TransportUp(int id, bool up) {
    int count;
    {
        std::scoped_lock lock{g_mutex};
        if (up) {
            g_transports.insert(id);
        } else {
            g_transports.erase(id);
        }
        count = int(g_transports.size());
    }
    SetPeers(count);
}

void Notice(const std::string& text) {
    const std::string line = OneLine(text);
    if (line.empty()) {
        return;
    }
    // The banner is one text at a time (a newer one replaces an older one the app has not read yet).
    bbgpu_overlay_banner(line.c_str());
}

} // namespace BbNetStatus
