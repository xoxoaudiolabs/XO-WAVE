// XO-Wave touchscreen UI for a 480x320 display.
//
// Pages (tabs along the bottom): OSC  FILTER  ENV  MOD  KEYS  PRESETS  SYSTEM
// Top bar: logo, preset name with prev/next, MIDI activity, Bluetooth,
// voice count and output meter.
//
// Knobs are "drag anywhere on the knob": slide up/right to increase. That is
// far easier on a small resistive touchscreen than following the arc.

#include "ui.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <unistd.h>

#ifdef __linux__
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/input.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#include "../config.h"
#include "../params.h"
#include "lvgl.h"

namespace xo {
namespace {

constexpr int kW = 480, kH = 320;
constexpr int kTopH = 34, kTabH = 40;
constexpr int kFiltPts = 80, kEnvPts = 60, kLfoPts = 60;

// XOXO//AUDIO//LABS palette (from xoxoaudiolabs.com): near-black, warm
// off-white, warm grey, hairline rules and one signal colour.
constexpr uint32_t kBg = 0x0b0b0b, kPanel = 0x141414, kPanel2 = 0x1c1c1b, kLine = 0x2a2a28, kLine2 = 0x3a3a38;
constexpr uint32_t kText = 0xf2f0eb, kDim = 0x8a8780, kSignal = 0xff3b1f;
constexpr uint32_t kAccent = kText, kAccent2 = kSignal, kWarn = kSignal;

// Fonts: Anton for display, IBM Plex Mono for everything else (TinyTTF).
// Montserrat stays as the fallback so LVGL symbols still render.
const lv_font_t* fLabel = &lv_font_montserrat_12;  // small caps labels
const lv_font_t* fSmall = &lv_font_montserrat_12;
const lv_font_t* fBody = &lv_font_montserrat_14;
const lv_font_t* fMed = &lv_font_montserrat_16;
const lv_font_t* fBig = &lv_font_montserrat_20;
const lv_font_t* fLogo = &lv_font_montserrat_24;

lv_color_t C(uint32_t hex) { return lv_color_hex(hex); }

uint32_t tickMs() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now() - t0).count());
}

struct Knob {
    int id = 0;
    lv_obj_t* root = nullptr;
    lv_obj_t* arc = nullptr;
    lv_obj_t* value = nullptr;
    float norm = 0;
    bool dragging = false;
};

struct BtDevice {
    std::string mac, name;
};

struct State {
    UiConfig cfg;
    std::vector<const char*> tableNames;
    int numTables = 0;

    float values[P_COUNT] = {};
    uint32_t pendingUntil[P_COUNT] = {};  // ignore engine echo briefly after UI edits
    uint32_t lastSerial = 0xffffffffu;
    uint32_t lastSerialXy = 0;
    int currentPreset = -1;

    std::vector<std::unique_ptr<Knob>> knobs;

    lv_display_t* disp = nullptr;
    lv_indev_t* touch = nullptr;
    lv_obj_t* tabview = nullptr;

    // top bar
    lv_obj_t *presetLabel = nullptr, *voicesLabel = nullptr, *midiDot = nullptr, *btIcon = nullptr;
    lv_obj_t *meterL = nullptr, *meterR = nullptr;
    float meterShownL = 0, meterShownR = 0;
    lv_obj_t* volButton = nullptr;
    lv_obj_t* volLabel = nullptr;
    lv_obj_t* volSlider = nullptr;
    lv_obj_t* volBig = nullptr;
    lv_obj_t* volMute = nullptr;
    float master = 0.8f;       // fader position shown
    float unmuted = 0.8f;      // level to return to after mute
    bool muted = false;
    uint32_t lastMidi = 0, midiFlashUntil = 0;

    // OSC
    int focusOsc = 0;                     // which oscillator the OSC page shows
    lv_obj_t* oscSelect = nullptr;
    lv_obj_t* tableName = nullptr;
    lv_obj_t* frameLabel = nullptr;
    lv_obj_t* knobRow[3] = {};   // OSC A, OSC B, SUB
    lv_obj_t* terrain = nullptr;          // 3D wavetable canvas
    std::vector<uint16_t> terrainBuf;
    float terrainPos = -1;                // position last drawn
    int terrainTable = -1, terrainOsc = -1;
    float terrainDragPos = 0;

    // FILTER
    lv_obj_t* filterMode = nullptr;
    lv_obj_t* filterChart = nullptr;
    lv_chart_series_t* filterSer = nullptr;
    int32_t filterY[kFiltPts] = {};

    // ENV
    lv_obj_t* envChart[2] = {};
    lv_chart_series_t* envSer[2] = {};
    int32_t envY[2][kEnvPts] = {};

    // MOD
    lv_obj_t* lfoShape = nullptr;
    lv_obj_t* lfoChart = nullptr;

    // FX
    lv_obj_t* pingPong = nullptr;

    // PLAY
    lv_obj_t* arpMode = nullptr;
    lv_obj_t* arpHoldBtn = nullptr;
    lv_obj_t* tempoLabel = nullptr;
    lv_obj_t* tempoSrc = nullptr;
    float tempoShown = -1;
    bool extShown = false;
    lv_chart_series_t* lfoSer = nullptr;
    int32_t lfoY[kLfoPts] = {};
    lv_obj_t* lfo2Shape = nullptr;
    lv_obj_t* lfo2Chart = nullptr;
    lv_chart_series_t* lfo2Ser = nullptr;
    int32_t lfo2Y[kLfoPts] = {};
    lv_obj_t* modViewSel = nullptr;
    lv_obj_t* modView[4] = {};
    lv_obj_t* paramLabel[P_COUNT] = {};    // text readouts (mod matrix source / destination)
    lv_obj_t* paramSlider[P_COUNT] = {};   // sliders (mod matrix amounts)

    // USB stick
    struct UsbFile { std::string path, name; bool wavetable; };
    std::vector<UsbFile> usbFiles;
    std::string usbRoot;
    lv_obj_t* usbList = nullptr;
    lv_obj_t* usbStatus = nullptr;
    lv_obj_t* usbRestart = nullptr;
    int usbImportedTables = 0;

    // Updates
    lv_obj_t* updStatus = nullptr;
    lv_obj_t* updInstall = nullptr;
    std::mutex updMutex;
    std::string updResult;
    std::atomic<int> updState{0};   // 0 idle, 1 checking, 2 result ready, 3 installing
    uint32_t updNextLog = 0;

    // Undo / compare
    std::vector<std::array<float, P_COUNT>> undo;
    std::array<float, P_COUNT> stable{};
    bool stableSet = false;
    uint32_t lastChange = 0;
    bool comparing = false, applying = false;
    std::array<float, P_COUNT> stash{};
    lv_obj_t* undoBtn = nullptr;
    lv_obj_t* compareBtn = nullptr;

    // Favourites
    bool favOnly = false;
    lv_obj_t* favBtn = nullptr;

    // XY pad
    lv_obj_t* xyPad = nullptr;
    lv_obj_t* xyDot = nullptr;
    lv_obj_t* xyName[2] = {};
    int xyParam[2] = {P_A_POS, P_CUTOFF};

    // KEYS
    int octave = 3;
    bool hold = false;
    bool latched[128] = {};
    lv_obj_t* octLabel = nullptr;
    lv_obj_t* holdBtn = nullptr;
    std::vector<lv_obj_t*> keys;  // user_data = note offset
    lv_obj_t* cLabels[3] = {};    // the C labels on the keyboard

    // PRESETS
    lv_obj_t* presetList = nullptr;
    lv_obj_t* presetInfo = nullptr;
    int listCount = -1, listCurrent = -2;

    // SETTINGS
    lv_obj_t* sysInfo = nullptr;
    uint32_t sysNext = 0;
    lv_obj_t* settingsNav = nullptr;
    lv_obj_t* section[6] = {};
    lv_obj_t* linkLabel = nullptr;      // SETTINGS > STUDIO: who we're linked to
    lv_obj_t* linkToggle = nullptr;
    lv_obj_t* linkTop = nullptr;        // "LINK" in the top bar
    lv_obj_t* restartBanner = nullptr;
    lv_obj_t* learnLabel = nullptr;
    lv_obj_t* learnBtn = nullptr;
    int learnParam = P_CUTOFF;
    bool learning = false;
    lv_obj_t* midiInputs = nullptr;
    lv_obj_t* btSpeakerLabel = nullptr;
    std::mutex midiMutex;
    std::string midiInputsText;
    std::atomic<bool> midiInputsReady{false};
    float knobSpeed = 1.0f;
    bool built = false;   // true once the UI is up; view changes after that are remembered

    // dialogs
    lv_obj_t* modal = nullptr;
    lv_obj_t* textArea = nullptr;
    int confirmDeleteIdx = -1;

    // Bluetooth background jobs
    std::mutex btMutex;
    std::vector<BtDevice> btFound;
    std::string btMessage;
    std::atomic<int> btState{0};  // 0 idle, 1 busy, 2 results ready
    lv_obj_t* btList = nullptr;
    lv_obj_t* btStatus = nullptr;

    // touch calibration
    bool calibrating = false;
    int calStep = 0, calFd = -1;
    int rawX = 0, rawY = 0;
    int calRaw[4][2] = {};
    uint32_t calStart = 0;
    lv_obj_t* calTarget = nullptr;
    lv_obj_t* calText = nullptr;

    KeyValues stateKv;
    bool stateDirty = false;
    uint32_t stateSaveAt = 0;

    std::thread thread;
    std::atomic<bool> quit{false};

    // Splash: shown as soon as the display is up, until the engine hands us
    // its config (uiStart) and at least kSplashMinMs have passed.
    std::mutex cfgMutex;
    bool cfgReady = false;
    UiConfig pendingCfg;
    std::string splashText = "STARTING";
    int splashPercent = 5;
    bool splashDirty = true;
    uint32_t splashShownAt = 0;
    lv_obj_t* splashLabel = nullptr;
    lv_obj_t* splashBar = nullptr;
    bool fontsLoaded = false;
};

State* S = nullptr;

// ---------------------------------------------------------------------------
// Engine I/O

void send(CmdType t, int a, float b) {
    if (S->cfg.cmds) S->cfg.cmds->push(Cmd{t, static_cast<int16_t>(a), b});
}

void refreshViews(int id);
void updateKnob(Knob& k);
void markState(const std::string& key, const std::string& value);
void openVolume(lv_event_t*);
void restartEvent(lv_event_t*);
void modalCancel(lv_event_t*);
void openXy(lv_event_t*);
bool isFav(const char* name);
void favToggle(lv_event_t*);

void endCompare();

void setParamUi(int id, float v) {
    v = paramClamp(id, v, S->numTables);
    if (v == S->values[id]) return;
    if (S->comparing && !S->applying) endCompare();   // editing the saved version now
    S->lastChange = tickMs();
    S->values[id] = v;
    S->pendingUntil[id] = tickMs() + 250;
    send(CmdType::SetParam, id, v);
    refreshViews(id);
}

void pushUndo();

void loadPresetUi(int idx) {
    if (!S->cfg.bank || idx < 0 || idx >= S->cfg.bank->count()) return;
    pushUndo();
    S->comparing = false;
    send(CmdType::Program, idx, 0);
    // Show the new values right away instead of waiting for the engine echo.
    float v[P_COUNT];
    S->cfg.bank->read(idx, v);
    uint32_t now = tickMs();
    for (int i = 0; i < P_COUNT; ++i) {
        S->values[i] = paramClamp(i, v[i], S->numTables);
        S->pendingUntil[i] = now + 250;
    }
    for (auto& k : S->knobs) updateKnob(*k);
    for (int i = 0; i < P_COUNT; ++i) refreshViews(i);
    S->currentPreset = idx;
    S->stateKv["preset"] = S->cfg.bank->name(idx);
    S->stateDirty = true;
    S->stateSaveAt = tickMs() + 2000;
}

// ---------------------------------------------------------------------------
// Styling helpers

lv_obj_t* panel(lv_obj_t* parent, int w, int h) {
    lv_obj_t* p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, C(kPanel), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, C(kLine), 0);
    lv_obj_set_style_pad_all(p, 4, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

lv_obj_t* row(lv_obj_t* parent, int w, int h) {
    lv_obj_t* r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, w, h);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

lv_obj_t* label(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t color) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, C(color), 0);
    if (font == fLabel || font == fSmall) lv_obj_set_style_text_letter_space(l, 1, 0);
    return l;
}

