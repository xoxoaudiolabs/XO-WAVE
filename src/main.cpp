// XO-Wave — standalone wavetable synth for Raspberry Pi.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "studio_link.h"
#include "synth.h"
#include "ui/ui.h"
#include "wavetable.h"

#ifndef XO_NO_ALSA
#include <sched.h>
#include <unistd.h>
#include <sys/mman.h>

#include "alsa_io.h"
#include "bt_output.h"
#endif

using namespace xo;

static void usage() {
    std::printf(
        "XO-Wave wavetable synth\n\n"
        "usage: xowave [options]\n"
        "  -c, --config FILE    config file (default /etc/xowave.conf, then ./xowave.conf)\n"
        "  -d, --device NAME    override audio.device\n"
        "      --list-devices   list ALSA playback devices\n"
        "      --list-tables    list wavetables\n"
        "      --list-params    list preset / CC parameter names\n"
        "      --render FILE    render a demo to a WAV file (no audio hardware needed)\n"
        "      --seconds N      length of --render (default: 2 s per preset)\n"
        "      --bench          measure CPU use at full polyphony\n"
        "      --ui-snapshot DIR  render every UI page to PNG files (no display needed)\n");
}

// ---------------------------------------------------------------------------

static void applyCcMap(Synth& synth, const KeyValues& conf) {
    const std::pair<int, int> defaults[] = {
        {74, P_CUTOFF}, {71, P_RESO}, {73, P_AMP_A}, {72, P_AMP_R}, {20, P_A_POS}, {21, P_B_POS},
        {22, P_A_TABLE}, {23, P_B_TABLE}, {24, P_LFO_RATE}, {25, P_LFO_POS}, {26, P_FENV},
        {27, P_MENV_POS}, {5, P_GLIDE},
    };
    for (auto [cc, id] : defaults) synth.setCcMapping(cc, id);
    // Config file and MIDI-learned mappings override the defaults ("none" unmaps).
    for (const auto& [k, v] : conf) {
        if (k.rfind("cc.", 0) != 0) continue;
        int cc = std::atoi(k.c_str() + 3);
        if (cc < 0 || cc > 127) continue;
        if (v == "none" || v.empty()) { synth.setCcMapping(cc, -1); continue; }
        int id = findParam(v);
        if (id < 0) std::fprintf(stderr, "xowave: bad CC mapping '%s = %s'\n", k.c_str(), v.c_str());
        else synth.setCcMapping(cc, id);
    }
}

static PresetBank gBank;
static EngineStatus gStatus;
static CommandQueue gCmds;

// Applies commands from the touchscreen UI (called on the audio thread).
// Notes played on the screen go to a linked Studio too.
static void drainCommands(Synth& synth, StudioLink* link = nullptr) {
    Cmd c;
    while (gCmds.pop(c)) {
        switch (c.type) {
            case CmdType::SetParam: synth.setParam(c.a, c.b); break;
            case CmdType::NoteOn: {
                synth.noteOn(c.a, static_cast<int>(c.b));
                const uint8_t m[3] = {0x90, static_cast<uint8_t>(c.a & 127), static_cast<uint8_t>(static_cast<int>(c.b) & 127)};
                if (link) link->pushMidiOut(m, 3);
                break;
            }
            case CmdType::NoteOff: {
                synth.noteOff(c.a);
                const uint8_t m[3] = {0x80, static_cast<uint8_t>(c.a & 127), 0};
                if (link) link->pushMidiOut(m, 3);
                break;
            }
            case CmdType::Program: synth.programChange(c.a); break;
            case CmdType::AllNotesOff: synth.allNotesOff(false); break;
        }
    }
}

static UiConfig makeUiConfig(const KeyValues& conf, const std::vector<Wavetable>& tables, unsigned rate,
                             unsigned period) {
    UiConfig ui;
    ui.cmds = &gCmds;
    ui.status = &gStatus;
    ui.bank = &gBank;
    ui.tables = &tables;
    ui.presetsDir = kvGet(conf, "presets.dir", "presets");
    ui.statePath = kvGet(conf, "ui.state", "");
    ui.fontsDir = kvGet(conf, "ui.fonts", "/opt/xowave/fonts");
    ui.audioDevice = kvGet(conf, "audio.device", "default");
    ui.btDevice = kvGet(conf, "bluetooth.audio", "");
    ui.rate = rate;
    ui.period = period;
    ui.periods = period ? static_cast<unsigned>(std::max(2, kvGetInt(conf, "audio.periods", 2))) : 2;
    return ui;
}

