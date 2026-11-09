// Stereo effects bus: drive -> chorus -> delay -> reverb.
// All buffers are allocated once in init(); process() never allocates.
#pragma once
#include <vector>

namespace xo {

// Musical divisions for tempo sync (delay, LFO, arpeggiator). Index 0 = free (not synced).
struct Division {
    const char* name;
    float beats;  // quarter notes
};
constexpr int kNumDivisions = 16;
extern const Division kDivisions[kNumDivisions];

class Chorus {
public:
    void init(float sampleRate);
    void process(float* l, float* r, int n, float rateHz, float depth, float mix);

private:
    std::vector<float> bufL_, bufR_;
    int w_ = 0;
    float phase_ = 0, sr_ = 48000;
};

class StereoDelay {
public:
    void init(float sampleRate, float maxSeconds);
    void process(float* l, float* r, int n, float timeSec, float feedback, float tone, float mix, bool pingPong);
    void clear();

private:
    std::vector<float> bufL_, bufR_;
    int w_ = 0;
    float sr_ = 48000, time_ = 0;  // smoothed delay in samples
    float lpL_ = 0, lpR_ = 0;
};

// Freeverb-style reverb (8 combs + 4 all-passes per side).
class Reverb {
public:
    void init(float sampleRate);
    void process(float* l, float* r, int n, float size, float damp, float mix);
    void clear();

private:
    struct Comb {
        std::vector<float> buf;
        int i = 0;
        float store = 0;
    };
    struct Allpass {
        std::vector<float> buf;
        int i = 0;
    };
    Comb combL_[8], combR_[8];
    Allpass apL_[4], apR_[4];
};

class FxBus {
public:
    void init(float sampleRate);
    // In-place on the voice mix. beats: current tempo (BPM) for synced delay.
    void process(float* l, float* r, int n, const float* params, float bpm);

private:
    Chorus chorus_;
    StereoDelay delay_;
    Reverb reverb_;
    float sr_ = 48000;
};

}  // namespace xo
