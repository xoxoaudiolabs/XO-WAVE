#include "studio_link.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "synth.h"

namespace xo {

namespace {

constexpr uint8_t kSign[2] = {0xff, 0xff};
constexpr uint32_t kVersion = 2;
constexpr double kSyncEvery = 10.0, kLost = 30.0, kRetry = 5.0;
constexpr uint8_t kAudioPayload = 127;  // PipeWire rtp-sink's default for L16

double nowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

uint64_t now100us() { return static_cast<uint64_t>(nowSec() * 10000.0); }

void put16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xff; }
void put32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = (v >> 16) & 0xff; p[2] = (v >> 8) & 0xff; p[3] = v & 0xff; }
void put64(uint8_t* p, uint64_t v) { put32(p, static_cast<uint32_t>(v >> 32)); put32(p + 4, static_cast<uint32_t>(v)); }
uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t get32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint64_t get64(const uint8_t* p) { return (uint64_t(get32(p)) << 32) | get32(p + 4); }

int udpSocket() {
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = 0;
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { ::close(s); return -1; }
    return s;
}

std::string ipString(const sockaddr_in& a) {
    char buf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &a.sin_addr, buf, sizeof buf);
    return buf;
}

// ---------------------------------------------------------------------------
// DNS-SD: the Studios' music sessions on each network we're on (as
// XOXO-Studio's find_studios, asked by legacy unicast so answers come to us).

struct Rec {
    std::string name;
    int type;
    std::string text;  // PTR target / SRV target / A address
    int port = 0;      // SRV
};

bool dnsName(const uint8_t* pkt, int len, int& i, std::string& out) {
    out.clear();
    int jumps = 0, end = -1;
    while (i < len) {
        int n = pkt[i];
        if (n == 0) { ++i; break; }
        if ((n & 0xC0) == 0xC0) {
            if (i + 1 >= len || ++jumps > 20) return false;
            if (end < 0) end = i + 2;
            i = ((n & 0x3F) << 8) | pkt[i + 1];
            continue;
        }
        if (i + 1 + n > len) return false;
        if (!out.empty()) out += '.';
        out.append(reinterpret_cast<const char*>(pkt + i + 1), static_cast<size_t>(n));
        i += 1 + n;
    }
    if (end >= 0) i = end;
    return true;
}

void parseDns(const uint8_t* pkt, int len, std::vector<Rec>& out) {
    if (len < 12) return;
    int qd = get16(pkt + 4), an = get16(pkt + 6), ns = get16(pkt + 8), ar = get16(pkt + 10);
    int i = 12;
    std::string name;
    for (int q = 0; q < qd; ++q) {
        if (!dnsName(pkt, len, i, name)) return;
        i += 4;
    }
    for (int r = 0; r < an + ns + ar; ++r) {
        if (!dnsName(pkt, len, i, name) || i + 10 > len) return;
        int type = get16(pkt + i);
        int rdlen = get16(pkt + i + 8);
        i += 10;
        if (i + rdlen > len) return;
        Rec rec;
        rec.name = name;
        rec.type = type;
        int j = i;
        if (type == 12) {
            if (dnsName(pkt, len, j, rec.text)) out.push_back(rec);
        } else if (type == 33 && rdlen >= 6) {
            rec.port = get16(pkt + i + 4);
            j = i + 6;
            if (dnsName(pkt, len, j, rec.text)) out.push_back(rec);
        } else if (type == 1 && rdlen == 4) {
            char buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, pkt + i, buf, sizeof buf);
            rec.text = buf;
            out.push_back(rec);
        }
        i += rdlen;
    }
}

// A Studio's music session: not a control surface (MCU), not a box linked to it.
bool isStudio(const std::string& instance) {
    std::string n = instance.substr(0, instance.find("._apple-midi"));
    if (n.find("MCU") != std::string::npos || n.find("Pocket") != std::string::npos ||
        n.find("XOXO Studio Link") != std::string::npos || n.find("XO-Wave") != std::string::npos)
        return false;
    return n.rfind("FM-1 Companion", 0) == 0 || n.find("Studio") != std::string::npos;
}