static void fillPresetBank(const KeyValues& conf, const std::vector<Wavetable>& tables) {
    std::string dir = kvGet(conf, "presets.dir", "presets");
    auto presets = loadPresetDir(dir, tables);
    if (presets.empty()) presets = loadPresetDir("presets", tables);  // running from the source folder
    for (const Preset& p : presets) gBank.write(gBank.count(), p.name.c_str(), p.file.c_str(), p.values.data());
}

static void applyStartupPreset(Synth& synth, const KeyValues& conf) {
    if (gBank.count() == 0) return;
    std::string want = kvGet(conf, "preset.startup", "0");
    KeyValues state;
    std::string statePath = kvGet(conf, "ui.state", "");
    if (!statePath.empty() && readKeyValues(statePath, state) && gBank.find(kvGet(state, "preset", "").c_str()) >= 0)
        want = kvGet(state, "preset", "");
    int idx = gBank.find(want.c_str());
    if (idx < 0) {
        char* end = nullptr;
        long n = std::strtol(want.c_str(), &end, 10);
        idx = (end && *end == 0 && n >= 0 && n < gBank.count()) ? static_cast<int>(n) : 0;
    }
    float v[P_COUNT];
    gBank.read(idx, v);
    synth.loadPreset(v, idx);
    std::fprintf(stderr, "xowave: preset %d: %s\n", idx, gBank.name(idx));
}

static std::unique_ptr<Synth> makeSynth(float rate, const KeyValues& conf, const std::vector<Wavetable>& tables) {
    auto synth = std::make_unique<Synth>(rate, kvGetInt(conf, "synth.voices", 8), tables);
    applyCcMap(*synth, conf);
    if (gBank.count() == 0) fillPresetBank(conf, tables);
    synth->setPresetBank(&gBank);
    synth->setStatus(&gStatus);
    applyStartupPreset(*synth, conf);
    return synth;
}

// ---------------------------------------------------------------------------

static bool writeWav(const std::string& path, const std::vector<float>& L, const std::vector<float>& R, int rate) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    uint32_t dataBytes = static_cast<uint32_t>(L.size() * 4);
    f.write("RIFF", 4); u32(36 + dataBytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16(2); u32(rate); u32(rate * 4); u16(4); u16(16);
    f.write("data", 4); u32(dataBytes);
    for (size_t i = 0; i < L.size(); ++i) {
        for (float s : {L[i], R[i]}) {
            float c = s < -1.0f ? -1.0f : (s > 1.0f ? 1.0f : s);
            u16(static_cast<uint16_t>(static_cast<int16_t>(c * 32767.0f)));
        }
    }
    return static_cast<bool>(f);
}

// Plays a chord progression, switching preset every 2 seconds.
static int renderDemo(Synth& synth, int rate, const std::string& path, double seconds) {
    static const int chords[4][4] = {{48, 60, 63, 67}, {44, 60, 63, 68}, {46, 58, 62, 65}, {43, 59, 62, 67}};
    const int numPresets = gBank.count();
    if (seconds <= 0) seconds = 2.0 * (numPresets > 0 ? numPresets : 4) + 2.0;
    const size_t total = static_cast<size_t>(seconds * rate);
    std::vector<float> L(total), R(total);
    const int block = 128;
    int seg = -1;
    bool chordOn = false;

    for (size_t pos = 0; pos < total; pos += block) {
        double t = static_cast<double>(pos) / rate;
        int s = static_cast<int>(t / 2.0);
        double inSeg = t - s * 2.0;
        if (s != seg && t < seconds - 2.0) {
            seg = s;
            if (numPresets > 0) {
                synth.programChange(s);
                std::fprintf(stderr, "  %5.1fs  preset %s\n", t, gBank.name(s % numPresets));
            }
            for (int n : chords[s % 4]) synth.noteOn(n, 100);
            chordOn = true;
        }
        if (chordOn && inSeg >= 1.5) {
            for (int n : chords[seg % 4]) synth.noteOff(n);
            chordOn = false;
        }
        int n = static_cast<int>(std::min<size_t>(block, total - pos));
        synth.render(&L[pos], &R[pos], n);
    }
    float peak = 0;
    for (size_t i = 0; i < total; ++i) peak = std::max(peak, std::max(std::fabs(L[i]), std::fabs(R[i])));
    if (!writeWav(path, L, R, rate)) { std::fprintf(stderr, "xowave: cannot write %s\n", path.c_str()); return 1; }
    std::printf("wrote %s (%.1f s, peak %.2f)\n", path.c_str(), seconds, peak);
    return 0;
}

