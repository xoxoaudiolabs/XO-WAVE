// Sound-engine checks: aliasing, stability at extreme settings, voice stealing.
// Build & run:  make test
#include <cmath>
#include <cstdio>
#include <vector>

#include "../src/synth.h"

using namespace xo;

static int failures = 0;
static void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

static std::vector<float> renderNote(Synth& s, int note, int frames) {
    std::vector<float> L(frames), R(frames);
    s.noteOn(note, 127);
    for (int i = 0; i < frames; i += 128) s.render(&L[i], &R[i], std::min(128, frames - i));
    s.allNotesOff(true);
    return L;
}

// Fraction of energy (dB) that is NOT at a harmonic of f0.
static double inharmonicDb(const std::vector<float>& x, int start, int len, double f0, double sr) {
    double harm = 0, other = 0;
    const double binHz = sr / len;
    for (int k = 1; k < len / 2; ++k) {
        double re = 0, im = 0, w = 2 * M_PI * k / len;
        for (int n = 0; n < len; ++n) {
            double ph = 2 * M_PI * n / (len - 1);
            double win = 0.35875 - 0.48829 * std::cos(ph) + 0.14128 * std::cos(2 * ph) - 0.01168 * std::cos(3 * ph);
            re += x[start + n] * win * std::cos(w * n);
            im -= x[start + n] * win * std::sin(w * n);
        }
        double p = re * re + im * im, f = k * binHz;
        double h = std::round(f / f0);
        if (h >= 1 && std::fabs(f - h * f0) < 5 * binHz) harm += p; else other += p;
    }
    return 10 * std::log10(other / harm);
}