std::vector<uint8_t> mdnsQuery(const char* name) {
    std::vector<uint8_t> q(12, 0);
    q[5] = 1;  // one question
    std::string s = name;
    size_t start = 0;
    while (start < s.size()) {
        size_t dot = s.find('.', start);
        if (dot == std::string::npos) dot = s.size();
        q.push_back(static_cast<uint8_t>(dot - start));
        q.insert(q.end(), s.begin() + static_cast<long>(start), s.begin() + static_cast<long>(dot));
        start = dot + 1;
    }
    q.push_back(0);
    q.push_back(0); q.push_back(12);  // PTR
    q.push_back(0); q.push_back(1);   // IN
    return q;
}

struct Found {
    std::string name;
    sockaddr_in control{};
};

std::vector<Found> findStudios(double timeout = 2.5) {
    std::vector<int> socks;
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) == 0) {
        for (ifaddrs* i = ifs; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
            in_addr a = reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr;
            if ((ntohl(a.s_addr) >> 24) == 127) continue;
            int s = ::socket(AF_INET, SOCK_DGRAM, 0);
            if (s < 0) continue;
            sockaddr_in b{};
            b.sin_family = AF_INET;
            b.sin_addr = a;
            unsigned char ttl = 255;
            setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &a, sizeof a);
            setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
            if (::bind(s, reinterpret_cast<sockaddr*>(&b), sizeof b) != 0) { ::close(s); continue; }
            socks.push_back(s);
        }
        freeifaddrs(ifs);
    }
    if (socks.empty()) return {};

    sockaddr_in mdns{};
    mdns.sin_family = AF_INET;
    mdns.sin_port = htons(5353);
    inet_pton(AF_INET, "224.0.0.251", &mdns.sin_addr);
    std::vector<uint8_t> q = mdnsQuery("_apple-midi._udp.local");
    std::vector<std::vector<Rec>> recs(socks.size());
    auto ask = [&](bool onlyQuiet) {
        for (size_t k = 0; k < socks.size(); ++k)
            if (!onlyQuiet || recs[k].empty())
                ::sendto(socks[k], q.data(), q.size(), 0, reinterpret_cast<sockaddr*>(&mdns), sizeof mdns);
    };
    ask(false);
    double start = nowSec(), again = start + 1.2;
    bool askedAgain = false;
    std::vector<pollfd> pfds(socks.size());
    while (nowSec() - start < timeout) {
        if (!askedAgain && nowSec() >= again) { ask(true); askedAgain = true; }
        for (size_t k = 0; k < socks.size(); ++k) pfds[k] = {socks[k], POLLIN, 0};
        if (::poll(pfds.data(), pfds.size(), 100) <= 0) continue;
        for (size_t k = 0; k < socks.size(); ++k) {
            if (!(pfds[k].revents & POLLIN)) continue;
            uint8_t buf[4096];
            ssize_t n = ::recv(socks[k], buf, sizeof buf, 0);
            if (n > 0) parseDns(buf, static_cast<int>(n), recs[k]);
        }
    }
    for (int s : socks) ::close(s);

    std::vector<Found> found;
    for (const auto& list : recs) {
        for (const Rec& ptr : list) {
            if (ptr.type != 12 || !isStudio(ptr.text)) continue;
            auto srv = std::find_if(list.begin(), list.end(), [&](const Rec& r) { return r.type == 33 && r.name == ptr.text; });
            if (srv == list.end()) continue;
            auto a = std::find_if(list.begin(), list.end(), [&](const Rec& r) { return r.type == 1 && r.name == srv->text; });
            Found f;
            f.name = ptr.text.substr(0, ptr.text.find("._apple-midi"));
            f.control.sin_family = AF_INET;
            f.control.sin_port = htons(static_cast<uint16_t>(srv->port));
            if (a != list.end()) {
                inet_pton(AF_INET, a->text.c_str(), &f.control.sin_addr);
            } else {
                addrinfo hints{}, *res = nullptr;
                hints.ai_family = AF_INET;
                if (getaddrinfo(srv->text.c_str(), nullptr, &hints, &res) != 0 || !res) continue;
                f.control.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
                freeaddrinfo(res);
            }
            bool dup = std::any_of(found.begin(), found.end(), [&](const Found& g) {
                return g.name == f.name && g.control.sin_addr.s_addr == f.control.sin_addr.s_addr;
            });
            if (!dup) found.push_back(f);
        }
    }
    // A cable straight to the Studio (link-local) first.
    std::stable_sort(found.begin(), found.end(), [](const Found& a, const Found& b) {
        bool la = (ntohl(a.control.sin_addr.s_addr) >> 16) == 0xA9FE, lb = (ntohl(b.control.sin_addr.s_addr) >> 16) == 0xA9FE;
        return la && !lb;
    });
    return found;
}