static int bench(Synth& synth, int rate, int voices) {
    // Worst case: every voice playing, 4x unison on both oscillators, drive on.
    synth.setParam(P_A_UNISON, 4);
    synth.setParam(P_B_UNISON, 4);
    synth.setParam(P_A_LEVEL, 0.8f);
    synth.setParam(P_B_LEVEL, 0.6f);
    synth.setParam(P_DRIVE, 0.5f);
    synth.setParam(P_LFO_POS, 0.5f);
    synth.setParam(P_AMP_S, 1.0f);
    // ... with every effect on.
    for (int id : {P_CHORUS_MIX, P_DELAY_MIX, P_REVERB_MIX, P_FX_DRIVE}) synth.setParam(id, 0.5f);
    for (int i = 0; i < voices; ++i) synth.noteOn(40 + i * 3, 100);

    const int block = 128;
    const double seconds = 10.0;
    std::vector<float> L(block), R(block);
    auto t0 = std::chrono::steady_clock::now();
    for (int done = 0; done < seconds * rate; done += block) synth.render(L.data(), R.data(), block);
    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%d voices x 2 osc x 4 unison + all FX: %.1f%% of one CPU core (%d active)\n", voices,
                100.0 * elapsed / seconds, synth.activeVoices());
    return 0;
}

// ---------------------------------------------------------------------------

#ifndef XO_NO_ALSA
static volatile std::sig_atomic_t gRunning = 1;
static void onSignal(int) { gRunning = 0; }

static void goRealtime(int priority) {
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) std::perror("xowave: mlockall (continuing)");
    sched_param sp{};
    sp.sched_priority = priority;
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
        std::perror("xowave: real-time priority unavailable (continuing; expect more xruns)");
}

