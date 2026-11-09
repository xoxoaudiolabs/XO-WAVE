// File fuzz: hostile wavetable WAVs and preset/settings text. Nothing may crash,
// and a corrupt preset must never put non-finite values into the engine.
//   make filefuzz SAN=1
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "../src/config.h"
#include "../src/synth.h"

using namespace xo;
static int failures = 0;
static void check(bool ok, const char* what) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failures; }

static std::string tmp(const char* name) { return std::string("/tmp/xowave-fuzz-") + name; }
static void writeFile(const std::string& p, const std::vector<uint8_t>& d) { std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(d.data()), static_cast<long>(d.size())); }
static void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x & 0xff); v.push_back(x >> 8); }
static void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((x >> (8 * i)) & 0xff); }

static std::vector<uint8_t> wav(int fmt, int ch, int bits, uint32_t dataLenClaim, size_t realBytes, const char* clm = nullptr) {
    std::vector<uint8_t> v = {'R', 'I', 'F', 'F'};
    put32(v, 0xFFFFFFFFu); v.insert(v.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(v, 16); put16(v, static_cast<uint16_t>(fmt)); put16(v, static_cast<uint16_t>(ch)); put32(v, 48000); put32(v, 0);
    put16(v, 0); put16(v, static_cast<uint16_t>(bits));
    if (clm) { v.insert(v.end(), {'c', 'l', 'm', ' '}); put32(v, static_cast<uint32_t>(std::strlen(clm))); v.insert(v.end(), clm, clm + std::strlen(clm)); }
    v.insert(v.end(), {'d', 'a', 't', 'a'}); put32(v, dataLenClaim);
    std::mt19937 r(7);
    for (size_t i = 0; i < realBytes; ++i) v.push_back(static_cast<uint8_t>(r()));
    return v;
}

int main() {
    std::mt19937 rng(99);
    int loaded = 0, rejected = 0;
    std::vector<std::vector<uint8_t>> cases = {
        {}, {'R', 'I', 'F', 'F'}, {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'},
        wav(1, 1, 16, 4096, 4096), wav(1, 1, 16, 0xFFFFFFF0u, 100), wav(1, 0, 16, 4096, 4096), wav(1, 1, 8, 4096, 4096),
        wav(1, 1, 12, 4096, 4096), wav(1, 1, 24, 4095, 4095), wav(3, 1, 32, 4096, 4096), wav(3, 2, 64, 4096, 4096),
        wav(0xFFFE, 1, 16, 4096, 4096), wav(1, 7, 16, 9, 9), wav(1, 1, 16, 2, 2), wav(1, 1, 16, 4096 * 300, 4096 * 300),
        wav(1, 1, 16, 4096, 4096, "<!>3"), wav(1, 1, 16, 4096, 4096, "<!>99999999"), wav(1, 1, 16, 4096, 4096, "<!>"),
        wav(1, 1, 16, 4096, 4096, "<!>2048 0000"), wav(1, 1, 32, 100000, 100000, "<!>17"),
    };
    for (int i = 0; i < 300; ++i) {   // random junk and random corruptions of a valid file
        std::vector<uint8_t> v = i % 2 ? wav(1, 1, 16, 8192, 8192) : std::vector<uint8_t>(static_cast<size_t>(rng() % 2000));
        for (int k = 0; k < 1 + static_cast<int>(rng() % 20); ++k) if (!v.empty()) v[rng() % v.size()] = static_cast<uint8_t>(rng());
        if (i % 3 == 0 && v.size() > 10) v.resize(rng() % v.size());
        cases.push_back(v);
    }
    bool finiteTables = true;
    for (size_t i = 0; i < cases.size(); ++i) {
        writeFile(tmp("t.wav"), cases[i]);
        Wavetable w;
        std::string err;
        if (loadWavetableWav(tmp("t.wav"), w, err)) {
            ++loaded;
            for (int f = 0; f < w.numFrames(); ++f)
                for (int k = 0; k < Wavetable::kStride; ++k)
                    if (!std::isfinite(w.table(f, 0)[k])) finiteTables = false;
        } else {
            ++rejected;
        }
    }
    char msg[200];
    std::snprintf(msg, sizeof msg, "%zu hostile WAVs: %d loaded, %d rejected, no crash; loaded tables all finite=%d",
                  cases.size(), loaded, rejected, finiteTables);
    check(finiteTables, msg);

    // Presets / settings: garbage text, binary, huge lines, NaN/inf/huge numbers.
    std::vector<Wavetable> tables = makeFactoryTables();
    std::vector<std::string> texts = {
        "", "=", "==", "name", "cutoff = nan\nresonance = inf\nvolume = -inf\na_pos = 1e99\nb_pos = -1e99\n",
        "a_table = No Such Table\nb_table = -5\nfilter_mode = 9999\narp_mode = nan\nchord = 1e40\n",
        "a_unison = 0\nb_unison = 99\nsub_octave = -3\ndelay_sync = 1e9\nlfo_sync = -1e9\n",
        std::string(100000, 'x') + " = " + std::string(100000, '9'), "cutoff=\n=5\n # = 3\n\x01\x02\x03 = \xff\xfe",
        "mod1_src = 9999\nmod1_dst = -9999\nmod1_amt = nan\nmod2_src = 3\nmod2_dst = 3\nmod2_amt = 1e30\n",
    };
    for (int i = 0; i < 200; ++i) {
        std::string t;
        for (int k = 0; k < static_cast<int>(rng() % 400); ++k) t += static_cast<char>(rng() % 256);
        texts.push_back(t);
    }
    bool allFinite = true;
    for (size_t i = 0; i < texts.size(); ++i) {
        std::ofstream(tmp("p.xowave"), std::ios::binary) << texts[i];
        KeyValues kv;
        readKeyValues(tmp("p.xowave"), kv);
        std::vector<std::string> warnings;
        Preset p = presetFromKeyValues("fuzz", kv, tables, warnings);
        Synth s(48000, 8, tables);
        s.loadPreset(p);
        for (int n : {36, 48, 60, 72, 84}) s.noteOn(n, 100);
        std::vector<float> L(480), R(480);
        for (int b = 0; b < 40; ++b) {
            s.render(L.data(), R.data(), 480);
            for (int k = 0; k < 480; ++k) if (!std::isfinite(L[k]) || !std::isfinite(R[k])) allFinite = false;
        }
        for (int id = 0; id < P_COUNT; ++id) if (!std::isfinite(s.param(id))) allFinite = false;
    }
    std::snprintf(msg, sizeof msg, "%zu hostile preset/settings files (nan, inf, 1e99, junk): engine params and sound stay finite",
                  texts.size());
    check(allFinite, msg);

    std::printf(failures ? "\n%d check(s) failed\n" : "\nall file fuzz checks passed\n", failures);
    return failures ? 1 : 0;
}
