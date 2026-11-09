#include "synth.h"

#include <algorithm>
#include <cmath>

namespace xo {

Synth::Synth(float sampleRate, int voices, const std::vector<Wavetable>& tables)
    : sr_(sampleRate), numVoices_(std::clamp(voices, 1, kMaxVoices)), tables_(tables) {
    clearCcMap();
    fx_.init(sampleRate);
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
        v.ladL.reset();
        v.ladR.reset();
        v.combL.reset();
        v.combR.reset();
        v.subPhase = 0;
        v.amp.kill();
        v.mod.kill();
        int uniA = static_cast<int>(target_[P_A_UNISON]);
        int uniB = static_cast<int>(target_[P_B_UNISON]);
        for (int u = 0; u < kMaxUnison; ++u) {
            v.phaseA[u] = (uniA == 1 && u == 0) ? 0u : rng_.next();
            v.phaseB[u] = (uniB == 1 && u == 0) ? 0u : rng_.next();
        }
    }
    if (fresh || v.note != note) v.pitch = (target_[P_GLIDE] > 0.0005f) ? lastPitch_ : static_cast<float>(note);
    v.note = note;
    v.vel = velocity / 127.0f;
    v.rnd = rng_.bi();
    v.gate = true;
    v.held = false;
    v.age = ++ageCounter_;
    v.amp.gateOn();
    v.mod.gateOn();
    lastPitch_ = static_cast<float>(note);
}

