#include "fx.h"

#include <algorithm>
#include <cmath>

#include "dsp.h"
#include "params.h"

namespace xo {

const Division kDivisions[kNumDivisions] = {
    {"FREE", 0},       {"1/32", 0.125f}, {"1/16T", 1.0f / 6}, {"1/16", 0.25f}, {"1/8T", 1.0f / 3},
    {"1/16D", 0.375f}, {"1/8", 0.5f},    {"1/4T", 2.0f / 3},  {"1/8D", 0.75f}, {"1/4", 1.0f},
    {"1/2T", 4.0f / 3}, {"1/4D", 1.5f},  {"1/2", 2.0f},       {"1 BAR", 4.0f}, {"2 BARS", 8.0f},
    {"4 BARS", 16.0f},
};

// ---------------------------------------------------------------------------
// Chorus: two modulated delay lines (left and right a quarter-cycle apart).

void Chorus::init(float sampleRate) {
    sr_ = sampleRate;
    int size = static_cast<int>(sampleRate * 0.05f) + 4;  // 50 ms
    bufL_.assign(static_cast<size_t>(size), 0.0f);
    bufR_.assign(static_cast<size_t>(size), 0.0f);
    w_ = 0;
}

static inline float readFrac(const std::vector<float>& b, int w, float delay) {
    const int size = static_cast<int>(b.size());
    float pos = static_cast<float>(w) - delay;
    while (pos < 0) pos += static_cast<float>(size);
    int i0 = static_cast<int>(pos);
    float fr = pos - static_cast<float>(i0);
    int i1 = i0 + 1 >= size ? 0 : i0 + 1;
    return b[static_cast<size_t>(i0)] + fr * (b[static_cast<size_t>(i1)] - b[static_cast<size_t>(i0)]);
}

void Chorus::process(float* l, float* r, int n, float rateHz, float depth, float mix) {
    if (mix < 0.001f) return;
    const int size = static_cast<int>(bufL_.size());
    const float base = 0.012f * sr_, swing = (0.001f + 0.006f * depth) * sr_;
    const float inc = rateHz / sr_;
    for (int i = 0; i < n; ++i) {
        bufL_[static_cast<size_t>(w_)] = l[i];
        bufR_[static_cast<size_t>(w_)] = r[i];
        float ml = std::sin(2.0f * kPi * phase_);
        float mr = std::cos(2.0f * kPi * phase_);
        float wl = readFrac(bufL_, w_, base + swing * (0.5f + 0.5f * ml));
        float wr = readFrac(bufR_, w_, base + swing * (0.5f + 0.5f * mr));
        l[i] += (wl - l[i]) * mix * 0.5f;
        r[i] += (wr - r[i]) * mix * 0.5f;
        phase_ += inc;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
        if (++w_ >= size) w_ = 0;
    }
}

// ---------------------------------------------------------------------------
// Delay

void StereoDelay::init(float sampleRate, float maxSeconds) {
    sr_ = sampleRate;
    size_t size = static_cast<size_t>(sampleRate * maxSeconds) + 4;
    bufL_.assign(size, 0.0f);
    bufR_.assign(size, 0.0f);
    w_ = 0;
    time_ = 0.375f * sampleRate;
}

void StereoDelay::clear() {
    std::fill(bufL_.begin(), bufL_.end(), 0.0f);
    std::fill(bufR_.begin(), bufR_.end(), 0.0f);
    lpL_ = lpR_ = 0;
}

void StereoDelay::process(float* l, float* r, int n, float timeSec, float feedback, float tone, float mix,
                          bool pingPong) {
    const int size = static_cast<int>(bufL_.size());
    const float target = clampf(timeSec * sr_, 16.0f, static_cast<float>(size - 4));
    const float lp = 0.08f + 0.9f * tone * tone;  // one-pole low-pass in the feedback path
    const bool idle = mix < 0.001f;
    for (int i = 0; i < n; ++i) {
        time_ += (target - time_) * 0.0005f;  // glide to new times (no clicks)
        float dl = readFrac(bufL_, w_, time_);
        float dr = readFrac(bufR_, w_, time_);
        lpL_ += (dl - lpL_) * lp;
        lpR_ += (dr - lpR_) * lp;
        float inL = idle ? 0.0f : l[i], inR = idle ? 0.0f : r[i];
        if (pingPong) {  // mono in on the left, repeats bounce left <-> right
            bufL_[static_cast<size_t>(w_)] = 0.5f * (inL + inR) + lpR_ * feedback;
            bufR_[static_cast<size_t>(w_)] = lpL_ * feedback;
        } else {
            bufL_[static_cast<size_t>(w_)] = inL + lpL_ * feedback;
            bufR_[static_cast<size_t>(w_)] = inR + lpR_ * feedback;
        }
        l[i] += dl * mix;
        r[i] += dr * mix;
        if (++w_ >= size) w_ = 0;
    }
}

// ---------------------------------------------------------------------------
// Reverb (Freeverb tunings, scaled from 44.1 kHz)

static const int kCombTune[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
static const int kApTune[4] = {556, 441, 341, 225};
static const int kSpread = 23;

void Reverb::init(float sampleRate) {
    const float k = sampleRate / 44100.0f;
    for (int i = 0; i < 8; ++i) {
        combL_[i].buf.assign(static_cast<size_t>(kCombTune[i] * k), 0.0f);
        combR_[i].buf.assign(static_cast<size_t>((kCombTune[i] + kSpread) * k), 0.0f);
    }
    for (int i = 0; i < 4; ++i) {
        apL_[i].buf.assign(static_cast<size_t>(kApTune[i] * k), 0.0f);
        apR_[i].buf.assign(static_cast<size_t>((kApTune[i] + kSpread) * k), 0.0f);
    }
}

void Reverb::clear() {
    for (auto* set : {combL_, combR_})
        for (int i = 0; i < 8; ++i) { std::fill(set[i].buf.begin(), set[i].buf.end(), 0.0f); set[i].store = 0; }
    for (auto* set : {apL_, apR_})
        for (int i = 0; i < 4; ++i) std::fill(set[i].buf.begin(), set[i].buf.end(), 0.0f);
}

void Reverb::process(float* l, float* r, int n, float size, float damp, float mix) {
    if (mix < 0.001f) return;
    const float fb = 0.70f + 0.28f * size;
    const float d1 = 0.4f * damp, d2 = 1.0f - d1;
    for (int i = 0; i < n; ++i) {
        const float in = (l[i] + r[i]) * 0.015f;
        float outL = 0, outR = 0;
        for (int c = 0; c < 8; ++c) {
            for (int side = 0; side < 2; ++side) {
                Comb& cb = side ? combR_[c] : combL_[c];
                float y = cb.buf[static_cast<size_t>(cb.i)];
                cb.store = y * d2 + cb.store * d1;
                cb.buf[static_cast<size_t>(cb.i)] = in + cb.store * fb;
                if (++cb.i >= static_cast<int>(cb.buf.size())) cb.i = 0;
                (side ? outR : outL) += y;
            }
        }
        for (int a = 0; a < 4; ++a) {
            for (int side = 0; side < 2; ++side) {
                Allpass& ap = side ? apR_[a] : apL_[a];
                float& x = side ? outR : outL;
                float b = ap.buf[static_cast<size_t>(ap.i)];
                ap.buf[static_cast<size_t>(ap.i)] = x + b * 0.5f;
                x = b - x;
                if (++ap.i >= static_cast<int>(ap.buf.size())) ap.i = 0;
            }
        }
        l[i] += outL * mix * 3.0f;
        r[i] += outR * mix * 3.0f;
    }
}

// ---------------------------------------------------------------------------

void FxBus::init(float sampleRate) {
    sr_ = sampleRate;
    chorus_.init(sampleRate);
    delay_.init(sampleRate, 2.2f);
    reverb_.init(sampleRate);
}

void FxBus::process(float* l, float* r, int n, const float* p, float bpm) {
    const float drive = p[P_FX_DRIVE];
    if (drive > 0.001f) {
        const float g = 1.0f + drive * 8.0f, comp = 1.0f / (1.0f + drive * 1.5f);
        for (int i = 0; i < n; ++i) {
            l[i] = softClip(l[i] * g) * comp;
            r[i] = softClip(r[i] * g) * comp;
        }
    }
    chorus_.process(l, r, n, p[P_CHORUS_RATE], p[P_CHORUS_DEPTH], p[P_CHORUS_MIX]);
    int sync = static_cast<int>(p[P_DELAY_SYNC]);
    float t = p[P_DELAY_TIME];
    if (sync > 0 && sync < kNumDivisions) t = kDivisions[sync].beats * 60.0f / std::max(20.0f, bpm);
    delay_.process(l, r, n, t, p[P_DELAY_FB], p[P_DELAY_TONE], p[P_DELAY_MIX], p[P_DELAY_PP] >= 0.5f);
    reverb_.process(l, r, n, p[P_REVERB_SIZE], p[P_REVERB_DAMP], p[P_REVERB_MIX]);
}

}  // namespace xo
