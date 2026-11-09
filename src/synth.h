// Polyphonic wavetable synth engine. Not thread-safe by design: MIDI and
// audio both run on the single real-time thread, so there is no locking.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "dsp.h"
#include "engine_link.h"
#include "fx.h"
#include "params.h"
#include "wavetable.h"

namespace xo {

struct Preset {
    std::string name;
    std::string file;
    std::array<float, P_COUNT> values;
};

class Synth {
public:
    static constexpr int kMaxVoices = 16;
    static constexpr int kMaxUnison = 4;
    static constexpr int kBlock = 16;  // control-rate subblock

    Synth(float sampleRate, int voices, const std::vector<Wavetable>& tables);

    // MIDI
    void noteOn(int note, int velocity);
    void noteOff(int note);
    void controlChange(int cc, int value);
    void pitchBend(float amount);  // -1 .. 1
    void programChange(int program);
    void allNotesOff(bool hard);

    void setCcMapping(int cc, int paramId);
    void clearCcMap();
    void setParam(int id, float value);
    float param(int id) const { return target_[id]; }

    // Presets live in a bank shared with the UI; program change reads it.
    void setPresetBank(const PresetBank* bank) { bank_ = bank; }
    const PresetBank* presetBank() const { return bank_; }
    void loadPreset(const float* values, int index = -1);
    void loadPreset(const Preset& p) { loadPreset(p.values.data()); }
    // Optional: publish state for the UI.
    void setStatus(EngineStatus* status);
    int numTables() const { return static_cast<int>(tables_.size()); }
    int activeVoices() const;

    // Renders n frames into separate left/right buffers (overwrites).
    void render(float* left, float* right, int n);

private:
    struct Voice {
        int note = -1;
        float vel = 0;
        float pitch = 60;  // current (gliding) pitch
        bool gate = false;
        bool held = false;  // released while sustain pedal down
        uint64_t age = 0;
        Env amp, mod;
        float phaseA[kMaxUnison] = {}, phaseB[kMaxUnison] = {};
        Svf filtL, filtR;
    };

    void renderBlock(float* left, float* right, int n);
    void renderVoice(Voice& v, float* outL, float* outR, int n);
    void renderOsc(const Wavetable& wt, float pos, float pitch, float level, int unison,
                   float detune, float* phases, float* bufL, float* bufR, int n);
    void startVoice(Voice& v, int note, int velocity, bool fresh);

    float sr_;
    int numVoices_;
    const std::vector<Wavetable>& tables_;
    const PresetBank* bank_ = nullptr;
    EngineStatus* status_ = nullptr;
    Voice voices_[kMaxVoices];
    uint64_t ageCounter_ = 0;
    int ccMap_[128];
    bool sustain_ = false;
    float bend_ = 0, modWheel_ = 0;
    float lastPitch_ = 60;

    std::array<float, P_COUNT> target_{};
    std::array<float, P_COUNT> smooth_{};

    // per-block derived state
    EnvCoefs ampCoefs_, modCoefs_;
    float lfoPhase_ = 0, lfoValue_ = 0, lfoSH_ = 0;
    float glideCoef_ = 0;
    float livePos_[2] = {-1, -1};
    float masterSmooth_ = 0.64f;
    FxBus fx_;
    float bpm_ = 120.0f;   // tempo for synced effects (external MIDI clock or the internal tempo)
    float panL_[kMaxUnison + 1][kMaxUnison], panR_[kMaxUnison + 1][kMaxUnison];
    Rng rng_;
};

}  // namespace xo
