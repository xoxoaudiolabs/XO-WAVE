// Small DSP building blocks: envelopes, filter, helpers.
#pragma once
#include <cmath>
#include <cstdint>

#if defined(__SSE__) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

namespace xo {

constexpr float kPi = 3.14159265358979323846f;

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline float midiToHz(float note) { return 440.0f * std::exp2((note - 69.0f) * (1.0f / 12.0f)); }

// Smooth saturator (rational tanh approximation), ~linear for small inputs.
inline float softClip(float x) {
    x = clampf(x, -3.0f, 3.0f);
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// Denormal protection: flush-to-zero on the current thread.
inline void enableFlushToZero() {
#if defined(__aarch64__)
    uint64_t fpcr;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ull << 24);
    asm volatile("msr fpcr, %0" ::"r"(fpcr));
#elif defined(__arm__) && defined(__ARM_FP)
    uint32_t fpscr;
    asm volatile("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1u << 24);
    asm volatile("vmsr fpscr, %0" ::"r"(fpscr));
#elif defined(__SSE__) || defined(__x86_64__)
    _mm_setcsr(_mm_getcsr() | 0x8040);
#endif
}

struct Rng {
    uint32_t s = 0x9E3779B9u;
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float uni() { return (next() >> 8) * (1.0f / 16777216.0f); }    // [0,1)
    float bi() { return uni() * 2.0f - 1.0f; }                       // [-1,1)
};

// ---------------------------------------------------------------------------
// ADSR: linear attack, exponential decay/release.
struct EnvCoefs {
    float atkInc = 1.0f;
    float decCoef = 0.99f;
    float sustain = 1.0f;
    float relCoef = 0.99f;

    // Times in seconds; rate in Hz (sample rate or control rate).
    void set(float a, float d, float s, float r, float rate) {
        atkInc = 1.0f / std::fmax(1.0f, a * rate);
        decCoef = std::exp(-6.9078f / std::fmax(1.0f, d * rate));   // -60 dB in d seconds
        sustain = s;
        relCoef = std::exp(-6.9078f / std::fmax(1.0f, r * rate));
    }
};

struct Env {
    enum Stage : uint8_t { Idle, Attack, Decay, Release };
    Stage stage = Idle;
    float v = 0.0f;

    void gateOn() { stage = Attack; }                 // attacks from current level (no click)
    void gateOff() { if (stage != Idle) stage = Release; }
    void kill() { stage = Idle; v = 0.0f; }
    bool idle() const { return stage == Idle; }

    inline float next(const EnvCoefs& c) {
        switch (stage) {
            case Attack:
                v += c.atkInc;
                if (v >= 1.0f) { v = 1.0f; stage = Decay; }
                break;
            case Decay:  // also covers sustain; tracks sustain changes smoothly
                v = c.sustain + (v - c.sustain) * c.decCoef;
                break;
            case Release:
                v *= c.relCoef;
                if (v < 1e-5f) { v = 0.0f; stage = Idle; }
                break;
            case Idle:
                break;
        }
        return v;
    }
};

// ---------------------------------------------------------------------------
// Zero-delay-feedback state variable filter (Simper/Cytomic topology).
struct SvfCoefs {
    float a1 = 0, a2 = 0, a3 = 0, k = 2;
    void set(float cutoffHz, float resonance, float sampleRate) {
        float fc = clampf(cutoffHz, 10.0f, sampleRate * 0.45f);
        float g = std::tan(kPi * fc / sampleRate);
        k = 2.0f - 1.96f * clampf(resonance, 0.0f, 1.0f);
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
};

struct Svf {
    float ic1 = 0, ic2 = 0;
    void reset() { ic1 = ic2 = 0; }
    // mode: 0 = low-pass, 1 = band-pass, 2 = high-pass
    inline float process(float v0, const SvfCoefs& c, int mode) {
        float v3 = v0 - ic2;
        float v1 = c.a1 * ic1 + c.a2 * v3;
        float v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        if (mode == 0) return v2;
        if (mode == 1) return v1;
        return v0 - c.k * v1 - v2;
    }
};

}  // namespace xo