void Synth::voiceOn(int note, int velocity) {
    if (velocity <= 0) { voiceOff(note); return; }
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

void Synth::voiceOff(int note) {
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
    arpN_ = 0;
    arpCurrent_ = -1;
    gateOff_ = -1;
    keysDown_ = 0;
    std::fill(std::begin(keyCount_), std::end(keyCount_), 0);
    for (int i = 0; i < kMaxVoices; ++i) {
        Voice& v = voices_[i];
        v.gate = false;
        v.held = false;
        if (hard) { v.amp.kill(); v.mod.kill(); v.note = -1; }
        else { v.amp.gateOff(); v.mod.gateOff(); }
    }
}

// ---------------------------------------------------------------------------
// Keys: chord memory, then the arpeggiator (or straight to the voices).

static const int kChordNotes[9][4] = {
    {0, -1, -1, -1}, {0, 4, 7, -1}, {0, 3, 7, -1}, {0, 4, 7, 10}, {0, 4, 7, 11},
    {0, 3, 7, 10},   {0, 5, 7, -1}, {0, 7, -1, -1}, {0, 12, -1, -1},
};

void Synth::noteOn(int note, int velocity) {
    if (velocity <= 0) { noteOff(note); return; }
    note = std::clamp(note, 0, 127);
    const int chord = std::clamp(static_cast<int>(target_[P_CHORD]), 0, 8);
    const bool arp = target_[P_ARP_MODE] >= 0.5f;
    if (arp && target_[P_ARP_HOLD] >= 0.5f && keysDown_ == 0) arpN_ = 0;  // held notes: a new chord replaces them
    if (keyCount_[note] == 0) ++keysDown_;
    keyCount_[note] = 0;
    for (int k = 0; k < 4 && kChordNotes[chord][k] >= 0; ++k) {
        int n = note + kChordNotes[chord][k];
        if (n > 127) continue;
        keyNotes_[note][keyCount_[note]++] = static_cast<int8_t>(n);
        if (arp) arpAdd(n, velocity);
        else voiceOn(n, velocity);
    }
}

void Synth::noteOff(int note) {
    note = std::clamp(note, 0, 127);
    if (keyCount_[note] == 0) { voiceOff(note); return; }
    const bool arp = target_[P_ARP_MODE] >= 0.5f;
    const bool hold = target_[P_ARP_HOLD] >= 0.5f;
    for (int k = 0; k < keyCount_[note]; ++k) {
        int n = keyNotes_[note][k];
        voiceOff(n);                 // (a note started before the arpeggiator went on)
        if (arp && !hold) arpRemove(n);
    }
    keyCount_[note] = 0;
    if (keysDown_ > 0) --keysDown_;
}

void Synth::arpAdd(int note, int vel) {
    for (int i = 0; i < arpN_; ++i)
        if (arpNotes_[i] == note) { arpVels_[i] = vel; return; }
    if (arpN_ < 16) { arpNotes_[arpN_] = note; arpVels_[arpN_] = vel; ++arpN_; }
}

void Synth::arpRemove(int note) {
    for (int i = 0; i < arpN_; ++i) {
        if (arpNotes_[i] != note) continue;
        for (int j = i; j < arpN_ - 1; ++j) { arpNotes_[j] = arpNotes_[j + 1]; arpVels_[j] = arpVels_[j + 1]; }
        --arpN_;
        return;
    }
}

bool Synth::pickArpNote(int mode, int& note, int& vel) {
    if (arpN_ == 0) return false;
    int base[16], bvel[16];
    for (int i = 0; i < arpN_; ++i) { base[i] = arpNotes_[i]; bvel[i] = arpVels_[i]; }
    if (mode != 5) {  // sorted low to high (5: as played)
        for (int i = 1; i < arpN_; ++i)
            for (int j = i; j > 0 && base[j - 1] > base[j]; --j) { std::swap(base[j - 1], base[j]); std::swap(bvel[j - 1], bvel[j]); }
    }
    const int oct = std::clamp(static_cast<int>(target_[P_ARP_OCT]), 1, 4);
    const int len = arpN_ * oct;
    int idx;
    switch (mode) {
        case 2: idx = len - 1 - (arpStep_ % len); break;                      // down
        case 3: {                                                             // up/down (ends once)
            int cyc = len > 1 ? 2 * len - 2 : 1;
            int k = arpStep_ % cyc;
            idx = k < len ? k : cyc - k;
            break;
        }
        case 4: idx = static_cast<int>(rng_.uni() * len) % len; break;        // random
        default: idx = arpStep_ % len; break;                                  // up / as played
    }
    ++arpStep_;
    note = std::min(127, base[idx % arpN_] + 12 * (idx / arpN_));
    vel = bvel[idx % arpN_];
    return true;
}

void Synth::runArp() {
    const int mode = static_cast<int>(target_[P_ARP_MODE]);
    if (mode <= 0) {
        if (arpCurrent_ >= 0) { voiceOff(arpCurrent_); arpCurrent_ = -1; }
        arpWasOn_ = false;
        arpN_ = 0;
        return;
    }
    const double step = kDivisions[std::clamp(static_cast<int>(target_[P_ARP_RATE]), 1, kNumDivisions - 1)].beats;
    if (!arpWasOn_ || beats_ - nextStep_ > step * 4 || nextStep_ - beats_ > step * 4) {
        nextStep_ = std::ceil(beats_ / step - 1e-9) * step;   // line up with the grid
        arpWasOn_ = true;
    }
    if (gateOff_ >= 0 && beats_ >= gateOff_) {
        if (arpCurrent_ >= 0) voiceOff(arpCurrent_);
        arpCurrent_ = -1;
        gateOff_ = -1;
    }
    if (beats_ < nextStep_) return;
    const double start = nextStep_;
    nextStep_ += step;
    if (arpCurrent_ >= 0) { voiceOff(arpCurrent_); arpCurrent_ = -1; }
    int note, vel;
    if (!pickArpNote(mode, note, vel)) { arpStep_ = 0; return; }
    voiceOn(note, vel);
    arpCurrent_ = note;
    gateOff_ = start + step * clampf(target_[P_ARP_GATE], 0.05f, 1.0f) * 0.98;
}

// ---------------------------------------------------------------------------
// Clock

void Synth::midiClock(double senderTime) {
    const uint64_t t = samplePos_;
    // Time each tick on the sender's clock when we have it (precise); else by arrival
    // here, which is only as fine as one audio period, so average over more ticks.
    double when;
    if (senderTime >= 0) {
        if (lastSender_ >= 0 && senderTime < lastSender_ - 1000.0) nTicks_ = 0;   // the sender's clock wrapped
        lastSender_ = senderTime;
        when = senderTime;
    } else {
        when = static_cast<double>(t) / sr_;
    }
    tickTimes_[tickW_] = when;
    tickW_ = (tickW_ + 1) % kTickWin;
    if (nTicks_ < kTickWin) ++nTicks_;
    if (extClock_) tickBeat_ += 1.0 / 24.0;
    if (nTicks_ >= 7) {
        double oldest = tickTimes_[(tickW_ - nTicks_ + 2 * kTickWin) % kTickWin];
        double interval = (when - oldest) / (nTicks_ - 1);
        if (interval > 0) {
            float raw = clampf(static_cast<float>(60.0 / (interval * 24.0)), 20.0f, 300.0f);
            extBpm_ = extClock_ ? extBpm_ + (raw - extBpm_) * 0.1f : raw;
            if (std::fabs(extBpm_ - extBpmShown_) > 0.2f) extBpmShown_ = std::round(extBpm_ * 10.0f) / 10.0f;
        }
        if (!extClock_) {         // start following: carry on from where we are
            extClock_ = true;
            tickBeat_ = beats_;
            extBpmShown_ = std::round(extBpm_ * 10.0f) / 10.0f;
        }
    }
    lastTickSample_ = t;
}

void Synth::midiStart() {
    beats_ = 0;
    tickBeat_ = -1.0 / 24.0;     // the next tick is the downbeat
    arpWasOn_ = false;
    arpStep_ = 0;
}

void Synth::advanceClock(int n) {
    if (extClock_ && samplePos_ - lastTickSample_ > static_cast<uint64_t>(sr_ * 0.5f)) {
        extClock_ = false;        // clock stopped arriving: back to the internal tempo
        nTicks_ = 0;
        lastSender_ = -1;
    }
    if (extClock_) {
        bpm_ = extBpmShown_;
        double proj = tickBeat_ + static_cast<double>(samplePos_ - lastTickSample_) * bpm_ / 60.0 / sr_;
        beats_ = std::max(beats_, std::min(proj, tickBeat_ + 1.0 / 24.0));
    } else {
        bpm_ = clampf(status_ ? status_->tempo.load(std::memory_order_relaxed) : 120.0f, 20.0f, 300.0f);
        beats_ += static_cast<double>(n) * bpm_ / 60.0 / sr_;
    }
    samplePos_ += static_cast<uint64_t>(n);
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
        status_->bpmNow.store(bpm_, std::memory_order_relaxed);
        status_->extClock.store(extClock_, std::memory_order_relaxed);
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

    advanceClock(n);
    runArp();

    // Global LFO: free (Hz) or locked to a note division of the tempo.
    const int lfoSync = static_cast<int>(target_[P_LFO_SYNC]);
    if (lfoSync > 0 && lfoSync < kNumDivisions) {
        double div = kDivisions[lfoSync].beats;
        float ph = static_cast<float>(beats_ / div - std::floor(beats_ / div));
        if (ph < lfoPhase_) lfoSH_ = rng_.bi();
        lfoPhase_ = ph;
    } else {
        lfoPhase_ += smooth_[P_LFO_RATE] * n / sr_;
        if (lfoPhase_ >= 1.0f) { lfoPhase_ -= std::floor(lfoPhase_); lfoSH_ = rng_.bi(); }
    }
    switch (static_cast<int>(target_[P_LFO_SHAPE])) {
        case 0: lfoValue_ = std::sin(2.0f * kPi * lfoPhase_); break;
        case 1: lfoValue_ = 1.0f - 4.0f * std::fabs(lfoPhase_ - 0.5f); break;
        case 2: lfoValue_ = 1.0f - 2.0f * lfoPhase_; break;
        case 3: lfoValue_ = lfoPhase_ < 0.5f ? 1.0f : -1.0f; break;
        default: lfoValue_ = lfoSH_; break;
    }

    // LFO 2 (a mod matrix source).
    {
        const int sync2 = static_cast<int>(target_[P_LFO2_SYNC]);
        if (sync2 > 0 && sync2 < kNumDivisions) {
            double div = kDivisions[sync2].beats;
            float ph = static_cast<float>(beats_ / div - std::floor(beats_ / div));
            if (ph < lfo2Phase_) lfo2SH_ = rng_.bi();
            lfo2Phase_ = ph;
        } else {
            lfo2Phase_ += smooth_[P_LFO2_RATE] * n / sr_;
            if (lfo2Phase_ >= 1.0f) { lfo2Phase_ -= std::floor(lfo2Phase_); lfo2SH_ = rng_.bi(); }
        }
        switch (static_cast<int>(target_[P_LFO2_SHAPE])) {
            case 0: lfo2Value_ = std::sin(2.0f * kPi * lfo2Phase_); break;
            case 1: lfo2Value_ = 1.0f - 4.0f * std::fabs(lfo2Phase_ - 0.5f); break;
            case 2: lfo2Value_ = 1.0f - 2.0f * lfo2Phase_; break;
            case 3: lfo2Value_ = lfo2Phase_ < 0.5f ? 1.0f : -1.0f; break;
            default: lfo2Value_ = lfo2SH_; break;
        }
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
    const float patch = smooth_[P_VOLUME] * 0.4f;
    for (int i = 0; i < n; ++i) { accL[i] *= patch; accR[i] *= patch; }
    fx_.process(accL, accR, n, smooth_.data(), bpm_);   // drive -> chorus -> delay -> reverb
    const float gain = masterSmooth_ / 0.64f;           // 0.8 = unity
    for (int i = 0; i < n; ++i) {
        left[i] = softClip(accL[i] * gain);
        right[i] = softClip(accR[i] * gain);
    }
}

void Synth::renderVoice(Voice& v, float* outL, float* outR, int n) {
    const float notePitch = static_cast<float>(v.note);
    v.pitch = glideCoef_ > 0.0f ? notePitch + (v.pitch - notePitch) * glideCoef_ : notePitch;

    const float modEnv = v.mod.next(modCoefs_);  // control rate

    // Mod matrix: each slot adds source x amount to a destination.
    float mDst[kModDests] = {};
    for (int k = 0; k < kModSlots; ++k) {
        const int src = static_cast<int>(target_[P_M1_SRC + 3 * k]);
        const int dst = static_cast<int>(target_[P_M1_DST + 3 * k]);
        const float amt = smooth_[P_M1_AMT + 3 * k];
        if (src <= 0 || dst <= 0 || dst >= kModDests || std::fabs(amt) < 1e-4f) continue;
        float sv;
        switch (src) {
            case 1: sv = lfoValue_; break;
            case 2: sv = lfo2Value_; break;
            case 3: sv = modEnv; break;
            case 4: sv = v.amp.v; break;
            case 5: sv = v.vel; break;
            case 6: sv = modWheel_; break;
            case 7: sv = aftertouch_; break;
            case 8: sv = clampf((v.pitch - 60.0f) / 36.0f, -1.0f, 1.0f); break;
            default: sv = v.rnd; break;
        }
        mDst[dst] += sv * amt;
    }

    const float vib = lfoValue_ * (smooth_[P_LFO_PITCH] + modWheel_ * 0.5f);
    const float basePitch = v.pitch + bend_ * target_[P_BEND_RANGE] + vib + mDst[5] * 12.0f;   // PITCH: +/-1 octave
    const float posMod = modEnv * smooth_[P_MENV_POS] + lfoValue_ * smooth_[P_LFO_POS] * 0.5f;
    const int last = numTables() - 1;
    const float levelA = clampf(smooth_[P_A_LEVEL] + mDst[6], 0.0f, 1.0f);
    const float levelB = clampf(smooth_[P_B_LEVEL] + mDst[7], 0.0f, 1.0f);

    float bufL[kBlock] = {}, bufR[kBlock] = {};
    if (levelA > 1e-4f) {
        const Wavetable& wt = tables_[std::min(static_cast<int>(target_[P_A_TABLE]), last)];
        livePos_[0] = clampf(smooth_[P_A_POS] + posMod + mDst[1], 0.0f, 1.0f);
        renderOsc(wt, livePos_[0],
                  basePitch + target_[P_A_SEMI] + smooth_[P_A_FINE] * 0.01f, levelA,
                  static_cast<int>(target_[P_A_UNISON]), smooth_[P_A_DETUNE], v.phaseA, bufL, bufR, n);
    }
    if (levelB > 1e-4f) {
        const Wavetable& wt = tables_[std::min(static_cast<int>(target_[P_B_TABLE]), last)];
        livePos_[1] = clampf(smooth_[P_B_POS] + posMod + mDst[2], 0.0f, 1.0f);
        renderOsc(wt, livePos_[1],
                  basePitch + target_[P_B_SEMI] + smooth_[P_B_FINE] * 0.01f, levelB,
                  static_cast<int>(target_[P_B_UNISON]), smooth_[P_B_DETUNE], v.phaseB, bufL, bufR, n);
    }

    // Sub oscillator (follows the played pitch) and noise, into the filter with the oscillators.
    const float subLevel = smooth_[P_SUB_LEVEL], noise = smooth_[P_NOISE];
    if (subLevel > 1e-4f) {
        const float inc = std::min(midiToHz(basePitch - 12.0f * target_[P_SUB_OCT]) / sr_, 0.45f);
        const bool square = target_[P_SUB_SHAPE] >= 0.5f;
        const float g = subLevel * 0.7f;
        auto blep = [inc](float t) {
            if (t < inc) { t /= inc; return t + t - t * t - 1.0f; }
            if (t > 1.0f - inc) { t = (t - 1.0f) / inc; return t * t + t + t + 1.0f; }
            return 0.0f;
        };
        for (int i = 0; i < n; ++i) {
            float ph = v.subPhase, x;
            if (square) {
                float t2 = ph + 0.5f;
                if (t2 >= 1.0f) t2 -= 1.0f;
                x = (ph < 0.5f ? 1.0f : -1.0f) + blep(ph) - blep(t2);
            } else {
                x = std::sin(2.0f * kPi * ph);
            }
            bufL[i] += x * g;
            bufR[i] += x * g;
            ph += inc;
            v.subPhase = ph >= 1.0f ? ph - 1.0f : ph;
        }
    }
    if (noise > 1e-4f) {
        for (int i = 0; i < n; ++i) { bufL[i] += rng_.bi() * noise * 0.5f; bufR[i] += rng_.bi() * noise * 0.5f; }
    }

    // Filter.
    const float velF = (1.0f - smooth_[P_VEL_FILTER]) + smooth_[P_VEL_FILTER] * v.vel;
    const float octaves = smooth_[P_FENV] * 6.0f * modEnv * velF +
                          smooth_[P_FKEY] * (v.pitch - 60.0f) * (1.0f / 12.0f) +
                          lfoValue_ * smooth_[P_LFO_CUTOFF] * 3.0f + mDst[3] * 6.0f;   // CUTOFF: +/-6 octaves
    const float cutoffHz = smooth_[P_CUTOFF] * std::exp2(octaves);
    const float reso = clampf(smooth_[P_RESO] + mDst[4], 0.0f, 1.0f);
    const int mode = static_cast<int>(target_[P_FILTER_MODE]);
    SvfCoefs c;
    LadderCoefs lc;
    float combDelay = 0, combFb = 0;
    if (mode == 3) lc.set(cutoffHz, reso, sr_);
    else if (mode == 5) {
        combDelay = clampf(sr_ / clampf(cutoffHz, 25.0f, sr_ * 0.45f), 2.0f, Comb::kLen - 4.0f);
        combFb = 0.5f + 0.48f * reso;
    } else c.set(cutoffHz, reso, sr_);
    const int svfMode = mode == 4 ? 3 : mode;   // NOTCH
    const float drive = clampf(smooth_[P_DRIVE] + mDst[8], 0.0f, 1.0f);
    const float driveGain = 1.0f + drive * 6.0f;
    const float velA = (1.0f - smooth_[P_VEL_AMP]) + smooth_[P_VEL_AMP] * v.vel;

    for (int i = 0; i < n; ++i) {
        float l = bufL[i], r = bufR[i];
        if (drive > 0.001f) { l = softClip(l * driveGain); r = softClip(r * driveGain); }
        if (mode == 3) { l = v.ladL.process(l, lc); r = v.ladR.process(r, lc); }
        else if (mode == 5) { l = v.combL.process(l, combDelay, combFb); r = v.combR.process(r, combDelay, combFb); }
        else { l = v.filtL.process(l, c, svfMode); r = v.filtR.process(r, c, svfMode); }
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
                      float detune, uint32_t* phases, float* bufL, float* bufR, int n) {
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
        // Fixed-point phase: the top 11 bits index the 2048-sample table, the
        // next 21 are the fraction; wrapping is free (unsigned overflow).
        constexpr int kFracBits = 32 - 11;
        constexpr float kFracScale = 1.0f / static_cast<float>(1u << kFracBits);
        const uint32_t dph = static_cast<uint32_t>(inc * 4294967296.0);
        uint32_t ph = phases[u];
        if (f0 == f1 || ff < 1e-4f) {   // on a single frame: half the reads
            for (int i = 0; i < n; ++i) {
                const uint32_t i0 = ph >> kFracBits;
                const float fr = static_cast<float>(ph & ((1u << kFracBits) - 1)) * kFracScale;
                const float s = t0[i0] + fr * (t0[i0 + 1] - t0[i0]);
                bufL[i] += s * gl;
                bufR[i] += s * gr;
                ph += dph;
            }
        } else {
            for (int i = 0; i < n; ++i) {
                const uint32_t i0 = ph >> kFracBits;
                const float fr = static_cast<float>(ph & ((1u << kFracBits) - 1)) * kFracScale;
                const float a = t0[i0] + fr * (t0[i0 + 1] - t0[i0]);
                const float b = t1[i0] + fr * (t1[i0 + 1] - t1[i0]);
                const float s = a + ff * (b - a);
                bufL[i] += s * gl;
                bufR[i] += s * gr;
                ph += dph;
            }
        }
        phases[u] = ph;
    }
}

}  // namespace xo