int main() {
    const float sr = 48000;
    std::vector<Wavetable> tables = makeFactoryTables();

    {  // 1. Aliasing: high saw note with the filter wide open.
        Synth s(sr, 8, tables);
        s.setParam(P_A_TABLE, 0); s.setParam(P_A_POS, 0.66f);  // saw
        s.setParam(P_CUTOFF, 20000); s.setParam(P_FENV, 0); s.setParam(P_FKEY, 0);
        s.setParam(P_RESO, 0); s.setParam(P_AMP_S, 1); s.setParam(P_VEL_AMP, 0);
        for (int note : {84, 96, 103}) {
            auto x = renderNote(s, note, 24000);
            double db = inharmonicDb(x, 12000, 4096, midiToHz(note), sr);
            char msg[96];
            std::snprintf(msg, sizeof msg, "saw note %d: inharmonic (aliasing) energy %.1f dB (< -50)", note, db);
            check(db < -50, msg);
        }
    }

    {  // 2. Stability: every table, every extreme, all finite and bounded.
        Synth s(sr, 16, tables);
        s.setParam(P_RESO, 1); s.setParam(P_DRIVE, 1); s.setParam(P_A_UNISON, 4); s.setParam(P_B_UNISON, 4);
        s.setParam(P_B_LEVEL, 1); s.setParam(P_LFO_RATE, 20); s.setParam(P_LFO_CUTOFF, 1); s.setParam(P_LFO_POS, 1);
        s.setParam(P_FENV, 1); s.setParam(P_VOLUME, 1);
        bool ok = true;
        float peak = 0;
        std::vector<float> L(128), R(128);
        for (int t = 0; t < static_cast<int>(tables.size()); ++t) {
            s.setParam(P_A_TABLE, static_cast<float>(t)); s.setParam(P_B_TABLE, static_cast<float>((t + 3) % tables.size()));
            for (int mode = 0; mode < 3; ++mode) {
                s.setParam(P_FILTER_MODE, static_cast<float>(mode));
                for (int n = 0; n < 16; ++n) s.noteOn(20 + n * 7, 127);
                for (int b = 0; b < 200; ++b) {
                    s.setParam(P_CUTOFF, (b % 2) ? 20000.0f : 20.0f);
                    s.render(L.data(), R.data(), 128);
                    for (int i = 0; i < 128; ++i) {
                        if (!std::isfinite(L[i]) || !std::isfinite(R[i])) ok = false;
                        peak = std::max(peak, std::max(std::fabs(L[i]), std::fabs(R[i])));
                    }
                }
                s.allNotesOff(true);
            }
        }
        std::printf("    finite=%d peak=%.4f\n", ok, peak);
        check(ok && peak <= 1.0001f, "all tables / filter modes / max resonance+drive: finite, within +/-1");
    }

    {  // 3. Voice stealing: many fast notes on 4 voices, no huge jumps.
        Synth s(sr, 4, tables);
        s.setParam(P_A_TABLE, 0); s.setParam(P_A_POS, 0.0f);  // sine: clicks would show up as big steps
        s.setParam(P_CUTOFF, 20000); s.setParam(P_AMP_R, 1.0f);
        std::vector<float> L(128), R(128);
        float maxStep = 0, prev = 0;
        for (int b = 0; b < 2000; ++b) {
            if (b % 10 == 0) s.noteOn(48 + (b / 10) % 24, 100);
            if (b % 10 == 5) s.noteOff(48 + (b / 10) % 24);
            s.render(L.data(), R.data(), 128);
            for (int i = 0; i < 128; ++i) { maxStep = std::max(maxStep, std::fabs(L[i] - prev)); prev = L[i]; }
        }
        char msg[96];
        std::snprintf(msg, sizeof msg, "voice stealing: max sample step %.3f (< 0.25)", maxStep);
        check(maxStep < 0.25f, msg);
    }

    {  // 4. Release finishes and frees the voice.
        Synth s(sr, 8, tables);
        s.setParam(P_AMP_R, 0.1f);
        std::vector<float> L(128), R(128);
        s.noteOn(60, 100);
        for (int b = 0; b < 40; ++b) s.render(L.data(), R.data(), 128);
        s.noteOff(60);
        for (int b = 0; b < 200; ++b) s.render(L.data(), R.data(), 128);
        check(s.activeVoices() == 0, "voice frees itself after release");
    }

    {  // 5. Effects: everything maxed stays finite and bounded; the delay really echoes.
        Synth s(sr, 8, tables);
        for (int id : {P_CHORUS_MIX, P_DELAY_MIX, P_REVERB_MIX, P_FX_DRIVE, P_REVERB_SIZE}) s.setParam(id, 1.0f);
        s.setParam(P_DELAY_FB, 0.95f);
        s.setParam(P_DELAY_SYNC, 0);
        s.setParam(P_DELAY_TIME, 0.25f);
        s.setParam(P_AMP_R, 0.01f);
        std::vector<float> L(128), R(128);
        bool ok = true;
        float peak = 0, echo = 0;
        s.noteOn(60, 127);
        for (int b = 0; b < 20; ++b) s.render(L.data(), R.data(), 128);
        s.allNotesOff(true);
        for (int b = 0; b < 3000; ++b) {   // 8 s of tail
            s.render(L.data(), R.data(), 128);
            for (int i = 0; i < 128; ++i) {
                if (!std::isfinite(L[i]) || !std::isfinite(R[i])) ok = false;
                peak = std::max(peak, std::max(std::fabs(L[i]), std::fabs(R[i])));
                if (b > 100 && b < 200) echo = std::max(echo, std::fabs(L[i]) + std::fabs(R[i]));
            }
        }
        char msg[120];
        std::snprintf(msg, sizeof msg, "FX maxed (feedback 95%%): finite=%d peak %.3f, echo after note off %.3f", ok, peak, echo);
        check(ok && peak <= 1.0001f && echo > 0.01f, msg);
    }

    {  // 6. Chord memory: one key, a major chord.
        Synth s(sr, 8, tables);
        s.setParam(P_CHORD, 1);
        std::vector<float> L(128), R(128);
        s.noteOn(60, 100);
        s.render(L.data(), R.data(), 128);
        char msg[96];
        std::snprintf(msg, sizeof msg, "chord memory MAJ: one key plays %d voices (3)", s.activeVoices());
        check(s.activeVoices() == 3, msg);
        s.noteOff(60);
    }

    {  // 7. Arpeggiator: 1/16 at the internal 120 BPM = 8 notes a second.
        EngineStatus st;
        Synth s(sr, 8, tables);
        s.setStatus(&st);
        s.setParam(P_ARP_MODE, 1); s.setParam(P_ARP_RATE, 3); s.setParam(P_ARP_GATE, 0.4f);
        s.setParam(P_AMP_A, 0.001f); s.setParam(P_AMP_S, 1); s.setParam(P_AMP_R, 0.002f);
        s.setParam(P_FX_DRIVE, 0); s.setParam(P_VEL_AMP, 0);
        s.noteOn(60, 100); s.noteOn(64, 100); s.noteOn(67, 100);
        std::vector<float> L(64), R(64);
        int onsets = 0; bool loud = false;
        for (int b = 0; b < static_cast<int>(sr * 2 / 64); ++b) {   // 2 s
            s.render(L.data(), R.data(), 64);
            float e = 0;
            for (int i = 0; i < 64; ++i) e = std::max(e, std::fabs(L[i]));
            if (!loud && e > 0.02f) { ++onsets; loud = true; }
            if (loud && e < 0.002f) loud = false;
        }
        char msg[96];
        std::snprintf(msg, sizeof msg, "arpeggiator 1/16 at 120 BPM: %d notes in 2 s (16)", onsets);
        check(onsets >= 15 && onsets <= 17, msg);
    }

    {  // 8. MIDI clock: follows 125 BPM ticks, then falls back to the internal tempo.
        EngineStatus st;
        Synth s(sr, 8, tables);
        s.setStatus(&st);
        const int perTick = 960;   // 48000 * 60 / (125 * 24)
        std::vector<float> L(perTick), R(perTick);
        for (int t = 0; t < 24 * 8; ++t) { s.midiClock(); s.render(L.data(), R.data(), perTick); }
        float following = st.bpmNow.load();
        bool ext = st.extClock.load();
        for (int b = 0; b < 60; ++b) s.render(L.data(), R.data(), perTick);   // 1.2 s without ticks
        char msg[120];
        std::snprintf(msg, sizeof msg, "MIDI clock: %.1f BPM while following (125), ext=%d, then %.1f (120)", following,
                      ext, st.bpmNow.load());
        check(ext && std::fabs(following - 125) < 0.6f && !st.extClock.load() && std::fabs(st.bpmNow.load() - 120) < 0.01f, msg);
    }

    {  // 9. Mod matrix: VELOCITY -> A LEVEL at -100%: a hard hit is silent, a soft one isn't.
        auto peakFor = [&](int vel) {
            Synth s(sr, 8, tables);
            s.setParam(P_M1_SRC, 5); s.setParam(P_M1_DST, 6); s.setParam(P_M1_AMT, -1.0f);
            s.setParam(P_VEL_AMP, 0); s.setParam(P_A_LEVEL, 0.8f);
            std::vector<float> L(256), R(256);
            s.noteOn(60, vel);
            float pk = 0;
            for (int b = 0; b < 40; ++b) {
                s.render(L.data(), R.data(), 256);
                if (b > 10) for (float x : L) pk = std::max(pk, std::fabs(x));
            }
            return pk;
        };
        float hard = peakFor(127), soft = peakFor(20);
        char msg[120];
        std::snprintf(msg, sizeof msg, "mod matrix VELOCITY>A LEVEL -100%%: hard hit %.4f (silent), soft %.3f", hard, soft);
        check(hard < 0.001f && soft > 0.02f, msg);
    }

    std::printf(failures ? "\n%d check(s) failed\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