lv_obj_t* button(lv_obj_t* parent, const char* text, int w, int h, lv_event_cb_t cb, void* user = nullptr) {
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, C(kBg), 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, C(kText), 0);
    lv_obj_set_style_text_color(b, C(kText), 0);
    lv_obj_set_style_bg_color(b, C(kText), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(b, C(kBg), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(b, C(kText), LV_STATE_CHECKED);
    lv_obj_set_style_text_color(b, C(kBg), LV_STATE_CHECKED);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_color(b, C(kLine2), LV_STATE_DISABLED);
    lv_obj_set_style_text_color(b, C(kLine2), LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(b, C(kBg), LV_STATE_DISABLED);
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_set_style_pad_all(b, 2, 0);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, fLabel, 0);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}

// A toggle button's on/off look, set directly (solid off-white with black text when on).
void setButtonOn(lv_obj_t* b, bool on, uint32_t onColor = kText) {
    if (!b) return;
    lv_obj_set_style_bg_color(b, C(on ? onColor : kBg), 0);
    lv_obj_set_style_border_color(b, C(on ? onColor : kText), 0);
    lv_obj_set_style_text_color(b, C(on ? kBg : kText), 0);
    lv_obj_t* l = lv_obj_get_child(b, 0);
    if (l) lv_obj_set_style_text_color(l, C(on ? kBg : kText), 0);
}

lv_obj_t* chart(lv_obj_t* parent, int w, int h, int ymin, int ymax, int points, int32_t* ys,
                lv_chart_series_t** ser, uint32_t color) {
    lv_obj_t* c = lv_chart_create(parent);
    lv_obj_set_size(c, w, h);
    lv_chart_set_type(c, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(c, static_cast<uint32_t>(points));
    lv_chart_set_range(c, LV_CHART_AXIS_PRIMARY_Y, ymin, ymax);
    lv_chart_set_div_line_count(c, 3, 0);
    lv_obj_set_style_bg_color(c, C(kBg), 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 0, 0);
    lv_obj_set_style_pad_all(c, 3, 0);
    lv_obj_set_style_line_color(c, C(kLine), LV_PART_MAIN);
    lv_obj_set_style_line_width(c, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(c, 0, 0, LV_PART_INDICATOR);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    *ser = lv_chart_add_series(c, C(color), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_ext_y_array(c, *ser, ys);
    return c;
}

lv_obj_t* buttonMatrix(lv_obj_t* parent, const char* const* map, int w, int h, lv_event_cb_t cb) {
    lv_obj_t* m = lv_buttonmatrix_create(parent);
    lv_buttonmatrix_set_map(m, map);
    lv_buttonmatrix_set_button_ctrl_all(m, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(m, true);
    lv_obj_set_size(m, w, h);
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_pad_all(m, 0, 0);
    lv_obj_set_style_pad_gap(m, 4, 0);
    lv_obj_set_style_bg_color(m, C(kBg), LV_PART_ITEMS);
    lv_obj_set_style_border_width(m, 2, LV_PART_ITEMS);
    lv_obj_set_style_border_color(m, C(kLine2), LV_PART_ITEMS);
    lv_obj_set_style_radius(m, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_color(m, C(kText), LV_PART_ITEMS);
    lv_obj_set_style_border_color(m, C(kText), static_cast<lv_style_selector_t>(LV_PART_ITEMS) | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(m, C(kAccent), static_cast<lv_style_selector_t>(LV_PART_ITEMS) | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(m, C(kBg), static_cast<lv_style_selector_t>(LV_PART_ITEMS) | LV_STATE_CHECKED);
    lv_obj_set_style_text_font(m, fLabel, LV_PART_ITEMS);
    lv_obj_set_style_text_letter_space(m, 1, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(m, 0, LV_PART_ITEMS);
    lv_obj_add_event_cb(m, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    return m;
}

void setMatrixChecked(lv_obj_t* m, int idx) {
    if (!m) return;
    lv_buttonmatrix_clear_button_ctrl_all(m, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_buttonmatrix_set_button_ctrl(m, static_cast<uint32_t>(idx), LV_BUTTONMATRIX_CTRL_CHECKED);
}

// ---------------------------------------------------------------------------
// Knob

void updateKnob(Knob& k) {
    float v = S->values[k.id];
    if (!k.dragging) k.norm = paramToNormalized(k.id, v, S->numTables);
    lv_arc_set_value(k.arc, static_cast<int32_t>(std::lround(paramToNormalized(k.id, v, S->numTables) * 1000)));
    char buf[24];
    paramFormat(k.id, v, buf, sizeof buf, S->tableNames.data(), S->numTables);
    lv_label_set_text(k.value, buf);
    lv_obj_set_style_text_font(k.value, std::strlen(buf) >= 6 ? fLabel : fSmall, 0);
    lv_obj_set_style_text_letter_space(k.value, 0, 0);
}

void knobEvent(lv_event_t* e) {
    Knob* k = static_cast<Knob*>(lv_event_get_user_data(e));
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        k->dragging = true;
        k->norm = paramToNormalized(k->id, S->values[k->id], S->numTables);
        lv_obj_set_style_arc_color(k->arc, C(kAccent2), LV_PART_INDICATOR);
    } else if (code == LV_EVENT_PRESSING) {
        lv_point_t v;
        lv_indev_get_vect(lv_indev_active(), &v);
        float d = (static_cast<float>(v.x) - static_cast<float>(v.y)) * 0.005f * S->knobSpeed;
        if (d == 0.0f) return;
        k->norm = std::clamp(k->norm + d, 0.0f, 1.0f);
        setParamUi(k->id, paramFromNormalized(k->id, k->norm, S->numTables));
        updateKnob(*k);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        k->dragging = false;
        lv_obj_set_style_arc_color(k->arc, C(kAccent), LV_PART_INDICATOR);
        updateKnob(*k);
    }
}

lv_obj_t* knob(lv_obj_t* parent, int id, const char* name) {
    auto k = std::make_unique<Knob>();
    k->id = id;
    lv_obj_t* root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, 72, 72);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLL_CHAIN);

    lv_obj_t* arc = lv_arc_create(root);
    lv_obj_set_size(arc, 52, 52);
    lv_obj_align(arc, LV_ALIGN_TOP_MID, 0, 0);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, 1000);
    if (paramIsBipolar(id)) lv_arc_set_mode(arc, LV_ARC_MODE_SYMMETRICAL);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, C(kLine2), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, false, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, false, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, C(kAccent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(arc, 0, LV_PART_KNOB);

    lv_obj_t* value = label(arc, "", fSmall, kText);
    lv_obj_center(value);

    lv_obj_t* nameL = label(root, name, fLabel, kDim);
    lv_obj_align(nameL, LV_ALIGN_BOTTOM_MID, 0, 0);

    k->root = root;
    k->arc = arc;
    k->value = value;
    lv_obj_add_event_cb(root, knobEvent, LV_EVENT_PRESSED, k.get());
    lv_obj_add_event_cb(root, knobEvent, LV_EVENT_PRESSING, k.get());
    lv_obj_add_event_cb(root, knobEvent, LV_EVENT_RELEASED, k.get());
    lv_obj_add_event_cb(root, knobEvent, LV_EVENT_PRESS_LOST, k.get());
    updateKnob(*k);
    S->knobs.push_back(std::move(k));
    return root;
}

// ---------------------------------------------------------------------------
// Visualisations

// 3D wavetable "terrain": every frame stacked in perspective, back to front,
// with hidden-line removal; the frame being played is drawn in signal red.
constexpr int kTerrW = 304, kTerrH = 134;

uint16_t rgb565(uint32_t hex) {
    return static_cast<uint16_t>(((hex >> 8) & 0xf800) | ((hex >> 5) & 0x07e0) | ((hex >> 3) & 0x001f));
}

uint32_t mixHex(uint32_t a, uint32_t b, float t) {
    auto ch = [&](int sh) {
        float x = ((a >> sh) & 0xff) * (1 - t) + ((b >> sh) & 0xff) * t;
        return static_cast<uint32_t>(x) << sh;
    };
    return ch(16) | ch(8) | ch(0);
}

void plot(int x, int y, uint16_t c) {
    if (x >= 0 && x < kTerrW && y >= 0 && y < kTerrH) S->terrainBuf[static_cast<size_t>(y) * kTerrW + x] = c;
}

void line(int x0, int y0, int x1, int y1, uint16_t c, bool thick) {
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        plot(x0, y0, c);
        if (thick) { plot(x0, y0 - 1, c); plot(x0 + 1, y0, c); }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// The wavetable shown/edited on the OSC page: B when OSC B is picked, else A (SUB shows A).
int wtOsc() { return S->focusOsc == 1 ? 1 : 0; }

void drawTerrain() {
    if (!S->terrain || !S->cfg.tables || S->cfg.tables->empty()) return;
    const int osc = wtOsc();
    const int tIdx = std::clamp(static_cast<int>(S->values[osc ? P_B_TABLE : P_A_TABLE]), 0, S->numTables - 1);
    const Wavetable& wt = (*S->cfg.tables)[static_cast<size_t>(tIdx)];
    float pos = S->cfg.status ? S->cfg.status->livePos[osc].load() : S->values[osc ? P_B_POS : P_A_POS];
    pos = std::clamp(pos, 0.0f, 1.0f);
    S->terrainPos = pos;
    S->terrainTable = tIdx;
    S->terrainOsc = osc;

    std::fill(S->terrainBuf.begin(), S->terrainBuf.end(), rgb565(kBg));
    const uint16_t bg = rgb565(kBg);
    const int frames = wt.numFrames();
    const int maxLines = 26;
    const int step = std::max(1, (frames + maxLines - 1) / maxLines);

    const int width = kTerrW - 78;  // each wave's width
    const float amp = 23.0f;
    auto project = [&](float depth, int i, float v, int& x, int& y) {
        // depth 0 = front (bottom-left), 1 = back (top-right)
        x = 6 + static_cast<int>(depth * 66.0f) + i;
        y = static_cast<int>(kTerrH - 26 - depth * 82.0f - v * amp);
    };
    auto drawFrame = [&](float framePos, uint16_t color, bool thick) {
        float fpos = framePos * (frames - 1);
        int f0 = std::min(static_cast<int>(fpos), frames - 1);
        int f1 = std::min(f0 + 1, frames - 1);
        float ff = fpos - f0;
        const float* a = wt.table(f0, 2);  // mip 2: smooth enough to draw
        const float* b = wt.table(f1, 2);
        int px = 0, py = 0;
        for (int i = 0; i < width; ++i) {
            int si = i * Wavetable::kSize / width;
            float v = a[si] + ff * (b[si] - a[si]);
            int x, y;
            project(framePos, i, v, x, y);
            // hidden-line removal: blank everything below this wave down to the floor
            int floorY = static_cast<int>(kTerrH - 26 - framePos * 82.0f + amp + 2);
            for (int yy = y + 1; yy <= floorY; ++yy) plot(x, yy, bg);
            if (thick) for (int yy = y + 2; yy <= floorY; ++yy) plot(x + 1, yy, bg);
            if (i > 0) line(px, py, x, y, color, thick);
            px = x; py = y;
        }
    };

    bool hiDrawn = false;
    for (int f = frames - 1; f >= 0; f -= step) {
        float d = frames > 1 ? static_cast<float>(f) / (frames - 1) : 0.0f;
        if (!hiDrawn && d < pos) { drawFrame(pos, rgb565(kSignal), true); hiDrawn = true; }
        float near = 1.0f - d;  // brighter towards the front
        drawFrame(d, rgb565(near > 0.5f ? mixHex(kDim, kText, (near - 0.5f) * 1.2f) : mixHex(kLine2, kDim, near * 2.0f)), false);
    }
    if (!hiDrawn) drawFrame(pos, rgb565(kSignal), true);

    lv_obj_invalidate(S->terrain);
    lv_label_set_text(S->tableName, wt.name.c_str());
    lv_label_set_text_fmt(S->frameLabel, "FRAME %d/%d", static_cast<int>(std::lround(pos * (frames - 1))) + 1, frames);
}

void drawWave(int osc) {
    if (osc == S->focusOsc) drawTerrain();
}

// Filter type buttons (2 x 3) <-> filter_mode values.
static const int kBtnToMode[6] = {0, 3, 1, 4, 2, 5};   // LP12 LP24 / BP NOTCH / HP COMB
static const int kModeToBtn[6] = {0, 2, 4, 1, 3, 5};

void drawFilter() {
    if (!S->filterChart) return;
    float fc = S->values[P_CUTOFF];
    float reso = S->values[P_RESO];
    float k = 2.0f - 1.96f * reso;
    int mode = std::clamp(static_cast<int>(S->values[P_FILTER_MODE]), 0, 5);
    for (int i = 0; i < kFiltPts; ++i) {
        float f = 20.0f * std::pow(1000.0f, static_cast<float>(i) / (kFiltPts - 1));  // 20 Hz .. 20 kHz
        float w = f / fc;
        float den = std::sqrt((1 - w * w) * (1 - w * w) + (k * w) * (k * w));
        float mag;
        switch (mode) {
            case 1: mag = k * w / den; break;                        // band
            case 2: mag = w * w / den; break;                        // high
            case 3: {                                                // ladder: 4 poles, sharper peak
                float lp = 1.0f / std::sqrt((1 - w * w) * (1 - w * w) + (1.4f * (1 - reso) * w + 0.05f) * (1.4f * (1 - reso) * w + 0.05f));
                mag = std::min(lp, 1.0f + 6.0f * reso) * (w > 1 ? 1.0f / (w * w) : 1.0f);
                break;
            }
            case 4: mag = std::fabs(1 - w * w) / den; break;         // notch
            case 5: {                                                // comb: peaks at multiples of the cutoff
                float fb = 0.5f + 0.48f * reso;
                mag = 0.5f / std::sqrt(1 - 2 * fb * std::cos(2 * 3.14159265f * w) + fb * fb);
                break;
            }
            default: mag = 1.0f / den; break;                        // low 12 dB
        }
        float db = 20.0f * std::log10(std::max(mag, 1e-4f));
        S->filterY[i] = static_cast<int32_t>(std::clamp(db, -48.0f, 24.0f));
    }
    lv_chart_refresh(S->filterChart);
    setMatrixChecked(S->filterMode, kModeToBtn[mode]);
}

void drawEnv(int which) {
    if (!S->envChart[which]) return;
    int base = which ? P_MOD_A : P_AMP_A;
    float a = S->values[base], d = S->values[base + 1], s = S->values[base + 2], r = S->values[base + 3];
    // Segment widths on a compressed time scale so 1 ms and 10 s both read well.
    auto wOf = [](float t) { return 0.15f + std::sqrt(t); };
    float wa = wOf(a), wd = wOf(d), wh = 0.8f, wr = wOf(r);
    float total = wa + wd + wh + wr;
    for (int i = 0; i < kEnvPts; ++i) {
        float x = total * i / (kEnvPts - 1);
        float y;
        if (x < wa) y = x / wa;
        else if (x < wa + wd) y = s + (1 - s) * std::exp(-5.0f * (x - wa) / wd);
        else if (x < wa + wd + wh) y = s + (1 - s) * std::exp(-5.0f);
        else y = s * std::exp(-5.0f * (x - wa - wd - wh) / wr);
        S->envY[which][i] = static_cast<int32_t>(y * 100.0f);
    }
    lv_chart_refresh(S->envChart[which]);
}

void drawLfoShape(int shape, int32_t* ys, lv_obj_t* chartObj, lv_obj_t* matrix) {
    if (!chartObj) return;
    uint32_t seed = 7;
    float sh = 0;
    for (int i = 0; i < kLfoPts; ++i) {
        float p = 2.0f * i / kLfoPts;  // two cycles
        float ph = p - std::floor(p);
        if (i == 0 || static_cast<int>(p * 4) != static_cast<int>(2.0f * (i - 1) / kLfoPts * 4)) {
            seed = seed * 1664525u + 1013904223u;
            sh = ((seed >> 8) / 8388608.0f) - 1.0f;
        }
        float v;
        switch (shape) {
            case 0: v = std::sin(2 * 3.14159265f * ph); break;
            case 1: v = 1.0f - 4.0f * std::fabs(ph - 0.5f); break;
            case 2: v = 1.0f - 2.0f * ph; break;
            case 3: v = ph < 0.5f ? 1.0f : -1.0f; break;
            default: v = sh; break;
        }
        ys[i] = static_cast<int32_t>(v * 100.0f);
    }
    lv_chart_refresh(chartObj);
    setMatrixChecked(matrix, shape);
}

void drawLfo() { drawLfoShape(static_cast<int>(S->values[P_LFO_SHAPE]), S->lfoY, S->lfoChart, S->lfoShape); }
void drawLfo2() { drawLfoShape(static_cast<int>(S->values[P_LFO2_SHAPE]), S->lfo2Y, S->lfo2Chart, S->lfo2Shape); }

void showParamReadout(int id) {
    if (S->paramLabel[id]) {
        char buf[24];
        paramFormat(id, S->values[id], buf, sizeof buf, S->tableNames.data(), S->numTables);
        lv_label_set_text(S->paramLabel[id], buf);
        bool off = S->values[id] < 0.5f;   // OFF source / destination dimmed
        lv_obj_set_style_text_color(S->paramLabel[id], C(off ? kDim : kText), 0);
    }
    if (S->paramSlider[id]) {
        lv_slider_set_value(S->paramSlider[id], static_cast<int32_t>(std::lround(S->values[id] * 100)), LV_ANIM_OFF);
        lv_obj_t* v = static_cast<lv_obj_t*>(lv_obj_get_user_data(S->paramSlider[id]));
        if (v) lv_label_set_text_fmt(v, "%+d", static_cast<int>(std::lround(S->values[id] * 100)));
    }
}

void refreshViews(int id) {
    switch (id) {
        case P_A_TABLE: case P_A_POS: drawWave(0); break;
        case P_B_TABLE: case P_B_POS: drawWave(1); break;
        case P_CUTOFF: case P_RESO: case P_FILTER_MODE: drawFilter(); break;
        case P_AMP_A: case P_AMP_D: case P_AMP_S: case P_AMP_R: drawEnv(0); break;
        case P_MOD_A: case P_MOD_D: case P_MOD_S: case P_MOD_R: drawEnv(1); break;
        case P_LFO_SHAPE: drawLfo(); break;
        case P_DELAY_PP: setButtonOn(S->pingPong, S->values[P_DELAY_PP] >= 0.5f); break;
        case P_ARP_MODE: setMatrixChecked(S->arpMode, static_cast<int>(S->values[P_ARP_MODE])); break;
        case P_ARP_HOLD: setButtonOn(S->arpHoldBtn, S->values[P_ARP_HOLD] >= 0.5f); break;
        case P_LFO2_SHAPE: drawLfo2(); break;
        default: break;
    }
    if (S->paramLabel[id] || S->paramSlider[id]) showParamReadout(id);
}

// ---------------------------------------------------------------------------
// Pages

lv_obj_t* addTab(const char* name) {
    lv_obj_t* t = lv_tabview_add_tab(S->tabview, name);
    lv_obj_set_style_pad_all(t, 4, 0);
    lv_obj_set_style_pad_gap(t, 4, 0);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    return t;
}


void terrainEvent(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    int id = wtOsc() ? P_B_POS : P_A_POS;
    if (code == LV_EVENT_PRESSED) {
        S->terrainDragPos = S->values[id];
    } else if (code == LV_EVENT_PRESSING) {
        lv_point_t v;
        lv_indev_get_vect(lv_indev_active(), &v);
        float d = (static_cast<float>(v.x) - static_cast<float>(v.y)) / 220.0f;
        if (d == 0.0f) return;
        S->terrainDragPos = std::clamp(S->terrainDragPos + d, 0.0f, 1.0f);
        setParamUi(id, S->terrainDragPos);
        for (auto& k : S->knobs) if (k->id == id) updateKnob(*k);
        drawTerrain();
    }
}

void rememberView(const char* key, int value) {
    if (S->built) markState(key, std::to_string(value));
}

void setFocusOsc(int osc) {
    S->focusOsc = osc;
    rememberView("ui.osc", osc);
    for (int i = 0; i < 3; ++i) {
        if (i == osc) lv_obj_remove_flag(S->knobRow[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(S->knobRow[i], LV_OBJ_FLAG_HIDDEN);
    }
    setMatrixChecked(S->oscSelect, osc);
    drawTerrain();
}

void oscSelectEvent(lv_event_t* e) {
    setFocusOsc(static_cast<int>(lv_buttonmatrix_get_selected_button(lv_event_get_target_obj(e))));
}

void tableStepFocus(lv_event_t* e) {
    int dir = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    int id = wtOsc() ? P_B_TABLE : P_A_TABLE;
    int next = (static_cast<int>(S->values[id]) + dir + S->numTables) % S->numTables;
    setParamUi(id, static_cast<float>(next));
    drawTerrain();
}

void buildOsc(lv_obj_t* tab) {
    // 3D wavetable view
    lv_obj_t* view = panel(tab, 312, 158);
    S->terrainBuf.assign(static_cast<size_t>(kTerrW) * kTerrH, rgb565(kBg));
    S->terrain = lv_canvas_create(view);
    lv_canvas_set_buffer(S->terrain, S->terrainBuf.data(), kTerrW, kTerrH, LV_COLOR_FORMAT_RGB565);
    lv_obj_align(S->terrain, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(S->terrain, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(S->terrain, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(S->terrain, terrainEvent, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(S->terrain, terrainEvent, LV_EVENT_PRESSING, nullptr);
    lv_obj_t* hint = label(view, "DRAG TO SCAN", fLabel, kDim);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 2, 0);
    S->frameLabel = label(view, "", fLabel, kText);
    lv_obj_align(S->frameLabel, LV_ALIGN_TOP_RIGHT, -2, 0);

    // side: oscillator select + wavetable select
    static const char* oscMap[] = {"OSC A", "OSC B", "SUB", ""};
    lv_obj_t* side = panel(tab, 156, 158);
    lv_obj_set_flex_flow(side, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(side, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    S->oscSelect = buttonMatrix(side, oscMap, 146, 36, oscSelectEvent);
    label(side, "WAVETABLE", fLabel, kDim);
    S->tableName = label(side, "", fBig, kText);
    lv_obj_set_width(S->tableName, 146);
    lv_label_set_long_mode(S->tableName, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(S->tableName, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t* arrows = row(side, 146, 38);
    lv_obj_set_flex_align(arrows, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    button(arrows, LV_SYMBOL_LEFT, 68, 36, tableStepFocus, reinterpret_cast<void*>(static_cast<intptr_t>(-1)));
    button(arrows, LV_SYMBOL_RIGHT, 68, 36, tableStepFocus, reinterpret_cast<void*>(static_cast<intptr_t>(1)));

    // knobs for the focused oscillator
    for (int osc = 0; osc < 2; ++osc) {
        lv_obj_t* r = panel(tab, 472, 74);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(r, 0, 0);
        int base = osc ? P_B_TABLE : P_A_TABLE;
        knob(r, base + 1, "POSITION");
        knob(r, base + 2, "LEVEL");
        knob(r, base + 3, "SEMI");
        knob(r, base + 4, "FINE");
        knob(r, base + 5, "UNISON");
        knob(r, base + 6, "DETUNE");
        S->knobRow[osc] = r;
    }
    {   // SUB: sub oscillator + noise
        lv_obj_t* r = panel(tab, 472, 74);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(r, 0, 0);
        knob(r, P_SUB_LEVEL, "SUB LEVEL");
        knob(r, P_SUB_OCT, "OCTAVE");
        knob(r, P_SUB_SHAPE, "SHAPE");
        knob(r, P_NOISE, "NOISE");
        S->knobRow[2] = r;
    }
    setFocusOsc(0);
}

void filterModeEvent(lv_event_t* e) {
    lv_obj_t* m = lv_event_get_target_obj(e);
    setParamUi(P_FILTER_MODE, static_cast<float>(kBtnToMode[std::min(5u, lv_buttonmatrix_get_selected_button(m))]));
}

void buildFilter(lv_obj_t* tab) {
    static const char* modes[] = {"LP12", "LP24", "\n", "BP", "NOTCH", "\n", "HP", "COMB", ""};
    lv_obj_t* left = panel(tab, 156, 146);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    label(left, "TYPE", fLabel, kDim);
    S->filterMode = buttonMatrix(left, modes, 146, 116, filterModeEvent);

    lv_obj_t* right = panel(tab, 308, 146);
    label(right, "RESPONSE", fLabel, kDim);
    S->filterChart = chart(right, 298, 120, -48, 24, kFiltPts, S->filterY, &S->filterSer, kAccent);
    lv_obj_align(S->filterChart, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_t* knobs = panel(tab, 472, 86);
    lv_obj_set_flex_flow(knobs, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(knobs, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    knob(knobs, P_CUTOFF, "CUTOFF");
    knob(knobs, P_RESO, "RESO");
    knob(knobs, P_FENV, "ENV AMT");
    knob(knobs, P_FKEY, "KEY TRK");
    knob(knobs, P_DRIVE, "DRIVE");
    knob(knobs, P_VEL_FILTER, "VEL");
    drawFilter();
}

void buildEnv(lv_obj_t* tab) {
    for (int which = 0; which < 2; ++which) {
        lv_obj_t* p = panel(tab, 472, 114);
        lv_obj_set_flex_flow(p, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(p, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t* left = lv_obj_create(p);
        lv_obj_remove_style_all(left);
        lv_obj_set_size(left, 150, 104);
        lv_obj_remove_flag(left, LV_OBJ_FLAG_SCROLLABLE);
        label(left, which ? "MOD ENVELOPE" : "AMP ENVELOPE", fSmall, which ? kAccent2 : kAccent);
        S->envChart[which] = chart(left, 150, 86, -5, 105, kEnvPts, S->envY[which], &S->envSer[which],
                                   which ? kAccent2 : kAccent);
        lv_obj_align(S->envChart[which], LV_ALIGN_BOTTOM_MID, 0, 0);
        int base = which ? P_MOD_A : P_AMP_A;
        knob(p, base, "ATTACK");
        knob(p, base + 1, "DECAY");
        knob(p, base + 2, "SUSTAIN");
        knob(p, base + 3, "RELEASE");
        drawEnv(which);
    }
}

void lfoShapeEvent(lv_event_t* e) {
    lv_obj_t* m = lv_event_get_target_obj(e);
    setParamUi(P_LFO_SHAPE, static_cast<float>(lv_buttonmatrix_get_selected_button(m)));
}

// FX ------------------------------------------------------------------------

void pingPongEvent(lv_event_t*) {
    setParamUi(P_DELAY_PP, S->values[P_DELAY_PP] >= 0.5f ? 0.0f : 1.0f);
}

lv_obj_t* fxPanel(lv_obj_t* tab, int w) {
    lv_obj_t* p = panel(tab, w, 116);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(p, 2, 0);
    return p;
}

lv_obj_t* fxTitle(lv_obj_t* parent, const char* text, int w) {
    lv_obj_t* t = label(parent, text, fLabel, kText);
    lv_obj_set_width(t, w);
    return t;
}

void buildFx(lv_obj_t* tab) {
    // DELAY + bus drive
    lv_obj_t* d = fxPanel(tab, 472);
    lv_obj_t* head = row(d, 460, 24);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    label(head, "DELAY", fLabel, kText);
    S->pingPong = button(head, "PING-PONG", 104, 22, pingPongEvent);
    knob(d, P_DELAY_SYNC, "SYNC");
    knob(d, P_DELAY_TIME, "TIME");
    knob(d, P_DELAY_FB, "FEEDBACK");
    knob(d, P_DELAY_TONE, "TONE");
    knob(d, P_DELAY_MIX, "MIX");
    knob(d, P_FX_DRIVE, "DRIVE");

    // CHORUS | REVERB
    lv_obj_t* cr = fxPanel(tab, 472);
    fxTitle(cr, "CHORUS", 220);
    fxTitle(cr, "REVERB", 220);
    knob(cr, P_CHORUS_RATE, "RATE");
    knob(cr, P_CHORUS_DEPTH, "DEPTH");
    knob(cr, P_CHORUS_MIX, "MIX");
    knob(cr, P_REVERB_SIZE, "SIZE");
    knob(cr, P_REVERB_DAMP, "DAMP");
    knob(cr, P_REVERB_MIX, "MIX");
    setButtonOn(S->pingPong, S->values[P_DELAY_PP] >= 0.5f);
}

void lfo2ShapeEvent(lv_event_t* e) {
    setParamUi(P_LFO2_SHAPE, static_cast<float>(lv_buttonmatrix_get_selected_button(lv_event_get_target_obj(e))));
}

void showModView(int idx) {
    for (int i = 0; i < 4; ++i) {
        if (i == idx) lv_obj_remove_flag(S->modView[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(S->modView[i], LV_OBJ_FLAG_HIDDEN);
    }
    setMatrixChecked(S->modViewSel, idx);
}

void modViewEvent(lv_event_t* e) {
    showModView(static_cast<int>(lv_buttonmatrix_get_selected_button(lv_event_get_target_obj(e))));
}

// Matrix: step a source/destination (user data: param id * 2 + (1 = next)).
void modStepEvent(lv_event_t* e) {
    intptr_t u = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    int id = static_cast<int>(u >> 1), dir = (u & 1) ? 1 : -1;
    int count = static_cast<int>(kParams[id].max) + 1;
    int v = (static_cast<int>(S->values[id]) + dir + count) % count;
    setParamUi(id, static_cast<float>(v));
}

void modAmountEvent(lv_event_t* e) {
    lv_obj_t* sl = lv_event_get_target_obj(e);
    int id = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    setParamUi(id, lv_slider_get_value(sl) / 100.0f);
    showParamReadout(id);
}

lv_obj_t* modStepper(lv_obj_t* parent, int id, int labelW) {
    button(parent, LV_SYMBOL_LEFT, 28, 32, modStepEvent, reinterpret_cast<void*>(static_cast<intptr_t>(id * 2)));
    lv_obj_t* l = label(parent, "", fLabel, kText);
    lv_obj_set_width(l, labelW);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    S->paramLabel[id] = l;
    button(parent, LV_SYMBOL_RIGHT, 28, 32, modStepEvent, reinterpret_cast<void*>(static_cast<intptr_t>(id * 2 + 1)));
    return l;
}

lv_obj_t* modPanel(lv_obj_t* tab) {
    lv_obj_t* p = panel(tab, 472, 198);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(p, 4, 0);
    return p;
}

void buildMod(lv_obj_t* tab) {
    static const char* views[] = {"LFO 1", "LFO 2", "MATRIX", "PERFORM", ""};
    static const char* shapes[] = {"SIN", "TRI", "SAW", "SQR", "S&H", ""};
    S->modViewSel = buttonMatrix(tab, views, 472, 30, modViewEvent);

    // LFO 1
    lv_obj_t* p1 = modPanel(tab);
    S->lfoShape = buttonMatrix(p1, shapes, 250, 34, lfoShapeEvent);
    S->lfoChart = chart(p1, 190, 34, -110, 110, kLfoPts, S->lfoY, &S->lfoSer, kAccent);
    knob(p1, P_LFO_RATE, "RATE");
    knob(p1, P_LFO_SYNC, "SYNC");
    knob(p1, P_LFO_POS, "> POS");
    knob(p1, P_LFO_CUTOFF, "> CUTOFF");
    knob(p1, P_LFO_PITCH, "> PITCH");
    knob(p1, P_MENV_POS, "ENV>POS");
    lv_obj_t* n1 = label(p1, "MOD WHEEL ADDS VIBRATO. MORE ROUTINGS IN MATRIX.", fLabel, kDim);
    lv_obj_set_width(n1, 460);
    S->modView[0] = p1;

    // LFO 2: a matrix source
    lv_obj_t* p2 = modPanel(tab);
    S->lfo2Shape = buttonMatrix(p2, shapes, 250, 34, lfo2ShapeEvent);
    S->lfo2Chart = chart(p2, 190, 34, -110, 110, kLfoPts, S->lfo2Y, &S->lfo2Ser, kSignal);
    knob(p2, P_LFO2_RATE, "RATE");
    knob(p2, P_LFO2_SYNC, "SYNC");
    lv_obj_t* n2 = label(p2, "LFO 2 GOES WHERE YOU ROUTE IT: PICK LFO 2 AS A SOURCE IN MATRIX.", fLabel, kDim);
    lv_obj_set_width(n2, 300);
    lv_label_set_long_mode(n2, LV_LABEL_LONG_WRAP);
    S->modView[1] = p2;

    // MATRIX: 4 slots
    lv_obj_t* pm = panel(tab, 472, 198);
    lv_obj_set_flex_flow(pm, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(pm, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int k = 0; k < kModSlots; ++k) {
        const int base = P_M1_SRC + 3 * k;
        lv_obj_t* r = row(pm, 462, 40);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t* num = label(r, "", fBig, kDim);
        lv_label_set_text_fmt(num, "%d", k + 1);
        modStepper(r, base, 82);
        modStepper(r, base + 1, 66);
        lv_obj_t* sl = lv_slider_create(r);
        lv_obj_set_size(sl, 74, 10);
        lv_slider_set_range(sl, -100, 100);
        lv_slider_set_mode(sl, LV_SLIDER_MODE_SYMMETRICAL);
        lv_obj_set_style_radius(sl, 0, 0);
        lv_obj_set_style_radius(sl, 0, LV_PART_INDICATOR);
        lv_obj_set_style_radius(sl, 0, LV_PART_KNOB);
        lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(sl, C(kLine2), 0);
        lv_obj_set_style_bg_color(sl, C(kText), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(sl, C(kSignal), LV_PART_KNOB);
        lv_obj_set_style_pad_all(sl, 5, LV_PART_KNOB);
        lv_obj_set_style_shadow_width(sl, 0, LV_PART_KNOB);
        lv_obj_set_ext_click_area(sl, 12);
        lv_obj_add_event_cb(sl, modAmountEvent, LV_EVENT_VALUE_CHANGED, reinterpret_cast<void*>(static_cast<intptr_t>(base + 2)));
        lv_obj_t* val = label(r, "", fLabel, kText);
        lv_obj_set_width(val, 34);
        lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_user_data(sl, val);
        S->paramSlider[base + 2] = sl;
        for (int j = 0; j < 3; ++j) showParamReadout(base + j);
    }
    S->modView[2] = pm;

    // PERFORM
    lv_obj_t* pp = modPanel(tab);
    knob(pp, P_VEL_AMP, "VEL>AMP");
    knob(pp, P_GLIDE, "GLIDE");
    knob(pp, P_BEND_RANGE, "BEND");
    knob(pp, P_SPREAD, "SPREAD");
    knob(pp, P_VOLUME, "PATCH VOL");
    S->modView[3] = pp;

    drawLfo();
    drawLfo2();
    showModView(0);
}

// KEYS ----------------------------------------------------------------------

int keyNote(lv_obj_t* key) {
    return 12 * (S->octave + 1) + static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(key)));
}

void keyEvent(lv_event_t* e) {
    lv_obj_t* key = lv_event_get_target_obj(e);
    int note = keyNote(key);
    if (note < 0 || note > 127) return;
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        if (S->hold) {
            S->latched[note] = !S->latched[note];
            send(S->latched[note] ? CmdType::NoteOn : CmdType::NoteOff, note, 100);
            if (S->latched[note]) lv_obj_add_state(key, LV_STATE_CHECKED);
            else lv_obj_remove_state(key, LV_STATE_CHECKED);
        } else {
            send(CmdType::NoteOn, note, 100);
        }
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && !S->hold) {
        send(CmdType::NoteOff, note, 0);
    }
}

void allNotesOffUi() {
    send(CmdType::AllNotesOff, 0, 0);
    std::fill(std::begin(S->latched), std::end(S->latched), false);
    for (lv_obj_t* k : S->keys) lv_obj_remove_state(k, LV_STATE_CHECKED);
}

void showOctave() {
    if (S->octLabel) lv_label_set_text_fmt(S->octLabel, "C%d", S->octave);
    for (int i = 0; i < 3; ++i)
        if (S->cLabels[i]) lv_label_set_text_fmt(S->cLabels[i], "C%d", S->octave + i);
}

void octaveEvent(lv_event_t* e) {
    int dir = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    allNotesOffUi();
    S->octave = std::clamp(S->octave + dir, 0, 7);
    showOctave();
    rememberView("ui.octave", S->octave);
}

void holdEvent(lv_event_t*) {
    S->hold = !S->hold;
    setButtonOn(S->holdBtn, S->hold);
    if (!S->hold) allNotesOffUi();
}

void allOffEvent(lv_event_t*) { allNotesOffUi(); }

void arpModeEvent(lv_event_t* e) {
    setParamUi(P_ARP_MODE, static_cast<float>(lv_buttonmatrix_get_selected_button(lv_event_get_target_obj(e))));
}

void arpHoldEvent(lv_event_t*) { setParamUi(P_ARP_HOLD, S->values[P_ARP_HOLD] >= 0.5f ? 0.0f : 1.0f); }

void showTempo() {
    if (!S->tempoLabel || !S->cfg.status) return;
    bool ext = S->cfg.status->extClock.load();
    float bpm = ext ? S->cfg.status->bpmNow.load() : S->cfg.status->tempo.load();
    if (!ext) {   // changed from the web app: remember it
        std::string t = std::to_string(static_cast<int>(std::lround(bpm)));
        auto it = S->stateKv.find("tempo");
        if (it == S->stateKv.end() ? std::lround(bpm) != 120 : it->second != t) markState("tempo", t);
    }
    if (bpm == S->tempoShown && ext == S->extShown) return;
    S->tempoShown = bpm;
    S->extShown = ext;
    if (std::fabs(bpm - std::round(bpm)) < 0.05f) lv_label_set_text_fmt(S->tempoLabel, "%d", static_cast<int>(std::lround(bpm)));
    else lv_label_set_text_fmt(S->tempoLabel, "%.1f", bpm);
    lv_label_set_text(S->tempoSrc, ext ? "BPM  MIDI CLOCK" : "BPM  INTERNAL");
    lv_obj_set_style_text_color(S->tempoSrc, C(ext ? kSignal : kDim), 0);
}

void tempoStep(lv_event_t* e) {
    if (!S->cfg.status) return;
    int dir = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    float t = std::clamp(std::round(S->cfg.status->tempo.load()) + dir, 30.0f, 300.0f);
    S->cfg.status->tempo.store(t);
    markState("tempo", std::to_string(static_cast<int>(t)));
    showTempo();
}

void buildKeys(lv_obj_t* tab) {
    // Arpeggiator + tempo
    static const char* modes[] = {"OFF", "UP", "DOWN", "UP/DN", "RAND", "PLAYED", ""};
    lv_obj_t* top = row(tab, 472, 34);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    S->arpMode = buttonMatrix(top, modes, 300, 32, arpModeEvent);
    lv_obj_t* tm = button(top, LV_SYMBOL_MINUS, 30, 30, tempoStep, reinterpret_cast<void*>(static_cast<intptr_t>(-1)));
    lv_obj_add_event_cb(tm, tempoStep, LV_EVENT_LONG_PRESSED_REPEAT, reinterpret_cast<void*>(static_cast<intptr_t>(-1)));
    lv_obj_t* tbox = lv_obj_create(top);
    lv_obj_remove_style_all(tbox);
    lv_obj_set_size(tbox, 92, 34);
    lv_obj_remove_flag(tbox, LV_OBJ_FLAG_SCROLLABLE);
    S->tempoLabel = label(tbox, "", fBig, kText);
    lv_obj_align(S->tempoLabel, LV_ALIGN_TOP_MID, 0, -4);
    S->tempoSrc = label(tbox, "", fLabel, kDim);
    lv_obj_set_style_text_font(S->tempoSrc, fSmall, 0);
    lv_obj_set_style_text_letter_space(S->tempoSrc, 0, 0);
    lv_obj_align(S->tempoSrc, LV_ALIGN_BOTTOM_MID, 0, 3);
    lv_obj_t* tp = button(top, LV_SYMBOL_PLUS, 30, 30, tempoStep, reinterpret_cast<void*>(static_cast<intptr_t>(1)));
    lv_obj_add_event_cb(tp, tempoStep, LV_EVENT_LONG_PRESSED_REPEAT, reinterpret_cast<void*>(static_cast<intptr_t>(1)));

    lv_obj_t* knobs = row(tab, 472, 72);
    knob(knobs, P_ARP_RATE, "RATE");
    knob(knobs, P_ARP_OCT, "OCTAVES");
    knob(knobs, P_ARP_GATE, "GATE");
    knob(knobs, P_CHORD, "CHORD");
    lv_obj_t* col = lv_obj_create(knobs);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, 104, 72);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    S->arpHoldBtn = button(col, "ARP HOLD", 100, 32, arpHoldEvent);
    button(col, "XY PAD", 100, 32, openXy);

    lv_obj_t* bar = row(tab, 472, 28);
    button(bar, LV_SYMBOL_MINUS " OCT", 80, 26, octaveEvent, reinterpret_cast<void*>(static_cast<intptr_t>(-1)));
    S->octLabel = label(bar, "C3", fMed, kText);
    button(bar, LV_SYMBOL_PLUS " OCT", 80, 26, octaveEvent, reinterpret_cast<void*>(static_cast<intptr_t>(1)));
    S->holdBtn = button(bar, "LATCH", 80, 26, holdEvent);
    button(bar, "ALL OFF", 90, 26, allOffEvent);

    const int kbH = 80;
    lv_obj_t* kb = lv_obj_create(tab);
    lv_obj_remove_style_all(kb);
    lv_obj_set_size(kb, 472, kbH);
    lv_obj_remove_flag(kb, LV_OBJ_FLAG_SCROLLABLE);

    static const int whiteSemis[] = {0, 2, 4, 5, 7, 9, 11};
    const int whites = 15;  // C .. C, two octaves
    const int ww = 472 / whites;
    auto styleKey = [](lv_obj_t* k, uint32_t bg) {
        lv_obj_remove_style_all(k);
        lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(k, C(bg), 0);
        lv_obj_set_style_bg_color(k, C(kSignal), LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(k, C(kSignal), LV_STATE_CHECKED);
        lv_obj_set_style_radius(k, 0, 0);
        lv_obj_set_style_border_width(k, 1, 0);
        lv_obj_set_style_border_color(k, C(kLine2), 0);
        lv_obj_add_flag(k, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(k, keyEvent, LV_EVENT_PRESSED, nullptr);
        lv_obj_add_event_cb(k, keyEvent, LV_EVENT_RELEASED, nullptr);
        lv_obj_add_event_cb(k, keyEvent, LV_EVENT_PRESS_LOST, nullptr);
    };
    for (int i = 0; i < whites; ++i) {
        lv_obj_t* k = lv_obj_create(kb);
        styleKey(k, kText);
        lv_obj_set_pos(k, i * ww, 0);
        lv_obj_set_size(k, ww, kbH);
        int semi = 12 * (i / 7) + whiteSemis[i % 7];
        lv_obj_set_user_data(k, reinterpret_cast<void*>(static_cast<intptr_t>(semi)));
        if (i % 7 == 0) {
            lv_obj_t* l = label(k, "", fLabel, kDim);
            S->cLabels[i / 7] = l;
            lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -2);
        }
        S->keys.push_back(k);
    }
    static const int blackAfter[] = {0, 1, 3, 4, 5};  // white index within octave
    static const int blackSemis[] = {1, 3, 6, 8, 10};
    for (int oct = 0; oct < 2; ++oct) {
        for (int j = 0; j < 5; ++j) {
            lv_obj_t* k = lv_obj_create(kb);
            styleKey(k, kBg);
            int x = (oct * 7 + blackAfter[j] + 1) * ww - 10;
            lv_obj_set_pos(k, x, 0);
            lv_obj_set_size(k, 20, kbH * 6 / 10);
            lv_obj_set_user_data(k, reinterpret_cast<void*>(static_cast<intptr_t>(12 * oct + blackSemis[j])));
            S->keys.push_back(k);
        }
    }
}

// PRESETS -------------------------------------------------------------------

void closeModal();
void openSaveAs(lv_event_t*);

void presetClick(lv_event_t* e) {
    int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    loadPresetUi(idx);
}

void rebuildPresetList() {
    if (!S->presetList || !S->cfg.bank) return;
    PresetBank& bank = *S->cfg.bank;
    lv_obj_clean(S->presetList);
    lv_obj_t* currentBtn = nullptr;
    for (int i = 0; i < bank.count(); ++i) {
        const bool fav = isFav(bank.name(i));
        if (S->favOnly && !fav && i != S->currentPreset) continue;
        char txt[64];
        std::snprintf(txt, sizeof txt, "%02d  %s", i, bank.name(i));
        lv_obj_t* b = lv_list_add_button(S->presetList, nullptr, txt);
        lv_obj_t* star = lv_button_create(b);   // tap the star to favourite
        lv_obj_remove_style_all(star);
        lv_obj_set_size(star, 34, 30);
        lv_obj_add_flag(star, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_align(star, LV_ALIGN_RIGHT_MID, 4, 0);
        lv_obj_t* sl = label(star, fav ? "\xE2\x98\x85" : "\xE2\x98\x86", fMed, fav ? kSignal : kLine2);
        lv_obj_set_style_text_font(sl, &lv_font_montserrat_16, 0);
        lv_label_set_text(sl, fav ? LV_SYMBOL_OK : LV_SYMBOL_PLUS);
        lv_obj_center(sl);
        lv_obj_add_event_cb(star, favToggle, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        if (i == S->currentPreset) currentBtn = b;
        lv_obj_set_height(b, 38);
        lv_obj_set_style_bg_color(b, C(kBg), 0);
        lv_obj_set_style_radius(b, 0, 0);
        lv_obj_set_style_text_font(b, fBody, 0);
        lv_obj_set_style_bg_color(b, C(kText), LV_STATE_CHECKED);
        lv_obj_set_style_text_color(b, C(kText), 0);
        lv_obj_set_style_text_color(b, C(kBg), LV_STATE_CHECKED);
        lv_obj_set_style_border_color(b, C(kLine), 0);
        if (i == S->currentPreset) lv_obj_add_state(b, LV_STATE_CHECKED);
        lv_obj_add_event_cb(b, presetClick, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    }
    S->listCount = bank.count();
    S->listCurrent = S->currentPreset;
    if (currentBtn) lv_obj_scroll_to_view(currentBtn, LV_ANIM_OFF);
}

std::string slugify(const std::string& name) {
    std::string s;
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c))) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else if (!s.empty() && s.back() != '-') s += '-';
    }
    while (!s.empty() && s.back() == '-') s.pop_back();
    return s.empty() ? "preset" : s;
}

void flashInfo(const char* msg) {
    if (S->presetInfo) lv_label_set_text(S->presetInfo, msg);
    else std::fprintf(stderr, "xowave: %s\n", msg);
}

bool savePreset(const std::string& name, int idx) {
    PresetBank& bank = *S->cfg.bank;
    std::string path;
    if (idx >= 0 && bank.file(idx)[0]) {
        path = bank.file(idx);
    } else {
        char prefix[16];
        std::snprintf(prefix, sizeof prefix, "%02d-", bank.count() + 1);
        path = S->cfg.presetsDir + "/" + prefix + slugify(name) + ".xowave";
        idx = bank.count();
    }
    if (!savePresetFile(path, name, S->values, *S->cfg.tables)) {
        flashInfo("Save failed:\ncan't write presets folder");
        return false;
    }
    bank.write(idx, name.c_str(), path.c_str(), S->values);
    S->currentPreset = idx;
    send(CmdType::Program, idx, 0);
    rebuildPresetList();
    flashInfo("Saved.");
    return true;
}

void saveEvent(lv_event_t* e) {
    int cur = S->currentPreset;
    if (cur < 0 || cur >= S->cfg.bank->count()) { openSaveAs(e); return; }
    savePreset(S->cfg.bank->name(cur), cur);
}

void modalBase(int w, int h) {
    closeModal();
    S->modal = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(S->modal);
    lv_obj_set_size(S->modal, kW, kH);
    lv_obj_set_style_bg_color(S->modal, C(0x000000), 0);
    lv_obj_set_style_bg_opa(S->modal, LV_OPA_70, 0);
    lv_obj_add_flag(S->modal, LV_OBJ_FLAG_CLICKABLE);  // swallow clicks behind
    lv_obj_t* box = panel(S->modal, w, h);
    lv_obj_center(box);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, C(kLine), 0);
    lv_obj_set_user_data(S->modal, box);
}

lv_obj_t* modalBox() { return static_cast<lv_obj_t*>(lv_obj_get_user_data(S->modal)); }

void closeModal() {
    if (S->modal) lv_obj_delete(S->modal);
    S->modal = nullptr;
    S->textArea = nullptr;
    S->btList = nullptr;
    S->btStatus = nullptr;
    S->updStatus = nullptr;
    S->updInstall = nullptr;
}

void saveAsKeyboard(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        std::string name = lv_textarea_get_text(S->textArea);
        while (!name.empty() && name.back() == ' ') name.pop_back();
        while (!name.empty() && name.front() == ' ') name.erase(name.begin());
        if (name.empty()) return;
        int existing = S->cfg.bank->find(name.c_str());
        closeModal();
        savePreset(name, existing);
    } else if (code == LV_EVENT_CANCEL) {
        closeModal();
    }
}

void openSaveAs(lv_event_t*) {
    modalBase(kW, kH);
    lv_obj_t* box = modalBox();
    lv_obj_set_size(box, kW, kH);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_t* t = label(box, "NAME THIS SOUND", fLabel, kDim);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 4, 4);
    S->textArea = lv_textarea_create(box);
    lv_textarea_set_one_line(S->textArea, true);
    lv_textarea_set_max_length(S->textArea, PresetBank::kNameLen - 1);
    int cur = S->currentPreset;
    lv_textarea_set_text(S->textArea, (cur >= 0 && cur < S->cfg.bank->count()) ? S->cfg.bank->name(cur) : "My Sound");
    lv_obj_set_size(S->textArea, 470, 40);
    lv_obj_align(S->textArea, LV_ALIGN_TOP_MID, 0, 26);
    lv_obj_add_state(S->textArea, LV_STATE_FOCUSED);
    lv_obj_set_scrollbar_mode(S->textArea, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(S->textArea, 0, 0);
    lv_obj_set_style_bg_color(S->textArea, C(kBg), 0);
    lv_obj_set_style_border_width(S->textArea, 2, 0);
    lv_obj_set_style_border_color(S->textArea, C(kText), 0);
    lv_obj_set_style_text_color(S->textArea, C(kText), 0);
    lv_obj_set_style_text_font(S->textArea, fMed, 0);
    lv_obj_set_style_bg_color(S->textArea, C(kSignal), LV_PART_CURSOR);
    lv_obj_set_style_border_color(S->textArea, C(kSignal), LV_PART_CURSOR);
    lv_obj_t* kb = lv_keyboard_create(box);
    lv_obj_set_size(kb, 472, 236);
    lv_obj_set_style_bg_color(kb, C(kBg), 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_pad_all(kb, 2, 0);
    lv_obj_set_style_pad_gap(kb, 3, 0);
    lv_obj_set_style_radius(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, C(kBg), LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_color(kb, C(kLine2), LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, C(kText), LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, fBody, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, C(kPanel2), static_cast<lv_style_selector_t>(LV_PART_ITEMS) | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, C(kText), static_cast<lv_style_selector_t>(LV_PART_ITEMS) | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(kb, C(kText), static_cast<lv_style_selector_t>(LV_PART_ITEMS) | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(kb, C(kBg), static_cast<lv_style_selector_t>(LV_PART_ITEMS) | LV_STATE_PRESSED);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, S->textArea);
    lv_obj_add_event_cb(kb, saveAsKeyboard, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(kb, saveAsKeyboard, LV_EVENT_CANCEL, nullptr);
}

void deleteConfirmed(lv_event_t*) {
    int idx = S->confirmDeleteIdx;
    closeModal();
    PresetBank& bank = *S->cfg.bank;
    if (idx < 0 || idx >= bank.count()) return;
    if (bank.file(idx)[0]) std::remove(bank.file(idx));
    bank.remove(idx);
    if (S->currentPreset == idx) S->currentPreset = -1;
    else if (S->currentPreset > idx) --S->currentPreset;
    rebuildPresetList();
    flashInfo("Deleted.");
}

void modalCancel(lv_event_t*) { closeModal(); }

void deleteEvent(lv_event_t*) {
    int cur = S->currentPreset;
    if (cur < 0 || cur >= S->cfg.bank->count()) { flashInfo("Pick a preset first."); return; }
    S->confirmDeleteIdx = cur;
    modalBase(320, 140);
    lv_obj_t* box = modalBox();
    lv_obj_t* l = label(box, "", fMed, kText);
    lv_label_set_text_fmt(l, "Delete \"%s\"?", S->cfg.bank->name(cur));
    lv_obj_set_width(l, 300);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_t* c = button(box, "CANCEL", 130, 44, modalCancel);
    lv_obj_align(c, LV_ALIGN_BOTTOM_LEFT, 6, -6);
    lv_obj_t* d = button(box, LV_SYMBOL_TRASH " DELETE", 130, 44, deleteConfirmed);
    lv_obj_set_style_bg_color(d, C(kSignal), 0);
    lv_obj_set_style_border_color(d, C(kSignal), 0);
    lv_obj_align(d, LV_ALIGN_BOTTOM_RIGHT, -6, -6);
}

// Undo / compare ------------------------------------------------------------

std::array<float, P_COUNT> currentValues() {
    std::array<float, P_COUNT> a;
    for (int i = 0; i < P_COUNT; ++i) a[i] = S->values[i];
    return a;
}

void applyValues(const std::array<float, P_COUNT>& a) {
    S->applying = true;
    for (int i = 0; i < P_COUNT; ++i) setParamUi(i, a[i]);
    S->applying = false;
    for (auto& k : S->knobs) updateKnob(*k);
    for (int i = 0; i < P_COUNT; ++i) refreshViews(i);
}

void showUndoCompare() {
    if (S->undoBtn) {
        if (S->undo.empty()) lv_obj_add_state(S->undoBtn, LV_STATE_DISABLED);
        else lv_obj_remove_state(S->undoBtn, LV_STATE_DISABLED);
    }
    setButtonOn(S->compareBtn, S->comparing, kSignal);
    if (S->presetLabel) lv_obj_set_style_text_color(S->presetLabel, C(S->comparing ? kSignal : kText), 0);
}

// The sound as it was before the latest edits goes on the undo stack.
void pushUndo() {
    if (!S->stableSet) { S->stable = currentValues(); S->stableSet = true; return; }
    if (S->stable == currentValues() && !S->undo.empty() && S->undo.back() == S->stable) return;
    S->undo.push_back(S->stable);
    if (S->undo.size() > 40) S->undo.erase(S->undo.begin());
    S->stable = currentValues();
    showUndoCompare();
}

// Called ~20x a second: edits that settle for 1.2 s become one undo step.
void trackUndo() {
    if (S->comparing) return;
    if (!S->stableSet) { S->stable = currentValues(); S->stableSet = true; return; }
    if (tickMs() - S->lastChange < 1200) return;
    auto now = currentValues();
    if (now == S->stable) return;
    S->undo.push_back(S->stable);
    if (S->undo.size() > 40) S->undo.erase(S->undo.begin());
    S->stable = now;
    showUndoCompare();
}

void undoEvent(lv_event_t*) {
    if (S->comparing) endCompare();
    auto now = currentValues();
    if (now != S->stable) { applyValues(S->stable); showUndoCompare(); return; }   // unsettled edits first
    if (S->undo.empty()) return;
    S->stable = S->undo.back();
    S->undo.pop_back();
    applyValues(S->stable);
    showUndoCompare();
}

void endCompare() {
    if (!S->comparing) return;
    S->comparing = false;
    showUndoCompare();
}

void compareEvent(lv_event_t*) {
    PresetBank* bank = S->cfg.bank;
    if (!bank || S->currentPreset < 0 || S->currentPreset >= bank->count()) return;
    if (!S->comparing) {      // hear the saved preset
        S->stash = currentValues();
        float v[P_COUNT];
        bank->read(S->currentPreset, v);
        std::array<float, P_COUNT> saved;
        for (int i = 0; i < P_COUNT; ++i) saved[i] = paramClamp(i, v[i], S->numTables);
        S->comparing = true;
        applyValues(saved);
    } else {                  // back to the edited sound
        S->comparing = false;
        applyValues(S->stash);
    }
    showUndoCompare();
}

// Favourites ------------------------------------------------------------------

bool isFav(const char* name) {
    auto it = S->stateKv.find(std::string("fav.") + name);
    return it != S->stateKv.end() && it->second == "1";
}

void favToggle(lv_event_t* e) {
    int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (!S->cfg.bank || idx >= S->cfg.bank->count()) return;
    std::string key = std::string("fav.") + S->cfg.bank->name(idx);
    if (isFav(S->cfg.bank->name(idx))) { S->stateKv.erase(key); S->stateDirty = true; S->stateSaveAt = tickMs() + 500; }
    else markState(key, "1");
    S->listCount = -1;   // rebuild
}

void favOnlyEvent(lv_event_t*) {
    S->favOnly = !S->favOnly;
    setButtonOn(S->favBtn, S->favOnly);
    S->listCount = -1;
}

// XY pad ------------------------------------------------------------------------

const int kXyChoices[] = {P_A_POS,  P_B_POS,     P_CUTOFF,   P_RESO,      P_FENV,      P_DRIVE,     P_LFO_RATE,
                          P_LFO_POS, P_LFO_CUTOFF, P_A_LEVEL,  P_B_LEVEL,   P_SUB_LEVEL, P_NOISE,     P_CHORUS_MIX,
                          P_DELAY_MIX, P_DELAY_FB, P_REVERB_MIX, P_REVERB_SIZE, P_FX_DRIVE, P_GLIDE};
constexpr int kNumXy = sizeof(kXyChoices) / sizeof(kXyChoices[0]);
constexpr int kXyW = 368, kXyH = 300;

std::string shortName(int id) {
    std::string n = kParams[id].name;
    for (char& c : n) c = (c == '_') ? ' ' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return n;
}

void showXy() {
    if (!S->xyDot) return;
    float nx = paramToNormalized(S->xyParam[0], S->values[S->xyParam[0]], S->numTables);
    float ny = paramToNormalized(S->xyParam[1], S->values[S->xyParam[1]], S->numTables);
    lv_obj_set_pos(S->xyDot, static_cast<int32_t>(nx * (kXyW - 1)) - 14, static_cast<int32_t>((1 - ny) * (kXyH - 1)) - 14);
    for (int a = 0; a < 2; ++a) {
        char val[24];
        paramFormat(S->xyParam[a], S->values[S->xyParam[a]], val, sizeof val, S->tableNames.data(), S->numTables);
        lv_label_set_text_fmt(S->xyName[a], "%s\n%s", shortName(S->xyParam[a]).c_str(), val);
    }
}

void xyTouch(lv_event_t* e) {
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_target_obj(e), &a);
    float nx = std::clamp(static_cast<float>(pt.x - a.x1) / (kXyW - 1), 0.0f, 1.0f);
    float ny = std::clamp(1.0f - static_cast<float>(pt.y - a.y1) / (kXyH - 1), 0.0f, 1.0f);
    setParamUi(S->xyParam[0], paramFromNormalized(S->xyParam[0], nx, S->numTables));
    setParamUi(S->xyParam[1], paramFromNormalized(S->xyParam[1], ny, S->numTables));
    showXy();
}

void xyPick(lv_event_t* e) {
    intptr_t u = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    int axis = static_cast<int>(u >> 1), dir = (u & 1) ? 1 : -1;
    int cur = 0;
    for (int i = 0; i < kNumXy; ++i) if (kXyChoices[i] == S->xyParam[axis]) cur = i;
    int next = (cur + dir + kNumXy) % kNumXy;
    if (kXyChoices[next] == S->xyParam[1 - axis]) next = (next + dir + kNumXy) % kNumXy;   // never both the same
    S->xyParam[axis] = kXyChoices[next];
    markState(axis ? "ui.xy_y" : "ui.xy_x", kParams[S->xyParam[axis]].name);
    showXy();
}

void xyClose(lv_event_t*) {
    if (S->xyPad) lv_obj_delete(S->xyPad);
    S->xyPad = S->xyDot = nullptr;
    S->xyName[0] = S->xyName[1] = nullptr;
}

void openXy(lv_event_t*) {
    for (int a = 0; a < 2; ++a) {
        auto it = S->stateKv.find(a ? "ui.xy_y" : "ui.xy_x");
        if (it != S->stateKv.end()) { int id = findParam(it->second); if (id >= 0) S->xyParam[a] = id; }
    }
    S->xyPad = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(S->xyPad);
    lv_obj_set_size(S->xyPad, kW, kH);
    lv_obj_set_style_bg_color(S->xyPad, C(kBg), 0);
    lv_obj_set_style_bg_opa(S->xyPad, LV_OPA_COVER, 0);
    lv_obj_add_flag(S->xyPad, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(S->xyPad, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* pad = lv_obj_create(S->xyPad);
    lv_obj_remove_style_all(pad);
    lv_obj_set_size(pad, kXyW, kXyH);
    lv_obj_set_pos(pad, 6, 10);
    lv_obj_set_style_border_width(pad, 2, 0);
    lv_obj_set_style_border_color(pad, C(kText), 0);
    lv_obj_add_flag(pad, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(pad, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(pad, LV_OBJ_FLAG_SCROLL_CHAIN);
    for (int g = 1; g < 4; ++g) {   // quarter grid
        lv_obj_t* v = lv_obj_create(pad);
        lv_obj_remove_style_all(v);
        lv_obj_set_size(v, 1, kXyH - 4);
        lv_obj_set_pos(v, kXyW * g / 4, 0);
        lv_obj_set_style_bg_color(v, C(kLine), 0);
        lv_obj_set_style_bg_opa(v, LV_OPA_COVER, 0);
        lv_obj_t* h = lv_obj_create(pad);
        lv_obj_remove_style_all(h);
        lv_obj_set_size(h, kXyW - 4, 1);
        lv_obj_set_pos(h, 0, kXyH * g / 4);
        lv_obj_set_style_bg_color(h, C(kLine), 0);
        lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
    }
    S->xyDot = lv_obj_create(pad);
    lv_obj_remove_style_all(S->xyDot);
    lv_obj_set_size(S->xyDot, 28, 28);
    lv_obj_set_style_radius(S->xyDot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(S->xyDot, C(kSignal), 0);
    lv_obj_set_style_bg_opa(S->xyDot, LV_OPA_COVER, 0);
    lv_obj_remove_flag(S->xyDot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(pad, xyTouch, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(pad, xyTouch, LV_EVENT_PRESSING, nullptr);

    for (int a = 0; a < 2; ++a) {   // axis pickers
        int y = 10 + a * 112;
        lv_obj_t* t = label(S->xyPad, a ? "Y  " LV_SYMBOL_UP : "X  " LV_SYMBOL_RIGHT, fLabel, kDim);
        lv_obj_set_pos(t, 384, y);
        S->xyName[a] = label(S->xyPad, "", fLabel, kText);
        lv_obj_set_width(S->xyName[a], 92);
        lv_label_set_long_mode(S->xyName[a], LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(S->xyName[a], 384, y + 18);
        lv_obj_t* l = button(S->xyPad, LV_SYMBOL_LEFT, 42, 34, xyPick, reinterpret_cast<void*>(static_cast<intptr_t>(a * 2)));
        lv_obj_set_pos(l, 384, y + 62);
        lv_obj_t* r = button(S->xyPad, LV_SYMBOL_RIGHT, 42, 34, xyPick, reinterpret_cast<void*>(static_cast<intptr_t>(a * 2 + 1)));
        lv_obj_set_pos(r, 432, y + 62);
    }
    lv_obj_t* done = button(S->xyPad, "DONE", 90, 40, xyClose);
    lv_obj_set_pos(done, 384, 270);
    showXy();
}

// USB stick ------------------------------------------------------------------

namespace fs = std::filesystem;

std::string lowerExt(const fs::path& p) {
    std::string e = p.extension().string();
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

// Mount points under /media (the udev rule mounts sticks at /media/usb-<dev>).
std::vector<std::string> usbMounts() {
    std::vector<std::string> out;
    std::ifstream f("/proc/mounts");
    std::string dev, mnt, rest;
    while (f >> dev >> mnt && std::getline(f, rest))
        if (mnt.rfind("/media/", 0) == 0) out.push_back(mnt);
    return out;
}

void scanUsb() {
    S->usbFiles.clear();
    S->usbRoot.clear();
    for (const std::string& root : usbMounts()) {
        if (S->usbRoot.empty()) S->usbRoot = root;
        std::error_code ec;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end && S->usbFiles.size() < 150; it.increment(ec)) {
            if (it.depth() > 3) { it.disable_recursion_pending(); continue; }
            const fs::path& p = it->path();
            std::string fn = p.filename().string();
            if (fn.empty() || fn[0] == '.') { if (it->is_directory(ec)) it.disable_recursion_pending(); continue; }
            if (!it->is_regular_file(ec)) continue;
            std::string ext = lowerExt(p);
            if (ext == ".wav" || ext == ".xowave")
                S->usbFiles.push_back({p.string(), p.stem().string(), ext == ".wav"});
        }
    }
    std::sort(S->usbFiles.begin(), S->usbFiles.end(), [](const auto& a, const auto& b) {
        return a.wavetable != b.wavetable ? a.wavetable : a.name < b.name;
    });
}

void usbSay(const std::string& text) { if (S->usbStatus) lv_label_set_text(S->usbStatus, text.c_str()); }

bool importWavetable(const State::UsbFile& f, std::string& err) {
    Wavetable check;
    if (!loadWavetableWav(f.path, check, err)) return false;
    std::error_code ec;
    fs::create_directories(S->cfg.wavetablesDir, ec);
    fs::copy_file(f.path, fs::path(S->cfg.wavetablesDir) / fs::path(f.path).filename(),
                  fs::copy_options::overwrite_existing, ec);
    if (ec) { err = ec.message(); return false; }
    ++S->usbImportedTables;
    return true;
}

bool importPreset(const State::UsbFile& f, std::string& err) {
    KeyValues kv;
    if (!readKeyValues(f.path, kv)) { err = "can't read"; return false; }
    std::vector<std::string> warnings;
    Preset p = presetFromKeyValues(f.name, kv, *S->cfg.tables, warnings);
    PresetBank& bank = *S->cfg.bank;
    int idx = bank.find(p.name.c_str());
    std::string path;
    if (idx >= 0 && bank.file(idx)[0]) {
        path = bank.file(idx);
    } else {
        char prefix[16];
        std::snprintf(prefix, sizeof prefix, "%02d-", bank.count() + 1);
        path = S->cfg.presetsDir + "/" + prefix + slugify(p.name) + ".xowave";
        idx = bank.count();
    }
    if (!savePresetFile(path, p.name, p.values.data(), *S->cfg.tables)) { err = "can't write presets"; return false; }
    bank.write(idx, p.name.c_str(), path.c_str(), p.values.data());
    return true;
}

void showUsbRestart() {
    if (!S->usbRestart) return;
    if (S->usbImportedTables > 0) lv_obj_remove_flag(S->usbRestart, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(S->usbRestart, LV_OBJ_FLAG_HIDDEN);
}

void usbImportOne(lv_event_t* e) {
    size_t i = static_cast<size_t>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (i >= S->usbFiles.size()) return;
    const auto& f = S->usbFiles[i];
    std::string err;
    bool ok = f.wavetable ? importWavetable(f, err) : importPreset(f, err);
    if (!ok) usbSay("COULDN'T IMPORT " + f.name + ": " + err);
    else if (f.wavetable) usbSay("WAVETABLE " + f.name + " IMPORTED. RESTART TO LOAD IT.");
    else { usbSay("PRESET " + f.name + " IMPORTED."); rebuildPresetList(); }
    lv_obj_add_state(lv_event_get_target_obj(e), LV_STATE_DISABLED);
    showUsbRestart();
}

void usbImportAll(lv_event_t*) {
    int tables = 0, presets = 0, failed = 0;
    for (const auto& f : S->usbFiles) {
        std::string err;
        bool ok = f.wavetable ? importWavetable(f, err) : importPreset(f, err);
        if (!ok) ++failed;
        else if (f.wavetable) ++tables;
        else ++presets;
    }
    rebuildPresetList();
    char msg[160];
    std::snprintf(msg, sizeof msg, "IMPORTED %d WAVETABLE(S), %d PRESET(S)%s%s", tables, presets,
                  failed ? ", SOME FAILED" : "", tables ? ". RESTART TO LOAD THE WAVETABLES." : ".");
    usbSay(msg);
    showUsbRestart();
}

void usbBackup(lv_event_t*) {
    if (S->usbRoot.empty()) { usbSay("NO USB STICK."); return; }
    fs::path dir = fs::path(S->usbRoot) / "XO-WAVE" / "presets";
    std::error_code ec;
    fs::create_directories(dir, ec);
    int n = 0;
    PresetBank& bank = *S->cfg.bank;
    for (int i = 0; i < bank.count(); ++i) {
        float v[P_COUNT];
        bank.read(i, v);
        char name[64];
        std::snprintf(name, sizeof name, "%02d-%s.xowave", i + 1, slugify(bank.name(i)).c_str());
        if (savePresetFile((dir / name).string(), bank.name(i), v, *S->cfg.tables)) ++n;
    }
    ::sync();
    usbSay("BACKED UP " + std::to_string(n) + " PRESETS TO XO-WAVE/PRESETS ON THE STICK. SAFE TO REMOVE.");
}

void fillUsbList() {
    if (!S->usbList) return;
    lv_obj_clean(S->usbList);
    for (size_t i = 0; i < S->usbFiles.size(); ++i) {
        const auto& f = S->usbFiles[i];
        lv_obj_t* r = row(S->usbList, 448, 34);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t* tag = label(r, f.wavetable ? "WAVETABLE" : "PRESET", fLabel, f.wavetable ? kSignal : kDim);
        lv_obj_set_width(tag, 80);
        lv_obj_t* n = label(r, f.name.c_str(), fSmall, kText);
        lv_obj_set_width(n, 250);
        lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
        button(r, "IMPORT", 84, 30, usbImportOne, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    }
    if (S->usbFiles.empty())
        usbSay(S->usbRoot.empty() ? "NO USB STICK FOUND. PLUG ONE IN AND TAP RESCAN."
                                  : "NO .WAV WAVETABLES OR .XOWAVE PRESETS ON THE STICK.");
    else
        usbSay(std::to_string(S->usbFiles.size()) + " FILE(S) FOUND ON THE STICK. TAP IMPORT, OR IMPORT ALL.");
}

void usbRescan(lv_event_t*) {
    scanUsb();
    fillUsbList();
}

void openUsb(lv_event_t*) {
    modalBase(470, 310);
    lv_obj_t* box = modalBox();
    lv_obj_t* t = label(box, LV_SYMBOL_USB " USB STICK", fMed, kText);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 4, 6);
    lv_obj_t* rescan = button(box, LV_SYMBOL_REFRESH " RESCAN", 100, 32, usbRescan);
    lv_obj_align(rescan, LV_ALIGN_TOP_RIGHT, -76, 0);
    lv_obj_t* close = button(box, LV_SYMBOL_CLOSE, 66, 32, modalCancel);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -4, 0);
    S->usbStatus = label(box, "", fLabel, kDim);
    lv_obj_set_width(S->usbStatus, 450);
    lv_label_set_long_mode(S->usbStatus, LV_LABEL_LONG_WRAP);
    lv_obj_align(S->usbStatus, LV_ALIGN_TOP_LEFT, 4, 38);
    S->usbList = lv_obj_create(box);
    lv_obj_remove_style_all(S->usbList);
    lv_obj_set_size(S->usbList, 456, 182);
    lv_obj_align(S->usbList, LV_ALIGN_TOP_MID, 0, 66);
    lv_obj_set_flex_flow(S->usbList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(S->usbList, 2, 0);
    lv_obj_add_flag(S->usbList, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(S->usbList, LV_DIR_VER);
    lv_obj_t* bar = row(box, 456, 38);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    button(bar, "IMPORT ALL", 130, 34, usbImportAll);
    button(bar, LV_SYMBOL_SAVE " BACK UP PRESETS", 180, 34, usbBackup);
    S->usbRestart = button(bar, "RESTART", 100, 34, restartEvent);
    lv_obj_set_style_bg_color(S->usbRestart, C(kSignal), 0);
    lv_obj_set_style_border_color(S->usbRestart, C(kSignal), 0);
    lv_obj_set_style_text_color(S->usbRestart, C(kBg), 0);
    showUsbRestart();
    scanUsb();
    fillUsbList();
}

void buildPresets(lv_obj_t* tab) {
    S->presetList = lv_list_create(tab);
    lv_obj_set_size(S->presetList, 300, 236);
    lv_obj_set_style_bg_color(S->presetList, C(kPanel), 0);
    lv_obj_set_style_border_width(S->presetList, 0, 0);
    lv_obj_set_style_pad_all(S->presetList, 2, 0);
    lv_obj_set_style_pad_row(S->presetList, 2, 0);

    lv_obj_t* side = panel(tab, 164, 236);
    lv_obj_set_flex_flow(side, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(side, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(side, 5, 0);
    button(side, LV_SYMBOL_SAVE " SAVE", 150, 38, saveEvent);
    button(side, LV_SYMBOL_EDIT " SAVE AS", 150, 38, openSaveAs);
    lv_obj_t* r1 = row(side, 150, 38);
    lv_obj_set_flex_align(r1, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    button(r1, LV_SYMBOL_TRASH, 72, 38, deleteEvent);
    button(r1, LV_SYMBOL_USB " USB", 72, 38, openUsb);
    lv_obj_t* r2 = row(side, 150, 38);
    lv_obj_set_flex_align(r2, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    S->undoBtn = button(r2, "UNDO", 72, 38, undoEvent);
    S->compareBtn = button(r2, "COMPARE", 72, 38, compareEvent);
    S->favBtn = button(side, LV_SYMBOL_OK " FAVOURITES", 150, 38, favOnlyEvent);
    S->presetInfo = nullptr;
    showUndoCompare();
    rebuildPresetList();
}

// SYSTEM --------------------------------------------------------------------

std::string readFirstLine(const char* path) {
    FILE* f = std::fopen(path, "r");
    if (!f) return "";
    char buf[128] = {};
    if (!std::fgets(buf, sizeof buf, f)) buf[0] = 0;
    std::fclose(f);
    std::string s = buf;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

std::string ipAddress() {
#ifdef __linux__
    ifaddrs* ifs = nullptr;
    std::string out;
    if (getifaddrs(&ifs) == 0) {
        for (ifaddrs* i = ifs; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
            if (std::strcmp(i->ifa_name, "lo") == 0) continue;
            char buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr, buf, sizeof buf);
            if (!out.empty()) out += "  ";
            out += buf;
        }
        freeifaddrs(ifs);
    }
    return out.empty() ? "offline" : out;
#else
    return "n/a";
#endif
}

void updateSystemInfo() {
    if (!S->sysInfo) return;
    EngineStatus* st = S->cfg.status;
    std::string temp = readFirstLine("/sys/class/thermal/thermal_zone0/temp");
    char tbuf[16] = "--";
    if (!temp.empty()) std::snprintf(tbuf, sizeof tbuf, "%.0fC", std::atof(temp.c_str()) / 1000.0);
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "%u HZ / %u FRAMES\n"
                  "%.0f%% AUDIO  /  %s\n"
                  "%u\n"
                  "%u EVENTS\n"
                  "%d PLAYING\n"
                  "%s\n"
                  "%s",
                  S->cfg.rate, S->cfg.period, st ? st->cpu.load() * 100.0 : 0.0, tbuf, st ? st->xruns.load() : 0u,
                  st ? st->midiEvents.load() : 0u, st ? st->voices.load() : 0, ipAddress().c_str(), S->cfg.version.c_str());
    lv_label_set_text(S->sysInfo, buf);
    if (S->btSpeakerLabel) {
        std::string bt = S->cfg.btDevice.empty() ? "NONE PAIRED"
                         : S->cfg.btDevice + (st && st->btConnected.load() ? "\nLINKED" : "\nWAITING FOR SPEAKER");
        lv_label_set_text(S->btSpeakerLabel, bt.c_str());
    }
}

// Runs a helper command off the UI thread.
void runJob(const std::string& cmd, bool parseDevices) {
    if (S->btState.load() == 1) return;
    S->btState = 1;
    std::thread([cmd, parseDevices] {
        std::string out;
        FILE* p = popen((cmd + " 2>&1").c_str(), "r");
        if (p) {
            char line[256];
            while (std::fgets(line, sizeof line, p)) out += line;
            pclose(p);
        } else {
            out = "could not run helper";
        }
        std::vector<BtDevice> found;
        int hidden = 0;
        if (parseDevices) {
            size_t pos = 0;
            while (pos < out.size()) {
                size_t nl = out.find('\n', pos);
                std::string l = out.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
                pos = nl == std::string::npos ? out.size() : nl + 1;
                if (l.rfind("Device ", 0) == 0 && l.size() > 25) {
                    std::string mac = l.substr(7, 17), name = l.substr(25), dashed = mac;
                    std::replace(dashed.begin(), dashed.end(), ':', '-');
                    if (name == dashed) { ++hidden; continue; }  // never sent a name: phones, beacons...
                    found.push_back({mac, name});
                }
            }
        }
        std::lock_guard<std::mutex> lock(S->btMutex);
        S->btFound = std::move(found);
        std::string last = out;
        while (!last.empty() && last.back() == '\n') last.pop_back();
        size_t cut = last.find_last_of('\n');
        S->btMessage = cut == std::string::npos ? last : last.substr(cut + 1);
        if (parseDevices && hidden > 0 && S->btMessage.rfind("ERROR", 0) != 0)
            S->btMessage = std::to_string(hidden) + " unnamed device(s) hidden.";
        S->btState = 2;
    }).detach();
}

void btPair(lv_event_t* e) {
    lv_obj_t* b = lv_event_get_target_obj(e);
    const char* kind = static_cast<const char*>(lv_event_get_user_data(e));
    const char* mac = static_cast<const char*>(lv_obj_get_user_data(b));
    if (!mac) return;
    if (S->btStatus) lv_label_set_text_fmt(S->btStatus, "Pairing %s as %s... XO-Wave restarts when done.", mac, kind);
    runJob(std::string("sudo -n /usr/local/bin/xowave-bt pair ") + mac + " " + kind, false);
}

void btScan(lv_event_t*) {
    if (S->btStatus) lv_label_set_text(S->btStatus, "Scanning 20 s - put the device in pairing mode...");
    runJob("sudo -n /usr/local/bin/xowave-bt scan", true);
}

void showBtResults() {
    std::lock_guard<std::mutex> lock(S->btMutex);
    if (!S->btList) return;
    lv_obj_clean(S->btList);
    static std::vector<std::string> macs;  // stable storage for user_data
    macs.clear();
    macs.reserve(S->btFound.size());
    for (const BtDevice& d : S->btFound) macs.push_back(d.mac);
    for (size_t i = 0; i < S->btFound.size(); ++i) {
        lv_obj_t* r = row(S->btList, 440, 40);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t* n = label(r, S->btFound[i].name.c_str(), fBody, kText);
        lv_obj_set_width(n, 220);
        lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
        lv_obj_t* a = button(r, LV_SYMBOL_AUDIO " SPEAKER", 104, 34, btPair, const_cast<char*>("audio"));
        lv_obj_set_user_data(a, const_cast<char*>(macs[i].c_str()));
        lv_obj_t* m = button(r, LV_SYMBOL_KEYBOARD " MIDI", 90, 34, btPair, const_cast<char*>("midi"));
        lv_obj_set_user_data(m, const_cast<char*>(macs[i].c_str()));
    }
    if (S->btStatus) {
        if (S->btFound.empty())
            lv_label_set_text(S->btStatus, S->btMessage.empty() ? "Nothing found. Is the device in pairing mode?"
                                                                : S->btMessage.c_str());
        else
            lv_label_set_text_fmt(S->btStatus, "%d found. Tap SPEAKER or MIDI to pair. %s",
                                  static_cast<int>(S->btFound.size()), S->btMessage.c_str());
    }
}

void openBluetooth(lv_event_t*) {
    modalBase(470, 310);
    lv_obj_t* box = modalBox();
    lv_obj_t* t = label(box, LV_SYMBOL_BLUETOOTH " BLUETOOTH", fMed, kAccent);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 4, 6);
    lv_obj_t* scan = button(box, LV_SYMBOL_REFRESH " SCAN", 100, 34, btScan);
    lv_obj_align(scan, LV_ALIGN_TOP_RIGHT, -96, 0);
    lv_obj_t* close = button(box, LV_SYMBOL_CLOSE, 80, 34, modalCancel);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -4, 0);
    S->btStatus = label(box, "Scan, then choose SPEAKER (audio out) or MIDI (controller).", fSmall, kDim);
    lv_obj_set_width(S->btStatus, 450);
    lv_label_set_long_mode(S->btStatus, LV_LABEL_LONG_WRAP);
    lv_obj_align(S->btStatus, LV_ALIGN_TOP_LEFT, 4, 40);
    S->btList = lv_obj_create(box);
    lv_obj_remove_style_all(S->btList);
    lv_obj_set_size(S->btList, 456, 224);
    lv_obj_align(S->btList, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(S->btList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(S->btList, 4, 0);
    lv_obj_add_flag(S->btList, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(S->btList, LV_DIR_VER);
}

void confirmAction(const char* question, const char* okText, lv_event_cb_t ok) {
    modalBase(320, 140);
    lv_obj_t* box = modalBox();
    lv_obj_t* l = label(box, question, fMed, kText);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_t* c = button(box, "CANCEL", 130, 44, modalCancel);
    lv_obj_align(c, LV_ALIGN_BOTTOM_LEFT, 6, -6);
    lv_obj_t* d = button(box, okText, 130, 44, ok);
    lv_obj_set_style_bg_color(d, C(kSignal), 0);
    lv_obj_set_style_border_color(d, C(kSignal), 0);
    lv_obj_align(d, LV_ALIGN_BOTTOM_RIGHT, -6, -6);
}

void doRestart(lv_event_t*) {
    closeModal();
    if (S->cfg.status) S->cfg.status->running = false;  // systemd starts us again
}

void doShutdown(lv_event_t*) {
    closeModal();
    lv_obj_t* l = label(lv_layer_top(), "Shutting down...\nWait for the green LED to stop.", fBig, kText);
    lv_obj_center(l);
    lv_refr_now(S->disp);
    int rc = std::system("sudo -n /usr/sbin/poweroff");
    (void)rc;
}

void restartEvent(lv_event_t*) { confirmAction("Restart XO-Wave?", "RESTART", doRestart); }
void shutdownEvent(lv_event_t*) { confirmAction("Shut down the Pi?", LV_SYMBOL_POWER " OFF", doShutdown); }
void startCalibration(lv_event_t*);

// SETTINGS ------------------------------------------------------------------

void markState(const std::string& key, const std::string& value) {
    S->stateKv[key] = value;
    S->stateDirty = true;
    S->stateSaveAt = tickMs() + 500;
}

void needsRestart() { if (S->restartBanner) lv_obj_remove_flag(S->restartBanner, LV_OBJ_FLAG_HIDDEN); }

// A labelled "-  value  +" control.
struct Stepper {
    lv_obj_t* value = nullptr;
    void (*change)(Stepper*, int) = nullptr;
};
std::vector<std::unique_ptr<Stepper>> gSteppers;

void stepperEvent(lv_event_t* e) {
    Stepper* st = static_cast<Stepper*>(lv_event_get_user_data(e));
    lv_obj_t* b = lv_event_get_target_obj(e);
    st->change(st, static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(b))));
}

Stepper* stepper(lv_obj_t* parent, const char* name, void (*change)(Stepper*, int)) {
    lv_obj_t* r = row(parent, 346, 40);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 6, 0);
    lv_obj_t* n = label(r, name, fLabel, kDim);
    lv_obj_set_width(n, 96);
    auto st = std::make_unique<Stepper>();
    st->change = change;
    lv_obj_t* minus = button(r, LV_SYMBOL_MINUS, 40, 34, nullptr);
    lv_obj_set_user_data(minus, reinterpret_cast<void*>(static_cast<intptr_t>(-1)));
    lv_obj_add_event_cb(minus, stepperEvent, LV_EVENT_CLICKED, st.get());
    st->value = label(r, "", fMed, kText);
    lv_obj_set_width(st->value, 140);
    lv_obj_set_style_text_align(st->value, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(st->value, LV_LABEL_LONG_DOT);
    lv_obj_t* plus = button(r, LV_SYMBOL_PLUS, 40, 34, nullptr);
    lv_obj_set_user_data(plus, reinterpret_cast<void*>(static_cast<intptr_t>(1)));
    lv_obj_add_event_cb(plus, stepperEvent, LV_EVENT_CLICKED, st.get());
    Stepper* raw = st.get();
    gSteppers.push_back(std::move(st));
    change(raw, 0);  // show initial value
    return raw;
}

std::string stateGet(const char* key, const char* def) {
    auto it = S->stateKv.find(key);
    return it == S->stateKv.end() ? def : it->second;
}

lv_obj_t* sectionTitle(lv_obj_t* parent, const char* title) {
    lv_obj_t* t = label(parent, title, fBig, kText);
    return t;
}

// MIDI
void midiChannelChange(Stepper* st, int dir) {
    int ch = std::clamp(S->cfg.status ? S->cfg.status->midiChannel.load() + dir : 0, 0, 16);
    if (S->cfg.status) S->cfg.status->midiChannel.store(ch);
    if (dir) markState("midi.channel", std::to_string(ch));
    if (ch == 0) lv_label_set_text(st->value, "OMNI");
    else lv_label_set_text_fmt(st->value, "CH %d", ch);
}

void learnParamChange(Stepper* st, int dir) {
    S->learnParam = (S->learnParam + dir + P_COUNT) % P_COUNT;
    std::string n = kParams[S->learnParam].name;
    for (char& c : n) c = (c == '_') ? ' ' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    lv_label_set_text(st->value, n.c_str());
}

void learnEvent(lv_event_t*) {
    if (!S->cfg.status) return;
    S->learning = !S->learning;
    S->cfg.status->learnedCc.store(-1);
    S->cfg.status->learnParam.store(S->learning ? S->learnParam : -1);
    if (S->learning) {
        setButtonOn(S->learnBtn, true, kSignal);
        lv_label_set_text(S->learnLabel, "MOVE A KNOB ON YOUR CONTROLLER...");
    } else {
        setButtonOn(S->learnBtn, false);
        lv_label_set_text(S->learnLabel, "");
    }
}

void pollLearn() {
    if (!S->learning || !S->cfg.status) return;
    int cc = S->cfg.status->learnedCc.load(std::memory_order_acquire);
    if (cc < 0) return;
    S->learning = false;
    S->cfg.status->learnedCc.store(-1);
    setButtonOn(S->learnBtn, false);
    lv_label_set_text_fmt(S->learnLabel, "CC %d  >  %s  (SAVED)", cc, kParams[S->learnParam].name);
    markState("cc." + std::to_string(cc), kParams[S->learnParam].name);
}

void refreshMidiInputs(lv_event_t*) {
    if (S->midiInputs) lv_label_set_text(S->midiInputs, "LOOKING...");
    std::thread([] {
        std::string out, list;
        FILE* p = popen("aconnect -i -l 2>/dev/null", "r");
        if (p) {
            char line[256];
            while (std::fgets(line, sizeof line, p)) out += line;
            pclose(p);
        }
        size_t pos = 0;
        while ((pos = out.find("client ", pos)) != std::string::npos) {
            size_t q1 = out.find('\'', pos), q2 = q1 == std::string::npos ? q1 : out.find('\'', q1 + 1);
            if (q2 == std::string::npos) break;
            std::string name = out.substr(q1 + 1, q2 - q1 - 1);
            pos = q2;
            if (name == "System" || name == "Midi Through" || name == "XO-Wave") continue;
            list += (list.empty() ? "" : "\n") + name;
        }
        std::lock_guard<std::mutex> lock(S->midiMutex);
        S->midiInputsText = list.empty() ? "NO MIDI INPUTS FOUND" : list;
        S->midiInputsReady = true;
    }).detach();
}

void buildMidiSection(lv_obj_t* p) {
    sectionTitle(p, "MIDI");
    stepper(p, "CHANNEL", midiChannelChange);
    stepper(p, "LEARN PARAM", learnParamChange);
    lv_obj_t* r = row(p, 346, 40);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 8, 0);
    S->learnBtn = button(r, "MIDI LEARN", 120, 34, learnEvent);
    S->learnLabel = label(r, "", fLabel, kSignal);
    lv_obj_set_width(S->learnLabel, 210);
    lv_label_set_long_mode(S->learnLabel, LV_LABEL_LONG_WRAP);
    lv_obj_t* r2 = row(p, 346, 30);
    lv_obj_set_flex_align(r2, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    label(r2, "CONNECTED INPUTS", fLabel, kDim);
    button(r2, LV_SYMBOL_REFRESH, 40, 28, refreshMidiInputs);
    S->midiInputs = label(p, "TAP REFRESH", fSmall, kText);
    lv_obj_set_width(S->midiInputs, 340);
    lv_label_set_long_mode(S->midiInputs, LV_LABEL_LONG_WRAP);
}

// BLUETOOTH
void btLatencyChange(Stepper* st, int dir) {
    int ms = std::stoi(stateGet("bluetooth.latency_ms", "150"));
    if (dir) {
        ms = std::clamp(ms + dir * 25, 50, 400);
        markState("bluetooth.latency_ms", std::to_string(ms));
        needsRestart();
    }
    lv_label_set_text_fmt(st->value, "%d MS", ms);
}

void doForgetSpeaker(lv_event_t*) {
    closeModal();
    if (S->cfg.btDevice.empty()) return;
    runJob("sudo -n /usr/local/bin/xowave-bt forget " + S->cfg.btDevice, false);
    lv_label_set_text(S->btSpeakerLabel, "FORGETTING... XO-WAVE WILL RESTART");
}

void forgetSpeakerEvent(lv_event_t*) {
    if (S->cfg.btDevice.empty()) return;
    confirmAction("Forget the speaker?", "FORGET", doForgetSpeaker);
}

void buildBluetoothSection(lv_obj_t* p) {
    sectionTitle(p, "BLUETOOTH");
    label(p, "SPEAKER / HEADPHONES", fLabel, kDim);
    S->btSpeakerLabel = label(p, "", fSmall, kText);
    lv_obj_t* r = row(p, 346, 42);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 8, 0);
    button(r, LV_SYMBOL_BLUETOOTH " SCAN & PAIR", 170, 36, openBluetooth);
    lv_obj_t* f = button(r, "FORGET", 100, 36, forgetSpeakerEvent);
    lv_obj_set_style_border_color(f, C(kSignal), 0);
    lv_obj_set_style_text_color(f, C(kSignal), 0);
    stepper(p, "LATENCY", btLatencyChange);
    lv_obj_t* note = label(p, "BLUETOOTH ADDS ~150-250 MS. PLAY FROM THE JACK.", fLabel, kDim);
    lv_obj_set_width(note, 340);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
}

// AUDIO
void bufferChange(Stepper* st, int dir) {
    static const int sizes[] = {64, 128, 256, 480, 512, 1024};
    const int n = 6;
    int cur = std::stoi(stateGet("audio.period", std::to_string(S->cfg.period).c_str()));
    int i = 0;
    for (int k = 0; k < n; ++k) if (std::abs(sizes[k] - cur) < std::abs(sizes[i] - cur)) i = k;
    if (dir) {
        i = std::clamp(i + dir, 0, n - 1);
        markState("audio.period", std::to_string(sizes[i]));
        needsRestart();
    }
    int periods = static_cast<int>(std::max(2u, S->cfg.periods));
    lv_label_set_text_fmt(st->value, "%d x %d  %.0f MS", sizes[i], periods, 1000.0 * sizes[i] * periods / S->cfg.rate);
}

void voicesChange(Stepper* st, int dir) {
    int v = std::stoi(stateGet("synth.voices", "8"));
    if (dir) {
        v = std::clamp(v + dir * 2, 2, 16);
        markState("synth.voices", std::to_string(v));
        needsRestart();
    }
    lv_label_set_text_fmt(st->value, "%d VOICES", v);
}

void restartEvent(lv_event_t*);

// STUDIO: the link to a XOXO Studio over Ethernet
void updateLinkUi() {
    bool on = S->cfg.linkEnabled ? S->cfg.linkEnabled() : false;
    int st = S->cfg.status ? S->cfg.status->linkState.load() : 0;
    if (S->linkLabel) {
        std::string d = S->cfg.linkDescribe ? S->cfg.linkDescribe() : std::string("NOT AVAILABLE");
        for (char& c : d) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        lv_label_set_text(S->linkLabel, d.c_str());
        lv_obj_set_style_text_color(S->linkLabel, C(st == 2 ? kText : kDim), 0);
    }
    if (S->linkToggle) {
        lv_obj_t* l = lv_obj_get_child(S->linkToggle, 0);
        lv_label_set_text(l, on ? "LINK: ON" : "LINK: OFF");
        setButtonOn(S->linkToggle, on);
    }
    if (S->linkTop) {
        lv_obj_set_style_text_color(S->linkTop, C(st == 2 ? kText : (st == 1 ? kLine2 : kBg)), 0);
    }
}

void linkToggleEvent(lv_event_t*) {
    if (!S->cfg.linkSetEnabled || !S->cfg.linkEnabled) return;
    bool on = !S->cfg.linkEnabled();
    S->cfg.linkSetEnabled(on);
    markState("link.enabled", on ? "on" : "off");
    updateLinkUi();
}

void returnLevelChange(Stepper* st, int dir) {
    if (!S->cfg.status) return;
    int pct = static_cast<int>(std::lround(S->cfg.status->returnLevel.load() * 100));
    if (dir) {
        pct = std::clamp(pct + dir * 10, 0, 100);
        S->cfg.status->returnLevel.store(pct / 100.0f);
        char buf[16];
        std::snprintf(buf, sizeof buf, "%.2f", pct / 100.0);
        markState("link.return_level", buf);
    }
    if (pct == 0) lv_label_set_text(st->value, "OFF");
    else lv_label_set_text_fmt(st->value, "%d%%", pct);
}

void buildStudioSection(lv_obj_t* p) {
    sectionTitle(p, "STUDIO LINK");
    label(p, "XOXO STUDIO", fLabel, kDim);
    S->linkLabel = label(p, "", fSmall, kText);
    lv_obj_set_width(S->linkLabel, 340);
    lv_label_set_long_mode(S->linkLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(S->linkLabel, 34);
    S->linkToggle = button(p, "LINK: ON", 160, 34, linkToggleEvent);
    stepper(p, "STUDIO MIX", returnLevelChange);
    lv_obj_t* note = label(p,
        "STUDIO MIX: HOW LOUD THE STUDIO IS IN XO-WAVE'S HEADPHONES. MIDI GOES BOTH WAYS; BLUETOOTH PAUSES "
        "WHILE LINKED.", fLabel, kDim);
    lv_obj_set_width(note, 340);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    updateLinkUi();
}

void buildAudioSection(lv_obj_t* p) {
    sectionTitle(p, "AUDIO");
    label(p, "OUTPUT", fLabel, kDim);
    lv_obj_t* dev = label(p, S->cfg.audioDevice.c_str(), fSmall, kText);
    lv_obj_set_width(dev, 340);
    lv_label_set_long_mode(dev, LV_LABEL_LONG_DOT);
    stepper(p, "BUFFER", bufferChange);
    stepper(p, "POLYPHONY", voicesChange);
    lv_obj_t* note = label(p, "SMALLER = LOWER LATENCY. RAISE IT IF YOU HEAR CLICKS. THE PI'S OWN JACK ALWAYS RUNS 480.", fLabel, kDim);
    lv_obj_set_width(note, 340);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
}

// DISPLAY
void knobSpeedChange(Stepper* st, int dir) {
    static const char* names[] = {"FINE", "NORMAL", "FAST"};
    static const float speeds[] = {0.4f, 1.0f, 2.0f};
    int i = std::clamp(std::stoi(stateGet("ui.knob_speed", "1")) + dir, 0, 2);
    if (dir) markState("ui.knob_speed", std::to_string(i));
    S->knobSpeed = speeds[i];
    lv_label_set_text(st->value, names[i]);
}

void buildDisplaySection(lv_obj_t* p) {
    sectionTitle(p, "DISPLAY");
    stepper(p, "KNOB SPEED", knobSpeedChange);
    lv_obj_t* r = row(p, 346, 44);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 8, 0);
    lv_obj_t* n = label(r, "TOUCH", fLabel, kDim);
    lv_obj_set_width(n, 96);
    button(r, LV_SYMBOL_SETTINGS " CALIBRATE", 160, 36, startCalibration);
    lv_obj_t* note = label(p, "DRAG UP OR RIGHT ON A KNOB TO TURN IT UP. DRAG ON THE WAVETABLE TO SCAN IT.",
                           fLabel, kDim);
    lv_obj_set_width(note, 340);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
}

// SYSTEM
void doReboot(lv_event_t*) {
    closeModal();
    lv_obj_t* l = label(lv_layer_top(), "REBOOTING...", fBig, kText);
    lv_obj_center(l);
    lv_refr_now(S->disp);
    int rc = std::system("sudo -n /usr/sbin/reboot");
    (void)rc;
}
void rebootEvent(lv_event_t*) { confirmAction("Reboot the Pi?", "REBOOT", doReboot); }

// UPDATE: from the private GitHub repo with this Pi's read-only deploy key (xowave-update).
void updCheck(lv_event_t*) {
    if (S->updState.load() == 1 || S->updState.load() == 3) return;
    if (S->updStatus) lv_label_set_text(S->updStatus, "CHECKING GITHUB...");
    if (S->updInstall) lv_obj_add_flag(S->updInstall, LV_OBJ_FLAG_HIDDEN);
    S->updState = 1;
    std::thread([] {
        std::string out;
        FILE* f = popen("sudo -n /usr/local/bin/xowave-update check 2>&1", "r");
        if (f) { char line[256]; while (std::fgets(line, sizeof line, f)) out += line; pclose(f); }
        while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
        size_t nl = out.find_last_of('\n');
        if (nl != std::string::npos) out = out.substr(nl + 1);
        if (out.empty()) out = "ERROR: COULDN'T RUN THE UPDATER";
        std::lock_guard<std::mutex> lock(S->updMutex);
        S->updResult = out;
        S->updState = 2;
    }).detach();
}

void updInstallEvent(lv_event_t*) {
    int rc = std::system("sudo -n /usr/bin/systemctl start --no-block xowave-update.service");
    (void)rc;
    S->updState = 3;
    S->updNextLog = 0;
    if (S->updInstall) lv_obj_add_flag(S->updInstall, LV_OBJ_FLAG_HIDDEN);
    if (S->updStatus) lv_label_set_text(S->updStatus, "STARTING...");
}

void pollUpdate() {
    if (!S->updStatus) return;
    if (S->updState.load() == 2) {
        S->updState = 0;
        std::lock_guard<std::mutex> lock(S->updMutex);
        lv_label_set_text(S->updStatus, S->updResult.c_str());
        bool avail = S->updResult.rfind("UPDATE AVAILABLE", 0) == 0;
        if (avail) lv_obj_remove_flag(S->updInstall, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(S->updStatus, C(avail ? kSignal : kText), 0);
    } else if (S->updState.load() == 3 && static_cast<int32_t>(tickMs() - S->updNextLog) >= 0) {
        S->updNextLog = tickMs() + 1000;   // the updater's progress (XO-Wave restarts when it's done)
        std::string last;
        std::ifstream f("/var/lib/xowave/update.log");
        for (std::string line; std::getline(f, line);) if (!line.empty()) last = line;
        if (!last.empty()) lv_label_set_text(S->updStatus, last.c_str());
    }
}

void openUpdate(lv_event_t*) {
    modalBase(440, 220);
    lv_obj_t* box = modalBox();
    lv_obj_t* t = label(box, "UPDATE", fBig, kText);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 6, 0);
    lv_obj_t* v = label(box, "", fLabel, kDim);
    lv_label_set_text_fmt(v, "INSTALLED: %s", S->cfg.version.empty() ? "?" : S->cfg.version.c_str());
    lv_obj_align(v, LV_ALIGN_TOP_LEFT, 6, 38);
    S->updStatus = label(box, "", fSmall, kText);
    lv_obj_set_width(S->updStatus, 420);
    lv_label_set_long_mode(S->updStatus, LV_LABEL_LONG_WRAP);
    lv_obj_align(S->updStatus, LV_ALIGN_TOP_LEFT, 6, 64);
    lv_obj_t* note = label(box, "FROM XOXOAUDIOLABS/XO-WAVE ON GITHUB. IT BUILDS AND TESTS FIRST, AND PUTS THE OLD VERSION "
                                "BACK IF THE NEW ONE WON'T START. XO-WAVE RESTARTS WHEN IT'S DONE.", fLabel, kDim);
    lv_obj_set_width(note, 420);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_align(note, LV_ALIGN_TOP_LEFT, 6, 104);
    lv_obj_t* r = row(box, 420, 40);
    lv_obj_align(r, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    button(r, LV_SYMBOL_REFRESH " CHECK", 120, 38, updCheck);
    S->updInstall = button(r, LV_SYMBOL_DOWNLOAD " INSTALL", 140, 38, updInstallEvent);
    lv_obj_set_style_bg_color(S->updInstall, C(kSignal), 0);
    lv_obj_set_style_border_color(S->updInstall, C(kSignal), 0);
    lv_obj_set_style_text_color(S->updInstall, C(kBg), 0);
    lv_obj_add_flag(S->updInstall, LV_OBJ_FLAG_HIDDEN);
    button(r, "CLOSE", 100, 38, modalCancel);
    updCheck(nullptr);
}

void buildSystemSection(lv_obj_t* p) {
    sectionTitle(p, "SYSTEM");
    lv_obj_t* box = lv_obj_create(p);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 346, 132);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* keys = label(box, "AUDIO\nCPU / TEMP\nXRUNS\nMIDI\nVOICES\nIP\nVERSION", fLabel, kDim);
    lv_obj_set_style_text_line_space(keys, 6, 0);
    S->sysInfo = label(box, "", fSmall, kText);
    lv_obj_set_style_text_line_space(S->sysInfo, 4, 0);
    lv_obj_set_pos(S->sysInfo, 100, 0);
    lv_obj_t* r = row(p, 346, 44);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    button(r, "UPDATE", 84, 38, openUpdate);
    button(r, "RESTART", 84, 38, restartEvent);
    button(r, "REBOOT", 80, 38, rebootEvent);
    lv_obj_t* off = button(r, LV_SYMBOL_POWER " OFF", 80, 38, shutdownEvent);
    lv_obj_set_style_border_color(off, C(kSignal), 0);
    lv_obj_set_style_text_color(off, C(kSignal), 0);
}

void settingsNavEvent(lv_event_t* e) {
    uint32_t sel = lv_buttonmatrix_get_selected_button(lv_event_get_target_obj(e));
    rememberView("ui.settings", static_cast<int>(sel));
    for (uint32_t i = 0; i < 6; ++i) {
        if (i == sel) lv_obj_remove_flag(S->section[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(S->section[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (sel == 0 && S->midiInputs && std::strcmp(lv_label_get_text(S->midiInputs), "TAP REFRESH") == 0)
        refreshMidiInputs(nullptr);
}

void showSettingsSection(int idx) {
    setMatrixChecked(S->settingsNav, idx);
    rememberView("ui.settings", idx);
    for (int i = 0; i < 6; ++i) {
        if (i == idx) lv_obj_remove_flag(S->section[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(S->section[i], LV_OBJ_FLAG_HIDDEN);
    }
}

void buildSettings(lv_obj_t* tab) {
    static const char* nav[] = {"MIDI", "\n", "BLUETOOTH", "\n", "STUDIO", "\n", "AUDIO", "\n", "DISPLAY", "\n",
                                "SYSTEM", ""};
    lv_obj_t* left = panel(tab, 112, 236);
    S->settingsNav = buttonMatrix(left, nav, 102, 226, settingsNavEvent);
    lv_obj_center(S->settingsNav);

    lv_obj_t* right = panel(tab, 356, 236);
    lv_obj_set_style_pad_all(right, 5, 0);
    void (*builders[6])(lv_obj_t*) = {buildMidiSection, buildBluetoothSection, buildStudioSection,
                                      buildAudioSection, buildDisplaySection, buildSystemSection};
    for (int i = 0; i < 6; ++i) {
        lv_obj_t* sec = lv_obj_create(right);
        lv_obj_remove_style_all(sec);
        lv_obj_set_size(sec, 346, 226);
        lv_obj_set_flex_flow(sec, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(sec, 4, 0);
        lv_obj_remove_flag(sec, LV_OBJ_FLAG_SCROLLABLE);
        S->section[i] = sec;
        builders[i](sec);
    }

    S->restartBanner = button(right, LV_SYMBOL_REFRESH " RESTART TO APPLY", 220, 32, restartEvent);
    lv_obj_set_style_bg_color(S->restartBanner, C(kSignal), 0);
    lv_obj_set_style_border_color(S->restartBanner, C(kSignal), 0);
    lv_obj_set_style_text_color(S->restartBanner, C(kBg), 0);
    lv_obj_align(S->restartBanner, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_flag(S->restartBanner, LV_OBJ_FLAG_HIDDEN);
    showSettingsSection(0);
    updateSystemInfo();
}

// ---------------------------------------------------------------------------
// Touch calibration (reads raw evdev coordinates itself)

const int kCalPts[4][2] = {{40, 40}, {440, 40}, {40, 280}, {440, 280}};

void saveState();

void applyCalibration() {
#ifdef __linux__
    if (!S->touch) return;
    auto get = [](const char* k) { auto it = S->stateKv.find(k); return it == S->stateKv.end() ? std::string() : it->second; };
    if (get("touch.min_x").empty()) return;
    lv_evdev_set_swap_axes(S->touch, get("touch.swap") == "1");
    lv_evdev_set_calibration(S->touch, std::atoi(get("touch.min_x").c_str()), std::atoi(get("touch.min_y").c_str()),
                             std::atoi(get("touch.max_x").c_str()), std::atoi(get("touch.max_y").c_str()));
#endif
}

[[maybe_unused]] void finishCalibration(bool ok) {
    S->calibrating = false;
    if (S->calFd >= 0) close(S->calFd);
    S->calFd = -1;
    closeModal();
    if (!ok) return;
    int r[4][2];
    std::memcpy(r, S->calRaw, sizeof r);
    // Does raw X follow screen X, or screen Y?
    long dxx = std::labs((r[1][0] + r[3][0]) - (r[0][0] + r[2][0]));
    long dxy = std::labs((r[1][1] + r[3][1]) - (r[0][1] + r[2][1]));
    bool swap = dxy > dxx;
    if (swap) for (auto& p : r) std::swap(p[0], p[1]);
    float left = (r[0][0] + r[2][0]) / 2.0f, right = (r[1][0] + r[3][0]) / 2.0f;
    float top = (r[0][1] + r[1][1]) / 2.0f, bottom = (r[2][1] + r[3][1]) / 2.0f;
    float sx = (right - left) / 400.0f, sy = (bottom - top) / 240.0f;
    S->stateKv["touch.swap"] = swap ? "1" : "0";
    S->stateKv["touch.min_x"] = std::to_string(static_cast<int>(std::lround(left - 40 * sx)));
    S->stateKv["touch.max_x"] = std::to_string(static_cast<int>(std::lround(right + 39 * sx)));
    S->stateKv["touch.min_y"] = std::to_string(static_cast<int>(std::lround(top - 40 * sy)));
    S->stateKv["touch.max_y"] = std::to_string(static_cast<int>(std::lround(bottom + 39 * sy)));
    applyCalibration();
    saveState();
}

[[maybe_unused]] void showCalTarget() {
    lv_obj_set_pos(S->calTarget, kCalPts[S->calStep][0] - 15, kCalPts[S->calStep][1] - 15);
    lv_label_set_text_fmt(S->calText, "TOUCH CALIBRATION\nTap the centre of the target  (%d/4)", S->calStep + 1);
}

void startCalibration(lv_event_t*) {
#ifdef __linux__
    if (S->cfg.touchDevice.empty()) return;
    S->calFd = open(S->cfg.touchDevice.c_str(), O_RDONLY | O_NONBLOCK);
    if (S->calFd < 0) return;
    modalBase(kW, kH);
    lv_obj_t* box = modalBox();
    lv_obj_set_size(box, kW, kH);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_set_style_bg_color(box, C(0x000000), 0);
    S->calText = label(box, "", fMed, kText);
    lv_obj_set_style_text_align(S->calText, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(S->calText);
    S->calTarget = lv_obj_create(box);
    lv_obj_remove_style_all(S->calTarget);
    lv_obj_set_size(S->calTarget, 30, 30);
    lv_obj_set_style_radius(S->calTarget, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(S->calTarget, 3, 0);
    lv_obj_set_style_border_color(S->calTarget, C(kAccent2), 0);
    lv_obj_t* dot = lv_obj_create(S->calTarget);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 6, 6);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, C(kText), 0);
    lv_obj_center(dot);
    S->calibrating = true;
    S->calStep = 0;
    S->calStart = tickMs();
    showCalTarget();
#endif
}

void pollCalibration() {
#ifdef __linux__
    if (!S->calibrating) return;
    if (tickMs() - S->calStart > 60000) { finishCalibration(false); return; }
    input_event ev;
    while (read(S->calFd, &ev, sizeof ev) == static_cast<ssize_t>(sizeof ev)) {
        if (ev.type == EV_ABS && ev.code == ABS_X) S->rawX = ev.value;
        else if (ev.type == EV_ABS && ev.code == ABS_Y) S->rawY = ev.value;
        else if (ev.type == EV_KEY && ev.code == BTN_TOUCH && ev.value == 0) {
            S->calRaw[S->calStep][0] = S->rawX;
            S->calRaw[S->calStep][1] = S->rawY;
            if (++S->calStep >= 4) { finishCalibration(true); return; }
            showCalTarget();
        }
    }
#endif
}

// ---------------------------------------------------------------------------
// Top bar

void presetStep(lv_event_t* e) {
    int dir = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    int n = S->cfg.bank ? S->cfg.bank->count() : 0;
    if (n == 0) return;
    int cur = S->currentPreset < 0 ? 0 : S->currentPreset;
    loadPresetUi((cur + dir + n) % n);
}

void presetLabelClick(lv_event_t*) { lv_tabview_set_active(S->tabview, 6, LV_ANIM_OFF); }

lv_obj_t* meterBar(lv_obj_t* parent, int y) {
    lv_obj_t* b = lv_bar_create(parent);
    lv_obj_set_size(b, 58, 3);
    lv_obj_set_pos(b, 4, y);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_bar_set_range(b, 0, 100);
    lv_obj_set_style_bg_color(b, C(kLine), 0);
    lv_obj_set_style_bg_color(b, C(kText), LV_PART_INDICATOR);
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_set_style_radius(b, 0, LV_PART_INDICATOR);
    return b;
}

void showMaster() {
    int pct = static_cast<int>(std::lround(S->master * 100));
    if (S->volLabel) {
        if (S->muted) lv_label_set_text(S->volLabel, "MUTED");
        else lv_label_set_text_fmt(S->volLabel, LV_SYMBOL_VOLUME_MAX " %d", pct);
        lv_obj_set_style_text_color(S->volLabel, C(S->muted ? kSignal : kText), 0);
    }
    if (S->volBig) {
        if (S->muted) lv_label_set_text(S->volBig, "MUTED");
        else lv_label_set_text_fmt(S->volBig, "%d", pct);
        lv_obj_set_style_text_color(S->volBig, C(S->muted ? kSignal : kText), 0);
    }
    if (S->volSlider) lv_slider_set_value(S->volSlider, pct, LV_ANIM_OFF);
    setButtonOn(S->volMute, S->muted, kSignal);
}

void setMaster(float v, bool fromUser) {
    v = std::clamp(v, 0.0f, 1.0f);
    S->master = v;
    if (fromUser) {
        S->muted = false;
        S->unmuted = v;
    }
    if (S->cfg.status) S->cfg.status->master.store(S->muted ? 0.0f : v);
    if (!S->muted) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%.2f", v);
        markState("master.volume", buf);
    }
    showMaster();
}

void volSliderEvent(lv_event_t* e) {
    setMaster(lv_slider_get_value(lv_event_get_target_obj(e)) / 100.0f, true);
}

void volStep(lv_event_t* e) {
    int dir = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    setMaster(std::round(S->master * 20 + dir) / 20.0f, true);  // 5% steps
}

void volMuteEvent(lv_event_t*) {
    S->muted = !S->muted;
    if (S->cfg.status) S->cfg.status->master.store(S->muted ? 0.0f : S->unmuted);
    S->master = S->unmuted;
    showMaster();
}

void volClose(lv_event_t*) {
    closeModal();
    S->volSlider = S->volBig = S->volMute = nullptr;
}

void openVolume(lv_event_t*) {
    modalBase(440, 230);
    lv_obj_t* box = modalBox();
    lv_obj_t* t = label(box, "MASTER VOLUME", fLabel, kDim);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 6, 6);
    S->volBig = label(box, "", fLogo, kText);
    lv_obj_align(S->volBig, LV_ALIGN_TOP_RIGHT, -8, 0);

    S->volSlider = lv_slider_create(box);
    lv_obj_set_size(S->volSlider, 396, 36);
    lv_obj_align(S->volSlider, LV_ALIGN_TOP_MID, 0, 62);
    lv_slider_set_range(S->volSlider, 0, 100);
    lv_obj_set_style_radius(S->volSlider, 0, 0);
    lv_obj_set_style_radius(S->volSlider, 0, LV_PART_INDICATOR);
    lv_obj_set_style_radius(S->volSlider, 0, LV_PART_KNOB);
    lv_obj_set_style_bg_color(S->volSlider, C(kLine), 0);
    lv_obj_set_style_bg_color(S->volSlider, C(kText), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(S->volSlider, C(kSignal), LV_PART_KNOB);
    lv_obj_set_style_pad_all(S->volSlider, 6, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(S->volSlider, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(S->volSlider, volSliderEvent, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t* r = row(box, 420, 48);
    lv_obj_align(r, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    button(r, LV_SYMBOL_MINUS, 80, 44, volStep, reinterpret_cast<void*>(static_cast<intptr_t>(-1)));
    S->volMute = button(r, LV_SYMBOL_MUTE " MUTE", 110, 44, volMuteEvent);
    lv_obj_set_style_bg_color(S->volMute, C(kSignal), LV_STATE_CHECKED);
    lv_obj_set_style_border_color(S->volMute, C(kSignal), LV_STATE_CHECKED);
    button(r, LV_SYMBOL_PLUS, 80, 44, volStep, reinterpret_cast<void*>(static_cast<intptr_t>(1)));
    button(r, "DONE", 100, 44, volClose);
    lv_obj_t* hint = label(box, "ALSO ON MIDI CC7. KEPT ACROSS PRESETS AND REBOOTS.", fLabel, kDim);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 6, 108);
    showMaster();
}

void buildTopBar(lv_obj_t* scr) {
    lv_obj_t* top = lv_obj_create(scr);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, kW, kTopH);
    lv_obj_set_style_bg_color(top, C(kBg), 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(top, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(top, 2, 0);
    lv_obj_set_style_border_color(top, C(kText), 0);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* logo = row(top, 104, kTopH - 2);
    lv_obj_set_pos(logo, 8, 0);
    lv_obj_set_flex_align(logo, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    label(logo, "XO", fLogo, kText);
    label(logo, "//", fLogo, kDim);
    label(logo, "WAVE", fLogo, kText);

    lv_obj_t* prev = button(top, LV_SYMBOL_LEFT, 34, 28, presetStep, reinterpret_cast<void*>(static_cast<intptr_t>(-1)));
    lv_obj_set_pos(prev, 122, 3);
    S->presetLabel = label(top, "", fBig, kText);
    lv_obj_set_width(S->presetLabel, 136);
    lv_obj_set_pos(S->presetLabel, 158, 3);
    lv_label_set_long_mode(S->presetLabel, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(S->presetLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(S->presetLabel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(S->presetLabel, presetLabelClick, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* next = button(top, LV_SYMBOL_RIGHT, 34, 28, presetStep, reinterpret_cast<void*>(static_cast<intptr_t>(1)));
    lv_obj_set_pos(next, 296, 3);

    S->linkTop = label(top, "LINK", fLabel, kBg);   // shown while linked to a XOXO Studio
    lv_obj_set_pos(S->linkTop, 336, 11);

    S->midiDot = lv_obj_create(top);
    lv_obj_remove_style_all(S->midiDot);
    lv_obj_set_size(S->midiDot, 10, 10);
    lv_obj_set_pos(S->midiDot, 374, 12);
    lv_obj_set_style_radius(S->midiDot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(S->midiDot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(S->midiDot, C(kLine2), 0);

    S->btIcon = label(top, LV_SYMBOL_BLUETOOTH, fMed, kPanel2);
    lv_obj_set_pos(S->btIcon, 388, 8);

    // Master volume: level + meters, tap for the big fader.
    S->volButton = lv_obj_create(top);
    lv_obj_remove_style_all(S->volButton);
    lv_obj_set_size(S->volButton, 66, 30);
    lv_obj_set_pos(S->volButton, 408, 1);
    lv_obj_set_style_border_width(S->volButton, 1, 0);
    lv_obj_set_style_border_color(S->volButton, C(kLine2), 0);
    lv_obj_set_style_bg_color(S->volButton, C(kText), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(S->volButton, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(S->volButton, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(S->volButton, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(S->volButton, openVolume, LV_EVENT_CLICKED, nullptr);
    S->volLabel = label(S->volButton, "", fLabel, kText);
    lv_obj_set_pos(S->volLabel, 4, 1);
    S->meterL = meterBar(S->volButton, 19);
    S->meterR = meterBar(S->volButton, 24);
    S->voicesLabel = nullptr;
}

// ---------------------------------------------------------------------------
// Periodic sync with the engine

void loadState() {
    if (!S->cfg.statePath.empty()) readKeyValues(S->cfg.statePath, S->stateKv);
}

void saveState() {
    S->stateDirty = false;
    if (!S->cfg.statePath.empty() && !writeKeyValues(S->cfg.statePath, S->stateKv))
        std::fprintf(stderr, "xowave: cannot save settings to %s\n", S->cfg.statePath.c_str());
}

float meterPercent(float peak) {
    if (peak <= 0.0001f) return 0;
    float db = 20.0f * std::log10(peak);
    return std::clamp((db + 48.0f) / 48.0f * 100.0f, 0.0f, 100.0f);
}

void syncFromEngine() {
    EngineStatus* st = S->cfg.status;
    if (!st) return;
    uint32_t now = tickMs();

    uint32_t serial = st->paramSerial.load(std::memory_order_acquire);
    if (serial != S->lastSerial) {
        S->lastSerial = serial;
        for (int i = 0; i < P_COUNT; ++i) {
            float v = st->param[i].load(std::memory_order_relaxed);
            if (v == S->values[i] || static_cast<int32_t>(now - S->pendingUntil[i]) < 0) continue;
            S->values[i] = v;
            S->lastChange = now;
            for (auto& k : S->knobs) if (k->id == i && !k->dragging) updateKnob(*k);
            refreshViews(i);
        }
    }

    int preset = st->preset.load(std::memory_order_relaxed);
    if (preset != S->currentPreset && preset >= 0) S->currentPreset = preset;
    int n = S->cfg.bank ? S->cfg.bank->count() : 0;
    const char* name = (S->currentPreset >= 0 && S->currentPreset < n) ? S->cfg.bank->name(S->currentPreset) : "-";
    if (std::strcmp(lv_label_get_text(S->presetLabel), name) != 0) lv_label_set_text(S->presetLabel, name);
    if (S->listCount != n || S->listCurrent != S->currentPreset) rebuildPresetList();

    float pl = st->peakL.exchange(0.0f), pr = st->peakR.exchange(0.0f);
    S->meterShownL = std::max(meterPercent(pl), S->meterShownL - 6.0f);
    S->meterShownR = std::max(meterPercent(pr), S->meterShownR - 6.0f);
    lv_bar_set_value(S->meterL, static_cast<int32_t>(S->meterShownL), LV_ANIM_OFF);
    lv_bar_set_value(S->meterR, static_cast<int32_t>(S->meterShownR), LV_ANIM_OFF);
    uint32_t clipColor = std::max(pl, pr) > 0.98f ? kWarn : kAccent;
    lv_obj_set_style_bg_color(S->meterL, C(clipColor), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(S->meterR, C(clipColor), LV_PART_INDICATOR);

    if (!S->muted) {
        float m = st->master.load(std::memory_order_relaxed);
        if (std::fabs(m - S->master) > 0.004f) {  // moved by MIDI CC7
            std::fprintf(stderr, "xowave: master volume %.0f%% -> %.0f%% (MIDI CC7)\n", S->master * 100, m * 100);
            setMaster(m, true);
        }
    }
    uint32_t midi = st->midiEvents.load();
    if (midi != S->lastMidi) { S->lastMidi = midi; S->midiFlashUntil = now + 120; }
    lv_obj_set_style_bg_color(S->midiDot, C(static_cast<int32_t>(now - S->midiFlashUntil) < 0 ? kSignal : kLine2), 0);
    uint32_t btColor = S->cfg.btDevice.empty() ? kLine2 : (st->btConnected.load() ? kText : kDim);
    lv_obj_set_style_text_color(S->btIcon, C(btColor), 0);

    if (S->terrain && lv_tabview_get_tab_active(S->tabview) == 0) {
        float live = st->livePos[wtOsc()].load();
        int tIdx = static_cast<int>(S->values[wtOsc() ? P_B_TABLE : P_A_TABLE]);
        if (std::fabs(live - S->terrainPos) > 0.004f || tIdx != S->terrainTable) drawTerrain();
    }

    if (static_cast<int32_t>(now - S->sysNext) >= 0) { S->sysNext = now + 1000; updateSystemInfo(); updateLinkUi(); }
    if (S->btState.load() == 2) { S->btState = 0; showBtResults(); }
    pollLearn();
    pollUpdate();
    showTempo();
    trackUndo();
    if (S->xyPad && serial != S->lastSerialXy) { S->lastSerialXy = serial; showXy(); }
    if (S->midiInputsReady.exchange(false) && S->midiInputs) {
        std::lock_guard<std::mutex> lock(S->midiMutex);
        lv_label_set_text(S->midiInputs, S->midiInputsText.c_str());
    }
    if (S->stateDirty && static_cast<int32_t>(now - S->stateSaveAt) >= 0) saveState();
}

void syncTimer(lv_timer_t*) { syncFromEngine(); }
void calTimer(lv_timer_t*) { pollCalibration(); }

void styleTabBar() {
    lv_obj_t* bar = lv_tabview_get_tab_bar(S->tabview);
    lv_obj_set_style_bg_color(bar, C(kBg), 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(bar, 2, 0);
    lv_obj_set_style_border_color(bar, C(kText), 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    uint32_t n = lv_obj_get_child_count(bar);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* b = lv_obj_get_child(bar, static_cast<int32_t>(i));
        lv_obj_set_style_text_font(b, fLabel, 0);
        lv_obj_set_style_text_letter_space(b, 0, 0);   // 8 tabs: 60 px each
        lv_obj_set_style_pad_hor(b, 0, 0);
        lv_obj_set_style_text_color(b, C(kDim), 0);
        lv_obj_set_style_radius(b, 0, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_border_width(b, 0, LV_STATE_CHECKED);
        lv_obj_set_style_text_color(b, C(kBg), LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(b, C(kText), LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_CHECKED);
    }
}

void loadFonts();

int stateInt(const char* key, int def, int lo, int hi) {
    auto it = S->stateKv.find(key);
    if (it == S->stateKv.end() || it->second.empty()) return def;
    return std::clamp(std::atoi(it->second.c_str()), lo, hi);
}

// Put the screen back exactly as it was before the last power-off.
void restoreView() {
    S->octave = stateInt("ui.octave", 3, 0, 7);
    showOctave();
    setFocusOsc(stateInt("ui.osc", 0, 0, 2));
    showSettingsSection(stateInt("ui.settings", 0, 0, 5));
    lv_tabview_set_active(S->tabview, 0, LV_ANIM_OFF);  // always start on OSC; everything else is kept
}

void buildUi() {
    loadFonts();
    lv_theme_t* th = lv_theme_default_init(S->disp, C(kSignal), C(kDim), true, fBody);
    lv_display_set_theme(S->disp, th);

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, C(kBg), 0);
    lv_obj_set_style_text_color(scr, C(kText), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    buildTopBar(scr);

    S->tabview = lv_tabview_create(scr);
    lv_tabview_set_tab_bar_position(S->tabview, LV_DIR_BOTTOM);
    lv_tabview_set_tab_bar_size(S->tabview, kTabH);
    lv_obj_set_size(S->tabview, kW, kH - kTopH);
    lv_obj_set_pos(S->tabview, 0, kTopH);
    lv_obj_set_style_bg_color(S->tabview, C(kBg), 0);
    lv_obj_remove_flag(lv_tabview_get_content(S->tabview), LV_OBJ_FLAG_SCROLLABLE);  // no swipe: knobs need drags

    buildOsc(addTab("OSC"));
    buildFilter(addTab("FILTER"));
    buildEnv(addTab("ENV"));
    buildMod(addTab("MOD"));
    buildFx(addTab("FX"));
    buildKeys(addTab("PLAY"));
    setMatrixChecked(S->arpMode, static_cast<int>(S->values[P_ARP_MODE]));
    setButtonOn(S->arpHoldBtn, S->values[P_ARP_HOLD] >= 0.5f);
    buildPresets(addTab("PRESETS"));
    buildSettings(addTab("SETTINGS"));
    styleTabBar();

    showMaster();
    restoreView();
    S->built = true;
    lv_timer_create(syncTimer, 50, nullptr);
    lv_timer_create(calTimer, 15, nullptr);
    syncFromEngine();
}

std::vector<std::vector<uint8_t>> gFontData;

const lv_font_t* ttf(const char* file, int px, const lv_font_t* fallback) {
    std::vector<std::string> dirs = {S->cfg.fontsDir, "fonts", "/opt/xowave/fonts"};
    for (const std::string& d : dirs) {
        if (d.empty()) continue;
        FILE* f = std::fopen((d + "/" + file).c_str(), "rb");
        if (!f) continue;
        std::vector<uint8_t> data;
        uint8_t buf[8192];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) data.insert(data.end(), buf, buf + n);
        std::fclose(f);
        gFontData.push_back(std::move(data));
        const std::vector<uint8_t>& d2 = gFontData.back();
        lv_font_t* font = lv_tiny_ttf_create_data(d2.data(), d2.size(), px);
        if (!font) return fallback;
        font->fallback = fallback;
        return font;
    }
    std::fprintf(stderr, "xowave: font %s not found, using built-in\n", file);
    return fallback;
}

const lv_font_t* fSplash = &lv_font_montserrat_24;

void loadFonts() {
    if (S->fontsLoaded) return;
    S->fontsLoaded = true;
    gFontData.reserve(8);
    fSplash = ttf("Anton-Regular.ttf", 78, &lv_font_montserrat_24);
    fLabel = ttf("IBMPlexMono-SemiBold.ttf", 11, &lv_font_montserrat_12);
    fSmall = ttf("IBMPlexMono-Medium.ttf", 12, &lv_font_montserrat_12);
    fBody = ttf("IBMPlexMono-Medium.ttf", 14, &lv_font_montserrat_14);
    fMed = ttf("IBMPlexMono-SemiBold.ttf", 15, &lv_font_montserrat_16);
    fBig = ttf("Anton-Regular.ttf", 22, &lv_font_montserrat_20);
    fLogo = ttf("Anton-Regular.ttf", 26, &lv_font_montserrat_24);
}

void createState() { S = new State(); }

void applyConfig(const UiConfig& cfg) {
    S->cfg = cfg;
    if (cfg.tables) {
        for (const Wavetable& w : *cfg.tables) S->tableNames.push_back(w.name.c_str());
        S->numTables = static_cast<int>(cfg.tables->size());
    }
    if (cfg.status) {
        for (int i = 0; i < P_COUNT; ++i) S->values[i] = cfg.status->param[i].load();
        S->currentPreset = cfg.status->preset.load();
        S->master = S->unmuted = cfg.status->master.load();
    } else {
        for (int i = 0; i < P_COUNT; ++i) S->values[i] = kParams[i].def;
    }
    loadState();
}

void initState(const UiConfig& cfg) {
    createState();
    applyConfig(cfg);
}

// ---------------------------------------------------------------------------
// Splash screen

constexpr uint32_t kSplashMinMs = 1500;

void drawSplash() {
    lv_obj_t* scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, C(kBg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* logo = row(scr, kW, 96);
    lv_obj_align(logo, LV_ALIGN_CENTER, 0, -40);
    lv_obj_set_flex_align(logo, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    label(logo, "XO", fSplash, kText);
    label(logo, "//", fSplash, kDim);
    label(logo, "WAVE", fSplash, kText);

    lv_obj_t* rule = lv_obj_create(scr);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, 320, 2);
    lv_obj_set_style_bg_color(rule, C(kText), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_align(rule, LV_ALIGN_CENTER, 0, 14);

    lv_obj_t* tag = label(scr, "WAVETABLE SYNTH  -  BUILT BY HAND. PLAYED LOUD.", fLabel, kDim);
    lv_obj_align(tag, LV_ALIGN_CENTER, 0, 32);

    S->splashBar = lv_bar_create(scr);
    lv_obj_remove_style_all(S->splashBar);
    lv_obj_set_size(S->splashBar, 320, 4);
    lv_obj_align(S->splashBar, LV_ALIGN_CENTER, 0, 84);
    lv_bar_set_range(S->splashBar, 0, 100);
    lv_obj_set_style_bg_opa(S->splashBar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(S->splashBar, C(kLine), 0);
    lv_obj_set_style_bg_opa(S->splashBar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(S->splashBar, C(kSignal), LV_PART_INDICATOR);
    lv_obj_set_style_anim_duration(S->splashBar, 300, 0);

    S->splashLabel = label(scr, "", fLabel, kDim);
    lv_obj_align(S->splashLabel, LV_ALIGN_CENTER, 0, 102);

    lv_obj_t* lab = label(scr, "XOXO//AUDIO//LABS", fLabel, kLine2);
    lv_obj_align(lab, LV_ALIGN_BOTTOM_MID, 0, -10);
    S->splashShownAt = tickMs();
    S->splashDirty = true;
}

void updateSplash() {
    std::string text;
    int pct;
    {
        std::lock_guard<std::mutex> lock(S->cfgMutex);
        if (!S->splashDirty) return;
        S->splashDirty = false;
        text = S->splashText;
        pct = S->splashPercent;
    }
    if (S->splashLabel) lv_label_set_text(S->splashLabel, text.c_str());
    if (S->splashBar) lv_bar_set_value(S->splashBar, pct, LV_ANIM_ON);
}

// ---------------------------------------------------------------------------
// PNG writer (uncompressed deflate) for snapshots

uint32_t crc32(const uint8_t* d, size_t n, uint32_t c = 0xffffffffu) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t r = i;
            for (int k = 0; k < 8; ++k) r = (r & 1) ? 0xedb88320u ^ (r >> 1) : r >> 1;
            table[i] = r;
        }
        init = true;
    }
    for (size_t i = 0; i < n; ++i) c = table[(c ^ d[i]) & 0xff] ^ (c >> 8);
    return c;
}

bool writePng(const std::string& path, int w, int h, const std::vector<uint8_t>& rgb) {
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(h) * (w * 3 + 1));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb.begin() + static_cast<long>(y) * w * 3, rgb.begin() + static_cast<long>(y + 1) * w * 3);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t n = std::min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + n >= raw.size() ? 1 : 0);
        z.push_back(n & 0xff); z.push_back(n >> 8);
        z.push_back(~n & 0xff); z.push_back((~n >> 8) & 0xff);
        z.insert(z.end(), raw.begin() + static_cast<long>(pos), raw.begin() + static_cast<long>(pos + n));
        pos += n;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    uint32_t adler = (b << 16) | a;
    for (int i = 3; i >= 0; --i) z.push_back((adler >> (i * 8)) & 0xff);

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    auto be32 = [](std::vector<uint8_t>& v, uint32_t x) { for (int i = 3; i >= 0; --i) v.push_back((x >> (i * 8)) & 0xff); };
    auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        std::vector<uint8_t> c;
        be32(c, static_cast<uint32_t>(data.size()));
        c.insert(c.end(), type, type + 4);
        c.insert(c.end(), data.begin(), data.end());
        be32(c, crc32(c.data() + 4, c.size() - 4) ^ 0xffffffffu);
        std::fwrite(c.data(), 1, c.size(), f);
    };
    const uint8_t sig[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr;
    be32(ihdr, static_cast<uint32_t>(w)); be32(ihdr, static_cast<uint32_t>(h));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    std::fclose(f);
    return true;
}

std::vector<uint8_t> gSnapRgb;

// LVGL log -> stderr, minus the expected "glyph not in TTF" noise (symbols
// come from the built-in fallback font).
void lvLog(lv_log_level_t, const char* msg) {
    if (std::strstr(msg, "ttf_get_glyph") || std::strstr(msg, "cache not allocated")) return;
    std::fprintf(stderr, "xowave ui: %s", msg);
}

void snapFlush(lv_display_t* disp, const lv_area_t* area, uint8_t* px) {
    const uint16_t* p = reinterpret_cast<const uint16_t*>(px);
    for (int y = area->y1; y <= area->y2; ++y) {
        for (int x = area->x1; x <= area->x2; ++x) {
            uint16_t c = *p++;
            size_t o = (static_cast<size_t>(y) * kW + x) * 3;
            gSnapRgb[o] = static_cast<uint8_t>(((c >> 11) & 0x1f) * 255 / 31);
            gSnapRgb[o + 1] = static_cast<uint8_t>(((c >> 5) & 0x3f) * 255 / 63);
            gSnapRgb[o + 2] = static_cast<uint8_t>((c & 0x1f) * 255 / 31);
        }
    }
    lv_display_flush_ready(disp);
}

#ifdef __linux__
void uiThreadMain() {
    lv_init();
    lv_log_register_print_cb(lvLog);
    lv_tick_set_cb(tickMs);
    S->disp = lv_linux_fbdev_create();
    lv_linux_fbdev_set_file(S->disp, S->cfg.fbDevice.c_str());

    // Splash until the engine is ready.
    loadFonts();
    drawSplash();
    for (;;) {
        updateSplash();
        uint32_t wait = lv_timer_handler();
        bool ready;
        {
            std::lock_guard<std::mutex> lock(S->cfgMutex);
            ready = S->cfgReady;
        }
        if (S->quit.load()) return;
        if (ready && tickMs() - S->splashShownAt >= kSplashMinMs) break;
        if (wait == LV_NO_TIMER_READY || wait > 20) wait = 20;
        usleep(wait * 1000);
    }
    {
        std::lock_guard<std::mutex> lock(S->cfgMutex);
        std::string fb = S->cfg.fbDevice;
        applyConfig(S->pendingCfg);
        S->cfg.fbDevice = fb;
    }
    lv_bar_set_value(S->splashBar, 100, LV_ANIM_OFF);
    lv_refr_now(S->disp);
    lv_obj_clean(lv_screen_active());
    S->splashBar = S->splashLabel = nullptr;

    if (!S->cfg.touchDevice.empty()) {
        S->touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, S->cfg.touchDevice.c_str());
        applyCalibration();
    }
    buildUi();
    if (S->touch && S->stateKv.find("touch.min_x") == S->stateKv.end()) startCalibration(nullptr);  // first boot
    while (!S->quit.load()) {
        uint32_t wait = lv_timer_handler();
        if (wait == LV_NO_TIMER_READY || wait > 20) wait = 20;
        usleep(wait * 1000);
    }
    if (S->stateDirty) saveState();
}
#endif

}  // namespace

// ---------------------------------------------------------------------------
// Public API

static bool nameMatches(const std::string& n) {
    std::string l;
    for (char c : n) l += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return l.find("ili9") != std::string::npos || l.find("fb_") != std::string::npos ||
           l.find("st77") != std::string::npos || l.find("hx8357") != std::string::npos ||
           l.find("piscreen") != std::string::npos || l.find("mipi-dbi") != std::string::npos;
}

std::string uiFindFramebuffer(const std::string& want) {
    if (!want.empty() && want != "auto") return want;
    for (int i = 0; i < 4; ++i) {
        std::string name = readFirstLine(("/sys/class/graphics/fb" + std::to_string(i) + "/name").c_str());
        if (!name.empty() && nameMatches(name)) return "/dev/fb" + std::to_string(i);
    }
    return "";
}

std::string uiFindTouch(const std::string& want) {
    if (!want.empty() && want != "auto") return want;
    for (int i = 0; i < 16; ++i) {
        std::string name = readFirstLine(("/sys/class/input/event" + std::to_string(i) + "/device/name").c_str());
        std::string l;
        for (char c : name) l += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (l.find("ads7846") != std::string::npos || l.find("touch") != std::string::npos ||
            l.find("xpt2046") != std::string::npos)
            return "/dev/input/event" + std::to_string(i);
    }
    return "";
}

bool uiSplash(const std::string& fbDevice, const std::string& fontsDir) {
#ifdef __linux__
    if (S || fbDevice.empty()) return false;
    createState();
    S->cfg.fbDevice = fbDevice;
    S->cfg.fontsDir = fontsDir;
    S->thread = std::thread(uiThreadMain);
    return true;
#else
    (void)fbDevice;
    (void)fontsDir;
    return false;
#endif
}

void uiSplashStatus(const char* text, int percent) {
    if (!S) return;
    std::lock_guard<std::mutex> lock(S->cfgMutex);
    S->splashText = text;
    S->splashPercent = percent;
    S->splashDirty = true;
}

bool uiStart(const UiConfig& cfg) {
#ifdef __linux__
    if (cfg.fbDevice.empty()) return false;
    if (!S) uiSplash(cfg.fbDevice, cfg.fontsDir);
    uiSplashStatus("READY", 100);
    std::lock_guard<std::mutex> lock(S->cfgMutex);
    S->pendingCfg = cfg;
    S->cfgReady = true;
    return true;
#else
    (void)cfg;
    return false;
#endif
}

void uiStop() {
    if (!S) return;
    S->quit = true;
    if (S->thread.joinable()) S->thread.join();
}

bool uiSnapshot(const UiConfig& cfg, const std::string& dir) {
    initState(cfg);
    lv_init();
    lv_log_register_print_cb(lvLog);
    lv_tick_set_cb(tickMs);
    S->disp = lv_display_create(kW, kH);
    static std::vector<uint8_t> buf(static_cast<size_t>(kW) * kH * 2);
    gSnapRgb.assign(static_cast<size_t>(kW) * kH * 3, 0);
    lv_display_set_buffers(S->disp, buf.data(), nullptr, static_cast<uint32_t>(buf.size()), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(S->disp, snapFlush);
    loadFonts();
    drawSplash();
    lv_label_set_text(S->splashLabel, "LOADING WAVETABLES");
    lv_bar_set_value(S->splashBar, 45, LV_ANIM_OFF);
    lv_refr_now(S->disp);
    writePng(dir + "/0-splash.png", kW, kH, gSnapRgb);
    std::printf("wrote %s/0-splash.png\n", dir.c_str());
    lv_obj_clean(lv_screen_active());
    S->splashBar = S->splashLabel = nullptr;
    buildUi();

    auto shot = [&](const std::string& name) {
        lv_timer_handler();
        lv_refr_now(S->disp);
        std::string path = dir + "/" + name + ".png";
        bool ok = writePng(path, kW, kH, gSnapRgb);
        std::printf("%s %s\n", ok ? "wrote" : "FAILED", path.c_str());
    };
    static const char* names[] = {"1-osc-a", "2-filter", "3-env", "4-mod", "4b-fx", "5-play", "6-presets", "7-settings-midi"};
    for (uint32_t i = 0; i < 8; ++i) {
        lv_tabview_set_active(S->tabview, i, LV_ANIM_OFF);
        syncFromEngine();
        shot(names[i]);
    }
    lv_tabview_set_active(S->tabview, 3, LV_ANIM_OFF);
    {
        static const char* mv[] = {"", "4-mod-lfo2", "4-mod-matrix", "4-mod-perform"};
        // a sample routing so the matrix shows something
        S->values[P_M1_SRC] = 2; S->values[P_M1_DST] = 3; S->values[P_M1_AMT] = 0.4f;
        S->values[P_M2_SRC] = 5; S->values[P_M2_DST] = 1; S->values[P_M2_AMT] = -0.6f;
        for (int j = 0; j < 6; ++j) showParamReadout(P_M1_SRC + j);
        for (int i = 1; i < 4; ++i) { showModView(i); shot(mv[i]); }
        showModView(0);
    }
    lv_tabview_set_active(S->tabview, 7, LV_ANIM_OFF);
    static const char* secNames[] = {"", "7-settings-bluetooth", "7-settings-studio", "7-settings-audio", "7-settings-display",
                                     "7-settings-system"};
    for (int i = 1; i < 6; ++i) { showSettingsSection(i); shot(secNames[i]); }
    showSettingsSection(0);
    lv_tabview_set_active(S->tabview, 0, LV_ANIM_OFF);
    setFocusOsc(1);
    shot("1-osc-b");
    setFocusOsc(2);
    shot("1-osc-sub");
    setFocusOsc(0);
    {
        lv_tabview_set_active(S->tabview, 6, LV_ANIM_OFF);
        openUsb(nullptr);
        S->usbRoot = "/media/usb-sda1";
        S->usbFiles = {{"/media/usb-sda1/Tables/Analog Brass.wav", "Analog Brass", true},
                       {"/media/usb-sda1/Tables/Vox Morph.wav", "Vox Morph", true},
                       {"/media/usb-sda1/XO-WAVE/presets/09-dusk-pad.xowave", "09-dusk-pad", false}};
        fillUsbList();
        shot("8-usb");
        closeModal();
    }
    openUpdate(nullptr);
    S->updState = 0;
    lv_label_set_text(S->updStatus, "UPDATE AVAILABLE: 0.3.1 (a1b2c3d), INSTALLED 0.3.0");
    lv_obj_set_style_text_color(S->updStatus, C(kSignal), 0);
    lv_obj_remove_flag(S->updInstall, LV_OBJ_FLAG_HIDDEN);
    shot("8-update");
    closeModal();
    openXy(nullptr);
    shot("8-xy-pad");
    xyClose(nullptr);
    openVolume(nullptr);
    shot("8-volume");
    volClose(nullptr);
    openSaveAs(nullptr);
    shot("8-save-as");
    closeModal();
    openBluetooth(nullptr);
    {
        std::lock_guard<std::mutex> lock(S->btMutex);
        S->btFound = {{"AA:BB:CC:11:22:33", "JBL Flip 6"}, {"DD:EE:FF:44:55:66", "microKEY Air-37"}};
    }
    showBtResults();
    shot("9-bluetooth");
    closeModal();
    return true;
}

}  // namespace xo
