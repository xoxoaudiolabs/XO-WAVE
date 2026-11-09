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
float paramToNormalized(int id, float v, int numTables);
bool paramIsBipolar(int id);
// Human-readable value for the UI. tableNames may be null.
void paramFormat(int id, float v, char* out, int len, const char* const* tableNames, int numTables);

}  // namespace xo