static int runLive(const KeyValues& conf, const std::vector<Wavetable>& tables, const std::string& deviceOverride) {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    std::string device = deviceOverride.empty() ? kvGet(conf, "audio.device", "default") : deviceOverride;
    AlsaAudio audio;
    std::string err;
    uiSplashStatus("OPENING AUDIO", 55);
    if (!audio.open(device, kvGetInt(conf, "audio.rate", 48000), kvGetInt(conf, "audio.period", 128),
                    kvGetInt(conf, "audio.periods", 3), err)) {
        std::fprintf(stderr, "xowave: %s\n", err.c_str());
        uiSplashStatus("NO AUDIO OUTPUT - RETRYING", 55);
        std::this_thread::sleep_for(std::chrono::seconds(3));  // systemd restarts us
        uiStop();
        return 1;
    }
    uiSplashStatus("STARTING SYNTH", 75);
    std::fprintf(stderr, "xowave: audio %s  %u Hz  period %u  buffer %u (%.1f ms)  %s\n", device.c_str(), audio.rate(),
                 audio.period(), audio.bufferSize(), 1000.0 * audio.bufferSize() / audio.rate(), audio.formatName());

    auto synth = makeSynth(static_cast<float>(audio.rate()), conf, tables);

    // Pick up exactly where we left off: the last sound, including edits
    // that were never saved as a preset.
    const std::string sessionPath = kvGet(conf, "session.file", "/var/lib/xowave/session.xowave");
    {
        KeyValues kv;
        if (readKeyValues(sessionPath, kv)) {
            std::vector<std::string> warnings;
            Preset p = presetFromKeyValues("session", kv, tables, warnings);
            synth->loadPreset(p.values.data(), gStatus.preset.load());
            std::fprintf(stderr, "xowave: restored last session\n");
        }
    }

    uiSplashStatus("CONNECTING MIDI", 85);
    AlsaMidi midi;
    if (!midi.open("XO-Wave", err)) std::fprintf(stderr, "xowave: %s (continuing without MIDI)\n", err.c_str());
    gStatus.midiChannel.store(kvGetInt(conf, "midi.channel", 0));

    // XOXO Studio link (its own thread, normal priority). While linked, the
    // Studio is the audio out: the jack stays on as a monitor, Bluetooth pauses.
    StudioLink link;
    {
        StudioLink::Config lc;
        lc.enabled = kvGet(conf, "link.enabled", "on") != "off";
        lc.host = kvGet(conf, "link.studio", "auto");
        lc.port = kvGetInt(conf, "link.studio_port", 5004);
        lc.audioPort = kvGetInt(conf, "link.port", 46030);
        char host[64] = "xowave";
        gethostname(host, sizeof host - 1);
        lc.name = std::string("XO-Wave on ") + host;
        link.start(lc);
    }

    // Bluetooth thread must be created before we go real-time so it stays
    // at normal priority.
    BtOutput bt;
    std::string btDev = kvGet(conf, "bluetooth.audio", "");
    if (!btDev.empty()) bt.start(btDev, audio.rate(), static_cast<unsigned>(kvGetInt(conf, "bluetooth.latency_ms", 150)));

    // Touchscreen UI (normal priority thread, started before we go real-time).
    bool uiRunning = false;
    std::string uiMode = kvGet(conf, "ui.enabled", "auto");
    if (uiMode != "off") {
        UiConfig ui = makeUiConfig(conf, tables, audio.rate(), audio.period());
        ui.periods = audio.bufferSize() / std::max(1u, audio.period());
        ui.linkDescribe = [&link] { return link.describe(); };
        ui.linkEnabled = [&link] { return link.enabled(); };
        ui.linkSetEnabled = [&link](bool on) { link.setEnabled(on); };
        ui.fbDevice = uiFindFramebuffer(kvGet(conf, "ui.fb", "auto"));
        ui.touchDevice = uiFindTouch(kvGet(conf, "ui.touch", "auto"));
        if (ui.fbDevice.empty()) {
            std::fprintf(stderr, "xowave: no TFT framebuffer found; running headless\n");
        } else {
            std::fprintf(stderr, "xowave: UI on %s, touch %s\n", ui.fbDevice.c_str(),
                         ui.touchDevice.empty() ? "none" : ui.touchDevice.c_str());
            uiRunning = uiStart(ui);
        }
    }

    // Session autosave: writes the current sound ~1 s after it stops changing
    // (screen, MIDI knobs or presets). Normal priority; never touches the audio thread.
    std::atomic<bool> saverQuit{false};
    std::thread saver([&] {
        auto save = [&] {
            float v[P_COUNT];
            for (int i = 0; i < P_COUNT; ++i) v[i] = gStatus.param[i].load(std::memory_order_relaxed);
            int idx = gStatus.preset.load();
            const char* name = (idx >= 0 && idx < gBank.count()) ? gBank.name(idx) : "Session";
            if (!savePresetFile(sessionPath, name, v, tables))
                std::fprintf(stderr, "xowave: cannot save session to %s\n", sessionPath.c_str());
        };
        uint32_t saved = gStatus.paramSerial.load(), seen = saved;
        int quiet = 0;
        bool clockWas = false;
        while (!saverQuit.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            bool clock = gStatus.extClock.load();
            static float bpmLogged = 0;   // log the clock's tempo when it changes
            if (clock && std::fabs(gStatus.bpmNow.load() - bpmLogged) >= 0.5f && clockWas) {
                bpmLogged = gStatus.bpmNow.load();
                std::fprintf(stderr, "xowave: MIDI clock now %.1f BPM\n", bpmLogged);
            }
            if (clock && !clockWas) bpmLogged = gStatus.bpmNow.load();
            if (clock != clockWas) {
                clockWas = clock;
                if (clock) std::fprintf(stderr, "xowave: following MIDI clock, %.1f BPM\n", gStatus.bpmNow.load());
                else std::fprintf(stderr, "xowave: MIDI clock stopped; internal tempo %.0f BPM\n", gStatus.tempo.load());
            }
            uint32_t now = gStatus.paramSerial.load(std::memory_order_acquire);
            if (now != seen) { seen = now; quiet = 0; continue; }
            if (seen != saved && ++quiet >= 2) { save(); saved = seen; }
        }
        if (gStatus.paramSerial.load() != saved) save();  // final save on shutdown
    });

    goRealtime(kvGetInt(conf, "audio.priority", 70));
    enableFlushToZero();

    const int period = static_cast<int>(audio.period());
    std::vector<float> L(period), R(period);
    uint64_t lastXruns = 0;
    uint64_t frames = 0;
    std::fprintf(stderr, "xowave: running\n");

    const double periodSec = static_cast<double>(period) / audio.rate();
    float cpu = 0;
    while (gRunning && gStatus.running.load(std::memory_order_relaxed)) {
        drainCommands(*synth, &link);
        const int ch = gStatus.midiChannel.load(std::memory_order_relaxed);
        int ev = midi.poll(*synth, ch, [](void* l, const uint8_t* m, int n) { static_cast<StudioLink*>(l)->pushMidiOut(m, n); },
                           &link);
        MidiMsg nm;
        while (link.popMidiIn(nm)) { applyMidi(*synth, nm, ch); ++ev; }   // the Studio's routes playing XO-Wave
        if (ev) gStatus.midiEvents.fetch_add(static_cast<uint32_t>(ev), std::memory_order_relaxed);
        auto t0 = std::chrono::steady_clock::now();
        synth->render(L.data(), R.data(), period);
        double used = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        cpu += (static_cast<float>(used / periodSec) - cpu) * 0.02f;
        gStatus.cpu.store(cpu, std::memory_order_relaxed);
        gStatus.xruns.store(static_cast<uint32_t>(audio.xruns()), std::memory_order_relaxed);
        gStatus.btConnected.store(bt.connected(), std::memory_order_relaxed);
        // While a Studio plays XO-Wave's sound it is the audio out (and owns the
        // speaker): Bluetooth pauses, the jack stays on. An older Studio links for MIDI only.
        const bool toStudio = link.audioAccepted();
        gStatus.linkState.store(link.state(), std::memory_order_relaxed);
        if (toStudio) link.sendAudio(L.data(), R.data(), period);
        else bt.push(L.data(), R.data(), static_cast<size_t>(period));
        if (!audio.write(L.data(), R.data(), period)) break;
        frames += static_cast<uint64_t>(period);
        if (frames >= 10ull * audio.rate()) {
            frames = 0;
            if (audio.xruns() != lastXruns) {
                std::fprintf(stderr, "xowave: %llu xruns so far (raise audio.period if this keeps growing)\n",
                             static_cast<unsigned long long>(audio.xruns()));
                lastXruns = audio.xruns();
            }
        }
    }
    std::fprintf(stderr, "xowave: stopping\n");
    if (uiRunning) uiStop();
    saverQuit = true;
    saver.join();
    link.stop();   // says BY, so the Studio drops XO-WAVE at once
    bt.stop();
    // Exit code 1 (restart requested from the UI or audio failure) makes systemd restart us.
    return (gRunning && !gStatus.running.load()) ? 1 : (gRunning ? 1 : 0);
}
#endif

