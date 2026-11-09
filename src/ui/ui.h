// XO-Wave touchscreen UI (LVGL). Runs on its own normal-priority thread and
// talks to the engine only through engine_link.h.
#pragma once
#include <string>
#include <vector>

#include "../engine_link.h"
#include "../wavetable.h"

namespace xo {

struct UiConfig {
    CommandQueue* cmds = nullptr;
    EngineStatus* status = nullptr;
    PresetBank* bank = nullptr;
    const std::vector<Wavetable>* tables = nullptr;

    std::string presetsDir;    // where "Save as" writes new presets
    std::string statePath;     // touch calibration + last preset
    std::string audioDevice;   // shown on SYSTEM page
    std::string btDevice;
    unsigned rate = 48000, period = 128, periods = 3;

    std::string fontsDir;      // Anton + IBM Plex Mono .ttf files

    std::string fbDevice;      // Linux framebuffer, e.g. /dev/fb1
    std::string touchDevice;   // evdev node, e.g. /dev/input/event2
};

// Linux: finds the TFT framebuffer / touchscreen ("auto" or an explicit path).
std::string uiFindFramebuffer(const std::string& want);
std::string uiFindTouch(const std::string& want);

// Shows the splash screen right away (call before the slow start-up work).
bool uiSplash(const std::string& fbDevice, const std::string& fontsDir);
// Progress text + bar on the splash (thread-safe, no-op without a splash).
void uiSplashStatus(const char* text, int percent);

// Hands the engine to the UI: the splash gives way to the full UI
// (starts the UI thread first if uiSplash wasn't called).
bool uiStart(const UiConfig& cfg);
void uiStop();

// Headless: renders every page (and the dialogs) to PNG files in dir.
bool uiSnapshot(const UiConfig& cfg, const std::string& dir);

}  // namespace xo
