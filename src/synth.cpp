#include "synth.h"

#include <algorithm>
#include <cmath>

namespace xo {

Synth::Synth(float sampleRate, int voices, const std::vector<Wavetable>& tables)
    : sr_(sampleRate), numVoices_(std::clamp(voices, 1, kMaxVoices)), tables_(tables) {
    clearCcMap();
    for (int i = 0; i < P_COUNT; ++i) target_[i] = paramClamp(i, kParams[i].def, numTables());
    smooth_ = target_;
    for (int u = 0; u < kMaxUnison; ++u) {
        for (int i = 0; i <= kMaxUnison; ++i) panL_[i][u] = panR_[i][u] = 0.7071f;
    }
}

void Synth::clearCcMap() { std::fill(std::begin(ccMap_), std::end(ccMap_), -1); }

void Synth::setCcMapping(int cc, int paramId) {
    if (cc >= 0 && cc < 128) ccMap_[cc] = paramId;
}

void Synth::setParam(int id, float value) {
    if (id < 0 || id >= P_COUNT) return;
    target_[id] = paramClamp(id, value, numTables());
    if (status_) {
        status_->param[id].store(target_[id], std::memory_order_relaxed);
        status_->paramSerial.fetch_add(1, std::memory_order_release);
    }
}

void Synth::setStatus(EngineStatus* status) {
    status_ = status;
    if (status_)
        for (int i = 0; i < P_COUNT; ++i) status_->param[i].store(target_[i], std::memory_order_relaxed);
}

void Synth::loadPreset(const float* values, int index) {
    for (int i = 0; i < P_COUNT; ++i) setParam(i, values[i]);
    if (status_ && index >= 0) status_->preset.store(index, std::memory_order_relaxed);
}

int Synth::activeVoices() const {
    int n = 0;
    for (int i = 0; i < numVoices_; ++i) n += voices_[i].amp.idle() ? 0 : 1;
    return n;
}

// ---------------------------------------------------------------------------
// MIDI

void Synth::startVoice(Voice& v, int note, int velocity, bool fresh) {
    if (fresh) {
        v.filtL.reset();
        v.filtR.reset();
        v.amp.kill();
        v.mod.kill();
        int uniA = static_cast<int>(target_[P_A_UNISON]);
        int uniB = static_cast<int>(target_[P_B_UNISON]);
        for (int u = 0; u < kMaxUnison; ++u) {
            v.phaseA[u] = (uniA == 1 && u == 0) ? 0.0f : rng_.uni();
            v.phaseB[u] = (uniB == 1 && u == 0) ? 0.0f : rng_.uni();
        }
    }
    if (fresh || v.note != note) v.pitch = (target_[P_GLIDE] > 0.0005f) ? lastPitch_ : static_cast<float>(note);
    v.note = note;
    v.vel = velocity / 127.0f;
    v.gate = true;
    v.held = false;
    v.age = ++ageCounter_;
    v.amp.gateOn();
    v.mod.gateOn();
    lastPitch_ = static_cast<float>(note);
}

void Synth::noteOn(int note, int velocity) {
    if (velocity <= 0) { noteOff(note); return; }
    note = std::clamp(note, 0, 127);

    Voice* pick = nullptr;
    bool fresh = false;
    for (int i = 0; i < numVoices_ && !pick; ++i)  // same note already sounding
        if (!voices_[i].amp.idle() && voices_[i].note == note) pick = &voices_[i];
    for (int i = 0; i < numVoices_ && !pick; ++i)  // free voice
        if (voices_[i].amp.idle()) { pick = &voices_[i]; fresh = true; }
    if (!pick) {  // quietest released voice
        float best = 2.0f;
        for (int i = 0; i < numVoices_; ++i) {
            Voice& v = voices_[i];
            if (!v.gate && v.amp.v < best) { best = v.amp.v; pick = &v; }
        }
    }
    if (!pick) {  // oldest
        pick = &voices_[0];
        for (int i = 1; i < numVoices_; ++i)
            if (voices_[i].age < pick->age) pick = &voices_[i];
    }
    startVoice(*pick, note, velocity, fresh);
}

void Synth::noteOff(int note) {
    for (int i = 0; i < numVoices_; ++i) {
        Voice& v = voices_[i];
        if (v.note == note && v.gate) {
            v.gate = false;
            if (sustain_) v.held = true;
            else { v.amp.gateOff(); v.mod.gateOff(); }
        }
    }
}

void Synth::allNotesOff(bool hard) {
    sustain_ = false;
    for (int i = 0; i < kMaxVoices; ++i) {
        Voice& v = voices_[i];
        v.gate = false;
        v.held = false;
        if (hard) { v.amp.kill(); v.mod.kill(); v.note = -1; }
        else { v.amp.gateOff(); v.mod.gateOff(); }
    }
}

void Synth::pitchBend(float amount) { bend_ = std::clamp(amount, -1.0f, 1.0f); }

void Synth::programChange(int program) {
    if (!bank_ || bank_->count() == 0) return;
    int idx = program % bank_->count();
    float v[P_COUNT];
    bank_->read(idx, v);
    loadPreset(v, idx);
}

void Synth::controlChange(int cc, int value) {
    if (cc < 0 || cc > 127) return;
    switch (cc) {
        case 1: modWheel_ = value / 127.0f; break;
        case 7:  // channel volume -> master volume
            if (status_) status_->master.store(value / 127.0f, std::memory_order_relaxed);
            return;
        case 64: {
            bool on = value >= 64;
            if (sustain_ && !on) {
                for (int i = 0; i < numVoices_; ++i) {
                    Voice& v = voices_[i];
                    if (v.held) { v.held = false; v.amp.gateOff(); v.mod.gateOff(); }
                }
            }
            sustain_ = on;
            break;
        }
        case 120: allNotesOff(true); return;
        case 121: bend_ = 0; modWheel_ = 0; return;
        case 123: allNotesOff(false); return;
        default: break;
    }
    if (status_ && cc != 64) {  // MIDI learn: bind the next moved controller
        int learn = status_->learnParam.load(std::memory_order_relaxed);
        if (learn >= 0 && learn < P_COUNT) {
            ccMap_[cc] = learn;
            status_->learnParam.store(-1, std::memory_order_relaxed);
            status_->learnedCc.store(cc, std::memory_order_release);
        }
    }
    int id = ccMap_[cc];
    if (id >= 0) setParam(id, paramFromNormalized(id, value / 127.0f, numTables()));
}

// ---------------------------------------------------------------------------
// Audio

void Synth::render(float* left, float* right, int n) {
    livePos_[0] = livePos_[1] = -1.0f;
    int off = 0;
    while (off < n) {
        int m = std::min(kBlock, n - off);
        renderBlock(left + off, right + off, m);
        off += m;
    }
    if (status_) {
        float pl = 0, pr = 0;
        for (int i = 0; i < n; ++i) { pl = std::max(pl, std::fabs(left[i])); pr = std::max(pr, std::fabs(right[i])); }
        if (pl > status_->peakL.load(std::memory_order_relaxed)) status_->peakL.store(pl, std::memory_order_relaxed);
        if (pr > status_->peakR.load(std::memory_order_relaxed)) status_->peakR.store(pr, std::memory_order_relaxed);
        status_->voices.store(activeVoices(), std::memory_order_relaxed);
        status_->lfo.store(lfoValue_, std::memory_order_relaxed);
        status_->livePos[0].store(livePos_[0] >= 0 ? livePos_[0] : target_[P_A_POS], std::memory_order_relaxed);
        status_->livePos[1].store(livePos_[1] >= 0 ? livePos_[1] : target_[P_B_POS], std::memory_order_relaxed);
    }
}

void Synth::renderBlock(float* left, float* right, int n) {
    // Parameter smoothing (control rate, ~5 ms).
    for (int i = 0; i < P_COUNT; ++i) {
        if (kParams[i].integer) smooth_[i] = target_[i];
        else smooth_[i] += (target_[i] - smooth_[i]) * 0.15f;
    }
    const float ctrlRate = sr_ / kBlock;
    ampCoefs_.set(smooth_[P_AMP_A], smooth_[P_AMP_D], smooth_[P_AMP_S], smooth_[P_AMP_R], sr_);
    modCoefs_.set(smooth_[P_MOD_A], smooth_[P_MOD_D], smooth_[P_MOD_S], smooth_[P_MOD_R], ctrlRate);
    float g = target_[P_GLIDE];
    glideCoef_ = g > 0.0005f ? std::exp(-4.6f / (g * ctrlRate)) : 0.0f;

    // Global LFO.
    lfoPhase_ += smooth_[P_LFO_RATE] * n / sr_;
    if (lfoPhase_ >= 1.0f) { lfoPhase_ -= std::floor(lfoPhase_); lfoSH_ = rng_.bi(); }
    switch (static_cast<int>(target_[P_LFO_SHAPE])) {
        case 0: lfoValue_ = std::sin(2.0f * kPi * lfoPhase_); break;
        case 1: lfoValue_ = 1.0f - 4.0f * std::fabs(lfoPhase_ - 0.5f); break;
        case 2: lfoValue_ = 1.0f - 2.0f * lfoPhase_; break;
        case 3: lfoValue_ = lfoPhase_ < 0.5f ? 1.0f : -1.0f; break;
        default: lfoValue_ = lfoSH_; break;
    }

    // Equal-power pan positions for each unison count.
    float spread = smooth_[P_SPREAD];
    for (int U = 1; U <= kMaxUnison; ++U) {
        for (int u = 0; u < U; ++u) {
            float p = (U == 1) ? 0.0f : spread * (2.0f * u / (U - 1) - 1.0f);
            float a = (p + 1.0f) * (kPi / 4.0f);
            panL_[U][u] = std::cos(a);
            panR_[U][u] = std::sin(a);
        }
    }

    float accL[kBlock] = {}, accR[kBlock] = {};
    for (int i = 0; i < numVoices_; ++i)
        if (!voices_[i].amp.idle()) renderVoice(voices_[i], accL, accR, n);

    // Master volume: fader position squared (roughly how loudness feels), smoothed so moves don't zipper.
    float fader = status_ ? clampf(status_->master.load(std::memory_order_relaxed), 0.0f, 1.0f) : 0.8f;
    masterSmooth_ += (fader * fader - masterSmooth_) * 0.15f;
    const float gain = smooth_[P_VOLUME] * 0.4f * masterSmooth_ / 0.64f;  // 0.8 = unity
    for (int i = 0; i < n; ++i) {
        left[i] = softClip(accL[i] * gain);
        right[i] = softClip(accR[i] * gain);
    }
}

void Synth::renderVoice(Voice& v, float* outL, float* outR, int n) {
    const float notePitch = static_cast<float>(v.note);
    v.pitch = glideCoef_ > 0.0f ? notePitch + (v.pitch - notePitch) * glideCoef_ : notePitch;

    const float modEnv = v.mod.next(modCoefs_);  // control rate
    const float vib = lfoValue_ * (smooth_[P_LFO_PITCH] + modWheel_ * 0.5f);
    const float basePitch = v.pitch + bend_ * target_[P_BEND_RANGE] + vib;
    const float posMod = modEnv * smooth_[P_MENV_POS] + lfoValue_ * smooth_[P_LFO_POS] * 0.5f;
    const int last = numTables() - 1;

    float bufL[kBlock] = {}, bufR[kBlock] = {};
    if (smooth_[P_A_LEVEL] > 1e-4f) {
        const Wavetable& wt = tables_[std::min(static_cast<int>(target_[P_A_TABLE]), last)];
        livePos_[0] = clampf(smooth_[P_A_POS] + posMod, 0.0f, 1.0f);
        renderOsc(wt, livePos_[0],
                  basePitch + target_[P_A_SEMI] + smooth_[P_A_FINE] * 0.01f, smooth_[P_A_LEVEL],
                  static_cast<int>(target_[P_A_UNISON]), smooth_[P_A_DETUNE], v.phaseA, bufL, bufR, n);
    }
    if (smooth_[P_B_LEVEL] > 1e-4f) {
        const Wavetable& wt = tables_[std::min(static_cast<int>(target_[P_B_TABLE]), last)];
        livePos_[1] = clampf(smooth_[P_B_POS] + posMod, 0.0f, 1.0f);
        renderOsc(wt, livePos_[1],
                  basePitch + target_[P_B_SEMI] + smooth_[P_B_FINE] * 0.01f, smooth_[P_B_LEVEL],
                  static_cast<int>(target_[P_B_UNISON]), smooth_[P_B_DETUNE], v.phaseB, bufL, bufR, n);
    }

    // Filter.
    const float velF = (1.0f - smooth_[P_VEL_FILTER]) + smooth_[P_VEL_FILTER] * v.vel;
    const float octaves = smooth_[P_FENV] * 6.0f * modEnv * velF +
                          smooth_[P_FKEY] * (v.pitch - 60.0f) * (1.0f / 12.0f) +
                          lfoValue_ * smooth_[P_LFO_CUTOFF] * 3.0f;
    SvfCoefs c;
    c.set(smooth_[P_CUTOFF] * std::exp2(octaves), smooth_[P_RESO], sr_);
    const int mode = static_cast<int>(target_[P_FILTER_MODE]);
    const float drive = smooth_[P_DRIVE];
    const float driveGain = 1.0f + drive * 6.0f;
    const float velA = (1.0f - smooth_[P_VEL_AMP]) + smooth_[P_VEL_AMP] * v.vel;

    for (int i = 0; i < n; ++i) {
        float l = bufL[i], r = bufR[i];
        if (drive > 0.001f) { l = softClip(l * driveGain); r = softClip(r * driveGain); }
        l = v.filtL.process(l, c, mode);
        r = v.filtR.process(r, c, mode);
        float a = v.amp.next(ampCoefs_) * velA;
        outL[i] += l * a;
        outR[i] += r * a;
    }

    if (v.amp.idle()) {
        v.note = -1;
        v.gate = v.held = false;
        v.mod.kill();
    }
}

void Synth::renderOsc(const Wavetable& wt, float pos, float pitch, float level, int unison,
                      float detune, float* phases, float* bufL, float* bufR, int n) {
    const int frames = wt.numFrames();
    const float fpos = pos * (frames - 1);
    int f0 = static_cast<int>(fpos);
    int f1;
    float ff;
    if (f0 >= frames - 1) { f0 = f1 = frames - 1; ff = 0.0f; }
    else { f1 = f0 + 1; ff = fpos - f0; }

    unison = std::clamp(unison, 1, kMaxUnison);
    const float baseInc = midiToHz(pitch) / sr_;
    const float g = level / std::sqrt(static_cast<float>(unison));

    for (int u = 0; u < unison; ++u) {
        float cents = (unison == 1) ? 0.0f : detune * (2.0f * u / (unison - 1) - 1.0f);
        float inc = std::min(baseInc * std::exp2(cents * (1.0f / 1200.0f)), 0.49f);
        int mip = mipForIncrement(inc);
        const float* t0 = wt.table(f0, mip);
        const float* t1 = wt.table(f1, mip);
        const float gl = g * panL_[unison][u], gr = g * panR_[unison][u];
        float ph = phases[u];
        for (int i = 0; i < n; ++i) {
            float x = ph * Wavetable::kSize;
            int i0 = static_cast<int>(x);
            float fr = x - i0;
            float a = t0[i0] + fr * (t0[i0 + 1] - t0[i0]);
            float b = t1[i0] + fr * (t1[i0 + 1] - t1[i0]);
            float s = a + ff * (b - a);
            bufL[i] += s * gl;
            bufR[i] += s * gr;
            ph += inc;
            if (ph >= 1.0f) ph -= 1.0f;
        }
        phases[u] = ph;
    }
}

}  // namespace xo