// RTP-MIDI packet -> MIDI messages (no journal), as XOXO-Studio's decode().
template <typename F>
void decodeMidi(const uint8_t* p, int len, F&& emit) {
    if (len < 13 || (p[0] & 0xC0) != 0x80 || (p[1] & 0x7F) != 0x61) return;
    int i = 12;
    int flags = p[i++];
    int length = flags & 0x0F;
    if (flags & 0x80) {
        if (i >= len) return;
        length = (length << 8) | p[i++];
    }
    const double sent = get32(p + 4) / 10000.0;   // the sender's clock, 100 us units
    const uint8_t* d = p + i;
    int n = std::min(length, len - i);
    int k = 0, status = -1;
    bool first = true;
    while (k < n) {
        if (!first || (flags & 0x20)) {  // delta time
            while (k < n && (d[k] & 0x80)) ++k;
            ++k;
            if (k >= n) break;
        }
        first = false;
        if (d[k] == 0xF0) {  // SysEx: skipped
            ++k;
            while (k < n && d[k] != 0xF7 && d[k] != 0xF0 && d[k] != 0xF4) ++k;
            ++k;
            continue;
        }
        if (d[k] >= 0xF8) { MidiMsg m; m.len = 1; m.b[0] = d[k++]; m.sent = sent; emit(m); continue; }  // real time
        if (d[k] & 0x80) status = d[k++];
        if (status < 0) break;
        int type = status & 0xF0;
        int size = (type == 0xC0 || type == 0xD0 || status == 0xF1 || status == 0xF3) ? 2 : (status == 0xF6 ? 1 : 3);
        if (k + size - 1 > n) break;
        MidiMsg m;
        m.len = static_cast<uint8_t>(size);
        m.b[0] = static_cast<uint8_t>(status);
        for (int j = 1; j < size; ++j) m.b[j] = d[k + j - 1];
        emit(m);
        k += size - 1;
        if (status >= 0xF0) status = -1;
    }
}

}  // namespace

// ---------------------------------------------------------------------------

void StudioLink::start(const Config& cfg) {
    if (running_.load()) return;
    cfg_ = cfg;
    enabled_.store(cfg.enabled);
    std::random_device rd;
    ssrc_ = rd();
    audioSsrc_ = rd() | 1;
    midiSeq_ = static_cast<uint16_t>(rd());
    rtpSeq_ = static_cast<uint16_t>(rd());
    rtpTs_ = rd();
    ctl_ = udpSocket();
    data_ = udpSocket();
    audio_ = udpSocket();
    if (ctl_ < 0 || data_ < 0 || audio_ < 0) {
        setStatus("NO NETWORK SOCKETS");
        return;
    }
    running_.store(true);
    thread_ = std::thread([this] { run(); });
}

void StudioLink::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    leave("stopped");
    for (int* s : {&ctl_, &data_, &audio_}) {
        if (*s >= 0) ::close(*s);
        *s = -1;
    }
}

void StudioLink::setStatus(const std::string& s) {
    std::lock_guard<std::mutex> lock(mu_);
    status_ = s;
}

std::string StudioLink::describe() const {
    std::lock_guard<std::mutex> lock(mu_);
    if (state_.load() == Linked) {
        if (!audioOk_.load()) return "LINKED FOR MIDI: " + peerName_ + " (" + peerAddr_ + ")\nUPDATE THE STUDIO FOR XO-WAVE SOUND";
        return "LINKED: " + peerName_ + "\n" + peerAddr_ + "  SOUND TO STUDIO";
    }
    return status_;
}