int main(int argc, char** argv) {
    std::string configPath, renderPath, device, snapshotDir;
    double seconds = 0;
    bool doBench = false, listTables = false, listParams = false, listDevices = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "xowave: %s needs a value\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "-c" || a == "--config") configPath = next();
        else if (a == "-d" || a == "--device") device = next();
        else if (a == "--render") renderPath = next();
        else if (a == "--seconds") seconds = std::atof(next().c_str());
        else if (a == "--bench") doBench = true;
        else if (a == "--ui-snapshot") snapshotDir = next();
        else if (a == "--list-tables") listTables = true;
        else if (a == "--list-params") listParams = true;
        else if (a == "--list-devices") listDevices = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { std::fprintf(stderr, "xowave: unknown option %s\n", a.c_str()); usage(); return 2; }
    }

    if (listParams) {
        for (int i = 0; i < P_COUNT; ++i)
            std::printf("%-12s %8g .. %-8g default %-8g %s\n", kParams[i].name, kParams[i].min, kParams[i].max,
                        kParams[i].def, kParams[i].help);
        return 0;
    }
    if (listDevices) {
#ifndef XO_NO_ALSA
        AlsaAudio::listDevices();
        return 0;
#else
        std::fprintf(stderr, "xowave: built without ALSA\n");
        return 1;
#endif
    }

    KeyValues conf;
    if (!configPath.empty()) {
        if (!readKeyValues(configPath, conf)) { std::fprintf(stderr, "xowave: cannot read %s\n", configPath.c_str()); return 1; }
    } else if (!readKeyValues("/etc/xowave.conf", conf)) {
        readKeyValues("xowave.conf", conf);
    }

    // Settings changed on the touchscreen live in the UI state file and
    // override the config file.
    {
        KeyValues state;
        std::string statePath = kvGet(conf, "ui.state", "");
        if (!statePath.empty() && readKeyValues(statePath, state)) {
            for (const auto& [k, v] : state) {
                if (k.rfind("cc.", 0) == 0 || k == "midi.channel" || k == "audio.period" || k == "synth.voices" ||
                    k == "bluetooth.latency_ms" || k == "master.volume" || k == "link.enabled" || k == "tempo")
                    conf[k] = v;
            }
        }
    }

    gStatus.master.store(std::clamp(static_cast<float>(std::atof(kvGet(conf, "master.volume", "0.8").c_str())), 0.0f, 1.0f));
    std::fprintf(stderr, "xowave: master volume %.0f%%\n", gStatus.master.load() * 100);
    gStatus.tempo.store(std::clamp(static_cast<float>(std::atof(kvGet(conf, "tempo", "120").c_str())), 30.0f, 300.0f));

