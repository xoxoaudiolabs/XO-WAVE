#include "params.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <strings.h>

namespace xo {

#define LIN Curve::Linear
#define EXP Curve::Exp

const ParamInfo kParams[P_COUNT] = {
    {"a_table", 0, 255, 0, LIN, true, "Osc A wavetable (index or name)"},
    {"a_pos", 0, 1, 0, LIN, false, "Osc A wavetable position"},
    {"a_level", 0, 1, 0.8f, LIN, false, "Osc A level"},
    {"a_semi", -24, 24, 0, LIN, true, "Osc A semitones"},
    {"a_fine", -100, 100, 0, LIN, false, "Osc A fine tune (cents)"},
    {"a_unison", 1, 4, 1, LIN, true, "Osc A unison voices"},
    {"a_detune", 0, 50, 12, LIN, false, "Osc A unison detune (cents)"},
    {"b_table", 0, 255, 2, LIN, true, "Osc B wavetable"},
    {"b_pos", 0, 1, 0, LIN, false, "Osc B wavetable position"},
    {"b_level", 0, 1, 0, LIN, false, "Osc B level"},
    {"b_semi", -24, 24, 0, LIN, true, "Osc B semitones"},
    {"b_fine", -100, 100, 0, LIN, false, "Osc B fine tune (cents)"},
    {"b_unison", 1, 4, 1, LIN, true, "Osc B unison voices"},
    {"b_detune", 0, 50, 12, LIN, false, "Osc B unison detune (cents)"},
    {"spread", 0, 1, 0.6f, LIN, false, "Unison stereo width"},
    {"filter_mode", 0, 2, 0, LIN, true, "0 low-pass, 1 band-pass, 2 high-pass"},
    {"cutoff", 20, 20000, 8000, EXP, false, "Filter cutoff (Hz)"},
    {"resonance", 0, 1, 0.2f, LIN, false, "Filter resonance"},
    {"filter_env", -1, 1, 0.3f, LIN, false, "Mod envelope -> cutoff (x6 octaves)"},
    {"filter_key", 0, 1, 0.5f, LIN, false, "Cutoff keyboard tracking"},
    {"drive", 0, 1, 0, LIN, false, "Pre-filter drive"},
    {"amp_attack", 0.001f, 10, 0.005f, EXP, false, "Amp attack (s)"},
    {"amp_decay", 0.001f, 10, 0.3f, EXP, false, "Amp decay (s)"},
    {"amp_sustain", 0, 1, 0.8f, LIN, false, "Amp sustain level"},
    {"amp_release", 0.001f, 10, 0.3f, EXP, false, "Amp release (s)"},
    {"mod_attack", 0.001f, 10, 0.01f, EXP, false, "Mod env attack (s)"},
    {"mod_decay", 0.001f, 10, 0.6f, EXP, false, "Mod env decay (s)"},
    {"mod_sustain", 0, 1, 0, LIN, false, "Mod env sustain"},
    {"mod_release", 0.001f, 10, 0.5f, EXP, false, "Mod env release (s)"},
    {"menv_pos", -1, 1, 0, LIN, false, "Mod envelope -> wavetable position"},
    {"lfo_rate", 0.01f, 20, 2, EXP, false, "LFO rate (Hz)"},
    {"lfo_shape", 0, 4, 0, LIN, true, "0 sine, 1 tri, 2 saw, 3 square, 4 S&H"},
    {"lfo_pos", 0, 1, 0, LIN, false, "LFO -> wavetable position"},
    {"lfo_cutoff", 0, 1, 0, LIN, false, "LFO -> cutoff (x3 octaves)"},
    {"lfo_pitch", 0, 1, 0, LIN, false, "LFO -> pitch (semitones); mod wheel adds"},
    {"vel_amp", 0, 1, 0.7f, LIN, false, "Velocity -> amplitude"},
    {"vel_filter", 0, 1, 0.3f, LIN, false, "Velocity -> filter env amount"},
    {"glide", 0, 2, 0, LIN, false, "Portamento time (s)"},
    {"bend_range", 0, 24, 2, LIN, true, "Pitch bend range (semitones)"},
    {"volume", 0, 1, 0.7f, LIN, false, "Patch volume"},
    {"fx_drive", 0, 1, 0, LIN, false, "FX bus drive (before the effects)"},
    {"chorus_mix", 0, 1, 0, LIN, false, "Chorus amount"},
    {"chorus_rate", 0.05f, 5, 0.5f, EXP, false, "Chorus rate (Hz)"},
    {"chorus_depth", 0, 1, 0.5f, LIN, false, "Chorus depth"},
    {"delay_mix", 0, 1, 0, LIN, false, "Delay amount"},
    {"delay_time", 0.01f, 2, 0.375f, EXP, false, "Delay time (s) when not synced"},
    {"delay_sync", 0, 15, 8, LIN, true, "Delay sync: 0 free, else a note division of the tempo"},
    {"delay_feedback", 0, 0.95f, 0.35f, LIN, false, "Delay feedback"},
    {"delay_tone", 0, 1, 0.6f, LIN, false, "Delay tone (darker repeats when low)"},
    {"delay_pingpong", 0, 1, 1, LIN, true, "Delay ping-pong between left and right"},
    {"reverb_mix", 0, 1, 0, LIN, false, "Reverb amount"},
    {"reverb_size", 0, 1, 0.6f, LIN, false, "Reverb room size"},
    {"reverb_damp", 0, 1, 0.5f, LIN, false, "Reverb damping (darker when high)"},
};

int findParam(const std::string& name) {
    for (int i = 0; i < P_COUNT; ++i)
        if (strcasecmp(kParams[i].name, name.c_str()) == 0) return i;
    return -1;
}

static float maxFor(int id, int numTables) {
    if (id == P_A_TABLE || id == P_B_TABLE) return static_cast<float>(std::max(0, numTables - 1));
    return kParams[id].max;
}

float paramClamp(int id, float v, int numTables) {
    const ParamInfo& p = kParams[id];
    v = std::clamp(v, p.min, maxFor(id, numTables));
    if (p.integer) v = std::round(v);
    return v;
}

float paramFromNormalized(int id, float n, int numTables) {
    const ParamInfo& p = kParams[id];
    n = std::clamp(n, 0.0f, 1.0f);
    float hi = maxFor(id, numTables);
    float v = (p.curve == Curve::Exp) ? p.min * std::pow(hi / p.min, n) : p.min + (hi - p.min) * n;
    return paramClamp(id, v, numTables);
}

float paramToNormalized(int id, float v, int numTables) {
    const ParamInfo& p = kParams[id];
    float hi = maxFor(id, numTables);
    if (hi <= p.min) return 0.0f;
    v = std::clamp(v, p.min, hi);
    if (p.curve == Curve::Exp) return std::log(v / p.min) / std::log(hi / p.min);
    return (v - p.min) / (hi - p.min);
}

bool paramIsBipolar(int id) { return kParams[id].min < 0; }

static void fmtTime(float s, char* out, int len) {
    if (s < 1.0f) std::snprintf(out, len, "%.0fms", s * 1000.0f);
    else std::snprintf(out, len, "%.2fs", s);
}

void paramFormat(int id, float v, char* out, int len, const char* const* tableNames, int numTables) {
    switch (id) {
        case P_A_TABLE:
        case P_B_TABLE: {
            int t = static_cast<int>(v);
            if (tableNames && t >= 0 && t < numTables) std::snprintf(out, len, "%s", tableNames[t]);
            else std::snprintf(out, len, "%d", t);
            return;
        }
        case P_FILTER_MODE: {
            static const char* m[] = {"LP", "BP", "HP"};
            std::snprintf(out, len, "%s", m[std::clamp(static_cast<int>(v), 0, 2)]);
            return;
        }
        case P_LFO_SHAPE: {
            static const char* m[] = {"SIN", "TRI", "SAW", "SQR", "S&H"};
            std::snprintf(out, len, "%s", m[std::clamp(static_cast<int>(v), 0, 4)]);
            return;
        }
        case P_CUTOFF:
            if (v >= 1000.0f) std::snprintf(out, len, "%.1fk", v / 1000.0f);
            else std::snprintf(out, len, "%.0f", v);
            return;
        case P_AMP_A: case P_AMP_D: case P_AMP_R:
        case P_MOD_A: case P_MOD_D: case P_MOD_R:
            fmtTime(v, out, len);
            return;
        case P_DELAY_TIME:
            fmtTime(v, out, len);
            return;
        case P_DELAY_SYNC: {
            static const char* d[] = {"FREE", "1/32", "1/16T", "1/16", "1/8T", "1/16D", "1/8", "1/4T",
                                      "1/8D", "1/4", "1/2T", "1/4D", "1/2", "1 BAR", "2 BARS", "4 BARS"};
            std::snprintf(out, len, "%s", d[std::clamp(static_cast<int>(v), 0, 15)]);
            return;
        }
        case P_DELAY_PP:
            std::snprintf(out, len, "%s", v >= 0.5f ? "ON" : "OFF");
            return;
        case P_CHORUS_RATE:
            std::snprintf(out, len, v < 1.0f ? "%.2fHz" : "%.1fHz", v);
            return;
        case P_GLIDE:
            if (v < 0.001f) std::snprintf(out, len, "OFF");
            else fmtTime(v, out, len);
            return;
        case P_LFO_RATE:
            std::snprintf(out, len, v < 1.0f ? "%.2fHz" : (v < 10.0f ? "%.1fHz" : "%.0fHz"), v);
            return;
        case P_A_SEMI: case P_B_SEMI:
            std::snprintf(out, len, "%+d", static_cast<int>(v));
            return;
        case P_BEND_RANGE:
            std::snprintf(out, len, "%dst", static_cast<int>(v));
            return;
        case P_A_FINE: case P_B_FINE:
            std::snprintf(out, len, "%+.0fc", v);
            return;
        case P_A_DETUNE: case P_B_DETUNE:
            std::snprintf(out, len, "%.0fc", v);
            return;
        case P_A_UNISON: case P_B_UNISON:
            std::snprintf(out, len, "%dx", static_cast<int>(v));
            return;
        default:
            if (paramIsBipolar(id)) std::snprintf(out, len, "%+.0f%%", v * 100.0f);
            else std::snprintf(out, len, "%.0f%%", v * 100.0f);
            return;
    }
}

}  // namespace xo
