// Tiny "key = value" file format used for both xowave.conf and presets.
#pragma once
#include <map>
#include <string>
#include <vector>

#include "synth.h"

namespace xo {

using KeyValues = std::map<std::string, std::string>;

bool readKeyValues(const std::string& path, KeyValues& out);
std::string kvGet(const KeyValues& kv, const std::string& key, const std::string& def);
int kvGetInt(const KeyValues& kv, const std::string& key, int def);

// Builds a preset from key=value pairs on top of the default values.
// Wavetable params accept a table name ("Formant") or an index.
Preset presetFromKeyValues(const std::string& name, const KeyValues& kv,
                           const std::vector<Wavetable>& tables, std::vector<std::string>& warnings);

// Writes a preset file (all parameters; wavetables by name).
bool savePresetFile(const std::string& path, const std::string& name, const float* values,
                    const std::vector<Wavetable>& tables);
// Power-cut-safe write (temp file + fsync + rename).
bool writeFileDurable(const std::string& path, const std::string& content);

// Writes key=value pairs (used for the UI state file).
bool writeKeyValues(const std::string& path, const KeyValues& kv);

// Loads every *.xowave file in dir, sorted by filename.
std::vector<Preset> loadPresetDir(const std::string& dir, const std::vector<Wavetable>& tables);

// Loads every *.wav in dir as a user wavetable, sorted by filename.
void loadWavetableDir(const std::string& dir, std::vector<Wavetable>& tables);

}  // namespace xo