#ifndef XO_NO_ALSA
    // Splash on the touchscreen as early as possible, before the slow work.
    const bool liveMode = renderPath.empty() && !doBench && snapshotDir.empty() && !listTables;
    if (liveMode && kvGet(conf, "ui.enabled", "auto") != "off") {
        std::string fb = uiFindFramebuffer(kvGet(conf, "ui.fb", "auto"));
        if (!fb.empty() && uiSplash(fb, kvGet(conf, "ui.fonts", "/opt/xowave/fonts")))
            uiSplashStatus("LOADING WAVETABLES", 15);
    }
#endif

    auto t0 = std::chrono::steady_clock::now();
    std::vector<Wavetable> tables = makeFactoryTables();
    loadWavetableDir(kvGet(conf, "wavetables.dir", "wavetables"), tables);
    std::fprintf(stderr, "xowave: %zu wavetables ready in %.2f s\n", tables.size(),
                 std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());

    if (listTables) {
        for (size_t i = 0; i < tables.size(); ++i)
            std::printf("%3zu  %-20s %d frames\n", i, tables[i].name.c_str(), tables[i].numFrames());
        return 0;
    }

    const int rate = kvGetInt(conf, "audio.rate", 48000);
    if (!renderPath.empty()) {
        auto synth = makeSynth(static_cast<float>(rate), conf, tables);
        return renderDemo(*synth, rate, renderPath, seconds);
    }
    if (!snapshotDir.empty()) {
        auto synth = makeSynth(static_cast<float>(rate), conf, tables);
        // Play a chord with some movement so meters and the wavetable view show life.
        synth->setParam(P_LFO_POS, 0.4f);
        for (int n : {48, 55, 60, 63}) synth->noteOn(n, 100);
        std::vector<float> L(128), R(128);
        for (int i = 0; i < 60; ++i) synth->render(L.data(), R.data(), 128);
        UiConfig ui = makeUiConfig(conf, tables, static_cast<unsigned>(rate), 128);
        ui.fontsDir = "fonts";
        ui.presetsDir = snapshotDir;  // never write into the real presets folder
        // Show the Studio link as it looks when linked.
        gStatus.linkState.store(2);
        ui.linkDescribe = [] { return std::string("LINKED: XOXO Studio on studio\n169.254.12.7"); };
        ui.linkEnabled = [] { return true; };
        return uiSnapshot(ui, snapshotDir) ? 0 : 1;
    }
    if (doBench) {
        int voices = kvGetInt(conf, "synth.voices", 8);
        auto synth = makeSynth(static_cast<float>(rate), conf, tables);
        return bench(*synth, rate, voices);
    }

#ifndef XO_NO_ALSA
    return runLive(conf, tables, device);
#else
    std::fprintf(stderr, "xowave: built without ALSA; use --render or --bench on this machine\n");
    return 1;
#endif
}