// The Studio's web status: a Studio that takes XO-Wave's sound reports "xowave".
void StudioLink::checkStudio() {
    sockaddr_in a;
    {
        std::lock_guard<std::mutex> lock(mu_);
        a = peerCtl_;
    }
    a.sin_port = htons(80);
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return;
    timeval tv{2, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    std::string body;
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) {
        std::string req = "GET /api/status HTTP/1.0\r\nHost: " + ipString(a) + "\r\nConnection: close\r\n\r\n";
        ::send(s, req.data(), req.size(), 0);
        char buf[4096];
        ssize_t n;
        while ((n = ::recv(s, buf, sizeof buf, 0)) > 0 && body.size() < (1u << 20)) body.append(buf, static_cast<size_t>(n));
    }
    ::close(s);
    if (body.empty()) return;  // no answer: keep what we knew
    bool ok = body.find("\"xowave\"") != std::string::npos;
    if (ok != audioOk_.load())
        std::fprintf(stderr, "xowave: studio %s XO-Wave's sound\n", ok ? "takes" : "doesn't take (update it) -");
    audioOk_.store(ok);
}

bool StudioLink::invite(int sock, const sockaddr_in& to, std::string& peerName) {
    std::random_device rd;
    uint32_t token = rd();
    std::vector<uint8_t> pkt(16);
    pkt[0] = kSign[0]; pkt[1] = kSign[1]; pkt[2] = 'I'; pkt[3] = 'N';
    put32(&pkt[4], kVersion);
    put32(&pkt[8], token);
    put32(&pkt[12], ssrc_);
    pkt.insert(pkt.end(), cfg_.name.begin(), cfg_.name.end());
    pkt.push_back(0);
    ::sendto(sock, pkt.data(), pkt.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
    double end = nowSec() + 1.5;
    while (nowSec() < end) {
        pollfd p{sock, POLLIN, 0};
        if (::poll(&p, 1, 100) <= 0) continue;
        uint8_t buf[512];
        ssize_t n = ::recv(sock, buf, sizeof buf - 1, 0);
        if (n < 16 || buf[0] != 0xff || buf[1] != 0xff || get32(buf + 8) != token) continue;
        if (buf[2] == 'N' && buf[3] == 'O') {
            setStatus("STUDIO SAID NO\n(it follows another Studio)");
            return false;
        }
        if (buf[2] == 'O' && buf[3] == 'K') {
            peerSsrc_ = get32(buf + 12);
            buf[n] = 0;
            peerName = reinterpret_cast<char*>(buf + 16);
            if (peerName.empty()) peerName = "Studio";
            return true;
        }
    }
    return false;
}

bool StudioLink::findStudio(std::string& name, sockaddr_in& control) {
    if (cfg_.host != "auto" && !cfg_.host.empty()) {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        if (getaddrinfo(cfg_.host.c_str(), nullptr, &hints, &res) != 0 || !res) {
            setStatus("CAN'T FIND " + cfg_.host);
            return false;
        }
        control = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
        control.sin_port = htons(static_cast<uint16_t>(cfg_.port));
        freeaddrinfo(res);
        name = cfg_.host;
        return true;
    }
    std::vector<Found> found = findStudios();
    if (found.empty()) {
        setStatus("SEARCHING FOR XOXO STUDIO...\n(cable or same network)");
        return false;
    }
    name = found[0].name;
    control = found[0].control;
    return true;
}

bool StudioLink::connect() {
    std::string name;
    sockaddr_in control{};
    if (!findStudio(name, control)) return false;
    sockaddr_in data = control;
    data.sin_port = htons(static_cast<uint16_t>(ntohs(control.sin_port) + 1));
    std::string studio;
    if (!invite(ctl_, control, studio) || !invite(data_, data, studio)) {
        if (state_.load() != Linked) {
            std::lock_guard<std::mutex> lock(mu_);
            if (status_.rfind("STUDIO SAID NO", 0) != 0) status_ = name + ": NO ANSWER";
        }
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        peerCtl_ = control;
        peerData_ = data;
        peerName_ = studio;
        peerAddr_ = ipString(control);
    }
    heard_ = nowSec();
    fill_ = 0;
    audioDest_.store(control.sin_addr.s_addr, std::memory_order_release);
    state_.store(Linked);
    audioOk_.store(false);
    checkStudio();
    nextCheck_ = nowSec() + 30;
    std::fprintf(stderr, "xowave: linked to %s at %s\n", studio.c_str(), ipString(control).c_str());
    return true;
}

void StudioLink::leave(const char* why) {
    if (state_.load() != Linked) return;
    audioDest_.store(0, std::memory_order_release);
    state_.store(enabled_.load() ? Searching : Off);
    uint8_t by[16] = {0xff, 0xff, 'B', 'Y'};
    put32(by + 4, kVersion);
    put32(by + 12, ssrc_);
    ::sendto(ctl_, by, sizeof by, 0, reinterpret_cast<sockaddr*>(&peerCtl_), sizeof peerCtl_);
    ::sendto(data_, by, sizeof by, 0, reinterpret_cast<sockaddr*>(&peerData_), sizeof peerData_);
    std::fprintf(stderr, "xowave: studio link ended (%s)\n", why);
}

void StudioLink::sendSync(int count, uint64_t t1, uint64_t t2) {
    uint8_t ck[36] = {0xff, 0xff, 'C', 'K'};
    put32(ck + 4, ssrc_);
    ck[8] = static_cast<uint8_t>(count);
    put64(ck + 12, t1);
    put64(ck + 20, t2);
    put64(ck + 28, count == 2 ? now100us() : 0);
    ::sendto(data_, ck, sizeof ck, 0, reinterpret_cast<sockaddr*>(&peerData_), sizeof peerData_);
}

void StudioLink::received(const uint8_t* p, int len, bool dataPort) {
    if (len >= 4 && p[0] == 0xff && p[1] == 0xff) {
        if (p[2] == 'C' && p[3] == 'K' && len >= 36) {
            if (get32(p + 4) != peerSsrc_) {  // the Studio restarted: a new session that doesn't know us
                leave("Studio restarted");
                setStatus("STUDIO RESTARTED, LINKING AGAIN");
                return;
            }
            heard_ = nowSec();
            if (p[8] == 1) sendSync(2, get64(p + 12), get64(p + 20));  // finish the exchange
        } else if (p[2] == 'B' && p[3] == 'Y') {
            audioDest_.store(0, std::memory_order_release);
            state_.store(enabled_.load() ? Searching : Off);
            setStatus("STUDIO ENDED THE LINK");
            std::fprintf(stderr, "xowave: studio ended the link (BY)\n");
        } else if (p[2] == 'O' && p[3] == 'K') {
            heard_ = nowSec();
        }
        return;
    }
    if (!dataPort) return;
    heard_ = nowSec();
    decodeMidi(p, len, [this](const MidiMsg& m) { in_.push(m); });
}

void StudioLink::pushMidiOut(const uint8_t* msg, int len) {
    if (state_.load(std::memory_order_relaxed) != Linked || len < 1 || len > 3) return;
    MidiMsg m;
    m.len = static_cast<uint8_t>(len);
    std::memcpy(m.b, msg, static_cast<size_t>(len));
    out_.push(m);
}

void StudioLink::flushMidiOut() {
    uint8_t pkt[12 + 2 + 400];
    int body = 0;
    MidiMsg m;
    uint8_t* d = pkt + 14;
    while (body < 380 && out_.pop(m)) {
        if (body) d[body++] = 0;  // delta time 0
        std::memcpy(d + body, m.b, m.len);
        body += m.len;
    }
    if (!body || state_.load() != Linked) return;
    pkt[0] = 0x80;
    pkt[1] = 0x61;
    put16(pkt + 2, midiSeq_++);
    put32(pkt + 4, static_cast<uint32_t>(now100us()));
    put32(pkt + 8, ssrc_);
    int off;
    if (body <= 15) {  // short header: move the body up one byte
        pkt[12] = static_cast<uint8_t>(body);
        std::memmove(pkt + 13, d, static_cast<size_t>(body));
        off = 13;
    } else {
        pkt[12] = static_cast<uint8_t>(0x80 | ((body >> 8) & 0x0F));
        pkt[13] = static_cast<uint8_t>(body & 0xFF);
        off = 14;
    }
    ::sendto(data_, pkt, static_cast<size_t>(off + body), 0, reinterpret_cast<sockaddr*>(&peerData_), sizeof peerData_);
}

void StudioLink::run() {
    double nextSync = 0, retryAt = 0;
    while (running_.load()) {
        double now = nowSec();
        if (!enabled_.load()) {
            if (state_.load() == Linked) leave("switched off");
            state_.store(Off);
            setStatus("OFF");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        if (state_.load() != Linked) {
            state_.store(Searching);
            if (now < retryAt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            if (!connect()) {
                retryAt = nowSec() + kRetry;
                continue;
            }
            nextSync = 0;
        }
        if (now >= nextSync) {
            sendSync(0, now100us(), 0);
            nextSync = now + kSyncEvery;
        }
        if (now - heard_ > kLost) {
            leave("Studio lost");
            setStatus("STUDIO LOST, SEARCHING...");
            continue;
        }
        if (now >= nextCheck_) { checkStudio(); nextCheck_ = now + 30; }
        flushMidiOut();
        pollfd p[2] = {{ctl_, POLLIN, 0}, {data_, POLLIN, 0}};
        if (::poll(p, 2, 2) > 0) {
            for (int k = 0; k < 2; ++k) {
                if (!(p[k].revents & POLLIN)) continue;
                uint8_t buf[1500];
                ssize_t n = ::recv(p[k].fd, buf, sizeof buf, 0);
                if (n > 0) received(buf, static_cast<int>(n), k == 1);
            }
        }
    }
}

// Audio thread: float -> S16BE in 310-frame RTP packets to the Studio.
void StudioLink::sendAudio(const float* left, const float* right, int n) {
    uint32_t dest = audioDest_.load(std::memory_order_acquire);
    if (!dest) { fill_ = 0; return; }
    for (int i = 0; i < n; ++i) {
        uint8_t* s = packet_ + 12 + fill_ * 4;
        auto conv = [](float x) {
            x = x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x);
            return static_cast<int16_t>(x * 32767.0f);
        };
        int16_t l = conv(left[i]), r = conv(right[i]);
        s[0] = static_cast<uint8_t>(l >> 8); s[1] = static_cast<uint8_t>(l & 0xff);
        s[2] = static_cast<uint8_t>(r >> 8); s[3] = static_cast<uint8_t>(r & 0xff);
        if (++fill_ == kFrames) {
            packet_[0] = 0x80;
            packet_[1] = kAudioPayload;
            put16(packet_ + 2, rtpSeq_++);
            put32(packet_ + 4, rtpTs_);
            put32(packet_ + 8, audioSsrc_);
            rtpTs_ += kFrames;
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_addr.s_addr = dest;
            to.sin_port = htons(static_cast<uint16_t>(cfg_.audioPort));
            ::sendto(audio_, packet_, sizeof packet_, MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&to), sizeof to);
            fill_ = 0;
        }
    }
}

// ---------------------------------------------------------------------------

void applyMidi(Synth& synth, const MidiMsg& m, int channel) {
    if (m.len >= 1) {   // the Studio's clock: tempo, start, stop
        switch (m.b[0]) {
            case 0xF8: synth.midiClock(m.sent); return;   // timed by the Studio's own clock
            case 0xFA: synth.midiStart(); return;
            case 0xFB: synth.midiContinue(); return;
            case 0xFC: synth.midiStop(); return;
            default: break;
        }
    }
    if (m.len < 1 || m.b[0] < 0x80 || m.b[0] >= 0xF0) return;
    int ch = m.b[0] & 0x0F;
    if (channel != 0 && ch != channel - 1) return;
    switch (m.b[0] & 0xF0) {
        case 0x90:
            if (m.len == 3) synth.noteOn(m.b[1], m.b[2]);
            break;
        case 0x80:
            if (m.len == 3) synth.noteOff(m.b[1]);
            break;
        case 0xB0:
            if (m.len == 3) synth.controlChange(m.b[1], m.b[2]);
            break;
        case 0xE0:
            if (m.len == 3) synth.pitchBend((((m.b[2] << 7) | m.b[1]) - 8192) / 8192.0f);
            break;
        case 0xC0:
            if (m.len >= 2) synth.programChange(m.b[1]);
            break;
        case 0xD0:
            if (m.len >= 2) synth.aftertouch(m.b[1] / 127.0f);
            break;
        default:
            break;
    }
}

}  // namespace xo
