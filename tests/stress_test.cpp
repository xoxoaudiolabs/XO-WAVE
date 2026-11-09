// Engine stress test: random MIDI storms, random (and extreme) parameter
// changes, preset switching, arpeggiator/chord toggling, MIDI clock coming
// and going, in long runs. Checks the output stays finite and bounded, the
// engine is fast enough, and nothing keeps sounding after "all notes off".
// Build with sanitizers to catch memory and undefined-behaviour bugs:
//   make stress   (or: make stress SAN=1)
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "../src/synth.h"

using namespace xo;

static int failures = 0;
static void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

int main(int argc, char** argv) {
    const float sr = 48000;
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 120;
    std::vector<Wavetable> tables = makeFactoryTables();
    PresetBank bank;
    std::mt19937 rng(argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 12345u);
    auto uni = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); };
    auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<unsigned>(n)); };

    // A few random presets in the bank (program change reads it).
    for (int p = 0; p < 6; ++p) {
        float v[P_COUNT];
        for (int i = 0; i < P_COUNT; ++i) v[i] = paramFromNormalized(i, uni(0, 1), static_cast<int>(tables.size()));
        bank.write(bank.count(), ("rnd" + std::to_string(p)).c_str(), "", v);
    }

    EngineStatus st;
    Synth s(sr, 16, tables);
    s.setStatus(&st);
    s.setPresetBank(&bank);
    const bool guard = argc > 3 ? std::atoi(argv[3]) != 0 : true;
    s.setCpuGuard(guard);
    int late = 0;
    for (int cc = 0; cc < 128; ++cc) s.setCcMapping(cc, cc % P_COUNT);   // every CC moves something

    const int block = 480;
    std::vector<float> L(block), R(block);
    bool finite = true;
    float peak = 0;
    double worst = 0;
    long blocks = 0, events = 0;
    bool clockOn = false;
    int clockLeft = 0;
    auto t0 = std::chrono::steady_clock::now();

    for (long done = 0; done < static_cast<long>(seconds * sr); done += block) {
        // A storm of events before each period.
        int n = pick(12);
        for (int e = 0; e < n; ++e, ++events) {
            switch (pick(14)) {
                case 0: case 1: case 2: s.noteOn(pick(128), 1 + pick(127)); break;
                case 3: case 4: s.noteOff(pick(128)); break;
                case 5: s.controlChange(pick(128), pick(128)); break;
                case 6: s.pitchBend(uni(-1, 1)); break;
                case 7: s.aftertouch(uni(0, 1)); break;
                case 8: if (pick(20) == 0) s.programChange(pick(128)); break;
                case 9: { int id = pick(P_COUNT); s.setParam(id, uni(kParams[id].min - 10, kParams[id].max + 10)); break; }
                case 10: s.setParam(P_ARP_MODE, static_cast<float>(pick(6))); break;
                case 11: s.setParam(P_ARP_HOLD, static_cast<float>(pick(2))); break;
                case 12: s.setParam(P_CHORD, static_cast<float>(pick(9))); break;
                case 13: if (pick(50) == 0) s.midiStart(); break;
            }
        }
        // MIDI clock appears and disappears, at random tempos.
        if (clockLeft-- <= 0) { clockOn = !clockOn; clockLeft = 50 + pick(400); }
        if (clockOn) for (int t = 0; t < 1 + pick(2); ++t) s.midiClock(pick(3) == 0 ? -1 : uni(0, 1e5));
        if (pick(2000) == 0) s.allNotesOff(pick(2) == 0);
        if (pick(500) == 0) s.setParam(P_A_UNISON, 4), s.setParam(P_B_UNISON, 4);

        auto b0 = std::chrono::steady_clock::now();
        s.render(L.data(), R.data(), block);
        double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - b0).count();
        worst = std::max(worst, took);
        if (took > 0.010) ++late;
        for (int i = 0; i < block; ++i) {
            if (!std::isfinite(L[i]) || !std::isfinite(R[i])) finite = false;
            peak = std::max(peak, std::max(std::fabs(L[i]), std::fabs(R[i])));
        }
        ++blocks;
    }
    double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    char msg[200];
    std::snprintf(msg, sizeof msg, "%d s of random storms (%ld events): output finite=%d, peak %.4f", seconds, events,
                  finite, peak);
    check(finite && peak <= 1.0001f, msg);
    std::snprintf(msg, sizeof msg, "speed (CPU guard %s): %.1f%% of real time on average, worst period %.2f ms of 10 ms, %d of %ld periods late",
                  guard ? "on" : "off", 100.0 * total / seconds, worst * 1000, late, blocks);
    check(late * 200 <= blocks, msg);   // at most 0.5% of periods (the guard reacts one period late)

    // All notes off (hard) leaves silence, whatever state the storm left.
    s.setParam(P_ARP_MODE, 0);
    s.allNotesOff(true);
    for (int id : {P_DELAY_MIX, P_REVERB_MIX, P_CHORUS_MIX}) s.setParam(id, 0);
    float tail = 0;
    for (int b = 0; b < 200; ++b) {
        s.render(L.data(), R.data(), block);
        if (b > 100) for (int i = 0; i < block; ++i) tail = std::max(tail, std::fabs(L[i]) + std::fabs(R[i]));
    }
    std::snprintf(msg, sizeof msg, "after all-notes-off + FX off: %d voices, level %.6f", s.activeVoices(), tail);
    check(s.activeVoices() == 0 && tail < 0.001f, msg);

    // Released keys always end: random notes on/off with the arp on and off, then all keys up.
    Synth k(sr, 8, tables);
    k.setStatus(&st);
    for (int round = 0; round < 2000; ++round) {
        int note = 36 + pick(48);
        if (pick(2)) k.noteOn(note, 100); else k.noteOff(note);
        if (pick(30) == 0) k.setParam(P_ARP_MODE, static_cast<float>(pick(6)));
        if (pick(30) == 0) k.setParam(P_CHORD, static_cast<float>(pick(9)));
        if (pick(40) == 0) k.setParam(P_ARP_HOLD, static_cast<float>(pick(2)));
        if (pick(60) == 0) k.controlChange(64, pick(2) ? 127 : 0);
        k.render(L.data(), R.data(), 64);
    }
    k.controlChange(64, 0);
    k.setParam(P_ARP_HOLD, 0);
    for (int note = 0; note < 128; ++note) k.noteOff(note);   // every key up
    k.setParam(P_AMP_R, 0.05f);
    for (int b = 0; b < 400; ++b) k.render(L.data(), R.data(), 480);
    std::snprintf(msg, sizeof msg, "random playing (arp/chord/hold/sustain toggling), then every key up: %d voices left",
                  k.activeVoices());
    if (k.activeVoices()) std::printf("%s", k.debugState().c_str());
    check(k.activeVoices() == 0, msg);

    std::printf(failures ? "\n%d check(s) failed\n" : "\nall stress checks passed\n", failures);
    return failures ? 1 : 0;
}
