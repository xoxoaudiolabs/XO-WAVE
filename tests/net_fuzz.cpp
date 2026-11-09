// Network fuzz: the web app's HTTP server with hostile clients, and the Studio
// link's packet parsers with random packets. Nothing may crash or hang, numbers
// from the network must never become nan/inf in the engine's state.
//   make netfuzz SAN=1
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "../src/studio_link.h"
#include "../src/web.h"

using namespace xo;
static int failures = 0;
static void check(bool ok, const char* what) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failures; }

static std::string request(int port, const std::string& raw, int waitMs = 2000) {
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { ::close(s); return "CONNECT FAILED"; }
    timeval tv{waitMs / 1000, (waitMs % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (!raw.empty()) ::send(s, raw.data(), raw.size(), 0);
    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = ::recv(s, buf, sizeof buf, 0)) > 0) out.append(buf, static_cast<size_t>(n));
    ::close(s);
    return out;
}

int main() {
    std::mt19937 rng(5);
    std::vector<Wavetable> tables = makeFactoryTables();
    EngineStatus st;
    PresetBank bank;
    float v[P_COUNT];
    for (int i = 0; i < P_COUNT; ++i) v[i] = kParams[i].def;
    bank.write(0, "Init", "", v);
    CommandQueue cmds;
    WebServer web;
    WebConfig cfg;
    cfg.cmds = &cmds; cfg.status = &st; cfg.bank = &bank; cfg.tables = &tables; cfg.port = 18080; cfg.version = "test";
    if (!web.start(cfg)) { std::printf("can't start web server\n"); return 1; }
    const int port = web.port();

    // 1. Hostile requests: malformed, huge, binary, every endpoint with junk.
    std::vector<std::string> reqs = {
        "", "\r\n\r\n", "GET\r\n\r\n", "GET /\r\n\r\n", "POST / HTTP/1.0\r\n\r\n", "GET / HTTP/1.0\r\n" + std::string(20000, 'H') + "\r\n\r\n",
        "GET /" + std::string(10000, 'a') + " HTTP/1.0\r\n\r\n", "GET /api/set?p=cutoff&n=nan HTTP/1.0\r\n\r\n",
        "GET /api/set?p=cutoff&v=inf HTTP/1.0\r\n\r\n", "GET /api/set?p=nosuch&n=0.5 HTTP/1.0\r\n\r\n", "GET /api/set?p=cutoff HTTP/1.0\r\n\r\n",
        "GET /api/master?v=nan HTTP/1.0\r\n\r\n", "GET /api/master?v=-inf HTTP/1.0\r\n\r\n", "GET /api/tempo?v=nan HTTP/1.0\r\n\r\n",
        "GET /api/tempo?v=1e99 HTTP/1.0\r\n\r\n", "GET /api/preset?i=-5 HTTP/1.0\r\n\r\n", "GET /api/preset?i=99999999999 HTTP/1.0\r\n\r\n",
        "GET /api/note?n=9999&v=-3 HTTP/1.0\r\n\r\n", "GET /api/noteoff?n=nan HTTP/1.0\r\n\r\n", "GET /fonts/../../etc/passwd HTTP/1.0\r\n\r\n",
        "GET /fonts/%2e%2e%2fsecret HTTP/1.0\r\n\r\n", "GET /api/state?&&&=&= HTTP/1.0\r\n\r\n",
    };
    for (int i = 0; i < 200; ++i) {
        std::string r;
        for (int k = 0; k < static_cast<int>(rng() % 600); ++k) r += static_cast<char>(rng() % 256);
        reqs.push_back(i % 2 ? "GET /api/set?" + r + " HTTP/1.0\r\n\r\n" : r);
    }
    int answered = 0;
    for (const auto& r : reqs) if (!request(port, r, 1000).empty()) ++answered;
    bool leakedFile = request(port, "GET /fonts/../../etc/passwd HTTP/1.0\r\n\r\n").find("root:") != std::string::npos;
    Cmd c;
    bool finiteCmds = true;
    while (cmds.pop(c)) if (!isFiniteBits(c.b)) finiteCmds = false;
    bool finiteStatus = isFiniteBits(st.master.load()) && isFiniteBits(st.tempo.load()) && st.tempo.load() >= 30 &&
                        st.tempo.load() <= 300 && st.master.load() >= 0 && st.master.load() <= 1;
    char msg[200];
    std::snprintf(msg, sizeof msg, "%zu hostile HTTP requests: no crash, %d answered; no file outside fonts served=%d",
                  reqs.size(), answered, !leakedFile);
    check(!leakedFile, msg);
    std::snprintf(msg, sizeof msg, "web numbers (nan, inf, 1e99) never reach the engine: commands finite=%d, master/tempo in range=%d",
                  finiteCmds, finiteStatus);
    check(finiteCmds && finiteStatus, msg);

    // 2. A client that connects and sends nothing must not stall everyone else.
    std::thread silent([&] { request(port, "", 3000); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto t0 = std::chrono::steady_clock::now();
    std::string ok = request(port, "GET /api/state HTTP/1.0\r\n\r\n");
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    silent.join();
    std::snprintf(msg, sizeof msg, "a silent client doesn't block others: state answered in %.0f ms (< 600)", ms);
    check(ok.find("\"params\"") != std::string::npos && ms < 600, msg);
    web.stop();

    // 3. Studio link packets: random, truncated, crafted headers.
    int midiOut = 0, frames = 0;
    bool finiteAudio = true;
    for (int i = 0; i < 200000; ++i) {
        std::vector<uint8_t> p(rng() % 1600);
        for (auto& b : p) b = static_cast<uint8_t>(rng());
        if (p.size() > 2 && i % 2) { p[0] = static_cast<uint8_t>(0x80 | (rng() & 0x3F)); p[1] = 0x61; }   // looks like RTP-MIDI
        if (p.size() > 2 && i % 3 == 0) p[0] = static_cast<uint8_t>(0x80 | (rng() & 0x3F));                 // looks like RTP audio
        MidiMsg m[128];
        midiOut += decodeRtpMidi(p.data(), static_cast<int>(p.size()), m, 128);
        float l[512], r[512];
        int f = parseRtpL16(p.data(), static_cast<int>(p.size()), l, r, 512);
        frames += f;
        for (int k = 0; k < f; ++k) if (!std::isfinite(l[k]) || !std::isfinite(r[k]) || std::fabs(l[k]) > 1.0f) finiteAudio = false;
    }
    std::snprintf(msg, sizeof msg, "200000 random link packets: no crash (%d MIDI messages, %d audio frames decoded), audio in range=%d",
                  midiOut, frames, finiteAudio);
    check(finiteAudio, msg);

    std::printf(failures ? "\n%d check(s) failed\n" : "\nall network fuzz checks passed\n", failures);
    return failures ? 1 : 0;
}
