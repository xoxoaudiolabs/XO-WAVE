// Synth parameter definitions. Every parameter has a text name used by
// presets and by the MIDI CC map in the config file.
#pragma once
#include <string>

namespace xo {

enum ParamId {
    P_A_TABLE, P_A_POS, P_A_LEVEL, P_A_SEMI, P_A_FINE, P_A_UNISON, P_A_DETUNE,
    P_B_TABLE, P_B_POS, P_B_LEVEL, P_B_SEMI, P_B_FINE, P_B_UNISON, P_B_DETUNE,
    P_SPREAD,
    P_FILTER_MODE, P_CUTOFF, P_RESO, P_FENV, P_FKEY, P_DRIVE,
    P_AMP_A, P_AMP_D, P_AMP_S, P_AMP_R,
    P_MOD_A, P_MOD_D, P_MOD_S, P_MOD_R,
    P_MENV_POS,
    P_LFO_RATE, P_LFO_SHAPE, P_LFO_POS, P_LFO_CUTOFF, P_LFO_PITCH,
    P_VEL_AMP, P_VEL_FILTER,
    P_GLIDE, P_BEND_RANGE, P_VOLUME,
    // FX bus
    P_FX_DRIVE,
    P_CHORUS_MIX, P_CHORUS_RATE, P_CHORUS_DEPTH,
    P_DELAY_MIX, P_DELAY_TIME, P_DELAY_SYNC, P_DELAY_FB, P_DELAY_TONE, P_DELAY_PP,
    P_REVERB_MIX, P_REVERB_SIZE, P_REVERB_DAMP,
    // Tempo sync, arpeggiator, chord memory
    P_LFO_SYNC,
    P_ARP_MODE, P_ARP_RATE, P_ARP_OCT, P_ARP_GATE, P_ARP_HOLD,
    P_CHORD,
    // LFO 2 + mod matrix (4 slots: source, destination, amount; contiguous)
    P_LFO2_RATE, P_LFO2_SHAPE, P_LFO2_SYNC,
    P_M1_SRC, P_M1_DST, P_M1_AMT,
    P_M2_SRC, P_M2_DST, P_M2_AMT,
    P_M3_SRC, P_M3_DST, P_M3_AMT,
    P_M4_SRC, P_M4_DST, P_M4_AMT,
    P_COUNT
};

enum class Curve { Linear, Exp };

struct ParamInfo {
    const char* name;
    float min, max, def;
    Curve curve;
    bool integer;
    const char* help;
};

extern const ParamInfo kParams[P_COUNT];

int findParam(const std::string& name);                // -1 if unknown
float paramFromNormalized(int id, float n, int numTables);
float paramClamp(int id, float v, int numTables);
constexpr int kModSlots = 4;
constexpr int kModSources = 10;   // OFF, LFO 1, LFO 2, MOD ENV, AMP ENV, VELOCITY, MOD WHEEL, AFTERTOUCH, KEY, RANDOM
constexpr int kModDests = 9;      // OFF, A POS, B POS, CUTOFF, RESO, PITCH, A LEVEL, B LEVEL, DRIVE
extern const char* const kModSourceNames[kModSources];
extern const char* const kModDestNames[kModDests];
float paramToNormalized(int id, float v, int numTables);
bool paramIsBipolar(int id);
// Human-readable value for the UI. tableNames may be null.
void paramFormat(int id, float v, char* out, int len, const char* const* tableNames, int numTables);

}  // namespace xo
